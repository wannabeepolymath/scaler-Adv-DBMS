#pragma once

#include <cstdint>
#include <cstring>

#include "common/config.h"
#include "common/rid.h"
#include "common/value.h"
#include "index/key_codec.h"
#include "storage/page.h"

namespace minidb {

// A typed view over a raw 4 KB buffer-pool Page holding one B+ tree node.
//
// Header (8 bytes, identical for both node kinds):
//   [0] uint8  node_type   0 = internal, 1 = leaf
//   [1] uint8  (padding)
//   [2] uint16 key_count   number of keys currently stored
//   [4] int32  next        leaf: next leaf page id; internal: unused
//
// Leaf body (fixed stride so an RID never moves): entry i lives at
//   8 + i*(key_width + 8)  =  key_width key bytes  ++  8 RID bytes
//
// Internal body (children and keys kept in fixed-capacity regions so their
// offsets never shift as the count changes): a node with `count` keys has
// `count + 1` children.
//   children: child i at  8 + i*4                    (capacity InternalCap + 1)
//   keys:     key j  at  8 + (InternalCap+1)*4 + j*key_width
//
// The NodeView itself is a cheap, copyable handle (a pointer + the key width);
// it owns nothing. The caller is responsible for pin/unpin and dirty marking.
class NodeView {
 public:
  static constexpr size_t HEADER = 8;
  static constexpr size_t OFF_TYPE = 0;
  static constexpr size_t OFF_COUNT = 2;
  static constexpr size_t OFF_NEXT = 4;
  static constexpr size_t RID_BYTES = 8;

  static NodeView Of(Page *page, uint32_t key_width) {
    return NodeView{page->GetData(), key_width};
  }

  // --- header -------------------------------------------------------------
  void InitLeaf() {
    std::memset(data_, 0, PAGE_SIZE);
    SetU8(OFF_TYPE, 1);
    SetCount(0);
    SetNext(INVALID_PAGE_ID);
  }
  void InitInternal() {
    std::memset(data_, 0, PAGE_SIZE);
    SetU8(OFF_TYPE, 0);
    SetCount(0);
    SetNext(INVALID_PAGE_ID);
  }
  bool IsLeaf() const { return GetU8(OFF_TYPE) == 1; }

  uint16_t Count() const { return GetU16(OFF_COUNT); }
  void SetCount(uint16_t n) { SetU16(OFF_COUNT, n); }

  page_id_t Next() const { return GetI32(OFF_NEXT); }
  void SetNext(page_id_t n) { SetI32(OFF_NEXT, n); }

  // --- capacities ---------------------------------------------------------
  uint32_t LeafCap() const {
    return static_cast<uint32_t>((PAGE_SIZE - HEADER) / (key_width_ + RID_BYTES));
  }
  uint32_t InternalCap() const {
    return static_cast<uint32_t>((PAGE_SIZE - HEADER - 4) / (4 + key_width_));
  }

  // --- leaf accessors -----------------------------------------------------
  const char *LeafKey(int i) const { return data_ + LeafEntryOff(i); }
  RID LeafRID(int i) const { return DecodeRID(data_ + LeafEntryOff(i) + key_width_); }
  void SetLeafEntry(int i, const char *key, RID rid) {
    std::memcpy(data_ + LeafEntryOff(i), key, key_width_);
    EncodeRID(rid, data_ + LeafEntryOff(i) + key_width_);
  }

  // --- internal accessors -------------------------------------------------
  page_id_t Child(int i) const { return GetI32(ChildOff(i)); }
  void SetChild(int i, page_id_t pid) { SetI32(ChildOff(i), pid); }
  const char *IntKey(int i) const { return data_ + IntKeyOff(i); }
  void SetIntKey(int i, const char *key) { std::memcpy(data_ + IntKeyOff(i), key, key_width_); }

  // First index in [0, Count) whose key is >= the probe key, decoding both
  // sides to Value so comparison respects type semantics (never raw bytes).
  int LowerBound(const char *probe_key, TypeId type) const {
    Value probe = DecodeKey(probe_key, key_width_, type);
    int lo = 0, hi = Count();
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      Value mid_key = DecodeKey(KeyPtr(mid), key_width_, type);
      if (mid_key.Compare(probe) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  char *Data() { return data_; }
  uint32_t KeyWidthBytes() const { return key_width_; }

 private:
  NodeView(char *data, uint32_t key_width) : data_(data), key_width_(key_width) {}

  size_t LeafEntryOff(int i) const { return HEADER + static_cast<size_t>(i) * (key_width_ + RID_BYTES); }
  size_t ChildOff(int i) const { return HEADER + static_cast<size_t>(i) * 4; }
  size_t IntKeyOff(int i) const {
    return HEADER + (static_cast<size_t>(InternalCap()) + 1) * 4 + static_cast<size_t>(i) * key_width_;
  }
  const char *KeyPtr(int i) const { return IsLeaf() ? LeafKey(i) : IntKey(i); }

  uint8_t GetU8(size_t off) const { return static_cast<uint8_t>(data_[off]); }
  void SetU8(size_t off, uint8_t v) { data_[off] = static_cast<char>(v); }
  uint16_t GetU16(size_t off) const {
    uint16_t v;
    std::memcpy(&v, data_ + off, 2);
    return v;
  }
  void SetU16(size_t off, uint16_t v) { std::memcpy(data_ + off, &v, 2); }
  int32_t GetI32(size_t off) const {
    int32_t v;
    std::memcpy(&v, data_ + off, 4);
    return v;
  }
  void SetI32(size_t off, int32_t v) { std::memcpy(data_ + off, &v, 4); }

  char *data_;
  uint32_t key_width_;
};

}  // namespace minidb
