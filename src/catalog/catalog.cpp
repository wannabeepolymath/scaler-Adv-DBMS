#include "catalog/catalog.h"

#include <cstring>

#include "common/exception.h"

namespace minidb {

// --- minimal append/read byte cursors for catalog serialization ------------
namespace {
struct Writer {
  std::vector<char> buf;
  void U8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
  void U16(uint16_t v) { Raw(&v, 2); }
  void U32(uint32_t v) { Raw(&v, 4); }
  void I32(int32_t v) { Raw(&v, 4); }
  void Str(const std::string &s) {
    U16(static_cast<uint16_t>(s.size()));
    if (!s.empty()) buf.insert(buf.end(), s.begin(), s.end());
  }
  void Raw(const void *p, size_t n) {
    const char *c = static_cast<const char *>(p);
    buf.insert(buf.end(), c, c + n);
  }
};

struct Reader {
  const char *p;
  size_t n;
  size_t pos = 0;
  uint8_t U8() { return static_cast<uint8_t>(p[pos++]); }
  uint16_t U16() {
    uint16_t v;
    std::memcpy(&v, p + pos, 2);
    pos += 2;
    return v;
  }
  uint32_t U32() {
    uint32_t v;
    std::memcpy(&v, p + pos, 4);
    pos += 4;
    return v;
  }
  int32_t I32() {
    int32_t v;
    std::memcpy(&v, p + pos, 4);
    pos += 4;
    return v;
  }
  std::string Str() {
    uint16_t len = U16();
    std::string s(p + pos, len);
    pos += len;
    return s;
  }
};

constexpr uint32_t CATALOG_MAGIC = 0x4D444244;  // "MDBD" (bumped: adds num_rows + distinct_keys)
}  // namespace

Catalog::Catalog(BufferPoolManager *bpm) : bpm_(bpm) {
  if (bpm_->GetDiskManager()->GetNumPages() == 0) {
    // Fresh database: claim page 0 for the catalog and write an empty image.
    page_id_t pid;
    bpm_->NewPage(&pid);
    if (pid != CATALOG_PAGE_ID) {
      throw Exception(ErrorKind::kIO, "catalog must own page 0 (construct it first)");
    }
    bpm_->UnpinPage(pid, true);
    Persist();
  } else {
    Load();
  }
}

void Catalog::Persist() {
  Writer w;
  w.U32(CATALOG_MAGIC);
  w.U32(static_cast<uint32_t>(tables_.size()));
  for (auto &kv : tables_) {
    const TableMeta &t = kv.second;
    w.Str(t.name);
    w.I32(t.first_page_id);
    w.U32(t.num_rows);
    const auto &cols = t.schema.GetColumns();
    w.U16(static_cast<uint16_t>(cols.size()));
    for (const auto &c : cols) {
      w.Str(c.name);
      w.U8(static_cast<uint8_t>(c.type));
      w.U32(c.length);
    }
    w.U16(static_cast<uint16_t>(t.indexes.size()));
    for (const auto &idx : t.indexes) {
      w.Str(idx.name);
      w.I32(idx.root_page_id);
      w.U32(idx.key_col);
      w.U32(idx.distinct_keys);
    }
  }
  if (w.buf.size() > PAGE_SIZE) {
    throw Exception(ErrorKind::kIO, "catalog too large for a single page");
  }
  Page *page = bpm_->FetchPage(CATALOG_PAGE_ID);
  std::memset(page->GetData(), 0, PAGE_SIZE);
  std::memcpy(page->GetData(), w.buf.data(), w.buf.size());
  bpm_->UnpinPage(CATALOG_PAGE_ID, true);
  bpm_->FlushPage(CATALOG_PAGE_ID);
}

void Catalog::Load() {
  Page *page = bpm_->FetchPage(CATALOG_PAGE_ID);
  Reader r{page->GetData(), PAGE_SIZE};
  uint32_t magic = r.U32();
  if (magic != CATALOG_MAGIC) {
    bpm_->UnpinPage(CATALOG_PAGE_ID, false);
    throw Exception(ErrorKind::kIO, "bad catalog magic; file is not a MiniDB database");
  }
  uint32_t num_tables = r.U32();
  for (uint32_t i = 0; i < num_tables; i++) {
    TableMeta t;
    t.name = r.Str();
    t.first_page_id = r.I32();
    t.num_rows = r.U32();
    uint16_t num_cols = r.U16();
    std::vector<Column> cols;
    for (uint16_t c = 0; c < num_cols; c++) {
      std::string cname = r.Str();
      auto type = static_cast<TypeId>(r.U8());
      uint32_t len = r.U32();
      cols.emplace_back(cname, type, len);
    }
    t.schema = Schema(std::move(cols));
    uint16_t num_idx = r.U16();
    for (uint16_t k = 0; k < num_idx; k++) {
      IndexMeta idx;
      idx.name = r.Str();
      idx.root_page_id = r.I32();
      idx.key_col = r.U32();
      idx.distinct_keys = r.U32();
      t.indexes.push_back(idx);
    }
    tables_[t.name] = std::move(t);
  }
  bpm_->UnpinPage(CATALOG_PAGE_ID, false);
}

TableMeta *Catalog::CreateTable(const std::string &name, const Schema &schema) {
  if (tables_.count(name) != 0) return nullptr;
  TableMeta t;
  t.name = name;
  t.schema = schema;
  t.first_page_id = TableHeap::CreateNew(bpm_);
  tables_[name] = std::move(t);
  Persist();
  return &tables_[name];
}

TableMeta *Catalog::GetTable(const std::string &name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::GetTableNames() const {
  std::vector<std::string> names;
  for (auto &kv : tables_) names.push_back(kv.first);
  return names;
}

void Catalog::UpsertIndex(const std::string &table, const IndexMeta &idx) {
  TableMeta *t = GetTable(table);
  if (t == nullptr) throw Exception(ErrorKind::kBinder, "no such table: " + table);
  for (auto &existing : t->indexes) {
    if (existing.name == idx.name) {
      existing = idx;
      Persist();
      return;
    }
  }
  t->indexes.push_back(idx);
  Persist();
}

void Catalog::SetIndexRoot(const std::string &table, const std::string &index, page_id_t root) {
  TableMeta *t = GetTable(table);
  if (t == nullptr) return;
  for (auto &idx : t->indexes) {
    if (idx.name == index) {
      idx.root_page_id = root;
      Persist();
      return;
    }
  }
}

TableHeap *Catalog::GetTableHeap(const std::string &name) {
  auto it = heaps_.find(name);
  if (it != heaps_.end()) return it->second.get();
  TableMeta *t = GetTable(name);
  if (t == nullptr) return nullptr;
  heaps_[name] = std::make_unique<TableHeap>(bpm_, t->first_page_id);
  return heaps_[name].get();
}

BPlusTree *Catalog::GetIndex(const std::string &table, const std::string &index) {
  std::string key = table + "." + index;
  auto it = indexes_.find(key);
  if (it != indexes_.end()) return it->second.get();

  TableMeta *t = GetTable(table);
  if (t == nullptr) return nullptr;
  IndexMeta *im = nullptr;
  for (auto &idx : t->indexes) {
    if (idx.name == index) {
      im = &idx;
      break;
    }
  }
  if (im == nullptr) return nullptr;

  const Column &col = t->schema.GetColumn(im->key_col);
  uint32_t kw = KeyWidth(col.type, col.length);
  // When the tree's root changes, persist it back into the catalog.
  auto on_root = [this, table, index](page_id_t r) { SetIndexRoot(table, index, r); };
  indexes_[key] =
      std::make_unique<BPlusTree>(bpm_, im->root_page_id, col.type, kw, on_root);
  return indexes_[key].get();
}

}  // namespace minidb
