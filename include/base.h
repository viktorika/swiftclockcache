/*
 * @Author: victorika
 * @Date: 2026-03-16 15:32:52
 * @Last Modified by: victorika
 * @Last Modified time: 2026-03-23 15:56:07
 */
#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#ifdef __linux__
#  include <time.h>
#endif

namespace swiftclockcache {

#if defined(__GNUC__) || defined(__clang__)
#  define LIKELY(x) __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define LIKELY(x) (x)
#  define UNLIKELY(x) (x)
#endif

#ifdef __cpp_lib_hardware_interference_size
#  define CACHE_LINE_SIZE std::hardware_destructive_interference_size
#else
#  define CACHE_LINE_SIZE 64
#endif

enum class ErrorCode : uint8_t {
  kOk,
  kCapacityExceeded,
  kTableFull,
  kExpired,
};

inline uint32_t NowSeconds() {
#ifdef __linux__
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<uint32_t>(ts.tv_sec);
#else
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

inline uint32_t CalcExpireTime(uint32_t now, uint32_t ttl_seconds) {
  if (ttl_seconds == 0) {
    return 0;
  }
  return now + ttl_seconds;
}

inline bool IsExpiredAt(uint32_t expire_at, uint32_t now) { return expire_at != 0 && now >= expire_at; }

inline int FloorLog2(size_t v) {
  assert(v > 0);
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanReverse64(&index, static_cast<unsigned long long>(v));
  return static_cast<int>(index);
#else
  return 63 - __builtin_clzll(static_cast<unsigned long long>(v));
#endif
}

inline size_t RoundUpToPowerOf2(size_t n) {
  if (n <= 1) {
    return 1;
  }
  if ((n & (n - 1)) == 0) {
    return n;
  }
  return size_t{1} << (FloorLog2(n - 1) + 1);
}

inline uint32_t Upper32of64(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

}  // namespace swiftclockcache