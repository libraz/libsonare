#include "cli_support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
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
  // RFC 8259 forbids NaN/Infinity as JSON numbers; emit `null` (as util/json.h
  // does) so a non-finite reading -- e.g. a -inf LUFS/true-peak for a fully
  // silent input -- yields valid JSON that json.loads/jq/JSON.parse can read.
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<float>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(double v) {
  append_separator();
  // See value(float): non-finite numbers serialize as JSON null, not "nan"/"inf".
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
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
  for (unsigned char c : s) {
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
        if (c < 0x20u) {
          constexpr char kHex[] = "0123456789abcdef";
          result += "\\u00";
          result += kHex[(c >> 4u) & 0x0Fu];
          result += kHex[c & 0x0Fu];
        } else {
          result += static_cast<char>(c);
        }
    }
  }
  return result;
}

namespace {

float parse_float_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  const float parsed = std::stof(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid numeric value for --" + option + ": " + value);
  }
  return parsed;
}

int parse_int_strict(const std::string& option, const std::string& value) {
  size_t consumed = 0;
  const int parsed = std::stoi(value, &consumed);
  if (consumed != value.size()) {
    throw std::invalid_argument("invalid integer value for --" + option + ": " + value);
  }
  return parsed;
}

}  // namespace

float CliArgs::get_float(const std::string& k, float def) const {
  auto it = options.find(k);
  return it != options.end() ? parse_float_strict(k, it->second) : def;
}

int CliArgs::get_int(const std::string& k, int def) const {
  auto it = options.find(k);
  return it != options.end() ? parse_int_strict(k, it->second) : def;
}

bool CliArgs::has(const std::string& k) const { return options.count(k) > 0; }

std::string CliArgs::get_string(const std::string& k, const std::string& def) const {
  auto it = options.find(k);
  return it != options.end() ? it->second : def;
}

CliArgs ArgParser::parse(int argc, char* argv[]) {
  CliArgs args;
  bool end_of_options = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--") {
      end_of_options = true;
    } else if (!end_of_options && (arg == "--help" || arg == "-h")) {
      args.help = true;
    } else if (!end_of_options && arg == "--json") {
      args.json_output = true;
    } else if (!end_of_options && (arg == "--quiet" || arg == "-q")) {
      args.quiet = true;
    } else if (!end_of_options && try_parse_global_option(args, arg, argv, i, argc)) {
      // Handled
    } else if (!end_of_options && arg.size() > 2 && arg.substr(0, 2) == "--") {
      const size_t equals = arg.find('=');
      if (equals == std::string::npos) {
        parse_option(args, arg.substr(2), argv, i, argc);
      } else {
        const std::string key = arg.substr(2, equals - 2);
        const std::string value = arg.substr(equals + 1);
        parse_option(args, key, argv, i, argc, &value);
      }
    } else if (!end_of_options && arg.size() > 1 && arg[0] == '-') {
      args.options[arg] = "true";
    } else if (args.command.empty()) {
      args.command = arg;
    } else {
      args.positionals.push_back(arg);
      if (args.input_file.empty()) args.input_file = arg;
    }
  }

  return args;
}

bool ArgParser::try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
                                        int argc) {
  static const std::map<std::string, std::function<void(CliArgs&, const std::string&)>>
      global_opts = {
          {"--n-fft",
           [](CliArgs& a, const std::string& v) {
             a.n_fft = parse_int_strict("n-fft", v);
             a.n_fft_explicit = true;
           }},
          {"--hop-length",
           [](CliArgs& a, const std::string& v) {
             a.hop_length = parse_int_strict("hop-length", v);
           }},
          {"--n-mels",
           [](CliArgs& a, const std::string& v) { a.n_mels = parse_int_strict("n-mels", v); }},
          {"--fmin",
           [](CliArgs& a, const std::string& v) { a.fmin = parse_float_strict("fmin", v); }},
          {"--fmax",
           [](CliArgs& a, const std::string& v) { a.fmax = parse_float_strict("fmax", v); }},
          {"-o", [](CliArgs& a, const std::string& v) { a.output_file = v; }},
          {"--output", [](CliArgs& a, const std::string& v) { a.output_file = v; }},
      };

  const size_t equals = arg.find('=');
  const std::string option = equals == std::string::npos ? arg : arg.substr(0, equals);
  auto it = global_opts.find(option);
  if (it == global_opts.end()) return false;
  if (equals != std::string::npos) {
    const std::string value = arg.substr(equals + 1);
    try {
      it->second(args, value);
    } catch (const std::invalid_argument&) {
      std::cerr << "Error: Invalid value for " << option << ": " << value << std::endl;
      std::exit(1);
    } catch (const std::out_of_range&) {
      std::cerr << "Error: Value out of range for " << option << ": " << value << std::endl;
      std::exit(1);
    }
    return true;
  }
  if (i + 1 >= argc) {
    args.missing_value_options.push_back(option);
    return true;
  }
  const std::string next = argv[i + 1];
  char* end = nullptr;
  std::strtod(next.c_str(), &end);
  const bool negative_number =
      next.size() > 1 && next[0] == '-' &&
      (std::isdigit(static_cast<unsigned char>(next[1])) || next[1] == '.' ||
       (end != next.c_str() && end != nullptr && *end == '\0'));
  if (next.size() > 1 && next[0] == '-' && !negative_number) {
    args.missing_value_options.push_back(option);
    return true;
  }
  {
    try {
      it->second(args, argv[++i]);
    } catch (const std::invalid_argument&) {
      std::cerr << "Error: Invalid value for " << option << ": " << argv[i] << std::endl;
      std::exit(1);
    } catch (const std::out_of_range&) {
      std::cerr << "Error: Value out of range for " << option << ": " << argv[i] << std::endl;
      std::exit(1);
    }
    return true;
  }
}

void ArgParser::parse_option(CliArgs& args, const std::string& key, char* argv[], int& i, int argc,
                             const std::string* inline_value) {
  enum class Arity { Unknown, Flag, RequiredValue, OptionalValue };
  static const std::map<std::string, Arity> arities = {
      {"assistant", Arity::Flag},      {"auto-gain", Arity::Flag},
      {"auto-threshold", Arity::Flag}, {"detect-inversions", Arity::Flag},
      {"dynamic", Arity::Flag},        {"enable-repair", Arity::Flag},
      {"explain", Arity::Flag},        {"exponential", Arity::Flag},
      {"hard-mask", Arity::Flag},      {"harmonic-only", Arity::Flag},
      {"hpss", Arity::Flag},           {"ir", Arity::Flag},
      {"key-context", Arity::Flag},    {"loudness-weighted", Arity::Flag},
      {"nnls", Arity::Flag},           {"no-hpss", Arity::Flag},
      {"no-pad", Arity::Flag},         {"percussive-only", Arity::Flag},
      {"proportional-q", Arity::Flag}, {"sabine", Arity::Flag},
      {"series", Arity::Flag},         {"stereo", Arity::Flag},
      {"triads-only", Arity::Flag},    {"use-hmm", Arity::Flag},
      {"use-hpss", Arity::Flag},       {"with-residual", Arity::Flag},
      {"with-seventh", Arity::Flag},   {"zero-phase", Arity::Flag},
      {"synth", Arity::OptionalValue},
  };
  const auto found = arities.find(key);
  const Arity arity = found == arities.end() ? Arity::RequiredValue : found->second;
  if (arity == Arity::Flag) {
    args.options[key] = "true";
    return;
  }

  if (inline_value != nullptr) {
    if (inline_value->empty() && arity == Arity::RequiredValue) {
      args.options[key] = "";
      args.missing_value_options.push_back("--" + key);
    } else {
      args.options[key] = *inline_value;
    }
    return;
  }

  if (i + 1 < argc) {
    std::string next = argv[i + 1];
    char* end = nullptr;
    std::strtod(next.c_str(), &end);
    const bool is_negative_num =
        next.size() > 1 && next[0] == '-' &&
        (std::isdigit(static_cast<unsigned char>(next[1])) || next[1] == '.' ||
         (end != next.c_str() && end != nullptr && *end == '\0'));
    const bool is_option = next.size() > 1 && next[0] == '-' && !is_negative_num;

    if (!is_option) {
      args.options[key] = argv[++i];
      return;
    }
  }
  if (arity == Arity::OptionalValue) {
    args.options[key] = "true";
  } else {
    args.options[key] = "";
    args.missing_value_options.push_back("--" + key);
  }
}

namespace {

struct CommandOptionSchema {
  std::vector<std::string> values;
  std::vector<std::string> flags;
  std::vector<std::string> optional_values;
};

const std::map<std::string, CommandOptionSchema>& command_option_schemas() {
  static const std::map<std::string, CommandOptionSchema> schemas = {
      {"key",
       {{"high-pass-hz", "genre-hint", "profile", "modes", "candidates"},
        {"use-hpss", "hpss", "loudness-weighted"},
        {}}},
      {"chords",
       {{"min-duration", "threshold", "hmm-beam-width", "key-root", "key-mode"},
        {"triads-only", "nnls", "use-hmm", "detect-inversions", "key-context"},
        {}}},
      {"sections", {{"min-duration", "threshold"}, {}, {}}},
      {"dynamics", {{"window-sec"}, {}, {}}},
      {"rhythm", {{"start-bpm", "bpm-min", "bpm-max"}, {}, {}}},
      {"melody", {{"threshold"}, {}, {}}},
      {"boundaries", {{"threshold", "kernel-size", "min-distance"}, {}, {}}},
      {"analyze", {{"chroma-highpass"}, {"with-seventh", "no-hpss"}, {}}},
      {"pitch-shift", {{"semitones"}, {}, {}}},
      {"time-stretch", {{"rate"}, {}, {}}},
      {"pitch-correct", {{"current-midi", "target-midi"}, {}, {}}},
      {"note-stretch", {{"onset", "offset", "ratio"}, {}, {}}},
      {"voice-change",
       {{"preset", "preset-json", "preset-pack", "set", "pitch-semitones", "formant-factor"},
        {},
        {}}},
      {"voice-preset", {{"preset"}, {}, {}}},
      {"voice-preset-validate", {{"preset-json", "preset", "set"}, {}, {}}},
      {"hpss",
       {{"kernel-harmonic", "kernel-percussive"},
        {"harmonic-only", "percussive-only", "with-residual", "hard-mask"},
        {}}},
      {"preemphasis", {{"coef"}, {}, {}}},
      {"deemphasis", {{"coef"}, {}, {}}},
      {"trim-silence", {{"threshold-db", "top-db"}, {}, {}}},
      {"split-silence", {{"top-db"}, {}, {}}},
      {"normalize", {{"mode", "target-db"}, {}, {}}},
      {"gain", {{"gain-db"}, {}, {}}},
      {"fade", {{"fade-in", "fade-out"}, {}, {}}},
      {"filter", {{"type", "order", "cutoff", "center", "bandwidth"}, {"zero-phase"}, {}}},
      {"resample", {{"target-rate", "target-sr"}, {}, {}}},
      {"tone", {{"frequency", "sr", "duration", "phase", "amplitude"}, {}, {}}},
      {"chirp", {{"sr", "duration"}, {"exponential"}, {}}},
      {"clicks", {{"times", "sr", "length", "frequency", "click-duration"}, {}, {}}},
      {"mastering",
       {{"preset", "config", "target-lufs", "ceiling-db", "params", "bits", "true-peak-oversample"},
        {"assistant", "enable-repair", "explain"},
        {}}},
      {"mastering-processor", {{"processor", "params", "bits"}, {"stereo"}, {}}},
      {"eq",
       {{"params",      "type",         "frequency-hz", "gain-db",      "q",
         "coeff-mode",  "slope-db-oct", "placement",    "threshold-db", "ratio",
         "range-db",    "attack-ms",    "release-ms",   "lookahead-ms", "sidechain-freq-hz",
         "sidechain-q", "phase-mode",   "resolution",   "gain-scale",   "output-gain-db",
         "output-pan",  "bits"},
        {"proportional-q", "dynamic", "auto-threshold", "auto-gain"},
        {}}},
      {"mastering-pair-processor", {{"processor", "reference", "params", "bits"}, {}, {}}},
      {"mastering-pair-analyze", {{"analysis", "reference", "params"}, {}, {}}},
      {"mastering-stereo-analyze", {{"analysis", "reference", "params"}, {}, {}}},
      {"mixing-preset", {{"preset"}, {}, {}}},
      {"mix", {{"input-trim-db", "fader-db", "pan", "pan-mode", "width"}, {}, {}}},
      {"pitch", {{"threshold", "algorithm"}, {}, {}}},
      {"tempogram", {{"win-length"}, {}, {}}},
      {"fourier-tempogram", {{"win-length"}, {}, {}}},
      {"tempogram-ratio", {{"win-length"}, {}, {}}},
      {"plp", {{"tempo-min", "tempo-max", "win-length"}, {}, {}}},
      {"cqt", {{"n-bins", "bins-per-octave"}, {}, {}}},
      {"vqt", {{"n-bins", "bins-per-octave", "gamma", "filter-scale"}, {}, {}}},
      {"mel-to-audio", {{"n-iter"}, {}, {}}},
      {"mfcc-to-audio", {{"n-mfcc", "n-iter"}, {}, {}}},
      {"acoustic", {{"n-bands", "min-decay-db", "noise-floor-margin-db"}, {"ir"}, {}}},
      {"synthesize-rir",
       {{"length", "width", "height", "absorption", "source-x", "source-y", "source-z",
         "listener-x", "listener-y", "listener-z", "sample-rate", "ism-order", "seed",
         "max-seconds"},
        {},
        {}}},
      {"estimate-room",
       {{"aspect-lw", "aspect-lh", "reference-absorption", "n-bands"}, {"sabine"}, {}}},
      {"room-morph",
       {{"length", "width", "height", "absorption", "source-x", "source-y", "source-z",
         "listener-x", "listener-y", "listener-z", "suppression", "wet", "ism-order", "seed",
         "max-seconds"},
        {},
        {}}},
      {"lufs", {{}, {"series"}, {}}},
      {"meter", {{"clip-threshold", "oversample"}, {}, {}}},
      {"clipping", {{"threshold", "min-region"}, {}, {}}},
      {"dynamic-range", {{"window-sec", "hop-sec", "low-percentile", "high-percentile"}, {}, {}}},
      {"stereo", {{"reference"}, {}, {}}},
      {"phase", {{"reference"}, {}, {}}},
      {"frames-to-samples", {{"frames"}, {}, {}}},
      {"samples-to-frames", {{"samples"}, {}, {}}},
      {"power-to-db", {{"values", "ref", "amin", "top-db"}, {}, {}}},
      {"amplitude-to-db", {{"values", "ref", "amin", "top-db"}, {}, {}}},
      {"db-to-power", {{"values", "ref"}, {}, {}}},
      {"db-to-amplitude", {{"values", "ref"}, {}, {}}},
      {"frame-signal", {{"values", "frame-length"}, {}, {}}},
      {"pad-center", {{"values", "size", "pad-value"}, {}, {}}},
      {"fix-length", {{"values", "size", "pad-value"}, {}, {}}},
      {"fix-frames", {{"values", "x-min", "x-max"}, {"no-pad"}, {}}},
      {"peak-pick",
       {{"values", "pre-max", "post-max", "pre-avg", "post-avg", "delta", "wait"}, {}, {}}},
      {"vector-normalize", {{"values", "norm-type", "threshold"}, {}, {}}},
      {"pcen",
       {{"values", "sample-rate", "time-constant", "gain", "bias", "power", "eps", "n-bins",
         "n-frames"},
        {},
        {}}},
      {"project",
       {{"in", "project", "sample-rate", "frames", "block-size", "channels", "instrument-latency",
         "smf", "midi2"},
        {},
        {"synth"}}},
  };
  return schemas;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

std::string validate_cli_arguments(const CliArgs& args, bool requires_audio) {
  const auto schema_it = command_option_schemas().find(args.command);
  const CommandOptionSchema empty;
  const CommandOptionSchema& schema =
      schema_it == command_option_schemas().end() ? empty : schema_it->second;

  for (const auto& option : args.options) {
    const std::string& key = option.first;
    if (!contains(schema.values, key) && !contains(schema.flags, key) &&
        !contains(schema.optional_values, key)) {
      return "Unknown option '" + (key.rfind("-", 0) == 0 ? key : "--" + key) + "' for command '" +
             args.command + "'";
    }
  }
  if (!args.missing_value_options.empty()) {
    return "Missing value for option '" + args.missing_value_options.front() + "'";
  }

  size_t max_positionals = requires_audio ? 1u : 0u;
  if (args.command == "project" || args.command == "voice-preset-validate") max_positionals = 1u;
  if (args.positionals.size() > max_positionals) {
    return "Unexpected positional argument '" + args.positionals[max_positionals] +
           "' for command '" + args.command + "'";
  }
  return {};
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
