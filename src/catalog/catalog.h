#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/schema.h"
#include "index/b_plus_tree.h"
#include "storage/table_heap.h"

namespace minidb {

// Metadata for one B+ tree index on a table.
struct IndexMeta {
  std::string name;
  page_id_t root_page_id;  // INVALID until the tree allocates a root
  uint32_t key_col;        // column index the index is keyed on
  uint32_t distinct_keys{0};  // NDV: distinct-key estimate for selectivity
};

// Everything the engine needs to know about a table: its name, schema, the
// first page of its heap, and any indexes.
struct TableMeta {
  std::string name;
  Schema schema;
  page_id_t first_page_id;
  std::vector<IndexMeta> indexes;
  uint32_t num_rows{0};  // live row count (N), maintained on insert/delete
};

// The catalog is the engine's persistent system table. It owns page 0, where
// it serializes all table/index metadata, and it hands out lazily-constructed
// TableHeap objects. Construct it BEFORE any user table so it claims page 0.
class Catalog {
 public:
  static constexpr page_id_t CATALOG_PAGE_ID = 0;

  explicit Catalog(BufferPoolManager *bpm);

  // Create a table (allocates its heap + first page). Returns nullptr if a
  // table with that name already exists.
  TableMeta *CreateTable(const std::string &name, const Schema &schema);

  TableMeta *GetTable(const std::string &name);
  std::vector<std::string> GetTableNames() const;

  // Register an index on an existing table and persist it.
  void UpsertIndex(const std::string &table, const IndexMeta &idx);
  void SetIndexRoot(const std::string &table, const std::string &index, page_id_t root);

  // A shared TableHeap for the table (built once, reused).
  TableHeap *GetTableHeap(const std::string &name);

  // A shared, lazily-built BPlusTree for the named index (nullptr if unknown).
  // Root changes are persisted automatically via SetIndexRoot.
  BPlusTree *GetIndex(const std::string &table, const std::string &index);

  void Persist();  // write the whole catalog to page 0

 private:
  void Load();  // read it back from page 0

  BufferPoolManager *bpm_;
  std::unordered_map<std::string, TableMeta> tables_;
  std::unordered_map<std::string, std::unique_ptr<TableHeap>> heaps_;
  std::unordered_map<std::string, std::unique_ptr<BPlusTree>> indexes_;  // "table.index" -> tree
};

}  // namespace minidb
