#include "optimizer/optimizer.h"

#include <vector>

#include "common/config.h"

namespace minidb {

// Gather equality comparisons along the AND-spine of a predicate. ORs and other
// shapes are ignored — only conjunctive equalities can drive an index probe.
static void CollectEqualities(const Expr *e, std::vector<const Expr *> *out) {
  if (e == nullptr) return;
  if (e->kind == ExprKind::kBinary && e->op == BinOp::kAnd) {
    CollectEqualities(e->left.get(), out);
    CollectEqualities(e->right.get(), out);
    return;
  }
  if (e->kind == ExprKind::kBinary && e->op == BinOp::kEq) out->push_back(e);
}

// If `e` is `column = const` (in either order), return its column index + value.
static bool AsColEqConst(const Expr *e, int *col, Value *val) {
  const Expr *l = e->left.get();
  const Expr *r = e->right.get();
  if (l->kind == ExprKind::kColumn && r->kind == ExprKind::kConst) {
    *col = l->index;
    *val = r->value;
    return true;
  }
  if (l->kind == ExprKind::kConst && r->kind == ExprKind::kColumn) {
    *col = r->index;
    *val = l->value;
    return true;
  }
  return false;
}

// Estimated bytes per row from the schema (INT=4, VARCHAR=4-byte len + chars).
static uint32_t RowSize(const Schema &s) {
  uint32_t sz = 0;
  for (size_t i = 0; i < s.GetColumnCount(); i++) {
    const Column &c = s.GetColumn(i);
    sz += (c.type == TypeId::INTEGER) ? 4u : (4u + c.length);
  }
  return sz == 0 ? 1u : sz;
}

AccessPath ChooseAccessPath(const TableMeta &meta, const Expr *where) {
  AccessPath best;  // defaults to kSeqScan
  if (where == nullptr) return best;

  std::vector<const Expr *> eqs;
  CollectEqualities(where, &eqs);
  if (eqs.empty()) return best;

  uint32_t n = meta.num_rows;
  uint32_t row_size = RowSize(meta.schema);
  uint32_t tuples_per_page = (PAGE_SIZE - 8) / row_size;
  if (tuples_per_page == 0) tuples_per_page = 1;
  uint32_t seq_cost = (n + tuples_per_page - 1) / tuples_per_page;
  if (seq_cost == 0) seq_cost = 1;

  uint32_t best_cost = seq_cost;
  for (const Expr *e : eqs) {
    int col;
    Value val;
    if (!AsColEqConst(e, &col, &val)) continue;
    for (const auto &idx : meta.indexes) {
      if (static_cast<int>(idx.key_col) != col) continue;
      uint32_t ndv = idx.distinct_keys == 0 ? 1 : idx.distinct_keys;
      uint32_t est_match = (n + ndv - 1) / ndv;
      if (est_match == 0) est_match = 1;
      uint32_t index_cost = est_match + 2;  // descent + matching heap fetches
      if (index_cost < best_cost) {
        best_cost = index_cost;
        best.kind = AccessPath::kIndexScan;
        best.index_name = idx.name;
        best.key = val;
      }
    }
  }
  return best;
}

// Find an index on `meta` keyed exactly on column `col`; nullptr if none.
static const IndexMeta *IndexOnColumn(const TableMeta &meta, int col) {
  for (const auto &idx : meta.indexes) {
    if (static_cast<int>(idx.key_col) == col) return &idx;
  }
  return nullptr;
}

JoinPlan ChooseJoinPlan(const TableMeta &left, const TableMeta &right, const Expr *join_on) {
  JoinPlan plan;
  if (join_on == nullptr || join_on->kind != ExprKind::kBinary || join_on->op != BinOp::kEq) {
    return plan;
  }
  const Expr *l = join_on->left.get();
  const Expr *r = join_on->right.get();
  if (l->kind != ExprKind::kColumn || r->kind != ExprKind::kColumn) return plan;

  // Split the combined column indices back into per-table columns. Left columns
  // occupy [0, L); right columns occupy [L, L + R).
  int split = static_cast<int>(left.schema.GetColumnCount());
  int a = l->index, b = r->index;
  int left_col, right_col;
  if (a < split && b >= split) {
    left_col = a;
    right_col = b - split;
  } else if (b < split && a >= split) {
    left_col = b;
    right_col = a - split;
  } else {
    return plan;  // both refs on the same side — not a cross-table equijoin
  }

  const IndexMeta *right_idx = IndexOnColumn(right, right_col);
  const IndexMeta *left_idx = IndexOnColumn(left, left_col);

  if (right_idx != nullptr) {  // default: probe the right table
    plan.use_index = true;
    plan.inner_is_left = false;
    plan.inner_index = right_idx->name;
    plan.inner_key_col = right_col;
    plan.outer_key_col = left_col;  // outer = left table
  }
  if (left_idx != nullptr) {
    // Prefer the left table as inner when the right is not indexed, or when the
    // left is the larger relation (avoiding more inner rescans).
    bool prefer_left = (right_idx == nullptr) || (left.num_rows >= right.num_rows);
    if (prefer_left) {
      plan.use_index = true;
      plan.inner_is_left = true;
      plan.inner_index = left_idx->name;
      plan.inner_key_col = left_col;
      plan.outer_key_col = right_col;  // outer = right table
    }
  }
  return plan;
}

}  // namespace minidb
