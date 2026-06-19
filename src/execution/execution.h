#pragma once

#include <memory>
#include <vector>

#include "catalog/schema.h"
#include "common/value.h"
#include "index/b_plus_tree.h"
#include "sql/sql.h"
#include "storage/table_heap.h"

namespace minidb {

// Evaluate an expression tree against a materialized row (column refs already
// bound to indices). Comparisons/AND/OR return bool; column/const return Value.
Value EvalScalar(const Expr *e, const std::vector<Value> &row);
bool EvalPredicate(const Expr *e, const std::vector<Value> &row);

// ---- Volcano-style iterator interface --------------------------------------
// Each executor pulls rows from its child(ren) one at a time via Next().
class Executor {
 public:
  virtual ~Executor() = default;
  virtual void Init() = 0;
  virtual bool Next(std::vector<Value> *row) = 0;
};

// Full scan of a heap file, decoding each live tuple into values.
class SeqScanExecutor : public Executor {
 public:
  SeqScanExecutor(TableHeap *heap, const Schema *schema)
      : heap_(heap), schema_(schema), it_(heap->Begin()) {}
  void Init() override { it_ = heap_->Begin(); }
  bool Next(std::vector<Value> *row) override;

 private:
  TableHeap *heap_;
  const Schema *schema_;
  TableHeap::Iterator it_;
};

// Probe a B+ tree for a single key, then fetch each matching tuple from the heap
// by RID. Tombstoned (deleted) RIDs are skipped. A FilterExecutor for the full
// WHERE still sits above this, so the index only needs to narrow the candidates.
class IndexScanExecutor : public Executor {
 public:
  IndexScanExecutor(BPlusTree *tree, TableHeap *heap, const Schema *schema, Value key)
      : tree_(tree), heap_(heap), schema_(schema), key_(std::move(key)) {}
  void Init() override {
    rids_ = tree_->Search(key_);
    cursor_ = 0;
  }
  bool Next(std::vector<Value> *row) override {
    while (cursor_ < rids_.size()) {
      RID rid = rids_[cursor_++];
      Tuple t;
      if (heap_->GetTuple(rid, &t)) {
        *row = t.GetValues(*schema_);
        return true;
      }
    }
    return false;
  }

 private:
  BPlusTree *tree_;
  TableHeap *heap_;
  const Schema *schema_;
  Value key_;
  std::vector<RID> rids_;
  size_t cursor_{0};
};

// Pass through only the rows satisfying `pred`.
class FilterExecutor : public Executor {
 public:
  FilterExecutor(std::unique_ptr<Executor> child, const Expr *pred)
      : child_(std::move(child)), pred_(pred) {}
  void Init() override { child_->Init(); }
  bool Next(std::vector<Value> *row) override;

 private:
  std::unique_ptr<Executor> child_;
  const Expr *pred_;
};

// Project a subset/reorder of the child's columns.
class ProjectionExecutor : public Executor {
 public:
  ProjectionExecutor(std::unique_ptr<Executor> child, std::vector<int> cols)
      : child_(std::move(child)), cols_(std::move(cols)) {}
  void Init() override { child_->Init(); }
  bool Next(std::vector<Value> *row) override;

 private:
  std::unique_ptr<Executor> child_;
  std::vector<int> cols_;
};

// Inner nested-loop join; emits left ++ right rows where `on` holds.
class NestedLoopJoinExecutor : public Executor {
 public:
  NestedLoopJoinExecutor(std::unique_ptr<Executor> left, std::unique_ptr<Executor> right,
                         const Expr *on)
      : left_(std::move(left)), right_(std::move(right)), on_(on) {}
  void Init() override;
  bool Next(std::vector<Value> *row) override;

 private:
  std::unique_ptr<Executor> left_;
  std::unique_ptr<Executor> right_;
  const Expr *on_;
  std::vector<Value> left_row_;
  bool have_left_{false};
};

// Index nested-loop join: drive the outer scan, and for each outer row probe the
// inner table's B+ tree on the join key instead of rescanning it. Output rows are
// always assembled in left-table ++ right-table order, regardless of which side
// is the probed inner, so projection/WHERE bindings stay valid.
class IndexNestedLoopJoinExecutor : public Executor {
 public:
  IndexNestedLoopJoinExecutor(std::unique_ptr<Executor> outer, int outer_key_col,
                              BPlusTree *inner_tree, TableHeap *inner_heap,
                              const Schema *inner_schema, bool inner_is_left)
      : outer_(std::move(outer)),
        outer_key_col_(outer_key_col),
        inner_tree_(inner_tree),
        inner_heap_(inner_heap),
        inner_schema_(inner_schema),
        inner_is_left_(inner_is_left) {}

  void Init() override {
    outer_->Init();
    have_outer_ = outer_->Next(&outer_row_);
    LoadInner();
  }

  bool Next(std::vector<Value> *row) override {
    while (have_outer_) {
      if (inner_cursor_ < inner_rows_.size()) {
        const std::vector<Value> &inner_row = inner_rows_[inner_cursor_++];
        row->clear();
        if (inner_is_left_) {
          *row = inner_row;
          row->insert(row->end(), outer_row_.begin(), outer_row_.end());
        } else {
          *row = outer_row_;
          row->insert(row->end(), inner_row.begin(), inner_row.end());
        }
        return true;
      }
      have_outer_ = outer_->Next(&outer_row_);
      LoadInner();
    }
    return false;
  }

 private:
  void LoadInner() {
    inner_rows_.clear();
    inner_cursor_ = 0;
    if (!have_outer_) return;
    for (RID rid : inner_tree_->Search(outer_row_[outer_key_col_])) {
      Tuple t;
      if (inner_heap_->GetTuple(rid, &t)) inner_rows_.push_back(t.GetValues(*inner_schema_));
    }
  }

  std::unique_ptr<Executor> outer_;
  int outer_key_col_;
  BPlusTree *inner_tree_;
  TableHeap *inner_heap_;
  const Schema *inner_schema_;
  bool inner_is_left_;
  std::vector<Value> outer_row_;
  bool have_outer_{false};
  std::vector<std::vector<Value>> inner_rows_;
  size_t inner_cursor_{0};
};

}  // namespace minidb
