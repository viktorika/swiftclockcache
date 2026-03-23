/*
 * SwiftClockCache vs LRU Cache 性能与命中率对比 Benchmark
 *
 * 编译方式（以 g++ 为例）：
 *   g++ -std=c++17 -O2 -o benchmark benchmark.cc xxhash.cc -lpthread
 *
 * 运行：
 *   ./benchmark
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "swift_clock_cache.h"
#include "xxhash.h"

using namespace swiftclockcache;

// ============================================================================
// 单分片 LRU Cache（内部使用，带 mutex）
// ============================================================================
template <typename Key, typename Value>
class LRUCacheShard {
 public:
  explicit LRUCacheShard(size_t capacity) : capacity_(capacity) {}

  bool Insert(const Key& key, const Value& value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      list_.erase(it->second);
      list_.push_front({key, value});
      it->second = list_.begin();
      return true;
    }
    if (map_.size() >= capacity_) {
      auto& back = list_.back();
      map_.erase(back.first);
      list_.pop_back();
    }
    list_.push_front({key, value});
    map_[key] = list_.begin();
    return true;
  }

  bool Lookup(const Key& key, Value* out_value = nullptr) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) {
      return false;
    }
    list_.splice(list_.begin(), list_, it->second);
    if (out_value) {
      *out_value = it->second->second;
    }
    return true;
  }

  void Erase(const Key& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      list_.erase(it->second);
      map_.erase(it);
    }
  }

  size_t GetSize() {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
  }

 private:
  using ListType = std::list<std::pair<Key, Value>>;
  size_t capacity_;
  ListType list_;
  std::unordered_map<Key, typename ListType::iterator> map_;
  std::mutex mu_;
};

// ============================================================================
// 分片 LRU Cache（与 SwiftClockCache 使用相同分片策略做公平对比）
// ============================================================================
template <typename Key, typename Value>
class ShardedLRUCache {
 public:
  explicit ShardedLRUCache(size_t capacity, size_t num_shards = 16) {
    num_shards_ = RoundUpToPowerOf2(num_shards);
    shard_bits_ = FloorLog2(num_shards_);
    size_t cap_per_shard = std::max(size_t{1}, capacity / num_shards_);
    shards_.reserve(num_shards_);
    for (size_t i = 0; i < num_shards_; i++) {
      shards_.emplace_back(std::make_unique<LRUCacheShard<Key, Value>>(cap_per_shard));
    }
  }

  bool Insert(const Key& key, const Value& value) {
    return GetShard(key).Insert(key, value);
  }

  bool Lookup(const Key& key, Value* out_value = nullptr) {
    return GetShard(key).Lookup(key, out_value);
  }

  void Erase(const Key& key) {
    GetShard(key).Erase(key);
  }

  size_t GetSize() {
    size_t total = 0;
    for (auto& s : shards_) total += s->GetSize();
    return total;
  }

 private:
  LRUCacheShard<Key, Value>& GetShard(const Key& key) {
    // 使用 XXH3 哈希（与 SwiftClockCache 相同），保证分片均匀
    auto h128 = XXH3_128bits(&key, sizeof(Key));
    uint64_t h = h128.low64;
    if (shard_bits_ == 0) return *shards_[0];
    uint32_t upper = static_cast<uint32_t>(h >> 32);
    size_t idx = upper >> (32 - shard_bits_);
    return *shards_[idx];
  }

  size_t num_shards_;
  int shard_bits_;
  std::vector<std::unique_ptr<LRUCacheShard<Key, Value>>> shards_;
};

// ============================================================================
// Zipf 分布生成器（用于模拟真实访问模式）
// ============================================================================
class ZipfGenerator {
 public:
  ZipfGenerator(size_t n, double alpha, uint64_t seed = 42) : n_(n), alpha_(alpha), rng_(seed) {
    // 预计算 CDF
    double c = 0.0;
    for (size_t i = 1; i <= n; i++) {
      c += 1.0 / std::pow(static_cast<double>(i), alpha);
    }
    c = 1.0 / c;

    cdf_.resize(n + 1, 0.0);
    for (size_t i = 1; i <= n; i++) {
      cdf_[i] = cdf_[i - 1] + c / std::pow(static_cast<double>(i), alpha);
    }
  }

  size_t Next() {
    double u = dist_(rng_);
    // 二分查找
    auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
    size_t idx = std::distance(cdf_.begin(), it);
    if (idx == 0) idx = 1;
    if (idx > n_) idx = n_;
    return idx - 1;  // 返回 [0, n-1]
  }

 private:
  size_t n_;
  double alpha_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> dist_{0.0, 1.0};
  std::vector<double> cdf_;
};

// ============================================================================
// 计时辅助
// ============================================================================
class Timer {
 public:
  void Start() { start_ = std::chrono::high_resolution_clock::now(); }
  void Stop() { end_ = std::chrono::high_resolution_clock::now(); }
  double ElapsedMs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(end_ - start_).count() / 1000.0;
  }
  double ElapsedUs() const {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end_ - start_).count());
  }

 private:
  std::chrono::high_resolution_clock::time_point start_, end_;
};

// ============================================================================
// 格式化输出辅助
// ============================================================================
void PrintSeparator() { std::cout << std::string(80, '-') << std::endl; }

void PrintHeader(const std::string& title) {
  std::cout << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << " " << title << std::endl;
  std::cout << "========================================" << std::endl;
}

void PrintResult(const std::string& cache_name, double time_ms, size_t ops, size_t hits, size_t total) {
  double throughput = static_cast<double>(ops) / (time_ms / 1000.0);
  double hit_rate = total > 0 ? 100.0 * hits / total : 0.0;
  double latency_ns = (time_ms * 1e6) / static_cast<double>(ops);

  std::cout << std::left << std::setw(20) << cache_name << " | "
            << "耗时: " << std::right << std::setw(8) << std::fixed << std::setprecision(2) << time_ms << " ms | "
            << "吞吐: " << std::setw(10) << std::setprecision(0) << throughput << " ops/s | "
            << "延迟: " << std::setw(6) << std::setprecision(1) << latency_ns << " ns/op | "
            << "命中率: " << std::setw(6) << std::setprecision(2) << hit_rate << "%" << std::endl;
}

// ============================================================================
// Benchmark 1：均匀随机访问模式
// ============================================================================
void BenchUniformRandom() {
  PrintHeader("Benchmark 1: 均匀随机访问模式");

  const size_t kCacheSize = 10000;
  const size_t kKeyRange = 100000;  // key 范围远大于 cache 容量 → 命中率约 10%
  const size_t kOps = 500000;

  std::cout << "  缓存容量=" << kCacheSize << " key范围=[0," << kKeyRange << ") 操作数=" << kOps << std::endl;
  PrintSeparator();

  // — SwiftClockCache —
  {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = kCacheSize;
    opts.num_shards = 16;
    SwiftClockCache<int, int> cache(opts);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> dist(0, kKeyRange - 1);

    // 预热
    for (size_t i = 0; i < kCacheSize; i++) {
      cache.Insert(dist(rng), 0);
    }

    rng.seed(99999);
    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = dist(rng);
      auto h = cache.Lookup(key);
      if (h) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, hits, kOps);
  }

  // — Sharded LRU Cache —
  {
    ShardedLRUCache<int, int> cache(kCacheSize, 16);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> dist(0, kKeyRange - 1);

    // 预热
    for (size_t i = 0; i < kCacheSize; i++) {
      cache.Insert(dist(rng), 0);
    }

    rng.seed(99999);
    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = dist(rng);
      if (cache.Lookup(key)) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, hits, kOps);
  }
}

// ============================================================================
// Benchmark 2：Zipf 分布访问模式（模拟真实热点访问）
// ============================================================================
void BenchZipf() {
  PrintHeader("Benchmark 2: Zipf 分布访问模式 (alpha=0.99)");

  const size_t kCacheSize = 10000;
  const size_t kKeyRange = 100000;
  const size_t kOps = 500000;

  std::cout << "  缓存容量=" << kCacheSize << " key范围=[0," << kKeyRange << ") 操作数=" << kOps << std::endl;
  PrintSeparator();

  // 预生成 Zipf key 序列（两个 cache 使用相同序列，保证公平对比）
  ZipfGenerator zipf(kKeyRange, 0.99, 42);
  std::vector<int> keys(kOps);
  for (size_t i = 0; i < kOps; i++) {
    keys[i] = static_cast<int>(zipf.Next());
  }

  // — SwiftClockCache —
  {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = kCacheSize;
    opts.num_shards = 16;
    SwiftClockCache<int, int> cache(opts);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = keys[i];
      auto h = cache.Lookup(key);
      if (h) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, hits, kOps);
  }

  // — Sharded LRU Cache —
  {
    ShardedLRUCache<int, int> cache(kCacheSize, 16);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = keys[i];
      if (cache.Lookup(key)) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, hits, kOps);
  }
}

// ============================================================================
// Benchmark 3：扫描抗性测试// Clock 算法在面对扫描时应该比 LRU 有更好的命中率
// ============================================================================
void BenchScanResistance() {
  PrintHeader("Benchmark 3: 扫描抗性测试 (热点 + 循环扫描)");

  const size_t kCacheSize = 5000;
  const size_t kHotKeys = 3000;    // 热点 key 范围
  const size_t kScanKeys = 20000;  // 扫描 key 范围（远大于缓存）
  const size_t kOps = 500000;

  std::cout << "  缓存容量=" << kCacheSize << " 热点key数=" << kHotKeys << " 扫描key数=" << kScanKeys
            << " 操作数=" << kOps << std::endl;
  std::cout << "  模式: 80% 热点随机访问 + 20% 顺序扫描" << std::endl;
  PrintSeparator();

  // 预生成访问序列
  std::mt19937_64 rng(777);
  std::uniform_int_distribution<int> hot_dist(0, kHotKeys - 1);
  std::uniform_real_distribution<double> ratio_dist(0.0, 1.0);
  std::vector<int> keys(kOps);
  size_t scan_cursor = 0;

  for (size_t i = 0; i < kOps; i++) {
    if (ratio_dist(rng) < 0.8) {
      // 80%: 热点随机访问
      keys[i] = hot_dist(rng);
    } else {
      // 20%: 顺序扫描（key 偏移到不同范围避免和热点重叠）
      keys[i] = static_cast<int>(kHotKeys + (scan_cursor % kScanKeys));
      scan_cursor++;
    }
  }

  // — SwiftClockCache —
  {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = kCacheSize;
    opts.num_shards = 16;
    SwiftClockCache<int, int> cache(opts);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = keys[i];
      auto h = cache.Lookup(key);
      if (h) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, hits, kOps);
  }

  // — Sharded LRU Cache —
  {
    ShardedLRUCache<int, int> cache(kCacheSize, 16);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = keys[i];
      if (cache.Lookup(key)) {
        hits++;
      } else {
        cache.Insert(key, key);
      }
    }

    timer.Stop();
    PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, hits, kOps);
  }
}

// ============================================================================
// Benchmark 4：纯读吞吐量// ============================================================================
void BenchPureReadThroughput() {
  PrintHeader("Benchmark 4: 纯读吞吐量 (100% 命中)");

  const size_t kCacheSize = 50000;
  const size_t kOps = 2000000;

  std::cout << "  缓存容量=" << kCacheSize << " 操作数=" << kOps << " (所有 key 均在缓存中)" << std::endl;
  PrintSeparator();

  // — SwiftClockCache —
  {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = kCacheSize;
    opts.num_shards = 16;
    SwiftClockCache<int, int> cache(opts);

    // 填满缓存
    for (size_t i = 0; i < kCacheSize; i++) {
      cache.Insert(static_cast<int>(i), static_cast<int>(i));
    }

    std::mt19937_64 rng(54321);
    std::uniform_int_distribution<int> dist(0, kCacheSize - 1);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = dist(rng);
      auto h = cache.Lookup(key);
      if (h) {
        hits++;
      }
    }

    timer.Stop();
    PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, hits, kOps);
  }

  // — Sharded LRU Cache —
  {
    ShardedLRUCache<int, int> cache(kCacheSize, 16);

    for (size_t i = 0; i < kCacheSize; i++) {
      cache.Insert(static_cast<int>(i), static_cast<int>(i));
    }

    std::mt19937_64 rng(54321);
    std::uniform_int_distribution<int> dist(0, kCacheSize - 1);

    size_t hits = 0;
    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      int key = dist(rng);
      if (cache.Lookup(key)) {
        hits++;
      }
    }

    timer.Stop();
    PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, hits, kOps);
  }
}

// ============================================================================
// Benchmark 5：多线程并发性能对比
// ============================================================================
void BenchConcurrent() {
  PrintHeader("Benchmark 5: 多线程并发性能对比");

  const size_t kCacheSize = 50000;
  const size_t kKeyRange = 100000;

  for (int num_threads : {1, 2, 4, 8}) {
    const size_t kOpsPerThread = 200000;

    std::cout << std::endl;
    std::cout << "  线程数=" << num_threads << " 每线程操作数=" << kOpsPerThread
              << " 缓存容量=" << kCacheSize << " key范围=[0," << kKeyRange << ")" << std::endl;
    PrintSeparator();

    // 为每个线程预生成 Zipf 访问序列
    std::vector<std::vector<int>> thread_keys(num_threads);
    for (int t = 0; t < num_threads; t++) {
      ZipfGenerator zipf(kKeyRange, 0.99, 1000 + t);
      thread_keys[t].resize(kOpsPerThread);
      for (size_t i = 0; i < kOpsPerThread; i++) {
        thread_keys[t][i] = static_cast<int>(zipf.Next());
      }
    }

    // — SwiftClockCache —
    {
      SwiftClockCache<int, int>::Options opts;
      opts.max_size = kCacheSize;
      opts.num_shards = 16;
      SwiftClockCache<int, int> cache(opts);

      // 预热
      for (size_t i = 0; i < kCacheSize / 2; i++) {
        cache.Insert(static_cast<int>(i), static_cast<int>(i));
      }

      std::atomic<size_t> total_hits{0};
      std::atomic<size_t> total_ops{0};

      Timer timer;
      timer.Start();

      std::vector<std::thread> threads;
      for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&, t]() {
          size_t local_hits = 0;
          for (size_t i = 0; i < kOpsPerThread; i++) {
            int key = thread_keys[t][i];
            auto h = cache.Lookup(key);
            if (h) {
              local_hits++;
            } else {
              cache.Insert(key, key);
            }
          }
          total_hits.fetch_add(local_hits, std::memory_order_relaxed);
          total_ops.fetch_add(kOpsPerThread, std::memory_order_relaxed);
        });
      }

      for (auto& th : threads) th.join();
      timer.Stop();

      PrintResult("SwiftClockCache", timer.ElapsedMs(), total_ops.load(), total_hits.load(), total_ops.load());
    }

    // — Sharded LRU Cache —
    {
      ShardedLRUCache<int, int> cache(kCacheSize, 16);

      // 预热
      for (size_t i = 0; i < kCacheSize / 2; i++) {
        cache.Insert(static_cast<int>(i), static_cast<int>(i));
      }

      std::atomic<size_t> total_hits{0};
      std::atomic<size_t> total_ops{0};

      Timer timer;
      timer.Start();

      std::vector<std::thread> threads;
      for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&, t]() {
          size_t local_hits = 0;
          for (size_t i = 0; i < kOpsPerThread; i++) {
            int key = thread_keys[t][i];
            if (cache.Lookup(key)) {
              local_hits++;
            } else {
              cache.Insert(key, key);
            }
          }
          total_hits.fetch_add(local_hits, std::memory_order_relaxed);
          total_ops.fetch_add(kOpsPerThread, std::memory_order_relaxed);
        });
      }

      for (auto& th : threads) th.join();
      timer.Stop();

      PrintResult("ShardedLRU(16)", timer.ElapsedMs(), total_ops.load(), total_hits.load(), total_ops.load());
    }
  }
}

// ============================================================================
// Benchmark 6：不同缓存容量下的命中率对比
// ============================================================================
void BenchHitRateVsCapacity() {
  PrintHeader("Benchmark 6: 不同缓存容量下的命中率对比 (Zipf alpha=0.99)");

  const size_t kKeyRange = 100000;
  const size_t kOps = 300000;

  // 预生成访问序列
  ZipfGenerator zipf(kKeyRange, 0.99, 2025);
  std::vector<int> keys(kOps);
  for (size_t i = 0; i < kOps; i++) {
    keys[i] = static_cast<int>(zipf.Next());
  }

  std::cout << std::left << std::setw(12) << "缓存容量" << " | " << std::setw(22) << "SwiftClockCache 命中率"
            << " | " << std::setw(22) << "ShardedLRU(16) 命中率"
            << " | " << "差异" << std::endl;
  PrintSeparator();

  for (size_t cache_size : {1000, 2000, 5000, 10000, 20000, 50000}) {
    // — SwiftClockCache —
    size_t clock_hits = 0;
    {
      SwiftClockCache<int, int>::Options opts;
      opts.max_size = cache_size;
      opts.num_shards = 16;
      SwiftClockCache<int, int> cache(opts);

      for (size_t i = 0; i < kOps; i++) {
        int key = keys[i];
        auto h = cache.Lookup(key);
        if (h) {
          clock_hits++;
        } else {
          cache.Insert(key, key);
        }
      }
    }

    // — Sharded LRU Cache —
    size_t lru_hits = 0;
    {
      ShardedLRUCache<int, int> cache(cache_size, 16);

      for (size_t i = 0; i < kOps; i++) {
        int key = keys[i];
        if (cache.Lookup(key)) {
          lru_hits++;
        } else {
          cache.Insert(key, key);
        }
      }
    }

    double clock_rate = 100.0 * clock_hits / kOps;
    double lru_rate = 100.0 * lru_hits / kOps;
    double diff = clock_rate - lru_rate;

    std::cout << std::left << std::setw(12) << cache_size << " | " << std::right << std::setw(20) << std::fixed
              << std::setprecision(2) << clock_rate << "%" << " | " << std::setw(20) << lru_rate << "%" << " | "
              << std::showpos << std::setprecision(2) << diff << "%" << std::noshowpos << std::endl;
  }
}

// ============================================================================
// Benchmark 7：纯插入吞吐量
// ============================================================================
void BenchInsertThroughput() {
  PrintHeader("Benchmark 7: 纯插入吞吐量");

  const size_t kCacheSize = 10000;
  const size_t kOps = 1000000;

  std::cout << "  缓存容量=" << kCacheSize << " 插入操作数=" << kOps << std::endl;
  PrintSeparator();

  // — SwiftClockCache —
  {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = kCacheSize;
    opts.num_shards = 16;
    SwiftClockCache<int, int> cache(opts);

    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      cache.Insert(static_cast<int>(i), static_cast<int>(i));
    }

    timer.Stop();
    PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, 0, 0);
  }

  // — Sharded LRU Cache —
  {
    ShardedLRUCache<int, int> cache(kCacheSize, 16);

    Timer timer;
    timer.Start();

    for (size_t i = 0; i < kOps; i++) {
      cache.Insert(static_cast<int>(i), static_cast<int>(i));
    }

    timer.Stop();
    PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, 0, 0);
  }
}

// ============================================================================
// Benchmark 8：混合读写比例对比 (Zipf)
// ============================================================================
void BenchMixedReadWrite() {
  PrintHeader("Benchmark 8: 不同读写比例下的性能对比 (Zipf alpha=0.99)");

  const size_t kCacheSize = 10000;
  const size_t kKeyRange = 100000;
  const size_t kOps = 500000;

  for (int read_pct : {50, 80, 95, 99}) {
    std::cout << std::endl;
    std::cout << "  读比例=" << read_pct << "% 写比例=" << (100 - read_pct) << "%" << std::endl;
    PrintSeparator();

    // 预生成 key 和操作类型序列
    ZipfGenerator zipf(kKeyRange, 0.99, 3000 + read_pct);
    std::mt19937_64 rng(4000 + read_pct);
    std::uniform_int_distribution<int> pct_dist(0, 99);

    std::vector<int> keys(kOps);
    std::vector<bool> is_read(kOps);
    for (size_t i = 0; i < kOps; i++) {
      keys[i] = static_cast<int>(zipf.Next());
      is_read[i] = (pct_dist(rng) < read_pct);
    }

    // — SwiftClockCache —
    {
      SwiftClockCache<int, int>::Options opts;
      opts.max_size = kCacheSize;
      opts.num_shards = 16;
      SwiftClockCache<int, int> cache(opts);

      // 预热
      for (size_t i = 0; i < kCacheSize / 2; i++) {
        cache.Insert(static_cast<int>(i), static_cast<int>(i));
      }

      size_t hits = 0, reads = 0;
      Timer timer;
      timer.Start();

      for (size_t i = 0; i < kOps; i++) {
        if (is_read[i]) {
          reads++;
          auto h = cache.Lookup(keys[i]);
          if (h) hits++;
        } else {
          cache.Insert(keys[i], keys[i]);
        }
      }

      timer.Stop();
      PrintResult("SwiftClockCache", timer.ElapsedMs(), kOps, hits, reads);
    }

    // — Sharded LRU Cache —
    {
      ShardedLRUCache<int, int> cache(kCacheSize, 16);

      // 预热
      for (size_t i = 0; i < kCacheSize / 2; i++) {
        cache.Insert(static_cast<int>(i), static_cast<int>(i));
      }

      size_t hits = 0, reads = 0;
      Timer timer;
      timer.Start();

      for (size_t i = 0; i < kOps; i++) {
        if (is_read[i]) {
          reads++;
          if (cache.Lookup(keys[i])) hits++;
        } else {
          cache.Insert(keys[i], keys[i]);
        }
      }

      timer.Stop();
      PrintResult("ShardedLRU(16)", timer.ElapsedMs(), kOps, hits, reads);
    }
  }
}

// ============================================================================
// main
// ============================================================================
int main() {
  std::cout << "================================================================" << std::endl;
  std::cout << " SwiftClockCache vs ShardedLRU — 性能与命中率对比 Benchmark" << std::endl;
  std::cout << "================================================================" << std::endl;

  BenchUniformRandom();
  BenchZipf();
  BenchScanResistance();
  BenchPureReadThroughput();
  BenchInsertThroughput();
  BenchMixedReadWrite();
  BenchHitRateVsCapacity();
  BenchConcurrent();

  std::cout << std::endl;
  std::cout << "================================================================" << std::endl;
  std::cout << " Benchmark 全部完成！" << std::endl;
  std::cout << "================================================================" << std::endl;

  return 0;
}
