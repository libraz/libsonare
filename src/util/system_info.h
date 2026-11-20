#pragma once

/// @file system_info.h
/// @brief Host facts the library reports to callers: core counts, memory, and
///        the parallel strategy derived from the core count.
/// @details A fact the platform cannot report is std::nullopt, never 0 or a
///          substitute reading such as the logical count standing in for the
///          physical one.

#include <cstddef>
#include <optional>
#include <string>

namespace sonare::system_info {

/// @brief Logical CPU count from std::thread::hardware_concurrency().
std::optional<int> logical_cores();
/// @brief Physical core count (sysctl on Apple, /proc/cpuinfo on Linux).
std::optional<int> physical_cores();
/// @brief Installed memory in bytes.
std::optional<std::size_t> total_memory_bytes();
/// @brief Free plus reclaimable memory in bytes.
std::optional<std::size_t> available_memory_bytes();
/// @brief Strategy name chosen from the logical core count (1 when unknown).
std::string parallel_strategy();
/// @brief Worker count chosen from the logical core count (1 when unknown).
int parallel_workers();
/// @brief True when at least two logical cores are known.
bool parallel_enabled();

}  // namespace sonare::system_info
