/*
 * @Author: victorika
 * @Date: 2026-03-16 15:31:20
 * @Last Modified by: victorika
 * @Last Modified time: 2026-03-23 15:55:31
 */
#pragma once

#include <functional>
#include "base.h"
#include "fixed_clock_table.h"

namespace swiftclockcache {

template <typename Key, typename Value>
class ClockCacheShard {
 public:
  using Table = FixedClockTable<Key, Value>;
  using Slot = typename Table::Slot;

  ClockCacheShard(size_t capacity, int eviction_effort_cap) : table_(capacity, eviction_effort_cap) {}

  template <typename V>
  ErrorCode Insert(const Key& key, V&& value, HashedKey hk, uint32_t now, uint32_t ttl_seconds = 0) {
    size_t current_size = table_.GetSize();
    size_t capacity = table_.GetCapacity();
    if (current_size >= capacity) {
      EvictionData ed{};
      table_.Evict(now, current_size - capacity + 1, &ed);
      if (0 == ed.freed_count) {
        return ErrorCode::kCapacityExceeded;
      }
    }

    uint32_t expire_at = CalcExpireTime(now, ttl_seconds);

    auto* result = table_.DoInsert(key, std::forward<V>(value), hk, expire_at);
    if (!result) {
      return ErrorCode::kTableFull;
    }

    return ErrorCode::kOk;
  }

  Slot* Lookup(const Key& key, HashedKey hk, uint32_t now) { return table_.Lookup(key, hk, now); }

  bool Release(Slot* s) { return table_.Release(s); }

  void Erase(const Key& key, HashedKey hk) { table_.Erase(key, hk); }

  void EraseUnRefEntries() { table_.EraseUnRefEntries(); }

  [[nodiscard]] size_t GetCapacity() const { return table_.GetCapacity(); }
  [[nodiscard]] size_t GetSize() const { return table_.GetSize(); }

 private:
  Table table_;
};

}  // namespace swiftclockcache