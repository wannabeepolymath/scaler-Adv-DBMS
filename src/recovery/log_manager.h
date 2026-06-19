#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"

namespace minidb {

// One write-ahead log record. INSERT/DELETE carry the table name and the
// schema-encoded row bytes; the control records carry only a txn id.
enum class LogType : uint8_t { kBegin = 1, kInsert = 2, kDelete = 3, kCommit = 4, kAbort = 5 };

struct LogRecord {
  LogType type;
  txn_id_t txn;
  std::string table;        // INSERT / DELETE only
  std::string tuple_bytes;  // INSERT / DELETE only (Tuple::Data())
};

// Append-only write-ahead log backed by a file (`<db>.wal`). The first byte of
// the file is a clean/dirty marker: a clean shutdown writes 1, every open writes
// 0. If a process opens the log and finds the marker still 0, the previous run
// crashed and the engine must recover from the log.
//
// MiniDB follows force-log-at-commit (the log is fsync'd on COMMIT) with a
// no-force / no-steal buffer policy in spirit, so recovery is redo-only: replay
// the committed records to reconstruct the data. The log is never truncated
// (no checkpointing) — a documented simplification.
class LogManager {
 public:
  explicit LogManager(const std::string &wal_file);
  ~LogManager();

  // True if the previous session did not shut down cleanly (crash detected).
  bool CrashDetected() const { return prev_dirty_; }

  // Distinct negative ids for auto-commit statement groups, so they never
  // collide with the positive ids handed out by the TransactionManager.
  txn_id_t NextAutoTxn() { return auto_next_.fetch_sub(1); }

  void Append(const LogRecord &rec);
  void Flush();        // force buffered records to the OS (called on COMMIT)
  void MarkClean();    // write the clean marker + flush (clean shutdown)
  void Reset();        // discard all records (used when opening a brand-new database)

  std::vector<LogRecord> ReadAll();  // every record, in append order

 private:
  void WriteHeader(uint8_t clean);

  std::string file_;
  std::fstream io_;
  std::mutex latch_;
  bool prev_dirty_{false};
  std::atomic<txn_id_t> auto_next_{-1};
};

}  // namespace minidb
