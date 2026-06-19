# scaler-Adv-DBMS

Advanced DBMS lab sessions. Specs live in [`lab_sessions/`](lab_sessions/).

| Lab | Topic | Location | Build & run |
|-----|-------|----------|-------------|
| 1 | File I/O kernel journey (strace/dtruss) | [`file_io/`](file_io/) — `reader.cpp`, `LAB1.md` | `g++ -std=c++17 -o reader reader.cpp && ./reader` |
| 2 | SQLite3 internals + PostgreSQL vs SQLite | [`sqlite_internals/`](sqlite_internals/) — `pragmas.sql`, `LAB2.md`, `PostgreSQL_vs_SQLite.md` | `sqlite3 students.db ".read sqlite_internals/pragmas.sql"` |
| 3 | ClockSweep buffer-pool replacement | [`storage_buffer/`](storage_buffer/) — `main.cpp` | `g++ -std=c++17 -o clocksweep main.cpp && ./clocksweep` |
| 4 | Red-Black Tree + full B-Tree | [`index/`](index/) — `rbt.cpp`, `btree.cpp` | `g++ -std=c++17 -o rbt rbt.cpp && ./rbt` / `... btree.cpp` |
| 5 | Shunting-Yard + minimal SQL SELECT | [`query_parser/`](query_parser/) — `sql_engine.cpp` | `g++ -std=c++17 -o sql_engine sql_engine.cpp && ./sql_engine` |
| 6 | MVCC + Strict 2PL + deadlock detection | [`txn_manager/`](txn_manager/) — `txn_manager.cpp` | `g++ -std=c++17 -pthread -o txmgr txn_manager.cpp && ./txmgr` |
| 7 | Modular SQL query engine (shunting-yard + SELECT) | [`query_engine/`](query_engine/) — `shunting_yard.{h,cpp}`, `sql_parser.{h,cpp}`, `main.cpp` | `cmake -S query_engine -B query_engine/build && cmake --build query_engine/build && ./query_engine/build/query_engine` |
| 8 | Modular transaction manager (MVCC + Strict 2PL + deadlock) | [`txn_engine/`](txn_engine/) — `transaction_manager.{h,cpp}`, `main.cpp` | `g++ -std=c++17 -pthread -o txn_engine txn_engine/main.cpp txn_engine/transaction_manager.cpp && ./txn_engine` |

All programs build with C++17 (tested on Apple clang 17) and run standalone.
