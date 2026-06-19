#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace minidb {

// The buffer pool caches a fixed number of disk pages in memory frames. It is
// the single chokepoint for all page access: higher layers call FetchPage /
// NewPage / UnpinPage rather than touching the DiskManager directly. A page is
// only evicted when its pin count is zero, and a dirty victim is written back
// before its frame is reused (the buffer-management half of the WAL contract).
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager *disk_manager);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager &) = delete;
  BufferPoolManager &operator=(const BufferPoolManager &) = delete;

  // Pin and return the page; reads it from disk on a miss. nullptr if no frame
  // is free and nothing is evictable.
  Page *FetchPage(page_id_t page_id);

  // Allocate a brand-new page, pin it, and return it (id written to *page_id).
  Page *NewPage(page_id_t *page_id);

  // Release a pin. is_dirty=true marks the page modified by this caller.
  bool UnpinPage(page_id_t page_id, bool is_dirty);

  // Force a single page / all pages to disk regardless of dirty flag.
  bool FlushPage(page_id_t page_id);
  void FlushAllPages();

  bool DeletePage(page_id_t page_id);

  // Simulate a crash: drop all in-memory (unflushed) pages on shutdown so the
  // data file is left at its last-flushed state. Used to exercise WAL recovery.
  void SimulateCrash() { crashed_ = true; }

  size_t GetPoolSize() const { return pool_size_; }
  DiskManager *GetDiskManager() { return disk_manager_; }

  // Simple instrumentation used by the benchmark / buffer-pool demo.
  size_t Hits() const { return num_hits_; }
  size_t Misses() const { return num_misses_; }

 private:
  // Obtain a usable frame: take from the free list, else evict an LRU victim
  // (flushing it first if dirty). Caller holds latch_.
  bool FindVictimFrame(frame_id_t *frame_id);

  size_t pool_size_;
  Page *pages_;  // array of `pool_size_` frames
  DiskManager *disk_manager_;
  std::unique_ptr<Replacer> replacer_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;  // page id -> frame
  std::list<frame_id_t> free_list_;
  std::mutex latch_;

  size_t num_hits_{0};
  size_t num_misses_{0};
  bool crashed_{false};  // when set, FlushAllPages is a no-op (crash simulation)
};

}  // namespace minidb
