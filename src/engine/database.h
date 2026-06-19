#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/value.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution.h"
#include "recovery/log_manager.h"
#include "sql/sql.h"
#include "storage/disk_manager.h"

namespace minidb {

// The result of executing one statement: a row set for SELECT, or a status
// message + affected-row count for DDL/DML.
struct ResultSet {
  bool is_query{false};
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  std::string message;
  int affected{0};
};

// The top-level engine. Owns the storage stack (disk + buffer pool + catalog)
// and turns a SQL string into a ResultSet: parse -> bind -> plan -> execute.
class Database {
 public:
  explicit Database(const std::string &db_file, size_t pool_size = BUFFER_POOL_SIZE);
  ~Database();

  ResultSet Execute(const std::string &sql);

  Catalog *GetCatalog() { return catalog_.get(); }
  BufferPoolManager *GetBufferPool() { return bpm_.get(); }

  // Simulate a crash: subsequent destruction discards unflushed pages and leaves
  // the WAL marked dirty, so the next open triggers recovery. (Testing/demo.)
  void SimulateCrash();

 private:
  ResultSet ExecCreateTable(Statement *stmt);
  ResultSet ExecCreateIndex(Statement *stmt);
  ResultSet ExecInsert(Statement *stmt);
  ResultSet ExecDelete(Statement *stmt);
  ResultSet ExecSelect(Statement *stmt);
  ResultSet ExecBegin();
  ResultSet ExecCommit();
  ResultSet ExecRollback();

  // Undo every write a transaction made, in reverse order (used by ROLLBACK).
  void UndoWrites(Transaction *txn);

  // Replay the WAL after a crash: reset table data, then redo committed records.
  void Recover();

  // Append a row-level record to the WAL under the given log txn id.
  void LogRow(LogType type, txn_id_t log_txn, const std::string &table, const Tuple &tuple);

  // Binding helpers: build the input column list for a SELECT (single table or
  // join), resolve column references in an expression to indices, resolve the
  // projection list.
  struct BoundInput {
    Schema schema;                    // combined column schema
    std::vector<std::string> tables;  // table-qualifier per column (parallel)
  };
  BoundInput BuildInput(Statement *stmt);
  void BindExpr(Expr *e, const BoundInput &in);
  int ResolveColumn(const BoundInput &in, const std::string &table, const std::string &name);

  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<LockManager> lock_mgr_;
  std::unique_ptr<TransactionManager> txn_mgr_;
  std::unique_ptr<LogManager> log_;
  Transaction *txn_{nullptr};  // current explicit transaction, or nullptr (auto-commit)
  bool crashed_{false};        // set by SimulateCrash() — skip clean shutdown
};

}  // namespace minidb
