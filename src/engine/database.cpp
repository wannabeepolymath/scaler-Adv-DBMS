#include "engine/database.h"

#include "common/exception.h"
#include "optimizer/optimizer.h"

namespace minidb {

Database::Database(const std::string &db_file, size_t pool_size) {
  disk_ = std::make_unique<DiskManager>(db_file);
  bpm_ = std::make_unique<BufferPoolManager>(pool_size, disk_.get());
  catalog_ = std::make_unique<Catalog>(bpm_.get());  // claims page 0
}

Database::~Database() {
  if (bpm_) bpm_->FlushAllPages();
}

ResultSet Database::Execute(const std::string &sql) {
  StmtPtr stmt = Parser::Parse(sql);
  switch (stmt->type) {
    case StmtType::kCreateTable: return ExecCreateTable(stmt.get());
    case StmtType::kInsert: return ExecInsert(stmt.get());
    case StmtType::kDelete: return ExecDelete(stmt.get());
    case StmtType::kSelect: return ExecSelect(stmt.get());
    case StmtType::kCreateIndex: return ExecCreateIndex(stmt.get());
  }
  throw Exception(ErrorKind::kExecution, "unhandled statement");
}

ResultSet Database::ExecCreateTable(Statement *stmt) {
  std::vector<Column> cols;
  for (auto &c : stmt->columns) cols.emplace_back(c.name, c.type, c.length);
  TableMeta *meta = catalog_->CreateTable(stmt->table, Schema(std::move(cols)));
  if (meta == nullptr) throw Exception(ErrorKind::kBinder, "table already exists: " + stmt->table);
  return {false, {}, {}, "CREATE TABLE " + stmt->table, 0};
}

ResultSet Database::ExecCreateIndex(Statement *stmt) {
  TableMeta *meta = catalog_->GetTable(stmt->table);
  if (meta == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + stmt->table);
  for (const auto &idx : meta->indexes) {
    if (idx.name == stmt->index_name) {
      throw Exception(ErrorKind::kBinder, "index already exists: " + stmt->index_name);
    }
  }
  int key_col = meta->schema.GetColIdx(stmt->index_column);
  if (key_col < 0) throw Exception(ErrorKind::kBinder, "no such column: " + stmt->index_column);

  const Column &col = meta->schema.GetColumn(key_col);
  uint32_t kw = KeyWidth(col.type, col.length);
  // Require enough fanout for a functional tree (>=3 entries/leaf, >=2 keys/internal).
  if ((PAGE_SIZE - 8) / (kw + 8) < 3 || (PAGE_SIZE - 12) / (4 + kw) < 2) {
    throw Exception(ErrorKind::kBinder, "index key too large for a page: " + stmt->index_column);
  }

  IndexMeta im;
  im.name = stmt->index_name;
  im.root_page_id = INVALID_PAGE_ID;
  im.key_col = static_cast<uint32_t>(key_col);
  im.distinct_keys = 0;
  catalog_->UpsertIndex(stmt->table, im);

  // Bulk-build: scan the heap once, inserting each live tuple's key.
  BPlusTree *tree = catalog_->GetIndex(stmt->table, stmt->index_name);
  TableHeap *heap = catalog_->GetTableHeap(stmt->table);
  const Schema &schema = meta->schema;
  uint32_t ndv = 0;
  for (auto it = heap->Begin(); it != heap->End(); ++it) {
    Tuple t = *it;
    Value key = t.GetValue(schema, static_cast<size_t>(key_col));
    if (tree->Insert(key, it.GetRID())) ndv++;
  }
  for (auto &idx : meta->indexes) {
    if (idx.name == stmt->index_name) {
      idx.distinct_keys = ndv;
      break;
    }
  }
  catalog_->Persist();
  return {false, {}, {}, "CREATE INDEX " + stmt->index_name, 0};
}

ResultSet Database::ExecInsert(Statement *stmt) {
  TableMeta *meta = catalog_->GetTable(stmt->table);
  if (meta == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + stmt->table);
  TableHeap *heap = catalog_->GetTableHeap(stmt->table);
  const Schema &schema = meta->schema;

  int count = 0;
  for (auto &row : stmt->rows) {
    if (row.size() != schema.GetColumnCount()) {
      throw Exception(ErrorKind::kBinder, "INSERT column count mismatch for " + stmt->table);
    }
    Tuple t(row, schema);
    RID rid;
    if (!heap->InsertTuple(t, &rid)) throw Exception(ErrorKind::kExecution, "insert failed (row too large?)");
    // Maintain every index on the table.
    for (auto &idx : meta->indexes) {
      BPlusTree *tree = catalog_->GetIndex(stmt->table, idx.name);
      if (tree->Insert(row[idx.key_col], rid)) idx.distinct_keys++;
    }
    count++;
  }
  meta->num_rows += static_cast<uint32_t>(count);
  catalog_->Persist();
  return {false, {}, {}, "INSERT " + std::to_string(count), count};
}

ResultSet Database::ExecDelete(Statement *stmt) {
  TableMeta *meta = catalog_->GetTable(stmt->table);
  if (meta == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + stmt->table);
  TableHeap *heap = catalog_->GetTableHeap(stmt->table);
  const Schema &schema = meta->schema;

  BoundInput in;
  in.schema = schema;
  in.tables.assign(schema.GetColumnCount(), stmt->table);
  if (stmt->where) BindExpr(stmt->where.get(), in);

  // Collect matching rows first (RID + decoded values), then delete (don't
  // mutate the heap while iterating). The values let us remove index entries.
  std::vector<std::pair<RID, std::vector<Value>>> victims;
  for (auto it = heap->Begin(); it != heap->End(); ++it) {
    Tuple t = *it;
    std::vector<Value> vals = t.GetValues(schema);
    if (!stmt->where || EvalPredicate(stmt->where.get(), vals)) {
      victims.emplace_back(it.GetRID(), std::move(vals));
    }
  }
  for (auto &v : victims) {
    heap->MarkDelete(v.first);
    for (const auto &idx : meta->indexes) {
      BPlusTree *tree = catalog_->GetIndex(stmt->table, idx.name);
      tree->Delete(v.second[idx.key_col], v.first);
    }
  }
  if (!victims.empty()) {
    auto n = static_cast<uint32_t>(victims.size());
    meta->num_rows = meta->num_rows >= n ? meta->num_rows - n : 0;
    catalog_->Persist();
  }
  return {false, {}, {}, "DELETE " + std::to_string(victims.size()), static_cast<int>(victims.size())};
}

// ---- SELECT: bind, plan a Volcano pipeline, run it -------------------------
Database::BoundInput Database::BuildInput(Statement *stmt) {
  TableMeta *left = catalog_->GetTable(stmt->from_table);
  if (left == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + stmt->from_table);

  BoundInput in;
  std::vector<Column> cols = left->schema.GetColumns();
  in.tables.assign(cols.size(), stmt->from_table);

  if (stmt->has_join) {
    TableMeta *right = catalog_->GetTable(stmt->join_table);
    if (right == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + stmt->join_table);
    for (auto &c : right->schema.GetColumns()) {
      cols.push_back(c);
      in.tables.push_back(stmt->join_table);
    }
  }
  in.schema = Schema(std::move(cols));
  return in;
}

int Database::ResolveColumn(const BoundInput &in, const std::string &table, const std::string &name) {
  int found = -1;
  for (size_t i = 0; i < in.schema.GetColumnCount(); i++) {
    if (in.schema.GetColumn(i).name != name) continue;
    if (!table.empty() && in.tables[i] != table) continue;
    if (found != -1) throw Exception(ErrorKind::kBinder, "ambiguous column: " + name);
    found = static_cast<int>(i);
  }
  if (found == -1) throw Exception(ErrorKind::kBinder, "unknown column: " + (table.empty() ? name : table + "." + name));
  return found;
}

void Database::BindExpr(Expr *e, const BoundInput &in) {
  if (e == nullptr) return;
  if (e->kind == ExprKind::kColumn) {
    e->index = ResolveColumn(in, e->col_table, e->col_name);
  } else if (e->kind == ExprKind::kBinary) {
    BindExpr(e->left.get(), in);
    BindExpr(e->right.get(), in);
  }
}

ResultSet Database::ExecSelect(Statement *stmt) {
  BoundInput in = BuildInput(stmt);

  // Build the scan / join source.
  std::unique_ptr<Executor> exec;
  TableHeap *left_heap = catalog_->GetTableHeap(stmt->from_table);
  const Schema &left_schema = catalog_->GetTable(stmt->from_table)->schema;

  if (stmt->has_join) {
    TableHeap *right_heap = catalog_->GetTableHeap(stmt->join_table);
    const Schema &right_schema = catalog_->GetTable(stmt->join_table)->schema;
    if (stmt->join_on) BindExpr(stmt->join_on.get(), in);

    TableMeta *lm = catalog_->GetTable(stmt->from_table);
    TableMeta *rm = catalog_->GetTable(stmt->join_table);
    JoinPlan jp = ChooseJoinPlan(*lm, *rm, stmt->join_on.get());

    if (jp.use_index && jp.inner_is_left) {
      // Right table drives; probe the left table's index. Output stays left++right.
      auto outer = std::make_unique<SeqScanExecutor>(right_heap, &right_schema);
      BPlusTree *tree = catalog_->GetIndex(stmt->from_table, jp.inner_index);
      exec = std::make_unique<IndexNestedLoopJoinExecutor>(
          std::move(outer), jp.outer_key_col, tree, left_heap, &left_schema, /*inner_is_left=*/true);
    } else if (jp.use_index) {
      // Left table drives; probe the right table's index.
      auto outer = std::make_unique<SeqScanExecutor>(left_heap, &left_schema);
      BPlusTree *tree = catalog_->GetIndex(stmt->join_table, jp.inner_index);
      exec = std::make_unique<IndexNestedLoopJoinExecutor>(
          std::move(outer), jp.outer_key_col, tree, right_heap, &right_schema, /*inner_is_left=*/false);
    } else {
      auto left_scan = std::make_unique<SeqScanExecutor>(left_heap, &left_schema);
      auto right_scan = std::make_unique<SeqScanExecutor>(right_heap, &right_schema);
      exec = std::make_unique<NestedLoopJoinExecutor>(std::move(left_scan), std::move(right_scan),
                                                      stmt->join_on.get());
    }
  } else {
    // Single table: bind WHERE first so the optimizer can read column indices,
    // then let the cost model choose an index scan vs a sequential scan.
    if (stmt->where) BindExpr(stmt->where.get(), in);
    TableMeta *meta = catalog_->GetTable(stmt->from_table);
    AccessPath path = ChooseAccessPath(*meta, stmt->where.get());
    if (path.kind == AccessPath::kIndexScan) {
      BPlusTree *tree = catalog_->GetIndex(stmt->from_table, path.index_name);
      exec = std::make_unique<IndexScanExecutor>(tree, left_heap, &left_schema, path.key);
    } else {
      exec = std::make_unique<SeqScanExecutor>(left_heap, &left_schema);
    }
  }

  // WHERE filter (idempotent re-bind covers the join path; harmless for the
  // single-table path which bound above).
  if (stmt->where) {
    BindExpr(stmt->where.get(), in);
    exec = std::make_unique<FilterExecutor>(std::move(exec), stmt->where.get());
  }

  // Projection + output column names.
  ResultSet rs;
  rs.is_query = true;
  std::vector<int> proj;
  if (stmt->select_star) {
    for (size_t i = 0; i < in.schema.GetColumnCount(); i++) {
      proj.push_back(static_cast<int>(i));
      rs.columns.push_back(in.tables[i] + "." + in.schema.GetColumn(i).name);
    }
  } else {
    for (const std::string &item : stmt->select_list) {
      std::string table, name;
      auto dot = item.find('.');
      if (dot != std::string::npos) {
        table = item.substr(0, dot);
        name = item.substr(dot + 1);
      } else {
        name = item;
      }
      int idx = ResolveColumn(in, table, name);
      proj.push_back(idx);
      rs.columns.push_back(item);
    }
  }
  exec = std::make_unique<ProjectionExecutor>(std::move(exec), proj);

  exec->Init();
  std::vector<Value> row;
  while (exec->Next(&row)) rs.rows.push_back(row);
  rs.affected = static_cast<int>(rs.rows.size());
  return rs;
}

}  // namespace minidb
