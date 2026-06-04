#pragma once

/// @file thread_local_cache.h
/// @brief Small helper for bounded thread-local map caches.

#include <cstddef>

namespace sonare {

template <typename Map, typename Key, typename Factory>
const typename Map::mapped_type& get_or_create_bounded_cache_entry(Map& cache, const Key& key,
                                                                   size_t max_size,
                                                                   Factory&& factory) {
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }

  if (cache.size() >= max_size) {
    cache.clear();
  }

  auto result = cache.emplace(key, factory());
  return result.first->second;
}

}  // namespace sonare
