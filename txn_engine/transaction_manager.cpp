// Lab 8: Transaction manager — MVCC + Strict 2PL + deadlock detection
//   (implementation). State lives in the TransactionManager object:
//     * transactions_ : per-tx status, snapshot xid, 2PL phase flag
//     * heap_         : per-key MVCC version chain (newest first)
//     * lock_table_   : per-key lock queue (Strict 2PL)
//     * waits_for_    : waits-for graph for deadlock detection
#include "transaction_manager.h"

#include <functional>
#include <iostream>

namespace txn {

// ───────────────────────────── transaction table ─────────────────────────────

TxID TransactionManager::begin() {
    std::lock_guard<std::mutex> lk(tx_mutex_);
    TxID xid = next_xid_.fetch_add(1);
    // snapshot = this xid: only commits that finished before us are visible.
    transactions_[xid] = Transaction{xid, xid, TxStatus::ACTIVE, false};
    return xid;
}

bool TransactionManager::is_committed(TxID xid) {
    std::lock_guard<std::mutex> lk(tx_mutex_);
    auto it = transactions_.find(xid);
    return it != transactions_.end() && it->second.status == TxStatus::COMMITTED;
}

TxID TransactionManager::snapshot_of(TxID xid) {
    std::lock_guard<std::mutex> lk(tx_mutex_);
    return transactions_.at(xid).snapshot_xid;
}

// ───────────────────────────── MVCC version chain ─────────────────────────────

bool TransactionManager::is_visible(const RowVersion& v, TxID snapshot_xid, TxID reader_xid) {
    // The creating tx must be ourselves, or a commit that landed before our snapshot.
    bool xmin_ok = (v.xmin == reader_xid)
                || (is_committed(v.xmin) && v.xmin < snapshot_xid);
    if (!xmin_ok) return false;

    if (v.xmax == 0) return true;                  // never deleted -> visible
    // Deleted: invisible only if WE deleted it, or a commit deleted it before our snapshot.
    bool xmax_invisible = (v.xmax == reader_xid)
                        || (is_committed(v.xmax) && v.xmax < snapshot_xid);
    return !xmax_invisible;
}

std::optional<std::string> TransactionManager::mvcc_read_key(const RowKey& key, TxID xid) {
    std::lock_guard<std::mutex> lk(heap_mutex_);
    TxID snap = snapshot_of(xid);
    auto it = heap_.find(key);
    if (it == heap_.end()) return std::nullopt;
    for (auto& v : it->second)
        if (is_visible(v, snap, xid)) return v.value;
    return std::nullopt;
}

void TransactionManager::mvcc_insert(const RowKey& key, const std::string& value, TxID xid) {
    std::lock_guard<std::mutex> lk(heap_mutex_);
    heap_[key].push_front({value, xid, 0});
}

void TransactionManager::mvcc_update(const RowKey& key, const std::string& new_value, TxID xid) {
    std::lock_guard<std::mutex> lk(heap_mutex_);
    TxID snap = snapshot_of(xid);
    auto it = heap_.find(key);
    if (it != heap_.end())
        for (auto& v : it->second)
            if (is_visible(v, snap, xid) && v.xmax == 0) { v.xmax = xid; break; }  // retire old
    heap_[key].push_front({new_value, xid, 0});                                    // add new
}

void TransactionManager::mvcc_delete(const RowKey& key, TxID xid) {
    std::lock_guard<std::mutex> lk(heap_mutex_);
    TxID snap = snapshot_of(xid);
    auto it = heap_.find(key);
    if (it == heap_.end()) return;
    for (auto& v : it->second)
        if (is_visible(v, snap, xid) && v.xmax == 0) { v.xmax = xid; return; }
}

// ───────────────────────────── lock manager (Strict 2PL) ─────────────────────────────

// DFS cycle check over waits_for_. Caller must hold lm_mutex_.
bool TransactionManager::has_cycle(TxID start) {
    std::unordered_set<TxID> visited, stack;
    std::function<bool(TxID)> dfs = [&](TxID node) -> bool {
        visited.insert(node);
        stack.insert(node);
        auto it = waits_for_.find(node);
        if (it != waits_for_.end()) {
            for (TxID nb : it->second) {
                if (stack.count(nb)) return true;
                if (!visited.count(nb) && dfs(nb)) return true;
            }
        }
        stack.erase(node);
        return false;
    };
    return dfs(start);
}

void TransactionManager::acquire_lock(const RowKey& key, TxID xid, LockMode mode) {
    {   // 2PL: once shrinking, no new locks may be acquired.
        std::lock_guard<std::mutex> lk(tx_mutex_);
        if (transactions_.at(xid).in_shrinking)
            throw std::runtime_error("2PL violation: cannot acquire lock in shrinking phase");
    }

    LockQueue& lq = lock_table_[key];              // node-based map: reference stays valid
    std::unique_lock<std::mutex> ul(lq.mu);

    for (auto& r : lq.requests)                    // already hold a compatible lock?
        if (r.xid == xid && r.granted) {
            if (mode == LockMode::SHARED) return;
            if (r.mode == LockMode::EXCLUSIVE) return;
        }

    lq.requests.push_back({xid, mode, false});
    auto& my_req = lq.requests.back();

    while (true) {
        bool conflict = false;
        std::unordered_set<TxID> blocking;
        for (auto& r : lq.requests) {
            if (&r == &my_req) break;              // only earlier requests can block us
            if (!r.granted) continue;
            if (mode == LockMode::EXCLUSIVE || r.mode == LockMode::EXCLUSIVE)
                if (r.xid != xid) { conflict = true; blocking.insert(r.xid); }
        }

        if (!conflict) {                           // grant
            my_req.granted = true;
            std::lock_guard<std::mutex> lk(lm_mutex_);
            waits_for_.erase(xid);
            return;
        }

        {   // record waits-for edges, then abort if we just closed a cycle
            std::lock_guard<std::mutex> lk(lm_mutex_);
            waits_for_[xid] = blocking;
            if (has_cycle(xid)) {
                waits_for_.erase(xid);
                lq.requests.remove_if([&](const LockRequest& r){ return r.xid == xid && !r.granted; });
                throw DeadlockException(xid);
            }
        }
        lq.cv.wait(ul);                            // sleep until a release wakes us
    }
}

void TransactionManager::release_locks(TxID xid) {
    {   // Strict 2PL: the shrinking phase happens all at once, here.
        std::lock_guard<std::mutex> lk(tx_mutex_);
        if (transactions_.count(xid)) transactions_.at(xid).in_shrinking = true;
    }
    for (auto& [key, lq] : lock_table_) {
        std::unique_lock<std::mutex> ul(lq.mu);
        lq.requests.remove_if([&](const LockRequest& r){ return r.xid == xid; });
        lq.cv.notify_all();                        // wake any waiters on this key
    }
    std::lock_guard<std::mutex> lk(lm_mutex_);
    waits_for_.erase(xid);
}

// ───────────────────────────── public API ─────────────────────────────

std::optional<std::string> TransactionManager::read(TxID xid, const RowKey& key) {
    acquire_lock(key, xid, LockMode::SHARED);
    return mvcc_read_key(key, xid);
}

void TransactionManager::insert(TxID xid, const RowKey& key, const std::string& value) {
    acquire_lock(key, xid, LockMode::EXCLUSIVE);
    mvcc_insert(key, value, xid);
}

void TransactionManager::update(TxID xid, const RowKey& key, const std::string& value) {
    acquire_lock(key, xid, LockMode::EXCLUSIVE);
    mvcc_update(key, value, xid);
}

void TransactionManager::remove(TxID xid, const RowKey& key) {
    acquire_lock(key, xid, LockMode::EXCLUSIVE);
    mvcc_delete(key, xid);
}

void TransactionManager::commit(TxID xid) {
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        transactions_.at(xid).status = TxStatus::COMMITTED;
    }
    release_locks(xid);
    std::cout << "[TX " << xid << "] COMMITTED\n";
}

void TransactionManager::abort(TxID xid) {
    {   // undo this tx's MVCC writes: hide own inserts, revive own deletes
        std::lock_guard<std::mutex> lk(heap_mutex_);
        for (auto& [key, chain] : heap_)
            for (auto& v : chain) {
                if (v.xmin == xid) v.xmax = xid;   // own insert -> invisible
                if (v.xmax == xid) v.xmax = 0;     // own delete -> undone
            }
    }
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        transactions_.at(xid).status = TxStatus::ABORTED;
    }
    release_locks(xid);
    std::cout << "[TX " << xid << "] ABORTED\n";
}

} // namespace txn
