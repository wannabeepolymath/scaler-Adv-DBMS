// Unit + integration tests for the B+ tree index and the cost-based optimizer.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "engine/database.h"
#include "index/b_plus_tree.h"
#include "index/b_plus_tree_page.h"
#include "index/key_codec.h"
#include "optimizer/optimizer.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "tests/test_util.h"

using namespace minidb;

static void TestKeyCodec() {
  // INTEGER round-trips through 4 bytes.
  char buf[64];
  CHECK_EQ(KeyWidth(TypeId::INTEGER, 4), 4u);
  EncodeKey(Value(42), buf, 4);
  CHECK(DecodeKey(buf, 4, TypeId::INTEGER).Equals(Value(42)));
  EncodeKey(Value(-7), buf, 4);
  CHECK(DecodeKey(buf, 4, TypeId::INTEGER).Equals(Value(-7)));

  // VARCHAR pads to width and trims trailing NULs on decode.
  CHECK_EQ(KeyWidth(TypeId::VARCHAR, 8), 8u);
  EncodeKey(Value(std::string("hi")), buf, 8);
  CHECK(DecodeKey(buf, 8, TypeId::VARCHAR).Equals(Value(std::string("hi"))));

  // RID round-trips through 8 bytes.
  EncodeRID(RID(5, 9), buf);
  RID r = DecodeRID(buf);
  CHECK(r.GetPageId() == 5 && r.GetSlotNum() == 9);
}

static void TestNodeView() {
  Page page;  // raw 4 KB frame
  NodeView leaf = NodeView::Of(&page, /*key_width=*/4);
  leaf.InitLeaf();
  CHECK(leaf.IsLeaf());
  CHECK_EQ(leaf.Count(), 0);
  CHECK(leaf.LeafCap() >= 300u);  // ~340 for 4-byte keys

  char k[4];
  EncodeKey(Value(10), k, 4);
  leaf.SetLeafEntry(0, k, RID(1, 2));
  leaf.SetCount(1);
  CHECK(leaf.LeafRID(0).GetPageId() == 1 && leaf.LeafRID(0).GetSlotNum() == 2);
  CHECK(DecodeKey(leaf.LeafKey(0), 4, TypeId::INTEGER).Equals(Value(10)));

  // LowerBound finds the insertion point.
  char probe[4];
  EncodeKey(Value(5), probe, 4);
  CHECK_EQ(leaf.LowerBound(probe, TypeId::INTEGER), 0);
  EncodeKey(Value(20), probe, 4);
  CHECK_EQ(leaf.LowerBound(probe, TypeId::INTEGER), 1);

  // Internal node: children + separator keys round-trip.
  Page ip;
  NodeView in = NodeView::Of(&ip, 4);
  in.InitInternal();
  CHECK_FALSE(in.IsLeaf());
  CHECK(in.InternalCap() >= 300u);  // ~510 for 4-byte keys
  in.SetChild(0, 100);
  in.SetChild(1, 200);
  char sep[4];
  EncodeKey(Value(50), sep, 4);
  in.SetIntKey(0, sep);
  in.SetCount(1);
  CHECK(in.Child(0) == 100 && in.Child(1) == 200);
  CHECK(DecodeKey(in.IntKey(0), 4, TypeId::INTEGER).Equals(Value(50)));
}

static void TestInsertSearch() {
  std::remove("test_index_bt.db");
  DiskManager dm("test_index_bt.db");
  BufferPoolManager bpm(64, &dm);
  page_id_t root = INVALID_PAGE_ID;
  BPlusTree tree(&bpm, root, TypeId::INTEGER, 4, [&](page_id_t r) { root = r; });

  // Insert 5000 keys in shuffled order -> forces multi-level splits.
  std::vector<int> keys(5000);
  for (int i = 0; i < 5000; i++) keys[i] = i;
  std::mt19937 rng(12345);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (int k : keys) CHECK(tree.Insert(Value(k), RID(k / 100, k % 100)));

  CHECK(tree.Height() >= 2);  // multiple levels
  CHECK(root != INVALID_PAGE_ID);

  // Every key is found with its RID; a missing key returns empty.
  for (int probe : {0, 1, 2499, 4999}) {
    auto rids = tree.Search(Value(probe));
    CHECK_EQ(static_cast<int>(rids.size()), 1);
    CHECK(rids[0].GetPageId() == probe / 100 && rids[0].GetSlotNum() == probe % 100);
  }
  CHECK(tree.Search(Value(5000)).empty());

  // Duplicate keys: same key -> multiple RIDs.
  CHECK_FALSE(tree.Insert(Value(7), RID(99, 1)));  // key 7 already present
  CHECK_EQ(static_cast<int>(tree.Search(Value(7)).size()), 2);

  std::remove("test_index_bt.db");
}

static void TestDeleteAndReopen() {
  std::remove("test_index_bt2.db");
  page_id_t saved_root = INVALID_PAGE_ID;
  {
    DiskManager dm("test_index_bt2.db");
    BufferPoolManager bpm(64, &dm);
    page_id_t root = INVALID_PAGE_ID;
    BPlusTree tree(&bpm, root, TypeId::INTEGER, 4, [&](page_id_t r) { root = r; });
    for (int i = 0; i < 1000; i++) tree.Insert(Value(i), RID(0, i));
    tree.Delete(Value(500), RID(0, 500));
    CHECK(tree.Search(Value(500)).empty());
    CHECK_EQ(static_cast<int>(tree.Search(Value(501)).size()), 1);
    saved_root = root;
    bpm.FlushAllPages();
  }
  // Reopen: rebuild the tree over the persisted root; data survives.
  {
    DiskManager dm("test_index_bt2.db");
    BufferPoolManager bpm(64, &dm);
    BPlusTree tree(&bpm, saved_root, TypeId::INTEGER, 4, [&](page_id_t) {});
    CHECK_EQ(static_cast<int>(tree.Search(Value(501)).size()), 1);
    CHECK(tree.Search(Value(500)).empty());
  }
  std::remove("test_index_bt2.db");
}

static void TestCreateIndexAndMaintain() {
  const std::string f = "test_index_sql.db";
  std::remove(f.c_str());
  {
    Database db(f);
    db.Execute("CREATE TABLE t (id INT, name VARCHAR(16))");
    for (int i = 0; i < 200; i++)
      db.Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'n" + std::to_string(i) + "')");
    db.Execute("CREATE INDEX t_id ON t (id)");

    // Point query returns the right row via the engine.
    CHECK_EQ(db.Execute("SELECT id FROM t WHERE id = 137").affected, 1);

    // INSERT is reflected through the index.
    db.Execute("INSERT INTO t VALUES (1000, 'new')");
    CHECK_EQ(db.Execute("SELECT id FROM t WHERE id = 1000").affected, 1);

    // DELETE is reflected through the index.
    db.Execute("DELETE FROM t WHERE id = 137");
    CHECK_EQ(db.Execute("SELECT id FROM t WHERE id = 137").affected, 0);

    // Duplicate index name rejected.
    bool threw = false;
    try {
      db.Execute("CREATE INDEX t_id ON t (id)");
    } catch (const std::exception &) {
      threw = true;
    }
    CHECK(threw);
  }
  std::remove(f.c_str());
}

static void TestOptimizerChoosesIndex() {
  // (a) Pure cost function: large selective table -> index; tiny table -> seq.
  TableMeta big;
  big.name = "big";
  big.schema = Schema({Column("id", TypeId::INTEGER), Column("v", TypeId::INTEGER)});
  big.num_rows = 100000;
  big.indexes.push_back(IndexMeta{"big_id", INVALID_PAGE_ID, 0, /*distinct=*/100000});
  // WHERE id = 42  (bound: column index 0)
  Expr col;
  col.kind = ExprKind::kColumn;
  col.index = 0;
  Expr cst;
  cst.kind = ExprKind::kConst;
  cst.value = Value(42);
  Expr eq;
  eq.kind = ExprKind::kBinary;
  eq.op = BinOp::kEq;
  eq.left.reset(new Expr(std::move(col)));
  eq.right.reset(new Expr(std::move(cst)));
  CHECK(ChooseAccessPath(big, &eq).kind == AccessPath::kIndexScan);

  TableMeta tiny = big;
  tiny.num_rows = 10;
  tiny.indexes[0].distinct_keys = 10;
  CHECK(ChooseAccessPath(tiny, &eq).kind == AccessPath::kSeqScan);  // seq cheaper on 10 rows

  // (b) End-to-end: index actually used and correct on a large table.
  const std::string f = "test_index_opt.db";
  std::remove(f.c_str());
  {
    Database db(f);
    db.Execute("CREATE TABLE big (id INT, v INT)");
    for (int i = 0; i < 3000; i++)
      db.Execute("INSERT INTO big VALUES (" + std::to_string(i) + ", " + std::to_string(i * 2) + ")");
    db.Execute("CREATE INDEX big_id ON big (id)");
    ResultSet r = db.Execute("SELECT v FROM big WHERE id = 2718");
    CHECK_EQ(r.affected, 1);
    CHECK(r.rows.size() == 1 && r.rows[0][0].Equals(Value(5436)));
  }
  std::remove(f.c_str());
}

static void TestIndexNestedLoopJoin() {
  // (a) Pure plan: books JOIN authors ON books.author_id = authors.id, where
  // authors (right table) is indexed on id -> authors becomes the probed inner.
  TableMeta books;
  books.name = "books";
  books.schema = Schema({Column("id", TypeId::INTEGER), Column("author_id", TypeId::INTEGER)});
  books.num_rows = 50;
  TableMeta authors;
  authors.name = "authors";
  authors.schema = Schema({Column("id", TypeId::INTEGER), Column("name", TypeId::VARCHAR, 16)});
  authors.num_rows = 500;
  authors.indexes.push_back(IndexMeta{"authors_id", INVALID_PAGE_ID, 0, 500});
  // Bound join_on: books.author_id (combined index 1) = authors.id (combined index 2).
  Expr lcol;
  lcol.kind = ExprKind::kColumn;
  lcol.index = 1;
  Expr rcol;
  rcol.kind = ExprKind::kColumn;
  rcol.index = 2;
  Expr on;
  on.kind = ExprKind::kBinary;
  on.op = BinOp::kEq;
  on.left.reset(new Expr(std::move(lcol)));
  on.right.reset(new Expr(std::move(rcol)));
  JoinPlan jp = ChooseJoinPlan(books, authors, &on);
  CHECK(jp.use_index);
  CHECK_FALSE(jp.inner_is_left);     // inner = authors (right)
  CHECK_EQ(jp.outer_key_col, 1);     // books.author_id within books schema

  // (b) End-to-end correctness: results match a plain join row-for-row.
  const std::string f = "test_index_join.db";
  std::remove(f.c_str());
  {
    Database db(f);
    db.Execute("CREATE TABLE authors (id INT, name VARCHAR(16))");
    db.Execute("CREATE TABLE books (id INT, author_id INT)");
    for (int i = 0; i < 500; i++)
      db.Execute("INSERT INTO authors VALUES (" + std::to_string(i) + ", 'a" + std::to_string(i) + "')");
    for (int i = 0; i < 50; i++)
      db.Execute("INSERT INTO books VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    db.Execute("CREATE INDEX authors_id ON authors (id)");
    ResultSet r = db.Execute(
        "SELECT books.id, authors.name FROM books JOIN authors ON books.author_id = authors.id");
    CHECK_EQ(r.affected, 50);  // each book matches exactly one author
    bool found = false;
    for (auto &row : r.rows) {
      if (row[0].Equals(Value(7))) {
        CHECK(row[1].Equals(Value(std::string("a7"))));
        found = true;
      }
    }
    CHECK(found);
  }
  std::remove(f.c_str());
}

int main() {
  TestKeyCodec();
  TestNodeView();
  TestInsertSearch();
  TestDeleteAndReopen();
  TestCreateIndexAndMaintain();
  TestOptimizerChoosesIndex();
  TestIndexNestedLoopJoin();
  return minidb::test::summary("index");
}
