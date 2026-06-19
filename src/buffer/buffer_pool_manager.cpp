#include "buffer/buffer_pool_manager.h"

namespace minidb {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = std::make_unique<LRUReplacer>(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.push_back(static_cast<frame_id_t>(i));
  }
}

BufferPoolManager::~BufferPoolManager() {
  FlushAllPages();
  delete[] pages_;
}

bool BufferPoolManager::FindVictimFrame(frame_id_t *frame_id) {
  if (!free_list_.empty()) {
    *frame_id = free_list_.front();
    free_list_.pop_front();
    return true;
  }
  if (replacer_->Victim(frame_id)) {
    Page &victim = pages_[*frame_id];
    if (victim.is_dirty_) {
      disk_manager_->WritePage(victim.page_id_, victim.data_);
      victim.is_dirty_ = false;
    }
    page_table_.erase(victim.page_id_);
    return true;
  }
  return false;  // every frame is pinned
}

Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> g(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {  // cache hit
    frame_id_t fid = it->second;
    Page &p = pages_[fid];
    p.pin_count_++;
    replacer_->Pin(fid);
    num_hits_++;
    return &p;
  }

  num_misses_++;
  frame_id_t fid;
  if (!FindVictimFrame(&fid)) return nullptr;

  Page &p = pages_[fid];
  p.page_id_ = page_id;
  p.pin_count_ = 1;
  p.is_dirty_ = false;
  disk_manager_->ReadPage(page_id, p.data_);
  page_table_[page_id] = fid;
  replacer_->Pin(fid);
  return &p;
}

Page *BufferPoolManager::NewPage(page_id_t *page_id) {
  std::lock_guard<std::mutex> g(latch_);

  frame_id_t fid;
  if (!FindVictimFrame(&fid)) return nullptr;

  page_id_t new_id = disk_manager_->AllocatePage();
  Page &p = pages_[fid];
  p.ResetMemory();
  p.page_id_ = new_id;
  p.pin_count_ = 1;
  p.is_dirty_ = true;  // a new page must eventually reach disk
  p.lsn_ = INVALID_LSN;
  page_table_[new_id] = fid;
  replacer_->Pin(fid);
  *page_id = new_id;
  return &p;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  Page &p = pages_[it->second];
  if (p.pin_count_ <= 0) return false;
  p.pin_count_--;
  if (is_dirty) p.is_dirty_ = true;
  if (p.pin_count_ == 0) replacer_->Unpin(it->second);
  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  Page &p = pages_[it->second];
  disk_manager_->WritePage(page_id, p.data_);
  p.is_dirty_ = false;
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> g(latch_);
  if (crashed_) return;  // crash simulation: discard unflushed pages
  for (auto &kv : page_table_) {
    Page &p = pages_[kv.second];
    if (p.is_dirty_) {
      disk_manager_->WritePage(p.page_id_, p.data_);
      p.is_dirty_ = false;
    }
  }
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::lock_guard<std::mutex> g(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return true;  // nothing to delete
  frame_id_t fid = it->second;
  Page &p = pages_[fid];
  if (p.pin_count_ > 0) return false;  // still in use

  replacer_->Pin(fid);  // remove from eviction list
  page_table_.erase(it);
  p.ResetMemory();
  p.page_id_ = INVALID_PAGE_ID;
  p.is_dirty_ = false;
  p.pin_count_ = 0;
  free_list_.push_back(fid);
  disk_manager_->DeallocatePage(page_id);
  return true;
}

}  // namespace minidb
