// Lab 8: Transaction manager combining MVCC + Strict 2PL + deadlock detection
//   (declarations). Unlike the Lab 6 prototype, all state is encapsulated inside
//   the TransactionManager object — no global tables — so multiple independent
//   managers can coexist and the lifetime is well defined.
#pragma once

#include <cstdint>
#include <string>
#include <list>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace txn {

using TxID   = std::uint64_t;
using RowKey = std::string;

enum class TxStatus { ACTIVE, COMMITTED, ABORTED };
enum class LockMode { SHARED, EXCLUSIVE };

// Thrown by a write/read that would close a cycle in the waits-for graph.
class DeadlockException : public std::runtime_error {
public:
    explicit DeadlockException(TxID victim)
        : std::runtime_error("Deadlock detected, aborting tx " + std::to_string(victim)),
          victim_(victim) {}
    TxID victim() const { return victim_; }
private:
    TxID victim_;
};

class TransactionManager {
public:
    TransactionManager() = default;
    TransactionManager(const TransactionManager&)            = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // Transaction lifecycle.
    TxID begin();
    void commit(TxID xid);
    void abort(TxID xid);

    // Data operations (each takes the appropriate 2PL lock first, then MVCC).
    // read takes a SHARED lock; insert/update/remove take an EXCLUSIVE lock.
    // Any of them may throw DeadlockException, in which case the caller aborts.
    std::optional<std::string> read(TxID xid, const RowKey& key);
    void insert(TxID xid, const RowKey& key, const std::string& value);
    void update(TxID xid, const RowKey& key, const std::string& value);
    void remove(TxID xid, const RowKey& key);

private:
    // ── transaction table ──
    struct Transaction {
        TxID     id           = 0;
        TxID     snapshot_xid = 0;          // sees commits with xid < snapshot_xid
        TxStatus status       = TxStatus::ACTIVE;
        bool     in_shrinking = false;      // 2PL phase flag
    };

    // ── MVCC version chain (newest first per key) ──
    struct RowVersion {
        std::string value;
        TxID        xmin;                   // created by
        TxID        xmax;                   // deleted/updated by (0 == still live)
    };

    // ── lock manager ──
    struct LockRequest { TxID xid; LockMode mode; bool granted = false; };
    struct LockQueue {
        std::list<LockRequest>  requests;
        std::mutex              mu;
        std::condition_variable cv;
    };

    // transaction-table helpers
    bool is_committed(TxID xid);
    TxID snapshot_of(TxID xid);

    // MVCC helpers
    bool is_visible(const RowVersion& v, TxID snapshot_xid, TxID reader_xid);
    std::optional<std::string> mvcc_read_key(const RowKey& key, TxID xid);
    void mvcc_insert(const RowKey& key, const std::string& value, TxID xid);
    void mvcc_update(const RowKey& key, const std::string& value, TxID xid);
    void mvcc_delete(const RowKey& key, TxID xid);

    // lock-manager helpers
    void acquire_lock(const RowKey& key, TxID xid, LockMode mode);
    void release_locks(TxID xid);
    bool has_cycle(TxID start);             // caller holds lm_mutex_

    std::atomic<TxID>                                  next_xid_{1};

    std::mutex                                         tx_mutex_;
    std::unordered_map<TxID, Transaction>              transactions_;

    std::mutex                                         heap_mutex_;
    std::unordered_map<RowKey, std::list<RowVersion>>  heap_;

    std::mutex                                         lm_mutex_;          // guards waits_for_
    std::unordered_map<RowKey, LockQueue>              lock_table_;
    std::unordered_map<TxID, std::unordered_set<TxID>> waits_for_;
};

} // namespace txn
