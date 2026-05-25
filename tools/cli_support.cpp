#include "cli_support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

JsonBuilder& JsonBuilder::begin_object() {
  append_separator();
  ss_ << "{";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_object() {
  ss_ << "}";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::begin_array() {
  append_separator();
  ss_ << "[";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_array() {
  ss_ << "]";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::key(const std::string& k) {
  append_separator();
  ss_ << "\"" << escape(k) << "\": ";
  needs_comma_.back() = false;
  return *this;
}

JsonBuilder& JsonBuilder::value(const std::string& v) {
  append_separator();
  ss_ << "\"" << escape(v) << "\"";
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(const char* v) { return value(std::string(v)); }

JsonBuilder& JsonBuilder::value(int v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(size_t v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(float v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(double v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(bool v) {
  append_separator();
  ss_ << (v ? "true" : "false");
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::kv(const std::string& k, const std::string& v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, const char* v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, int v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, size_t v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, float v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, double v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, bool v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::float_array(const std::vector<float>& arr) {
  begin_array();
  for (float v : arr) value(v);
  end_array();
  return *this;
}

std::string JsonBuilder::build() const { return ss_.str(); }

void JsonBuilder::print() const { std::cout << ss_.str() << "\n"; }

void JsonBuilder::append_separator() {
  if (!needs_comma_.empty() && needs_comma_.back()) {
    ss_ << ", ";
  }
}

std::string JsonBuilder::escape(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
    }
  }
  return result;
}

float CliArgs::get_float(const std::string& k, float def) const {
  auto it = options.find(k);
  return it != options.end() ? std::stof(it->second) : def;
}

int CliArgs::get_int(const std::string& k, int def) const {
  auto it = options.find(k);
  return it != options.end() ? std::stoi(it->second) : def;
}

bool CliArgs::has(const std::string& k) const { return options.count(k) > 0; }

std::string CliArgs::get_string(const std::string& k, const std::string& def) const {
  auto it = options.find(k);
  return it != options.end() ? it->second : def;
}

CliArgs ArgParser::parse(int argc, char* argv[]) {
  CliArgs args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      args.help = true;
    } else if (arg == "--json") {
      args.json_output = true;
    } else if (arg == "--quiet" || arg == "-q") {
      args.quiet = true;
    } else if (try_parse_global_option(args, arg, argv, i, argc)) {
      // Handled
    } else if (arg.substr(0, 2) == "--") {
      parse_option(args, arg.substr(2), argv, i, argc);
    } else if (args.command.empty()) {
      args.command = arg;
    } else if (args.input_file.empty()) {
      args.input_file = arg;
    }
  }

  return args;
}

bool ArgParser::try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
                                        int argc) {
  static const std::map<std::string, std::function<void(CliArgs&, const std::string&)>>
      global_opts = {
          {"--n-fft", [](CliArgs& a, const std::string& v) { a.n_fft = std::stoi(v); }},
          {"--hop-length", [](CliArgs& a, const std::string& v) { a.hop_length = std::stoi(v); }},
          {"--n-mels", [](CliArgs& a, const std::string& v) { a.n_mels = std::stoi(v); }},
          {"--fmin", [](CliArgs& a, const std::string& v) { a.fmin = std::stof(v); }},
          {"--fmax", [](CliArgs& a, const std::string& v) { a.fmax = std::stof(v); }},
          {"-o", [](CliArgs& a, const std::string& v) { a.output_file = v; }},
          {"--output", [](CliArgs& a, const std::string& v) { a.output_file = v; }},
      };

  auto it = global_opts.find(arg);
  if (it != global_opts.end() && i + 1 < argc) {
    try {
      it->second(args, argv[++i]);
    } catch (const std::invalid_argument&) {
      std::cerr << "Error: Invalid value for " << arg << ": " << argv[i] << std::endl;
      std::exit(1);
    } catch (const std::out_of_range&) {
      std::cerr << "Error: Value out of range for " << arg << ": " << argv[i] << std::endl;
      std::exit(1);
    }
    return true;
  }
  return false;
}

void ArgParser::parse_option(CliArgs& args, const std::string& key, char* argv[], int& i,
                             int argc) {
  static const std::vector<std::string> bool_flags = {"harmonic-only",     "percussive-only",
                                                      "with-residual",     "hard-mask",
                                                      "triads-only",       "no-hpss",
                                                      "with-seventh",      "no-pad",
                                                      "use-hpss",          "hpss",
                                                      "loudness-weighted", "nnls",
                                                      "use-hmm",           "detect-inversions",
                                                      "key-context",       "auto-gain",
                                                      "proportional-q"};

  bool is_flag = std::find(bool_flags.begin(), bool_flags.end(), key) != bool_flags.end() ||
                 key.find("-only") != std::string::npos;

  if (is_flag) {
    args.options[key] = "true";
    return;
  }

  if (i + 1 < argc) {
    std::string next = argv[i + 1];
    bool is_negative_num = next.size() > 1 && next[0] == '-' && std::isdigit(next[1]);
    bool is_option = next.size() > 1 && next[0] == '-' && !is_negative_num;

    if (!is_option) {
      args.options[key] = argv[++i];
      return;
    }
  }
  args.options[key] = "true";
}

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
const char* reset = "\033[0m";
const char* bold = "\033[1m";
const char* cyan = "\033[36m";
const char* green = "\033[32m";
const char* magenta = "\033[35m";
const char* yellow = "\033[33m";
const char* blue = "\033[34m";
const char* red = "\033[31m";
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
