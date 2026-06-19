// Lab 8: Driver for the modular transaction manager.
//   Build (CMake):  cmake -S . -B build && cmake --build build && ./build/txn_engine
//   Build (direct): g++ -std=c++17 -pthread -o txn_engine main.cpp transaction_manager.cpp
//
// Four scenarios exercise MVCC snapshot reads, shared-lock concurrency, an
// exclusive lock that blocks a reader, and a two-transaction deadlock that the
// waits-for graph detects and breaks.
#include "transaction_manager.h"

#include <iostream>
#include <thread>
#include <chrono>

using txn::TransactionManager;
using txn::TxID;
using txn::DeadlockException;
using RowKey = std::string;

static void print_val(const std::optional<std::string>& v, TxID xid, const RowKey& key) {
    std::cout << "  [TX " << xid << "] READ " << key << " = "
              << (v ? *v : "<not visible>") << "\n";
}

int main() {
    TransactionManager tm;

    std::cout << "=== Scenario 1: MVCC Snapshot Isolation ===\n";
    {
        TxID t1 = tm.begin();
        tm.insert(t1, "balance", "1000");
        tm.commit(t1);

        TxID t2 = tm.begin();                       // snapshot taken here
        TxID t3 = tm.begin();
        tm.update(t3, "balance", "2000");           // t3 commits a new version...
        tm.commit(t3);

        print_val(tm.read(t2, "balance"), t2, "balance");  // ...t2 still sees 1000
        tm.commit(t2);
    }

    std::cout << "\n=== Scenario 2: Concurrent Shared Locks ===\n";
    {
        TxID t4 = tm.begin();
        TxID t5 = tm.begin();
        print_val(tm.read(t4, "balance"), t4, "balance");  // both readers share the lock
        print_val(tm.read(t5, "balance"), t5, "balance");
        tm.commit(t4);
        tm.commit(t5);
    }

    std::cout << "\n=== Scenario 3: Exclusive Lock + Waiting ===\n";
    {
        TxID t6 = tm.begin();
        tm.update(t6, "balance", "3000");           // holds EXCLUSIVE lock on balance

        std::thread reader([&]() {
            TxID t7 = tm.begin();
            std::cout << "  [TX " << t7 << "] waiting for shared lock on balance...\n";
            print_val(tm.read(t7, "balance"), t7, "balance");  // unblocks after t6 commits
            tm.commit(t7);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        tm.commit(t6);                              // releases lock -> wakes reader
        reader.join();
    }

    std::cout << "\n=== Scenario 4: Deadlock Detection ===\n";
    {
        TxID ta = tm.begin();
        TxID tb = tm.begin();
        tm.insert(ta, "A", "val_a");
        tm.insert(tb, "B", "val_b");
        tm.commit(ta);
        tm.commit(tb);

        TxID t8 = tm.begin();
        TxID t9 = tm.begin();
        tm.update(t8, "A", "a1");                   // t8 holds X(A)
        tm.update(t9, "B", "b1");                   // t9 holds X(B)

        // t8 wants B (held by t9); t9 wants A (held by t8) -> cycle.
        std::thread th1([&]() {
            try { tm.update(t8, "B", "b2"); tm.commit(t8); }
            catch (DeadlockException& e) { std::cout << "  " << e.what() << "\n"; tm.abort(t8); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        try { tm.update(t9, "A", "a2"); tm.commit(t9); }
        catch (DeadlockException& e) { std::cout << "  " << e.what() << "\n"; tm.abort(t9); }
        th1.join();
    }

    std::cout << "\nAll scenarios complete.\n";
    return 0;
}
