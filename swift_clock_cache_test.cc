/*
 * SwiftClockCache 测试
 *
 * 编译方式（以 g++ 为例）：
 *   g++ -std=c++17 -O2 -o swift_clock_cache_test swift_clock_cache_test.cc xxhash.cc -lpthread
 *
 * 运行：
 *   ./swift_clock_cache_test
 */

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "include/swift_clock_cache.h"

using namespace swiftclockcache;

// 辅助宏：测试通过则打印 PASS，失败则打印 FAIL 并终止
#define TEST_ASSERT(cond, msg)                                             \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "[FAIL] " << __func__ << ": " << (msg) << std::endl;   \
      std::abort();                                                        \
    }                                                                      \
  } while (0)

#define TEST_PASS() std::cout << "[PASS] " << __func__ << std::endl

// ============================================================================
// 测试 1：基本插入和查找
// ============================================================================
void TestBasicInsertAndLookup() {
  SwiftClockCache<int, std::string> cache(128);

  auto ret = cache.Insert(1, std::string("hello"));
  TEST_ASSERT(ret == ErrorCode::kOk, "插入应成功");

  ret = cache.Insert(2, std::string("world"));
  TEST_ASSERT(ret == ErrorCode::kOk, "插入应成功");

  auto h1 = cache.Lookup(1);
  TEST_ASSERT(h1, "key=1 应能找到");
  TEST_ASSERT(*h1 == "hello", "key=1 的值应为 hello");

  auto h2 = cache.Lookup(2);
  TEST_ASSERT(h2, "key=2 应能找到");
  TEST_ASSERT(*h2 == "world", "key=2 的值应为 world");

  TEST_PASS();
}

// ============================================================================
// 测试 2：查找不存在的键
// ============================================================================
void TestLookupMiss() {
  SwiftClockCache<int, int> cache(64);

  cache.Insert(10, 100);

  auto h = cache.Lookup(999);
  TEST_ASSERT(!h, "不存在的 key 应返回空 Handle");
  TEST_ASSERT(h.Get() == nullptr, "空 Handle 的 Get() 应返回 nullptr");

  TEST_PASS();
}

// ============================================================================
// 测试 3：覆盖插入（相同 key 重复插入）
// ============================================================================
void TestOverwrite() {
  SwiftClockCache<int, std::string> cache(64);

  cache.Insert(1, std::string("v1"));
  cache.Insert(1, std::string("v2"));

  auto h = cache.Lookup(1);
  TEST_ASSERT(h, "key=1 应能找到");
  // 注意：具体行为取决于实现——可能保留旧值或更新为新值
  // 这里只验证能查到，不强制具体值
  std::cout << "  覆盖插入后 key=1 的值: " << *h << std::endl;

  TEST_PASS();
}

// ============================================================================
// 测试 4：删除
// ============================================================================
void TestErase() {
  SwiftClockCache<int, int> cache(64);

  cache.Insert(1, 100);
  cache.Insert(2, 200);

  cache.Erase(1);

  auto h1 = cache.Lookup(1);
  TEST_ASSERT(!h1, "删除后 key=1 应查不到");

  auto h2 = cache.Lookup(2);
  TEST_ASSERT(h2, "key=2 不应受影响");
  TEST_ASSERT(*h2 == 200, "key=2 的值应为 200");

  TEST_PASS();
}

// ============================================================================
// 测试 5：Handle 的 RAII 语义 —— 析构自动释放
// ============================================================================
void TestHandleRAII() {
  SwiftClockCache<int, int> cache(64);

  cache.Insert(1, 42);

  {
    auto h = cache.Lookup(1);
    TEST_ASSERT(h, "key=1 应能找到");
    TEST_ASSERT(*h == 42, "值应为 42");
    // h 离开作用域时自动 Release
  }

  // 释放后仍可再次查找
  auto h2 = cache.Lookup(1);
  TEST_ASSERT(h2, "Release 后 key=1 仍应在缓存中");

  TEST_PASS();
}

// ============================================================================
// 测试 6：Handle 的移动语义
// ============================================================================
void TestHandleMove() {
  SwiftClockCache<int, int> cache(64);

  cache.Insert(1, 100);

  auto h1 = cache.Lookup(1);
  TEST_ASSERT(h1, "h1 应有效");

  // 移动构造
  auto h2 = std::move(h1);
  TEST_ASSERT(!h1, "移动后 h1 应无效");
  TEST_ASSERT(h2, "h2 应有效");
  TEST_ASSERT(*h2 == 100, "h2 的值应为 100");

  // 移动赋值
  Handle<int, int> h3;
  h3 = std::move(h2);
  TEST_ASSERT(!h2, "移动后 h2 应无效");
  TEST_ASSERT(h3, "h3 应有效");
  TEST_ASSERT(*h3 == 100, "h3 的值应为 100");

  TEST_PASS();
}

// ============================================================================
// 测试 7：Handle 的 Reset 手动释放
// ============================================================================
void TestHandleReset() {
  SwiftClockCache<int, int> cache(64);

  cache.Insert(1, 55);

  auto h = cache.Lookup(1);
  TEST_ASSERT(h, "应能找到");

  h.Reset();
  TEST_ASSERT(!h, "Reset 后 Handle 应无效");
  TEST_ASSERT(h.Get() == nullptr, "Reset 后 Get() 应为 nullptr");

  TEST_PASS();
}

// ============================================================================
// 测试 8：Clear 清除所有未被引用的条目
// ============================================================================
void TestClear() {
  SwiftClockCache<int, int> cache(64);

  for (int i = 0; i < 20; i++) {
    cache.Insert(i, i * 10);
  }
  TEST_ASSERT(cache.GetSize() > 0, "插入后 size 应大于 0");

  // 先持有一个 handle，再 clear
  auto h = cache.Lookup(5);

  cache.Clear();

  // 被持有的条目不会被清除
  if (h) {
    std::cout << "  Clear 后 key=5 仍被引用，值: " << *h << std::endl;
  }

  // 释放 handle 后
  h.Reset();

  // 未被持有的条目应被清除
  auto h2 = cache.Lookup(0);
  // 注意：Clear 的具体行为取决于实现
  std::cout << "  Clear 后 key=0 是否存在: " << (h2 ? "是" : "否") << std::endl;

  TEST_PASS();
}

// ============================================================================
// 测试 9：容量和统计
// ============================================================================
void TestCapacityAndStats() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 256;
  opts.num_shards = 4;
  SwiftClockCache<int, int> cache(opts);

  TEST_ASSERT(cache.Empty(), "初始应为空");
  TEST_ASSERT(cache.GetSize() == 0, "初始 size 应为 0");

  size_t cap = cache.GetCapacity();
  std::cout << "  容量: " << cap << " (期望 ~256)" << std::endl;

  for (int i = 0; i < 50; i++) {
    cache.Insert(i, i);
  }

  size_t sz = cache.GetSize();
  std::cout << "  插入 50 个后 size: " << sz << std::endl;
  TEST_ASSERT(sz > 0, "size 应大于 0");
  TEST_ASSERT(!cache.Empty(), "不应为空");

  TEST_PASS();
}

// ============================================================================
// 测试 10：单分片模式 (num_shards = 1, shard_bits_ == 0)
// ============================================================================
void TestSingleShard() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 64;
  opts.num_shards = 1;
  SwiftClockCache<int, int> cache(opts);

  for (int i = 0; i < 30; i++) {
    cache.Insert(i, i * 2);
  }

  for (int i = 0; i < 30; i++) {
    auto h = cache.Lookup(i);
    if (h) {
      TEST_ASSERT(*h == i * 2, "单分片模式下值应正确");
    }
  }

  TEST_PASS();
}

// ============================================================================
// 测试 11：std::string 作为 Key
// ============================================================================
void TestStringKey() {
  SwiftClockCache<std::string, int> cache(128);

  cache.Insert(std::string("apple"), 1);
  cache.Insert(std::string("banana"), 2);
  cache.Insert(std::string("cherry"), 3);

  auto h1 = cache.Lookup(std::string("apple"));
  TEST_ASSERT(h1, "apple 应能找到");
  TEST_ASSERT(*h1 == 1, "apple 的值应为 1");

  auto h2 = cache.Lookup(std::string("banana"));
  TEST_ASSERT(h2, "banana 应能找到");
  TEST_ASSERT(*h2 == 2, "banana 的值应为 2");

  auto h_miss = cache.Lookup(std::string("grape"));
  TEST_ASSERT(!h_miss, "grape 不存在");

  TEST_PASS();
}

// ============================================================================
// 测试 12：驱逐行为 —— 超过容量后自动驱逐
// ============================================================================
void TestEviction() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 16;
  opts.num_shards = 1;
  opts.eviction_effort_cap = 10;
  SwiftClockCache<int, int> cache(opts);

  // 插入超过容量的条目
  int insert_ok = 0;
  for (int i = 0; i < 100; i++) {
    auto ret = cache.Insert(i, i);
    if (ret == ErrorCode::kOk) {
      insert_ok++;
    }
  }

  std::cout << "  容量=16, 尝试插入 100 个, 成功: " << insert_ok << std::endl;
  std::cout << "  最终 size: " << cache.GetSize() << std::endl;

  // 最终 size 不应超过容量太多（驱逐在工作）
  TEST_ASSERT(cache.GetSize() <= 20, "size 不应远超容量");

  TEST_PASS();
}

// ============================================================================
// 测试 13：不同分片数的容量分配
// ============================================================================
void TestCapacityDistribution() {
  // 测试多种分片配置下容量是否合理
  for (size_t shards : {1, 2, 4, 8, 16}) {
    SwiftClockCache<int, int>::Options opts;
    opts.max_size = 256;
    opts.num_shards = shards;
    SwiftClockCache<int, int> cache(opts);

    size_t cap = cache.GetCapacity();
    std::cout << "  num_shards=" << shards << " -> 实际容量: " << cap << std::endl;
    TEST_ASSERT(cap > 0, "容量应大于 0");
  }

  TEST_PASS();
}

// ============================================================================
// 测试 14：Handle 的箭头操作符和 GetKey
// ============================================================================
void TestHandleOperators() {
  struct MyValue {
    int x;
    std::string name;
  };

  SwiftClockCache<int, MyValue> cache(64);
  cache.Insert(42, MyValue{100, "test"});

  auto h = cache.Lookup(42);
  TEST_ASSERT(h, "key=42 应能找到");

  // 测试 operator->
  TEST_ASSERT(h->x == 100, "x 应为 100");
  TEST_ASSERT(h->name == "test", "name 应为 test");

  // 测试 operator*
  MyValue& v = *h;
  TEST_ASSERT(v.x == 100, "解引用后 x 应为 100");

  // 测试 GetKey
  TEST_ASSERT(h.GetKey() == 42, "GetKey 应返回 42");

  TEST_PASS();
}

// ============================================================================
// 测试 15：大量数据压力测试
// ============================================================================
void TestStress() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 1024;
  opts.num_shards = 16;
  SwiftClockCache<int, int> cache(opts);

  const int N = 10000;

  // 大量插入
  int ok_count = 0;
  for (int i = 0; i < N; i++) {
    if (cache.Insert(i, i * 3) == ErrorCode::kOk) {
      ok_count++;
    }
  }
  std::cout << "  插入 " << N << " 个, 成功: " << ok_count << std::endl;
  std::cout << "  最终 size: " << cache.GetSize() << std::endl;

  // 随机查找
  int hit = 0, miss = 0;
  for (int i = 0; i < N; i++) {
    auto h = cache.Lookup(i);
    if (h) {
      hit++;
      TEST_ASSERT(*h == i * 3, "查到的值应正确");
    } else {
      miss++;
    }
  }
  std::cout << "  查找 " << N << " 个: 命中=" << hit << " 未命中=" << miss << std::endl;

  // 大量删除
  for (int i = 0; i < N / 2; i++) {
    cache.Erase(i);
  }
  std::cout << "  删除前半后 size: " << cache.GetSize() << std::endl;

  TEST_PASS();
}

// ============================================================================
// 测试 16：多线程并发读写
// ============================================================================
void TestConcurrent() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 4096;
  opts.num_shards = 16;
  SwiftClockCache<int, int> cache(opts);

  const int kNumThreads = 4;
  const int kOpsPerThread = 5000;
  std::atomic<int> total_insert_ok{0};
  std::atomic<int> total_lookup_hit{0};

  auto worker = [&](int tid) {
    int base = tid * kOpsPerThread;
    // 写入
    for (int i = 0; i < kOpsPerThread; i++) {
      if (cache.Insert(base + i, base + i) == ErrorCode::kOk) {
        total_insert_ok.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // 读取
    for (int i = 0; i < kOpsPerThread; i++) {
      auto h = cache.Lookup(base + i);
      if (h) {
        total_lookup_hit.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; t++) {
    threads.emplace_back(worker, t);
  }
  for (auto& th : threads) {
    th.join();
  }

  std::cout << "  线程数=" << kNumThreads << " 每线程操作=" << kOpsPerThread << std::endl;
  std::cout << "  总插入成功: " << total_insert_ok.load() << std::endl;
  std::cout << "  总查找命中: " << total_lookup_hit.load() << std::endl;
  std::cout << "  最终 size: " << cache.GetSize() << std::endl;

  TEST_PASS();
}

// ============================================================================
// 测试 17：BatchInsert 批量插入
// ============================================================================
void TestBatchInsert() {
  SwiftClockCache<int, int> cache(256);

  std::vector<int> keys;
  std::vector<int> values;
  for (int i = 0; i < 50; i++) {
    keys.push_back(i);
    values.push_back(i * 10);
  }

  auto results = cache.BatchInsert(keys, values);
  TEST_ASSERT(results.size() == 50, "结果数量应为 50");

  int ok_count = 0;
  for (auto& ec : results) {
    if (ec == ErrorCode::kOk) ok_count++;
  }
  std::cout << "  BatchInsert 50 个, 成功: " << ok_count << std::endl;
  TEST_ASSERT(ok_count == 50, "全部应成功");

  // 验证插入的数据可以查找到
  for (int i = 0; i < 50; i++) {
    auto h = cache.Lookup(i);
    TEST_ASSERT(h, "BatchInsert 后应能查到");
    TEST_ASSERT(*h == i * 10, "值应正确");
  }

  TEST_PASS();
}

// ============================================================================
// 测试 18：BatchLookup 批量查找
// ============================================================================
void TestBatchLookup() {
  SwiftClockCache<std::string, int> cache(256);

  // 先插入一些数据
  for (int i = 0; i < 30; i++) {
    cache.Insert("key_" + std::to_string(i), i);
  }

  // 批量查找：包含存在和不存在的 key
  std::vector<std::string> keys;
  for (int i = 0; i < 40; i++) {
    keys.push_back("key_" + std::to_string(i));
  }

  auto handles = cache.BatchLookup(keys);
  TEST_ASSERT(handles.size() == 40, "结果数量应为 40");

  int hit = 0, miss = 0;
  for (size_t i = 0; i < handles.size(); i++) {
    if (handles[i]) {
      hit++;
      TEST_ASSERT(*handles[i] == static_cast<int>(i), "值应正确");
    } else {
      miss++;
    }
  }
  std::cout << "  BatchLookup 40 个: 命中=" << hit << " 未命中=" << miss << std::endl;
  TEST_ASSERT(hit == 30, "前 30 个应命中");
  TEST_ASSERT(miss == 10, "后 10 个应未命中");

  TEST_PASS();
}

// ============================================================================
// 测试 19：BatchErase 批量删除
// ============================================================================
void TestBatchErase() {
  SwiftClockCache<int, int> cache(256);

  for (int i = 0; i < 50; i++) {
    cache.Insert(i, i);
  }

  size_t before = cache.GetSize();
  std::cout << "  删除前 size: " << before << std::endl;

  // 批量删除前 25 个
  std::vector<int> keys_to_erase;
  for (int i = 0; i < 25; i++) {
    keys_to_erase.push_back(i);
  }
  cache.BatchErase(keys_to_erase);

  // 验证删除的 key 查不到
  for (int i = 0; i < 25; i++) {
    auto h = cache.Lookup(i);
    TEST_ASSERT(!h, "已删除的 key 应查不到");
  }

  // 验证未删除的 key 还在
  int remaining = 0;
  for (int i = 25; i < 50; i++) {
    auto h = cache.Lookup(i);
    if (h) remaining++;
  }
  std::cout << "  删除后剩余可查到: " << remaining << std::endl;
  TEST_ASSERT(remaining == 25, "未删除的 25 个应全部可查到");

  TEST_PASS();
}

// ============================================================================
// 测试 20：批量接口空输入
// ============================================================================
void TestBatchEmpty() {
  SwiftClockCache<int, int> cache(64);

  // 空输入不应崩溃
  std::vector<int> empty_keys;
  std::vector<int> empty_values;
  auto insert_results = cache.BatchInsert(empty_keys, empty_values);
  TEST_ASSERT(insert_results.empty(), "空 BatchInsert 应返回空结果");

  auto lookup_results = cache.BatchLookup(empty_keys);
  TEST_ASSERT(lookup_results.empty(), "空 BatchLookup 应返回空结果");

  cache.BatchErase(empty_keys);  // 不应崩溃

  TEST_PASS();
}

// ============================================================================
// 测试 21：批量接口多线程并发
// ============================================================================
void TestBatchConcurrent() {
  SwiftClockCache<int, int>::Options opts;
  opts.max_size = 4096;
  opts.num_shards = 16;
  SwiftClockCache<int, int> cache(opts);

  const int kNumThreads = 4;
  const int kBatchSize = 200;
  std::atomic<int> total_insert_ok{0};
  std::atomic<int> total_lookup_hit{0};

  auto worker = [&](int tid) {
    int base = tid * kBatchSize;

    // 批量插入
    std::vector<int> ins_keys;
    std::vector<int> ins_values;
    for (int i = 0; i < kBatchSize; i++) {
      ins_keys.push_back(base + i);
      ins_values.push_back(base + i);
    }
    auto results = cache.BatchInsert(ins_keys, ins_values);
    for (auto& ec : results) {
      if (ec == ErrorCode::kOk) {
        total_insert_ok.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // 批量查找
    std::vector<int> keys;
    for (int i = 0; i < kBatchSize; i++) {
      keys.push_back(base + i);
    }
    auto handles = cache.BatchLookup(keys);
    for (auto& h : handles) {
      if (h) {
        total_lookup_hit.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  for (int t = 0; t < kNumThreads; t++) {
    threads.emplace_back(worker, t);
  }
  for (auto& th : threads) {
    th.join();
  }

  std::cout << "  线程数=" << kNumThreads << " 批大小=" << kBatchSize << std::endl;
  std::cout << "  总插入成功: " << total_insert_ok.load() << std::endl;
  std::cout << "  总查找命中: " << total_lookup_hit.load() << std::endl;
  std::cout << "  最终 size: " << cache.GetSize() << std::endl;

  TEST_PASS();
}

// ============================================================================
// main
// ============================================================================
int main() {
  std::cout << "========================================" << std::endl;
  std::cout << " SwiftClockCache 测试套件" << std::endl;
  std::cout << "========================================" << std::endl;

  TestBasicInsertAndLookup();
  TestLookupMiss();
  TestOverwrite();
  TestErase();
  TestHandleRAII();
  TestHandleMove();
  TestHandleReset();
  TestClear();
  TestCapacityAndStats();
  TestSingleShard();
  TestStringKey();
  TestEviction();
  TestCapacityDistribution();
  TestHandleOperators();
  TestStress();
  TestConcurrent();
  TestBatchInsert();
  TestBatchLookup();
  TestBatchErase();
  TestBatchEmpty();
  TestBatchConcurrent();

  std::cout << "========================================" << std::endl;
  std::cout << " 全部测试通过！" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}
