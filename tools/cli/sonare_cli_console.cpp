#include "sonare_cli_console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numeric>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

Stats Stats::compute(const std::vector<float>& v) {
  Stats s{};
  if (v.empty()) return s;

  s.min = *std::min_element(v.begin(), v.end());
  s.max = *std::max_element(v.begin(), v.end());

  // Accumulate in double: a float sum drops low digits of a mean or a variance long before an
  // array of the sizes these commands report ends, and the other CLI publishes the same quantity.
  const double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
  double var = 0.0;
  for (float x : v) {
    const double deviation = static_cast<double>(x) - mean;
    var += deviation * deviation;
  }
  s.mean = static_cast<float>(mean);
  s.std = static_cast<float>(std::sqrt(var / static_cast<double>(v.size())));

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
  if (std::isnan(value)) return "-";
  if (value < 0.33f) return low;
  if (value < 0.67f) return mid;
  return high;
}

std::string measured_text(float value, int precision, const char* unit) {
  if (std::isnan(value)) return "-";
  const std::string suffix = unit != nullptr ? unit : "";
  if (std::isinf(value)) {
    const std::string infinity = value < 0.0f ? "-inf" : "inf";
    return suffix.empty() ? infinity : infinity + " " + suffix;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", precision, static_cast<double>(value));
  return suffix.empty() ? std::string(buffer) : std::string(buffer) + " " + suffix;
}

std::string basename(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}
