#include "sonare_cli_console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

Stats Stats::compute(const std::vector<float>& v) {
  Stats s{};
  if (v.empty()) return s;

  s.min = *std::min_element(v.begin(), v.end());
  s.max = *std::max_element(v.begin(), v.end());
  s.mean = std::accumulate(v.begin(), v.end(), 0.0f) / static_cast<float>(v.size());

  float var = 0.0f;
  for (float x : v) var += (x - s.mean) * (x - s.mean);
  s.std = std::sqrt(var / static_cast<float>(v.size()));

  return s;
}

namespace color {
const char* reset = "";
const char* bold = "";
const char* cyan = "";
const char* green = "";
const char* magenta = "";
const char* yellow = "";
const char* blue = "";
const char* red = "";

void configure() {
#if defined(_WIN32)
  const bool interactive = _isatty(_fileno(stdout)) && _isatty(_fileno(stderr));
#else
  const bool interactive = isatty(STDOUT_FILENO) && isatty(STDERR_FILENO);
#endif
  const bool enabled = std::getenv("NO_COLOR") == nullptr && interactive;
  reset = enabled ? "\033[0m" : "";
  bold = enabled ? "\033[1m" : "";
  cyan = enabled ? "\033[36m" : "";
  green = enabled ? "\033[32m" : "";
  magenta = enabled ? "\033[35m" : "";
  yellow = enabled ? "\033[33m" : "";
  blue = enabled ? "\033[34m" : "";
  red = enabled ? "\033[31m" : "";
}
}  // namespace color

namespace system_info {

int logical_cores() {
  int n = static_cast<int>(std::thread::hardware_concurrency());
  return n > 0 ? n : 1;
}

int physical_cores() {
#ifdef __APPLE__
  int cores = 0;
  size_t len = sizeof(cores);
  if (sysctlbyname("hw.physicalcpu", &cores, &len, nullptr, 0) == 0 && cores > 0) {
    return cores;
  }
#elif __linux__
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::vector<int> core_ids;
  while (std::getline(f, line)) {
    if (line.find("core id") == 0) {
      auto pos = line.find(':');
      if (pos != std::string::npos) {
        int id = std::stoi(line.substr(pos + 1));
        if (std::find(core_ids.begin(), core_ids.end(), id) == core_ids.end()) {
          core_ids.push_back(id);
        }
      }
    }
  }
  if (!core_ids.empty()) return static_cast<int>(core_ids.size());
#endif
  return logical_cores();
}

size_t total_memory_bytes() {
#ifdef __APPLE__
  int64_t mem = 0;
  size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
    return static_cast<size_t>(mem);
  }
#elif __linux__
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemTotal:") == 0) {
      size_t kb = std::stoull(line.substr(line.find(':') + 1));
      return kb * 1024;
    }
  }
#endif
  return 0;
}

size_t available_memory_bytes() {
#ifdef __APPLE__
  mach_port_t host = mach_host_self();
  vm_statistics64_data_t stats;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) ==
      KERN_SUCCESS) {
    return (stats.free_count + stats.inactive_count) * vm_page_size;
  }
#elif __linux__
  std::ifstream f("/proc/meminfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("MemAvailable:") == 0) {
      size_t kb = std::stoull(line.substr(line.find(':') + 1));
      return kb * 1024;
    }
  }
#endif
  return 0;
}

std::string parallel_strategy() {
  int cores = logical_cores();
  if (cores >= 8) return "aggressive_parallel";
  if (cores >= 4) return "balanced_parallel";
  if (cores >= 2) return "conservative_parallel";
  return "sequential_only";
}

int parallel_workers() {
  int cores = logical_cores();
  if (cores >= 8) return cores - 2;
  if (cores >= 4) return std::min(cores, 8);
  if (cores >= 2) return std::min(cores, 3);
  return 1;
}

bool parallel_enabled() { return logical_cores() >= 2; }

}  // namespace system_info

StageInfo get_stage_info(const char* stage) {
  static const std::map<std::string, StageInfo> stages = {
      {"features", {1, 9, "Computing features"}}, {"bpm", {2, 9, "Detecting BPM"}},
      {"key", {3, 9, "Detecting key"}},           {"beats", {4, 9, "Detecting beats"}},
      {"chords", {5, 9, "Analyzing chords"}},     {"sections", {6, 9, "Analyzing sections"}},
      {"timbre", {7, 9, "Analyzing timbre"}},     {"dynamics", {8, 9, "Analyzing dynamics"}},
      {"rhythm", {9, 9, "Analyzing rhythm"}},     {"complete", {9, 9, "Complete"}},
  };
  auto it = stages.find(stage);
  if (it != stages.end()) {
    return it->second;
  }
  return {0, 0, stage};
}

void progress_callback(float progress, const char* stage) {
  StageInfo info = get_stage_info(stage);
  int pct = static_cast<int>(progress * 100.0f);

  constexpr int bar_len = 30;
  int filled = static_cast<int>(progress * bar_len);
  std::string bar(filled, '#');
  bar += std::string(bar_len - filled, '-');

  if (info.number > 0) {
    fprintf(stderr, "\r%s[%s] %3d%% [%d/%d] %s...%s                ", color::blue, bar.c_str(), pct,
            info.number, info.total, info.description, color::reset);
  } else {
    fprintf(stderr, "\r%s[%s] %3d%% %s...%s          ", color::blue, bar.c_str(), pct, stage,
            color::reset);
  }
  fflush(stderr);
}

void clear_progress() {
  std::cerr << "\r                                                              \r" << std::flush;
}

std::string describe_level(float value, const char* low, const char* mid, const char* high) {
  if (value < 0.33f) return low;
  if (value < 0.67f) return mid;
  return high;
}

std::string basename(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}
