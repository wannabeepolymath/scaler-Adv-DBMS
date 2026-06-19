#pragma once

#include <functional>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "common/rid.h"
#include "common/value.h"
#include "index/b_plus_tree_page.h"

namespace minidb {

// A page-backed, non-unique B+ tree mapping a fixed-width key to RIDs. Each node
// is one buffer-pool page (see NodeView for the layout). Duplicate keys are
// allowed; leaf entries are ordered by key. Deletion is lazy (no merge): an
// entry is removed in place and under-full nodes are left as-is.
//
// The tree does not own its root id persistently — whenever the root changes
// (first insert, or a root split) it invokes `on_root_change` so the catalog can
// persist the new root page id. Construct with root == INVALID_PAGE_ID for an
// empty tree; the first insert lazily allocates the root leaf.
class BPlusTree {
 public:
  BPlusTree(BufferPoolManager *bpm, page_id_t root_page_id, TypeId key_type,
            uint32_t key_width, std::function<void(page_id_t)> on_root_change);

  // Insert (key, rid). Returns true if `key` was not already present (so callers
  // can maintain a distinct-key count). Duplicates are permitted.
  bool Insert(const Value &key, RID rid);

  // All RIDs stored under `key`, in leaf order (empty if absent).
  std::vector<RID> Search(const Value &key) const;

  // Remove the single entry exactly matching (key, rid). No-op if absent.
  void Delete(const Value &key, RID rid);

  bool IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }
  page_id_t GetRootPageId() const { return root_page_id_; }

  // Number of levels from root to leaf (0 if empty, 1 for a lone leaf root).
  int Height() const;

 private:
  // Descend from the root to the leaf that should hold `key_enc`, pushing every
  // internal ancestor page id onto `path` (root first). Returns the leaf id.
  page_id_t DescendToLeaf(const char *key_enc, std::vector<page_id_t> *path) const;

  // Insert separator `sep` + right_child into the parent of left_child, splitting
  // ancestors as needed. `path` holds the internal ancestors (root first); this
  // pops from it as it walks up.
  void InsertIntoParent(std::vector<page_id_t> *path, page_id_t left_child,
                        const std::string &sep, page_id_t right_child);

  NodeView Fetch(page_id_t pid, Page **out_page) const;

  BufferPoolManager *bpm_;
  page_id_t root_page_id_;
  TypeId key_type_;
  uint32_t key_width_;
  std::function<void(page_id_t)> on_root_change_;
};

}  // namespace minidb
