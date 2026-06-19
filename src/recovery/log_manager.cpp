#include "recovery/log_manager.h"

#include <cstring>

#include "common/exception.h"

namespace minidb {

LogManager::LogManager(const std::string &wal_file) : file_(wal_file) {
  // Open existing for read+write; create it if missing (mirrors DiskManager).
  io_.open(file_, std::ios::binary | std::ios::in | std::ios::out);
  bool fresh = false;
  if (!io_.is_open()) {
    io_.clear();
    std::ofstream create(file_, std::ios::binary);
    create.close();
    io_.open(file_, std::ios::binary | std::ios::in | std::ios::out);
    if (!io_.is_open()) throw Exception(ErrorKind::kIO, "cannot open or create WAL: " + file_);
    fresh = true;
  }

  if (fresh) {
    prev_dirty_ = false;  // brand-new log: no prior crash
  } else {
    io_.seekg(0);
    char marker = 1;
    io_.read(&marker, 1);
    if (io_.gcount() < 1) {
      marker = 1;  // empty file -> treat as clean
      io_.clear();
    }
    prev_dirty_ = (marker == 0);  // a 0 marker means the last run never marked clean
  }
  WriteHeader(0);  // mark in-use (dirty) for this session
}

LogManager::~LogManager() {
  if (io_.is_open()) {
    io_.flush();
    io_.close();
  }
}

void LogManager::WriteHeader(uint8_t clean) {
  io_.seekp(0);
  io_.write(reinterpret_cast<const char *>(&clean), 1);
  io_.flush();
}

void LogManager::MarkClean() {
  std::lock_guard<std::mutex> g(latch_);
  WriteHeader(1);
}

void LogManager::Append(const LogRecord &rec) {
  std::lock_guard<std::mutex> g(latch_);
  io_.seekp(0, std::ios::end);
  uint8_t type = static_cast<uint8_t>(rec.type);
  int32_t txn = rec.txn;
  io_.write(reinterpret_cast<const char *>(&type), 1);
  io_.write(reinterpret_cast<const char *>(&txn), 4);
  if (rec.type == LogType::kInsert || rec.type == LogType::kDelete) {
    uint16_t tlen = static_cast<uint16_t>(rec.table.size());
    uint32_t dlen = static_cast<uint32_t>(rec.tuple_bytes.size());
    io_.write(reinterpret_cast<const char *>(&tlen), 2);
    io_.write(rec.table.data(), tlen);
    io_.write(reinterpret_cast<const char *>(&dlen), 4);
    io_.write(rec.tuple_bytes.data(), dlen);
  }
  if (io_.bad()) throw Exception(ErrorKind::kIO, "WAL append failed");
}

void LogManager::Flush() {
  std::lock_guard<std::mutex> g(latch_);
  io_.flush();
}

void LogManager::Reset() {
  std::lock_guard<std::mutex> g(latch_);
  io_.close();
  io_.open(file_, std::ios::binary | std::ios::out | std::ios::trunc);  // empty the file
  uint8_t dirty = 0;
  io_.write(reinterpret_cast<const char *>(&dirty), 1);
  io_.close();
  io_.open(file_, std::ios::binary | std::ios::in | std::ios::out);
  prev_dirty_ = false;
}

std::vector<LogRecord> LogManager::ReadAll() {
  std::lock_guard<std::mutex> g(latch_);
  std::vector<LogRecord> out;
  io_.clear();
  io_.seekg(1);  // skip the 1-byte clean/dirty header
  while (true) {
    uint8_t type;
    io_.read(reinterpret_cast<char *>(&type), 1);
    if (io_.gcount() < 1) break;
    int32_t txn;
    if (io_.read(reinterpret_cast<char *>(&txn), 4).gcount() < 4) break;

    LogRecord rec;
    rec.type = static_cast<LogType>(type);
    rec.txn = txn;
    if (rec.type == LogType::kInsert || rec.type == LogType::kDelete) {
      uint16_t tlen;
      if (io_.read(reinterpret_cast<char *>(&tlen), 2).gcount() < 2) break;
      rec.table.resize(tlen);
      if (tlen > 0 && io_.read(&rec.table[0], tlen).gcount() < tlen) break;
      uint32_t dlen;
      if (io_.read(reinterpret_cast<char *>(&dlen), 4).gcount() < 4) break;
      rec.tuple_bytes.resize(dlen);
      if (dlen > 0 &&
          io_.read(&rec.tuple_bytes[0], dlen).gcount() < static_cast<std::streamsize>(dlen)) {
        break;
      }
    }
    out.push_back(std::move(rec));
  }
  io_.clear();
  return out;
}

}  // namespace minidb
