#pragma once

/// @file sonare_cli_console.h
/// @brief Terminal presentation and host-capability reporting for the CLI.

#include <cstddef>
#include <string>
#include <vector>

#include "util/system_info.h"

struct Stats {
  float mean = 0.0f;
  float std = 0.0f;
  float min = 0.0f;
  float max = 0.0f;

  static Stats compute(const std::vector<float>& v);
};

namespace color {
/// Configure ANSI escape sequences for interactive terminals. `NO_COLOR` and
/// either redirected standard stream disable them, keeping CLI output safe for
/// pipes, files, and machine parsers.
void configure();
extern const char* reset;
extern const char* bold;
extern const char* cyan;
extern const char* green;
extern const char* magenta;
extern const char* yellow;
extern const char* blue;
extern const char* red;
}  // namespace color

namespace system_info = sonare::system_info;

struct StageInfo {
  int number;
  int total;
  const char* description;
};

StageInfo get_stage_info(const char* stage);
void progress_callback(float progress, const char* stage);
void clear_progress();
/// Names the third of [0, 1] a value falls in; "-" when the value was not measured (NaN).
std::string describe_level(float value, const char* low, const char* mid, const char* high);
/// Text form of a measurement: "-" when not measured (NaN), "inf" / "-inf" for an infinite end,
/// otherwise the value to @p precision decimals; either is followed by " <unit>" when @p unit is
/// non-empty.
std::string measured_text(float value, int precision, const char* unit = "");
std::string basename(const std::string& path);
