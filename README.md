# SwiftClockCache

A general-purpose high-performance concurrent clock cache inspired by the HyperClockCache implementation in RocksDB.

## Features

- **High concurrency** — Lock-free design based on atomic operations; supports concurrent reads and writes across multiple threads.
- **Clock eviction** — Uses a clock (second-chance) algorithm for cache eviction, balancing hit rate and performance.
- **Sharded architecture** — Distributes data across multiple shards to reduce contention.
- **TTL support** — Optional per-entry time-to-live (TTL); expired entries are automatically evicted.
- **Batch operations** — Provides `BatchInsert`, `BatchLookup`, and `BatchErase` with shard-grouped execution for better cache locality.
- **Header-only** — Only depends on the C++ standard library and the bundled xxHash; simply include the header to use.

## Quick Start

```cpp
#include "include/swift_clock_cache.h"

using namespace swiftclockcache;

int main() {
    // Create a cache with a maximum of 1024 entries
    SwiftClockCache<std::string, int> cache(1024);

    // Insert
    cache.Insert("hello", 42);

    // Lookup — returns a Handle (RAII); automatically releases the reference on destruction
    auto handle = cache.Lookup("hello");
    if (handle) {
        int value = *handle;            // dereference to get the value
        // const auto& key = handle.GetKey();  // retrieve the key
    }

    // Erase
    cache.Erase("hello");

    // Clear all unreferenced entries
    cache.Clear();

    return 0;
}
```

## API Reference

### `SwiftClockCache<Key, Value>`

#### Construction

```cpp
// Simple: specify only the max number of entries
SwiftClockCache<Key, Value> cache(max_size);

// Advanced: use an Options struct
SwiftClockCache<Key, Value>::Options opts;
opts.max_size           = 10000;  // Maximum number of cached entries
opts.num_shards         = 32;     // Number of shards (will be rounded up to a power of 2)
opts.eviction_effort_cap = 10;    // Max eviction effort ratio (higher = more aggressive eviction)

SwiftClockCache<Key, Value> cache(opts);
```

#### `Insert(key, value)` → `ErrorCode`

Inserts a key-value pair into the cache. If the cache is full, the clock algorithm attempts to evict entries first.

```cpp
ErrorCode ec = cache.Insert("key", value);
// ec == ErrorCode::kOk              — success
// ec == ErrorCode::kCapacityExceeded — cache is full and eviction failed
// ec == ErrorCode::kTableFull        — internal hash table is full
```

#### `Lookup(key)` → `Handle<Key, Value>`

Looks up a key and returns an RAII handle. The entry is pinned (reference-counted) while the handle is alive.

```cpp
auto handle = cache.Lookup("key");
if (handle) {
    Value& v = *handle;         // operator*
    Value* p = handle.Get();    // or Get()
    handle->SomeMemberFunc();   // operator->
}
// Reference is automatically released when handle goes out of scope
// Or call handle.Reset() to release early
```

#### `Erase(key)`

Removes an entry from the cache. If the entry is currently referenced, it is marked invisible and will be freed when the last reference is released.

```cpp
cache.Erase("key");
```

#### `Clear()`

Removes all entries that are not currently referenced.

```cpp
cache.Clear();
```

#### `BatchInsert(keys, values)` → `std::vector<ErrorCode>`

Inserts multiple key-value pairs in a single call. Keys and values are provided as two separate vectors of equal length. Keys are grouped by shard internally for better CPU cache locality.

```cpp
std::vector<Key> keys = {"a", "b", "c"};
std::vector<Value> values = {1, 2, 3};
auto results = cache.BatchInsert(keys, values);
for (size_t i = 0; i < results.size(); i++) {
    if (results[i] == ErrorCode::kOk) { /* success */ }
}
```

#### `BatchLookup(keys)` → `std::vector<Handle<Key, Value>>`

Looks up multiple keys in a single call. Returns a vector of handles in the same order as the input keys. Each handle is independent and follows the same RAII semantics.

```cpp
std::vector<Key> keys = {"a", "b", "c", "nonexistent"};
auto handles = cache.BatchLookup(keys);
for (size_t i = 0; i < handles.size(); i++) {
    if (handles[i]) {
        Value& v = *handles[i];
    }
}
```

#### `BatchErase(keys)`

Removes multiple entries in a single call, grouped by shard for efficiency.

```cpp
std::vector<Key> keys = {"a", "b", "c"};
cache.BatchErase(keys);
```

#### `GetSize()` / `GetCapacity()` / `Empty()`

```cpp
size_t current = cache.GetSize();      // Number of entries currently in the cache
size_t cap     = cache.GetCapacity();  // Maximum capacity (sum across all shards)
bool   empty   = cache.Empty();
```

### `ErrorCode`

| Value                          | Description                                  |
| ------------------------------ | -------------------------------------------- |
| `ErrorCode::kOk`              | Operation succeeded                          |
| `ErrorCode::kCapacityExceeded` | Cache full; eviction could not free space    |
| `ErrorCode::kTableFull`       | Internal hash table is full (all slots taken)|

## TTL Support

Per-entry TTL is supported at the shard level. When calling `Insert` through the shard directly, you can specify a TTL in seconds:

```cpp
// shard.Insert(key, value, hashed_key, ttl_seconds)
// ttl_seconds = 0 means the entry never expires
```

Expired entries are lazily evicted during lookup and clock sweeps.

## Custom Key Types

Any **trivially copyable** type can be used as a key directly. For non-trivially-copyable types, specialize `KeyBytes`:

```cpp
// std::string and std::string_view are supported out of the box.

// Example: specializing for a custom key type
namespace swiftclockcache {
template <>
struct KeyBytes<MyCustomKey> {
    static BytesView GetBytes(const MyCustomKey& key) {
        return { key.data(), key.byte_size() };
    }
};
}
```

The key type must also support `operator==` for equality comparison.

## Thread Safety

- `Insert`, `Lookup`, `Erase`, `Clear`, `BatchInsert`, `BatchLookup`, and `BatchErase` are all **thread-safe** and can be called concurrently.
- `Handle` objects are **not thread-safe** — do not share a single `Handle` instance across threads. Each thread should hold its own handle from `Lookup`.
- The value pointed to by a `Handle` can be safely read concurrently, but concurrent mutation of the value itself requires external synchronization.

## Build

SwiftClockCache is truly header-only. Simply add the header files to your project's include path:

```bash
# Example with g++
g++ -std=c++17 -O2 -o my_app my_app.cc -lpthread -DNDEBUG
```
