#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/rid.h"
#include "common/value.h"

namespace minidb {

// Fixed-width key encoding for B+ tree nodes. INTEGER occupies 4 bytes; VARCHAR
// occupies the column's declared length, NUL-padded (and truncated if a value
// somehow exceeds it). Keys are decoded back to Value and compared via
// Value::Compare, so the raw byte order never has to match numeric order — this
// sidesteps the classic two's-complement problem where memcmp would order
// negative integers after positive ones.

inline uint32_t KeyWidth(TypeId type, uint32_t col_length) {
  return type == TypeId::INTEGER ? 4u : col_length;
}

inline void EncodeKey(const Value &v, char *dst, uint32_t width) {
  std::memset(dst, 0, width);
  if (v.GetType() == TypeId::INTEGER) {
    int32_t n = v.GetInt();
    std::memcpy(dst, &n, 4);
  } else {
    const std::string &s = v.GetString();
    uint32_t n = s.size() < width ? static_cast<uint32_t>(s.size()) : width;
    if (n > 0) std::memcpy(dst, s.data(), n);
  }
}

inline Value DecodeKey(const char *src, uint32_t width, TypeId type) {
  if (type == TypeId::INTEGER) {
    int32_t n;
    std::memcpy(&n, src, 4);
    return Value(n);
  }
  uint32_t len = width;
  while (len > 0 && src[len - 1] == '\0') len--;  // trim NUL padding
  return Value(std::string(src, len));
}

// An RID is stored in a node as two little int32s: page id, then slot.
inline void EncodeRID(RID rid, char *dst) {
  int32_t pid = rid.GetPageId();
  int32_t slot = rid.GetSlotNum();
  std::memcpy(dst, &pid, 4);
  std::memcpy(dst + 4, &slot, 4);
}

inline RID DecodeRID(const char *src) {
  int32_t pid, slot;
  std::memcpy(&pid, src, 4);
  std::memcpy(&slot, src + 4, 4);
  return RID(pid, static_cast<slot_id_t>(slot));
}

}  // namespace minidb
