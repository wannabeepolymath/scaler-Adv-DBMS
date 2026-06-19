#pragma once

#include <string>

#include "catalog/catalog.h"
#include "common/value.h"
#include "sql/sql.h"

namespace minidb {

// How the optimizer decided to scan a single table.
struct AccessPath {
  enum Kind { kSeqScan, kIndexScan };
  Kind kind{kSeqScan};
  std::string index_name;  // valid iff kIndexScan
  Value key;               // probe key iff kIndexScan
};

// Choose an access path for a single-table scan from the table's statistics
// (row count N, indexes with their distinct-key estimate NDV) and the BOUND
// where-clause (column indices already resolved to table column indices, or
// nullptr). Returns kIndexScan only when an equality `col = const` lands on an
// indexed column AND the estimated index cost beats a sequential scan:
//
//   est_match     = ceil(N / NDV)
//   index_cost    = est_match + 2      (root->leaf descent + matching fetches)
//   seq_cost      = ceil(N / tuples_per_page)
//   index chosen <=> index_cost < seq_cost
//
// A pure function: no storage access, so it is unit-testable in isolation.
AccessPath ChooseAccessPath(const TableMeta &meta, const Expr *where);

// How the optimizer decided to run a two-table join.
struct JoinPlan {
  bool use_index{false};      // false -> plain nested-loop (left drives right)
  bool inner_is_left{false};  // which physical table is the probed inner side
  std::string inner_index;    // index used on the inner table
  int outer_key_col{0};       // join column within the OUTER table's schema
  int inner_key_col{0};       // join column within the inner table's schema
};

// Choose a join strategy for `left JOIN right ON join_on`. `join_on` must be the
// BOUND predicate (column indices resolved against the combined [left ++ right]
// schema). Returns use_index=true when the ON clause is a single cross-table
// equality and one side has an index on its join column — that side becomes the
// probed inner, the other drives as the outer. When both sides are indexed the
// larger table is made the inner (more rescans avoided). Otherwise use_index is
// false and the caller falls back to a plain nested-loop join.
JoinPlan ChooseJoinPlan(const TableMeta &left, const TableMeta &right, const Expr *join_on);

}  // namespace minidb
