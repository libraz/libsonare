#pragma once

/// @file lru_cache.h
/// @brief Thread-safe, capacity-bounded least-recently-used cache.

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace sonare {

/// @brief Thread-safe LRU cache with O(1) hit, insert, and eviction.
///
/// @details Backed by an `std::unordered_map` for lookup plus an
/// `std::list<Key>` that orders entries by recency (front = most recently
/// used). Each map entry stores an iterator back into the list so a cache hit
/// promotes the key to the front with a single `splice` instead of an O(n)
/// search. Because `std::unordered_map` is node-based, references to a stored
/// value stay valid until that specific entry is evicted (a rehash does not
/// move nodes), which is what makes the `const Value&` return contract safe.
///
/// Two construction strategies are offered because the existing caches in this
/// library use both, and each is the right trade-off for its value type:
///  - `get_or_build` builds the value *outside* the lock and returns it by
///    `const&`. Use it when the value is expensive to build and cheap to keep
///    referenced in place (e.g. a large `std::vector<float>` filterbank): unique
///    keys then build concurrently rather than serializing on the mutex.
///  - `get_or_build_value` builds the value *inside* the lock and returns it by
///    value. Use it when callers take ownership of the result and need it to
///    survive concurrent eviction (e.g. a struct of `std::shared_ptr`s): the
///    copy is made while the lock is held, so the returned value cannot be
///    invalidated by another thread evicting the entry.
///
/// @tparam Key   Hashable, equality-comparable cache key.
/// @tparam Value Cached value type.
/// @tparam Hash  Hash functor for @p Key (defaults to `std::hash<Key>`).
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCache {
 public:
  /// @param capacity Maximum number of entries retained before eviction.
  explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

  /// @brief Returns the value for @p key, building it via @p build on a miss.
  /// @details The build runs outside the lock; a concurrent build of the same
  /// key is reconciled by a re-check (the late insert is dropped). The returned
  /// reference is valid until the entry is evicted.
  template <typename Build>
  const Value& get_or_build(const Key& key, Build&& build) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (const Value* hit = touch_locked(key)) return *hit;
    }
    // Build outside the lock: construction can be slow and unique keys should
    // not serialize. Worst case two threads race to build the same key; the
    // re-check below keeps the first insert and discards the second.
    Value built = std::forward<Build>(build)();

    std::lock_guard<std::mutex> lock(mutex_);
    if (const Value* hit = touch_locked(key)) return *hit;
    return insert_locked(key, std::move(built));
  }

  /// @brief Like get_or_build, but builds under the lock and returns a copy.
  /// @details Use when the caller stores the result by value and must not race
  /// with eviction. The build happens while the lock is held, so unique-key
  /// builds serialize; this is intended for cheap-to-copy value types (e.g.
  /// shared-pointer handles) whose construction is comparatively quick.
  template <typename Build>
  Value get_or_build_value(const Key& key, Build&& build) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const Value* hit = touch_locked(key)) return *hit;
    return insert_locked(key, build());
  }

  /// @brief Drops all cached entries.
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    lru_.clear();
  }

 private:
  struct Entry {
    Value value;
    typename std::list<Key>::iterator lru_it;
  };

  /// @brief Promotes @p key to most-recently-used and returns its value.
  /// @return Pointer to the stored value, or nullptr if absent.
  /// @note The caller must hold @ref mutex_.
  const Value* touch_locked(const Key& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    return &it->second.value;
  }

  /// @brief Evicts the oldest entries if at capacity, then inserts @p value.
  /// @note The caller must hold @ref mutex_.
  const Value& insert_locked(const Key& key, Value&& value) {
    while (map_.size() >= capacity_ && !lru_.empty()) {
      map_.erase(lru_.back());
      lru_.pop_back();
    }
    lru_.push_front(key);
    auto [it, _] = map_.emplace(key, Entry{std::move(value), lru_.begin()});
    return it->second.value;
  }

  std::size_t capacity_;
  std::mutex mutex_;
  std::list<Key> lru_;
  std::unordered_map<Key, Entry, Hash> map_;
};

}  // namespace sonare
