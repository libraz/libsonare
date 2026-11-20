#include "util/system_info.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace sonare::system_info {

namespace {

#ifdef __linux__
/// @brief Reads a "Key: <kB> kB" line of /proc/meminfo as bytes.
std::optional<std::size_t> meminfo_bytes(const char* key) {
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find(key) == 0) {
      const std::size_t kb = std::stoull(line.substr(line.find(':') + 1));
      return kb * 1024;
    }
  }
  return std::nullopt;
}
#endif

int known_logical_cores() { return logical_cores().value_or(1); }

}  // namespace

std::optional<int> logical_cores() {
  const int n = static_cast<int>(std::thread::hardware_concurrency());
  if (n > 0) return n;
  return std::nullopt;
}

std::optional<int> physical_cores() {
#ifdef __APPLE__
  int cores = 0;
  std::size_t len = sizeof(cores);
  if (sysctlbyname("hw.physicalcpu", &cores, &len, nullptr, 0) == 0 && cores > 0) {
    return cores;
  }
#elif defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::vector<int> core_ids;
  while (std::getline(f, line)) {
    if (line.find("core id") == 0) {
      const auto pos = line.find(':');
      if (pos != std::string::npos) {
        const int id = std::stoi(line.substr(pos + 1));
        if (std::find(core_ids.begin(), core_ids.end(), id) == core_ids.end()) {
          core_ids.push_back(id);
        }
      }
    }
  }
  if (!core_ids.empty()) return static_cast<int>(core_ids.size());
#endif
  return std::nullopt;
}

std::optional<std::size_t> total_memory_bytes() {
#ifdef __APPLE__
  int64_t mem = 0;
  std::size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0) {
    return static_cast<std::size_t>(mem);
  }
#elif defined(__linux__)
  return meminfo_bytes("MemTotal:");
#endif
  return std::nullopt;
}

std::optional<std::size_t> available_memory_bytes() {
#ifdef __APPLE__
  mach_port_t host = mach_host_self();
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) ==
      KERN_SUCCESS) {
    return static_cast<std::size_t>(stats.free_count + stats.inactive_count) * vm_page_size;
  }
#elif defined(__linux__)
  return meminfo_bytes("MemAvailable:");
#endif
  return std::nullopt;
}

std::string parallel_strategy() {
  const int cores = known_logical_cores();
  if (cores >= 8) return "aggressive_parallel";
  if (cores >= 4) return "balanced_parallel";
  if (cores >= 2) return "conservative_parallel";
  return "sequential_only";
}

int parallel_workers() {
  const int cores = known_logical_cores();
  if (cores >= 8) return cores - 2;
  if (cores >= 4) return std::min(cores, 8);
  if (cores >= 2) return std::min(cores, 3);
  return 1;
}

bool parallel_enabled() { return known_logical_cores() >= 2; }

}  // namespace sonare::system_info
