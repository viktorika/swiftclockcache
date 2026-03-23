/*
 * @Author: victorika
 * @Date: 2026-03-23 15:46:46
 * @Last Modified by: victorika
 * @Last Modified time: 2026-03-23 15:52:28
 */
#pragma once

#include <random>
#include "base.h"
#include "clock_cache_shard.h"

namespace swiftclockcache {
#include "xxhash.h"

struct BytesView {
  const void* data;
  size_t size;
};

template <typename Key, typename Enable = void>
struct KeyBytes {
  static_assert(std::is_trivially_copyable_v<Key>,
                "Non-trivially-copyable Key requires a specialization of swiftclockcache::KeyBytes");
  static BytesView GetBytes(const Key& key) { return {&key, sizeof(Key)}; }
};

template <>
struct KeyBytes<std::string> {
  static BytesView GetBytes(const std::string& key) { return {key.c_str(), key.size()}; }
};

template <>
struct KeyBytes<std::string_view> {
  static BytesView GetBytes(std::string_view key) { return {key.data(), key.size()}; }
};

template <typename Key>
inline HashedKey MakeHashedKey(const Key& key, uint64_t seed) {
  auto bytes = KeyBytes<Key>::GetBytes(key);
  XXH128_hash_t h = XXH3_128bits_withSeed(bytes.data, bytes.size, seed);
  return HashedKey{h.low64, h.high64};
}

template <typename Key, typename Value>
class SwiftClockCache;

template <typename Key, typename Value>
class Handle {
 public:
  Handle() noexcept = default;
  ~Handle() { Reset(); }

  Handle(Handle&& other) noexcept : shard_(other.shard_), slot_(other.slot_) {
    other.shard_ = nullptr;
    other.slot_ = nullptr;
  }

  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      Reset();
      shard_ = other.shard_;
      slot_ = other.slot_;
      other.shard_ = nullptr;
      other.slot_ = nullptr;
    }
    return *this;
  }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  explicit operator bool() const noexcept { return slot_ != nullptr; }

  Value* Get() const noexcept { return slot_ ? &(slot_->GetValue()) : nullptr; }

  Value& operator*() const noexcept { return slot_->GetValue(); }
  Value* operator->() const noexcept { return &(slot_->GetValue()); }

  const Key& GetKey() const { return slot_->GetKey(); }

  void Reset() noexcept {
    if (slot_ && shard_) {
      shard_->Release(slot_);
      slot_ = nullptr;
      shard_ = nullptr;
    }
  }

 private:
  using Shard = ClockCacheShard<Key, Value>;
  using Slot = typename Shard::Slot;
  friend class SwiftClockCache<Key, Value>;

  Handle(Shard* shard, Slot* slot) noexcept : shard_(shard), slot_(slot) {}

  Shard* shard_ = nullptr;
  Slot* slot_ = nullptr;
};

template <typename Key, typename Value>
class SwiftClockCache {
 public:
  using HandleType = Handle<Key, Value>;

  struct Options {
    size_t max_size = 1024;
    size_t num_shards = 32;
    int eviction_effort_cap = 10;
  };

  explicit SwiftClockCache(size_t max_size) {
    Options opts;
    opts.max_size = max_size;
    Init(opts);
  }

  explicit SwiftClockCache(const Options& opts) { Init(opts); }

  ~SwiftClockCache() = default;

  SwiftClockCache(const SwiftClockCache&) = delete;
  SwiftClockCache& operator=(const SwiftClockCache&) = delete;

  template <typename V>
  ErrorCode Insert(const Key& key, V&& value, uint32_t ttl_seconds = 0) {
    auto hk = MakeHashed(key);
    auto& shard = GetShard(hk);
    return shard.Insert(key, std::forward<V>(value), hk, ttl_seconds);
  }

  HandleType Lookup(const Key& key) {
    auto hk = MakeHashed(key);
    auto& shard = GetShard(hk);
    auto* slot = shard.Lookup(key, hk);
    if (slot == nullptr) {
      return {};
    }
    return HandleType(&shard, slot);
  }

  void Erase(const Key& key) {
    auto hk = MakeHashed(key);
    auto& shard = GetShard(hk);
    shard.Erase(key, hk);
  }

  void Clear() {
    for (auto& shard : shards_) {
      shard->EraseUnRefEntries();
    }
  }

  [[nodiscard]] size_t GetCapacity() const {
    size_t total = 0;
    for (auto& s : shards_) {
      total += s->GetCapacity();
    }
    return total;
  }

  [[nodiscard]] size_t GetSize() const {
    size_t total = 0;
    for (auto& s : shards_) {
      total += s->GetSize();
    }
    return total;
  }

  [[nodiscard]] bool Empty() const { return GetSize() == 0; }

 private:
  using Shard = ClockCacheShard<Key, Value>;

  void Init(const Options& opts) {
    assert(opts.max_size > 0);

    num_shards_ = RoundUpToPowerOf2(opts.num_shards);
    shard_bits_ = FloorLog2(num_shards_);

    std::random_device rd;
    seed_ = rd();

    size_t cap_per_shard = std::max(size_t{1}, opts.max_size / num_shards_);

    shards_.reserve(num_shards_);
    for (size_t i = 0; i < num_shards_; i++) {
      shards_.emplace_back(std::make_unique<Shard>(cap_per_shard, opts.eviction_effort_cap));
    }
  }

  HashedKey MakeHashed(const Key& key) const { return MakeHashedKey(key, seed_); }

  Shard& GetShard(HashedKey hk) {
    if (shard_bits_ == 0) {
      return *shards_[0];
    }
    uint32_t shard_idx = Upper32of64(hk.h0);
    return *shards_[shard_idx >> (32 - shard_bits_)];
  }

  size_t num_shards_;
  int shard_bits_;
  uint64_t seed_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace swiftclockcache