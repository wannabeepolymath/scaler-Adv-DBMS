#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/value.h"
#include "execution/execution.h"
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

 private:
  ResultSet ExecCreateTable(Statement *stmt);
  ResultSet ExecCreateIndex(Statement *stmt);
  ResultSet ExecInsert(Statement *stmt);
  ResultSet ExecDelete(Statement *stmt);
  ResultSet ExecSelect(Statement *stmt);

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
};

}  // namespace minidb
