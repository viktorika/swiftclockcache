/*
 * @Author: victorika
 * @Date: 2026-03-16 15:34:36
 * @Last Modified by: victorika
 * @Last Modified time: 2026-03-23 15:54:18
 */
#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include "base.h"

namespace swiftclockcache {

struct HashedKey {
  uint64_t h0;  // increment
  uint64_t h1;  // base
};

struct EvictionData {
  size_t freed_count = 0;
  size_t seen_pinned_count = 0;
};

// ============================================================================
// layout of SlotMeta (64 bits total):
//   [0:29]  AcquireCounter (30 bits)
//   [30:59] ReleaseCounter (30 bits)
//   [60]    HitFlag (deprecated)
//   [61]    OccupiedFlag
//   [62]    ShareableFlag
//   [63]    VisibleFlag
// ============================================================================

struct SlotMeta {
  uint64_t raw = 0;

  static constexpr int kCounterBits = 30;
  static constexpr uint64_t kCounterMask = (uint64_t{1} << kCounterBits) - 1;
  static constexpr int kAcquireShift = 0;
  static constexpr int kReleaseShift = 30;
  // static constexpr uint64_t kHitBit = uint64_t{1} << 60;
  static constexpr uint64_t kOccupiedBit = uint64_t{1} << 61;
  static constexpr uint64_t kShareableBit = uint64_t{1} << 62;
  static constexpr uint64_t kVisibleBit = uint64_t{1} << 63;

  [[nodiscard]] uint32_t GetAcquireCounter() const {
    return static_cast<uint32_t>((raw >> kAcquireShift) & kCounterMask);
  }
  [[nodiscard]] uint32_t GetReleaseCounter() const {
    return static_cast<uint32_t>((raw >> kReleaseShift) & kCounterMask);
  }
  [[nodiscard]] uint32_t GetRefcount() const { return GetAcquireCounter() - GetReleaseCounter(); }

  void SetAcquireCounter(uint32_t v) {
    raw =
        (raw & ~(kCounterMask << kAcquireShift)) | (uint64_t{v & static_cast<uint32_t>(kCounterMask)} << kAcquireShift);
  }
  void SetReleaseCounter(uint32_t v) {
    raw =
        (raw & ~(kCounterMask << kReleaseShift)) | (uint64_t{v & static_cast<uint32_t>(kCounterMask)} << kReleaseShift);
  }

  [[nodiscard]] bool IsEmpty() const { return (raw & kOccupiedBit) == 0; }
  [[nodiscard]] bool IsUnderConstruction() const {
    return ((raw & kOccupiedBit) != 0U) && ((raw & kShareableBit) == 0U);
  }
  [[nodiscard]] bool IsShareable() const { return (raw & kShareableBit) != 0; }
  [[nodiscard]] bool IsVisible() const { return ((raw & kShareableBit) != 0U) && ((raw & kVisibleBit) != 0U); }
  [[nodiscard]] bool IsInvisible() const { return ((raw & kShareableBit) != 0U) && ((raw & kVisibleBit) == 0U); }

  void SetUnderConstruction() { raw = (raw & ~(kVisibleBit | kShareableBit)) | kOccupiedBit; }
  void SetVisible() { raw |= kOccupiedBit | kShareableBit | kVisibleBit; }
  void ClearVisible() { raw &= ~kVisibleBit; }

  static constexpr uint8_t kMaxCountdown = 3;
};

class AtomicSlotMeta {
 public:
  AtomicSlotMeta() : v_{0} {}

  SlotMeta LoadRelaxed() const { return SlotMeta{v_.load(std::memory_order_relaxed)}; }
  void StoreRelaxed(SlotMeta m) { v_.store(m.raw, std::memory_order_relaxed); }

  SlotMeta Load() const { return SlotMeta{v_.load(std::memory_order_acquire)}; }
  void Store(SlotMeta m) { v_.store(m.raw, std::memory_order_release); }
  SlotMeta Exchange(SlotMeta m) { return SlotMeta{v_.exchange(m.raw, std::memory_order_acq_rel)}; }

  bool CasStrong(SlotMeta& expected, SlotMeta desired) {
    return v_.compare_exchange_strong(expected.raw, desired.raw, std::memory_order_acq_rel);
  }
  bool CasStrongRelaxed(SlotMeta& expected, SlotMeta desired) {
    return v_.compare_exchange_strong(expected.raw, desired.raw, std::memory_order_relaxed);
  }
  bool CasWeak(SlotMeta& expected, SlotMeta desired) {
    return v_.compare_exchange_weak(expected.raw, desired.raw, std::memory_order_acq_rel);
  }

  SlotMeta FetchAddAcquire(uint32_t count) {
    return SlotMeta{v_.fetch_add(uint64_t{count} << SlotMeta::kAcquireShift, std::memory_order_acq_rel)};
  }
  SlotMeta FetchSubAcquire(uint32_t count) {
    return SlotMeta{v_.fetch_sub(uint64_t{count} << SlotMeta::kAcquireShift, std::memory_order_acq_rel)};
  }
  SlotMeta FetchAddRelease(uint32_t count) {
    return SlotMeta{v_.fetch_add(uint64_t{count} << SlotMeta::kReleaseShift, std::memory_order_acq_rel)};
  }
  SlotMeta FetchSetOccupied() { return SlotMeta{v_.fetch_or(SlotMeta::kOccupiedBit, std::memory_order_acq_rel)}; }
  void FetchClearVisible() { v_.fetch_and(~SlotMeta::kVisibleBit, std::memory_order_acq_rel); }
  void FetchAndRelaxed(uint64_t mask) { v_.fetch_and(mask, std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> v_;
};

inline void CorrectNearOverflow(SlotMeta old_meta, AtomicSlotMeta& meta) {
  constexpr uint32_t kCounterTopBit = uint32_t{1} << (SlotMeta::kCounterBits - 1);
  constexpr uint32_t kThreshold = kCounterTopBit + SlotMeta::kMaxCountdown;
  if (UNLIKELY(old_meta.GetReleaseCounter() > kThreshold)) {
    constexpr uint64_t kAcqMask = uint64_t{kCounterTopBit - 1} << SlotMeta::kAcquireShift;
    constexpr uint64_t kRelMask = uint64_t{kCounterTopBit - 1} << SlotMeta::kReleaseShift;
    constexpr uint64_t kFlags =
        ~((SlotMeta::kCounterMask << SlotMeta::kAcquireShift) | (SlotMeta::kCounterMask << SlotMeta::kReleaseShift));
    meta.FetchAndRelaxed(kAcqMask | kRelMask | kFlags);
  }
}

inline void Unref(AtomicSlotMeta& meta, uint32_t count = 1) {
  auto old = meta.FetchSubAcquire(count);
  assert(old.GetRefcount() != 0);
}

template <typename Key, typename Value>
class FixedClockTable {
 public:
  // ========================================================================
  // Slot —— hash table slot
  // ========================================================================
  struct Slot {
    mutable AtomicSlotMeta meta;
    std::atomic<uint32_t> displacements{0};
    std::atomic<uint32_t> expire_at{0};
    HashedKey hashed_key = {};  // 128-bit hash value

    // Key
    alignas(alignof(Key)) unsigned char key_storage[sizeof(Key)];

    // Value
    alignas(alignof(Value)) unsigned char value_storage[sizeof(Value)];

    // ---- Key operations ----
    template <typename... Args>
    void ConstructKey(Args&&... args) {
      ::new (static_cast<void*>(key_storage)) Key(std::forward<Args>(args)...);
    }
    void DestroyKey() { reinterpret_cast<Key*>(key_storage)->~Key(); }
    const Key& GetKey() const { return *reinterpret_cast<const Key*>(key_storage); }

    // ---- Value operations ----
    template <typename... Args>
    void ConstructValue(Args&&... args) {
      ::new (static_cast<void*>(value_storage)) Value(std::forward<Args>(args)...);
    }
    void DestroyValue() { reinterpret_cast<Value*>(value_storage)->~Value(); }
    Value& GetValue() { return *reinterpret_cast<Value*>(value_storage); }
    const Value& GetValue() const { return *reinterpret_cast<const Value*>(value_storage); }

    // ---- Displacement operations ----
    uint32_t LoadDisplacements() const { return displacements.load(std::memory_order_relaxed); }
    void AddDisplacement() { displacements.fetch_add(1, std::memory_order_relaxed); }
    void SubDisplacement() { displacements.fetch_sub(1, std::memory_order_relaxed); }

    // ---- TTL operations ----
    void SetExpireAt(uint32_t t) { expire_at.store(t, std::memory_order_relaxed); }
    uint32_t GetExpireAt() const { return expire_at.load(std::memory_order_relaxed); }
    bool CheckExpiredAt(uint32_t now) const { return IsExpiredAt(expire_at.load(std::memory_order_relaxed), now); }
  };

  static constexpr double kLoadFactor = 0.7;

  FixedClockTable(size_t capacity, int eviction_effort_cap)
      : length_bits_(CalcHashBits(capacity)),
        length_bits_mask_((size_t{1} << length_bits_) - 1),
        eviction_effort_cap_(std::max(1, eviction_effort_cap)),
        capacity_(capacity) {
    size_t table_size = size_t{1} << length_bits_;
    slots_ = std::make_unique<Slot[]>(table_size);
    size_.store(0, std::memory_order_relaxed);
    clock_pointer_.store(0, std::memory_order_relaxed);
  }

  ~FixedClockTable() {
    size_t table_size = GetTableSize();
    for (size_t i = 0; i < table_size; i++) {
      Slot& s = slots_[i];
      SlotMeta m = s.meta.LoadRelaxed();
      if (m.IsShareable()) {
        FreeSlot(s);
      }
    }
  }

  FixedClockTable(const FixedClockTable&) = delete;
  FixedClockTable& operator=(const FixedClockTable&) = delete;

  size_t GetTableSize() const { return size_t{1} << length_bits_; }
  size_t GetCapacity() const { return capacity_; }
  size_t GetSize() const { return size_.load(std::memory_order_relaxed); }

  // ========================================================================
  // Insert
  // ========================================================================
  template <typename V>
  Slot* DoInsert(const Key& key, V&& value, HashedKey hk, uint32_t expire_at = 0) {
    bool already_matches = false;

    Slot* result = FindSlot(
        hk,
        [&](Slot* s) -> bool { return TryInsert(key, std::forward<V>(value), hk, *s, &already_matches, expire_at); },
        [&](Slot* s) -> bool {
          if (already_matches) {
            Rollback(hk, s);
            return true;
          }
          return false;
        },
        [&](Slot* s, bool is_last) {
          if (is_last) {
            Rollback(hk, s);
          } else {
            s->AddDisplacement();
          }
        });

    if (already_matches) {
      return nullptr;
    }

    if (result) {
      size_.fetch_add(1, std::memory_order_relaxed);
      return result;
    }

    return nullptr;
  }

  void Evict(uint32_t now, size_t requested_count, EvictionData* data) {
    assert(requested_count > 0);
    constexpr size_t kStepSize = 4;

    uint64_t old_cp = clock_pointer_.fetch_add(kStepSize, std::memory_order_relaxed);
    uint64_t max_cp = old_cp + (uint64_t{SlotMeta::kMaxCountdown} << length_bits_);

    for (;;) {
      for (size_t i = 0; i < kStepSize; i++) {
        Slot& s = slots_[ModTableSize(old_cp + i)];
        bool evicting = ClockUpdate(s, data, now);
        if (evicting) {
          Rollback(s.hashed_key, &s);
          FreeSlot(s);
          MarkEmpty(s);
          ReclaimEntry();
        }
      }

      if (data->freed_count >= requested_count) {
        return;
      }
      if (old_cp >= max_cp) {
        return;
      }
      if (IsEvictionEffortExceeded(*data)) {
        return;
      }

      old_cp = clock_pointer_.fetch_add(kStepSize, std::memory_order_relaxed);
    }
  }

  Slot* Lookup(const Key& key, HashedKey hk, uint32_t now) {
    bool expired = false;
    return FindSlot(
        hk,
        [&](Slot* s) -> bool {
          SlotMeta old_meta = s->meta.FetchAddAcquire(1);

          if (old_meta.IsVisible()) {
            if (s->hashed_key.h0 == hk.h0 && s->hashed_key.h1 == hk.h1 && s->GetKey() == key) {
              if (s->CheckExpiredAt(now)) {
                s->meta.FetchClearVisible();
                Unref(s->meta);
                expired = true;
                return false;
              }
              return true;
            }
            Unref(s->meta);
          } else if (UNLIKELY(old_meta.IsInvisible())) {
            Unref(s->meta);
          }
          return false;
        },
        [&](Slot* s) -> bool {
          if (expired) {
            return true;
          }
          return s->LoadDisplacements() == 0;
        },
        [&](Slot*, bool) {});
  }

  // ========================================================================
  // Release
  // ========================================================================
  bool Release(Slot* s) {
    SlotMeta old_meta = s->meta.FetchAddRelease(1);

    assert(old_meta.IsShareable());
    assert(old_meta.GetAcquireCounter() != old_meta.GetReleaseCounter());

    if (UNLIKELY(old_meta.IsInvisible())) {
      old_meta.SetReleaseCounter(old_meta.GetReleaseCounter() + 1);

      SlotMeta cm;
      cm.raw = 0;
      cm.SetUnderConstruction();

      do {
        if (old_meta.GetRefcount() != 0) {
          CorrectNearOverflow(old_meta, s->meta);
          return false;
        }
        if (!old_meta.IsShareable()) {
          return false;
        }
      } while (!s->meta.CasWeak(old_meta, cm));

      Rollback(s->hashed_key, s);
      FreeSlot(*s);
      MarkEmpty(*s);
      ReclaimEntry();
      return true;
    }

    CorrectNearOverflow(old_meta, s->meta);
    return false;
  }

  // ========================================================================
  // Erase
  // ========================================================================
  void Erase(const Key& key, HashedKey hk) {
    FindSlot(
        hk,
        [&](Slot* s) -> bool {
          SlotMeta old_meta = s->meta.FetchAddAcquire(1);

          if (old_meta.IsVisible()) {
            SlotMeta meta_after = old_meta;
            meta_after.SetAcquireCounter(old_meta.GetAcquireCounter() + 1);

            if (s->hashed_key.h0 == hk.h0 && s->hashed_key.h1 == hk.h1 && s->GetKey() == key) {
              for (;;) {
                uint32_t refcount = meta_after.GetRefcount();
                assert(refcount > 0);
                if (refcount > 1) {
                  s->meta.FetchClearVisible();
                  Unref(s->meta);
                  break;
                }
                SlotMeta cm;
                cm.raw = 0;
                cm.SetUnderConstruction();
                if (s->meta.CasWeak(meta_after, cm)) {
                  FreeSlot(*s);
                  MarkEmpty(*s);
                  ReclaimEntry();
                  Rollback(hk, s);
                  break;
                }
              }
            } else {
              Unref(s->meta);
            }
          } else if (UNLIKELY(old_meta.IsInvisible())) {
            Unref(s->meta);
          }
          return false;
        },
        [&](Slot* s) -> bool { return s->LoadDisplacements() == 0; }, [&](Slot*, bool) {});
  }

  // ========================================================================
  // EraseUnRefEntries
  // ========================================================================
  void EraseUnRefEntries() {
    for (size_t i = 0; i <= length_bits_mask_; i++) {
      Slot& s = slots_[i];
      SlotMeta m = s.meta.LoadRelaxed();
      if (m.IsShareable() && m.GetRefcount() == 0) {
        SlotMeta cm;
        cm.raw = 0;
        cm.SetUnderConstruction();
        if (s.meta.CasStrong(m, cm)) {
          Rollback(s.hashed_key, &s);
          FreeSlot(s);
          MarkEmpty(s);
          ReclaimEntry();
        }
      }
    }
  }

 private:
  // ========================================================================
  // ClockUpdate
  // ========================================================================
  static bool ClockUpdate(Slot& s, EvictionData* data, uint32_t now) {
    SlotMeta m = s.meta.LoadRelaxed();

    if (!m.IsShareable()) {
      return false;
    }

    uint32_t acq = m.GetAcquireCounter();
    uint32_t rel = m.GetReleaseCounter();

    if (acq != rel) {
      data->seen_pinned_count++;
      return false;
    }

    if (s.CheckExpiredAt(now)) {
      SlotMeta cm;
      cm.raw = 0;
      cm.SetUnderConstruction();
      if (s.meta.CasStrong(m, cm)) {
        data->freed_count += 1;
        return true;
      }
      return false;
    }

    if (m.IsVisible() && acq > 0) {
      uint32_t new_count = std::min(acq - 1, uint32_t{SlotMeta::kMaxCountdown} - 1);
      SlotMeta nm = m;
      nm.SetAcquireCounter(new_count);
      nm.SetReleaseCounter(new_count);
      s.meta.CasStrongRelaxed(m, nm);
      return false;
    }

    SlotMeta cm;
    cm.raw = 0;
    cm.SetUnderConstruction();

    if (s.meta.CasStrong(m, cm)) {
      data->freed_count += 1;
      return true;
    }
    return false;
  }

  template <typename MatchFn, typename AbortFn, typename UpdateFn>
  Slot* FindSlot(const HashedKey& hk, const MatchFn& match_fn, const AbortFn& abort_fn, const UpdateFn& update_fn) {
    size_t base = static_cast<size_t>(hk.h1);
    size_t increment = static_cast<size_t>(hk.h0) | 1U;

    size_t first = ModTableSize(base);
    size_t current = first;
    bool is_last;

    do {
      Slot* s = &slots_[current];
      if (match_fn(s)) {
        return s;
      }
      if (abort_fn(s)) {
        return nullptr;
      }

      current = ModTableSize(current + increment);
      is_last = (current == first);
      update_fn(s, is_last);
    } while (!is_last);

    return nullptr;
  }

  template <typename V>
  bool TryInsert(const Key& key, V&& value, HashedKey hk, Slot& s, bool* already_matches, uint32_t expire_at) {
    SlotMeta old_meta = s.meta.FetchSetOccupied();

    if (old_meta.IsEmpty()) {
      s.ConstructKey(key);
      s.ConstructValue(std::forward<V>(value));
      s.hashed_key = hk;
      s.SetExpireAt(expire_at);

      SlotMeta nm;
      nm.raw = 0;
      nm.SetVisible();
      nm.SetAcquireCounter(1);
      nm.SetReleaseCounter(1);
      s.meta.Store(nm);
      return true;
    }

    if (!old_meta.IsVisible()) {
      return false;
    }

    SlotMeta before_add = s.meta.FetchAddAcquire(1);
    if (before_add.IsVisible()) {
      if (s.hashed_key.h0 == hk.h0 && s.hashed_key.h1 == hk.h1 && s.GetKey() == key) {
        SlotMeta before_rel = s.meta.FetchAddRelease(1);
        CorrectNearOverflow(before_rel, s.meta);
        *already_matches = true;
        return false;
      }
      Unref(s.meta, 1);
    } else if (UNLIKELY(before_add.IsInvisible())) {
      Unref(s.meta, 1);
    }
    return false;
  }

  void Rollback(HashedKey hk, const Slot* target) {
    size_t current = ModTableSize(hk.h1);
    size_t increment = static_cast<size_t>(hk.h0) | 1U;
    while (&slots_[current] != target) {
      slots_[current].SubDisplacement();
      current = ModTableSize(current + increment);
    }
  }

  void FreeSlot(Slot& s) {
    s.DestroyValue();
    s.DestroyKey();
  }

  void MarkEmpty(Slot& s) { s.meta.Store(SlotMeta{}); }

  void ReclaimEntry() { size_.fetch_sub(1, std::memory_order_relaxed); }

  bool IsEvictionEffortExceeded(const EvictionData& data) const {
    return (data.freed_count + 1U) * uint64_t{static_cast<uint32_t>(eviction_effort_cap_)} <= data.seen_pinned_count;
  }

  size_t ModTableSize(uint64_t x) const { return static_cast<size_t>(x) & length_bits_mask_; }

  static int CalcHashBits(size_t capacity) {
    double target_slots = static_cast<double>(capacity) / kLoadFactor;
    int hash_bits = 0;
    while ((size_t{1} << hash_bits) < target_slots && hash_bits < 32) {
      ++hash_bits;
    }
    return std::max(hash_bits, 4);
  }

  const int length_bits_;
  const size_t length_bits_mask_;
  const int eviction_effort_cap_;
  size_t capacity_;

  std::unique_ptr<Slot[]> slots_;

  alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> clock_pointer_;
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> size_;
};

}  // namespace swiftclockcache