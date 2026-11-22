#pragma once

/// @file parallel_for.h
/// @brief Chunked fork-join over an index range. Native builds only: a caller keeps its serial
///        loop under `__EMSCRIPTEN__`, where these are not declared.

#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <future>
#include <thread>
#include <vector>

namespace sonare {

/// @brief Executes fn(start, end) across n_workers threads over [0, total).
template <typename F>
void parallel_for(int total, int n_workers, F&& fn) {
  if (n_workers <= 1 || total <= 1) {
    fn(0, total);
    return;
  }
  n_workers = std::min(n_workers, total);
  std::vector<std::future<void>> futures;
  futures.reserve(n_workers);
  int chunk = (total + n_workers - 1) / n_workers;
  for (int i = 0; i < n_workers; ++i) {
    int start = i * chunk;
    int end = std::min(start + chunk, total);
    if (start >= end) break;
    // Each worker takes its own copy; forwarding here would move an rvalue into the first only.
    futures.emplace_back(std::async(std::launch::async, fn, start, end));
  }
  for (auto& f : futures) f.get();
}

/// @brief Workers the host offers, as a worker-count policy takes it.
inline int available_workers() { return static_cast<int>(std::thread::hardware_concurrency()); }

}  // namespace sonare

#endif  // __EMSCRIPTEN__
