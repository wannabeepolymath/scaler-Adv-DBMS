#include "index/b_plus_tree.h"

#include <cstring>

#include "common/exception.h"
#include "index/key_codec.h"

namespace minidb {

BPlusTree::BPlusTree(BufferPoolManager *bpm, page_id_t root_page_id, TypeId key_type,
                     uint32_t key_width, std::function<void(page_id_t)> on_root_change)
    : bpm_(bpm),
      root_page_id_(root_page_id),
      key_type_(key_type),
      key_width_(key_width),
      on_root_change_(std::move(on_root_change)) {}

NodeView BPlusTree::Fetch(page_id_t pid, Page **out_page) const {
  Page *p = bpm_->FetchPage(pid);
  if (p == nullptr) throw Exception(ErrorKind::kIO, "B+ tree: failed to fetch page");
  *out_page = p;
  return NodeView::Of(p, key_width_);
}

// Number of separators <= key = index of the child to descend into. Keys equal
// to a separator route to the child on its right, matching the copy-up rule
// where a separator is the right leaf's smallest key.
static int ChildIndex(const NodeView &node, const Value &key, TypeId type, uint32_t width) {
  int lo = 0, hi = node.Count();
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    Value k = DecodeKey(node.IntKey(mid), width, type);
    if (k.Compare(key) <= 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

page_id_t BPlusTree::DescendToLeaf(const char *key_enc, std::vector<page_id_t> *path) const {
  Value key = DecodeKey(key_enc, key_width_, key_type_);
  page_id_t pid = root_page_id_;
  while (true) {
    Page *p;
    NodeView nv = Fetch(pid, &p);
    if (nv.IsLeaf()) {
      bpm_->UnpinPage(pid, false);
      return pid;
    }
    int ci = ChildIndex(nv, key, key_type_, key_width_);
    page_id_t child = nv.Child(ci);
    bpm_->UnpinPage(pid, false);
    if (path != nullptr) path->push_back(pid);
    pid = child;
  }
}

std::vector<RID> BPlusTree::Search(const Value &key) const {
  std::vector<RID> result;
  if (IsEmpty()) return result;

  std::vector<char> key_enc(key_width_);
  EncodeKey(key, key_enc.data(), key_width_);

  page_id_t pid = DescendToLeaf(key_enc.data(), nullptr);
  bool first = true;
  while (pid != INVALID_PAGE_ID) {
    Page *p;
    NodeView leaf = Fetch(pid, &p);
    int i = first ? leaf.LowerBound(key_enc.data(), key_type_) : 0;
    first = false;
    bool key_exceeded = false;
    for (; i < leaf.Count(); i++) {
      Value k = DecodeKey(leaf.LeafKey(i), key_width_, key_type_);
      int c = k.Compare(key);
      if (c < 0) continue;
      if (c > 0) {
        key_exceeded = true;
        break;
      }
      result.push_back(leaf.LeafRID(i));
    }
    bool consumed_to_end = (i == leaf.Count());
    page_id_t next = leaf.Next();
    bpm_->UnpinPage(pid, false);
    // Duplicates may spill into the next leaf only if we matched all the way to
    // the end of this one.
    if (key_exceeded || !consumed_to_end) break;
    pid = next;
  }
  return result;
}

// Shift leaf entries right from `pos` and write the new (key, rid) there.
static void InsertLeafAt(NodeView &leaf, int pos, const char *key_enc, RID rid, uint32_t width) {
  int n = leaf.Count();
  for (int i = n; i > pos; i--) {
    leaf.SetLeafEntry(i, leaf.LeafKey(i - 1), leaf.LeafRID(i - 1));
    (void)width;
  }
  leaf.SetLeafEntry(pos, key_enc, rid);
  leaf.SetCount(n + 1);
}

bool BPlusTree::Insert(const Value &key, RID rid) {
  std::vector<char> key_enc(key_width_);
  EncodeKey(key, key_enc.data(), key_width_);

  // Empty tree: allocate the root leaf.
  if (IsEmpty()) {
    page_id_t pid;
    Page *p = bpm_->NewPage(&pid);
    if (p == nullptr) throw Exception(ErrorKind::kIO, "B+ tree: out of pages");
    NodeView leaf = NodeView::Of(p, key_width_);
    leaf.InitLeaf();
    leaf.SetLeafEntry(0, key_enc.data(), rid);
    leaf.SetCount(1);
    bpm_->UnpinPage(pid, true);
    root_page_id_ = pid;
    on_root_change_(pid);
    return true;
  }

  std::vector<page_id_t> path;
  page_id_t leaf_pid = DescendToLeaf(key_enc.data(), &path);

  Page *lp;
  NodeView leaf = Fetch(leaf_pid, &lp);
  int pos = leaf.LowerBound(key_enc.data(), key_type_);
  bool key_new = !(pos < leaf.Count() &&
                   DecodeKey(leaf.LeafKey(pos), key_width_, key_type_).Compare(key) == 0);

  if (leaf.Count() < static_cast<int>(leaf.LeafCap())) {
    InsertLeafAt(leaf, pos, key_enc.data(), rid, key_width_);
    bpm_->UnpinPage(leaf_pid, true);
    return key_new;
  }

  // Leaf is full: gather all entries (plus the new one) and split in half.
  int total = leaf.Count() + 1;
  std::vector<std::string> ek(total);
  std::vector<RID> er(total);
  int w = 0;
  for (int i = 0; i < leaf.Count(); i++) {
    if (w == pos) {
      ek[w] = std::string(key_enc.data(), key_width_);
      er[w] = rid;
      w++;
    }
    ek[w] = std::string(leaf.LeafKey(i), key_width_);
    er[w] = leaf.LeafRID(i);
    w++;
  }
  if (w == pos) {  // new entry belongs at the very end
    ek[w] = std::string(key_enc.data(), key_width_);
    er[w] = rid;
  }

  int split = total / 2;  // left: [0,split)   right: [split,total)
  page_id_t right_pid;
  Page *rp = bpm_->NewPage(&right_pid);
  if (rp == nullptr) throw Exception(ErrorKind::kIO, "B+ tree: out of pages");
  NodeView right = NodeView::Of(rp, key_width_);
  right.InitLeaf();

  leaf.SetCount(0);
  for (int i = 0; i < split; i++) leaf.SetLeafEntry(i, ek[i].data(), er[i]);
  leaf.SetCount(split);
  for (int i = split; i < total; i++) right.SetLeafEntry(i - split, ek[i].data(), er[i]);
  right.SetCount(total - split);

  right.SetNext(leaf.Next());
  leaf.SetNext(right_pid);

  std::string sep = ek[split];  // copy-up: right leaf's smallest key
  bpm_->UnpinPage(right_pid, true);
  bpm_->UnpinPage(leaf_pid, true);

  InsertIntoParent(&path, leaf_pid, sep, right_pid);
  return key_new;
}

void BPlusTree::InsertIntoParent(std::vector<page_id_t> *path, page_id_t left_child,
                                 const std::string &sep, page_id_t right_child) {
  // The split node was the root: grow a new internal root.
  if (path->empty()) {
    page_id_t new_root;
    Page *p = bpm_->NewPage(&new_root);
    if (p == nullptr) throw Exception(ErrorKind::kIO, "B+ tree: out of pages");
    NodeView nr = NodeView::Of(p, key_width_);
    nr.InitInternal();
    nr.SetChild(0, left_child);
    nr.SetChild(1, right_child);
    nr.SetIntKey(0, sep.data());
    nr.SetCount(1);
    bpm_->UnpinPage(new_root, true);
    root_page_id_ = new_root;
    on_root_change_(new_root);
    return;
  }

  page_id_t parent_pid = path->back();
  path->pop_back();
  Page *pp;
  NodeView parent = Fetch(parent_pid, &pp);

  // Locate the slot holding left_child; the separator goes at key index ci,
  // and right_child becomes child ci+1.
  int ci = 0;
  while (ci <= parent.Count() && parent.Child(ci) != left_child) ci++;

  if (parent.Count() < static_cast<int>(parent.InternalCap())) {
    int n = parent.Count();
    for (int i = n; i > ci; i--) parent.SetIntKey(i, parent.IntKey(i - 1));
    for (int i = n + 1; i > ci + 1; i--) parent.SetChild(i, parent.Child(i - 1));
    parent.SetIntKey(ci, sep.data());
    parent.SetChild(ci + 1, right_child);
    parent.SetCount(n + 1);
    bpm_->UnpinPage(parent_pid, true);
    return;
  }

  // Parent is full: build the over-full key/child arrays, then split. For an
  // internal node the middle key moves UP (it is not duplicated downward).
  int nkeys = parent.Count();
  std::vector<std::string> keys(nkeys + 1);
  std::vector<page_id_t> kids(nkeys + 2);
  for (int i = 0; i < nkeys; i++) keys[i < ci ? i : i + 1] = std::string(parent.IntKey(i), key_width_);
  keys[ci] = sep;
  for (int i = 0; i <= nkeys; i++) kids[i <= ci ? i : i + 1] = parent.Child(i);
  kids[ci + 1] = right_child;

  int m = nkeys + 1;       // total keys after insertion
  int mid = m / 2;         // key that rises to the grandparent
  std::string up = keys[mid];

  // Left keeps keys[0,mid) and kids[0,mid+1); right gets keys[mid+1,m) and the rest.
  page_id_t right_int_pid;
  Page *rip = bpm_->NewPage(&right_int_pid);
  if (rip == nullptr) throw Exception(ErrorKind::kIO, "B+ tree: out of pages");
  NodeView right_int = NodeView::Of(rip, key_width_);
  right_int.InitInternal();

  parent.SetCount(0);
  for (int i = 0; i < mid; i++) parent.SetIntKey(i, keys[i].data());
  for (int i = 0; i <= mid; i++) parent.SetChild(i, kids[i]);
  parent.SetCount(mid);

  int rcount = m - mid - 1;
  for (int i = 0; i < rcount; i++) right_int.SetIntKey(i, keys[mid + 1 + i].data());
  for (int i = 0; i <= rcount; i++) right_int.SetChild(i, kids[mid + 1 + i]);
  right_int.SetCount(rcount);

  bpm_->UnpinPage(right_int_pid, true);
  bpm_->UnpinPage(parent_pid, true);

  InsertIntoParent(path, parent_pid, up, right_int_pid);
}

void BPlusTree::Delete(const Value &key, RID rid) {
  if (IsEmpty()) return;
  std::vector<char> key_enc(key_width_);
  EncodeKey(key, key_enc.data(), key_width_);

  page_id_t pid = DescendToLeaf(key_enc.data(), nullptr);
  bool first = true;
  while (pid != INVALID_PAGE_ID) {
    Page *p;
    NodeView leaf = Fetch(pid, &p);
    int i = first ? leaf.LowerBound(key_enc.data(), key_type_) : 0;
    first = false;
    int found = -1;
    bool key_exceeded = false;
    for (; i < leaf.Count(); i++) {
      Value k = DecodeKey(leaf.LeafKey(i), key_width_, key_type_);
      int c = k.Compare(key);
      if (c < 0) continue;
      if (c > 0) {
        key_exceeded = true;
        break;
      }
      if (leaf.LeafRID(i) == rid) {
        found = i;
        break;
      }
    }
    if (found >= 0) {
      int n = leaf.Count();
      for (int j = found; j < n - 1; j++) leaf.SetLeafEntry(j, leaf.LeafKey(j + 1), leaf.LeafRID(j + 1));
      leaf.SetCount(n - 1);
      bpm_->UnpinPage(pid, true);
      return;
    }
    bool consumed_to_end = (i == leaf.Count());
    page_id_t next = leaf.Next();
    bpm_->UnpinPage(pid, false);
    if (key_exceeded || !consumed_to_end) return;
    pid = next;
  }
}

int BPlusTree::Height() const {
  if (IsEmpty()) return 0;
  int levels = 0;
  page_id_t pid = root_page_id_;
  while (true) {
    Page *p;
    NodeView nv = Fetch(pid, &p);
    levels++;
    if (nv.IsLeaf()) {
      bpm_->UnpinPage(pid, false);
      break;
    }
    page_id_t child = nv.Child(0);
    bpm_->UnpinPage(pid, false);
    pid = child;
  }
  return levels;
}

}  // namespace minidb
