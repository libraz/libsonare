/// @file sonare_cli.cpp
/// @brief Command-line interface for sonare audio analysis.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/boundary_detector.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/audio_io.h"
#include "effects/hpss.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "quick.h"
#include "sonare.h"

using namespace sonare;

// ============================================================================
// JSON Builder - Fluent interface for building JSON output
// ============================================================================

class JsonBuilder {
 public:
  JsonBuilder& begin_object() {
    append_separator();
    ss_ << "{";
    needs_comma_.push_back(false);
    return *this;
  }

  JsonBuilder& end_object() {
    ss_ << "}";
    needs_comma_.pop_back();
    if (!needs_comma_.empty()) needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& begin_array() {
    append_separator();
    ss_ << "[";
    needs_comma_.push_back(false);
    return *this;
  }

  JsonBuilder& end_array() {
    ss_ << "]";
    needs_comma_.pop_back();
    if (!needs_comma_.empty()) needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& key(const std::string& k) {
    append_separator();
    ss_ << "\"" << escape(k) << "\": ";
    needs_comma_.back() = false;
    return *this;
  }

  JsonBuilder& value(const std::string& v) {
    append_separator();
    ss_ << "\"" << escape(v) << "\"";
    needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& value(const char* v) { return value(std::string(v)); }

  JsonBuilder& value(int v) {
    append_separator();
    ss_ << v;
    needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& value(size_t v) {
    append_separator();
    ss_ << v;
    needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& value(float v) {
    append_separator();
    ss_ << v;
    needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& value(double v) {
    append_separator();
    ss_ << v;
    needs_comma_.back() = true;
    return *this;
  }

  JsonBuilder& value(bool v) {
    append_separator();
    ss_ << (v ? "true" : "false");
    needs_comma_.back() = true;
    return *this;
  }

  // Convenience: key-value pairs
  JsonBuilder& kv(const std::string& k, const std::string& v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, const char* v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, int v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, size_t v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, float v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, double v) { return key(k).value(v); }
  JsonBuilder& kv(const std::string& k, bool v) { return key(k).value(v); }

  // Array of floats
  JsonBuilder& float_array(const std::vector<float>& arr) {
    begin_array();
    for (float v : arr) value(v);
    end_array();
    return *this;
  }

  std::string build() const { return ss_.str(); }
  void print() const { std::cout << ss_.str() << "\n"; }

 private:
  void append_separator() {
    if (!needs_comma_.empty() && needs_comma_.back()) {
      ss_ << ", ";
    }
  }

  static std::string escape(const std::string& s) {
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

  std::ostringstream ss_;
  std::vector<bool> needs_comma_;
};

// ============================================================================
// CLI Arguments
// ============================================================================

struct CliArgs {
  std::string command;
  std::string input_file;
  std::string output_file;
  bool json_output = false;
  bool quiet = false;
  bool help = false;

  int n_fft = 2048;
  int hop_length = 512;
  int n_mels = 128;
  float fmin = 0.0f;
  float fmax = 0.0f;

  std::map<std::string, std::string> options;

  float get_float(const std::string& k, float def) const {
    auto it = options.find(k);
    return it != options.end() ? std::stof(it->second) : def;
  }

  int get_int(const std::string& k, int def) const {
    auto it = options.find(k);
    return it != options.end() ? std::stoi(it->second) : def;
  }

  bool has(const std::string& k) const { return options.count(k) > 0; }

  std::string get_string(const std::string& k, const std::string& def = "") const {
    auto it = options.find(k);
    return it != options.end() ? it->second : def;
  }
};

// ============================================================================
// Argument Parser
// ============================================================================

class ArgParser {
 public:
  static CliArgs parse(int argc, char* argv[]) {
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

 private:
  static bool try_parse_global_option(CliArgs& args, const std::string& arg, char* argv[], int& i,
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
      it->second(args, argv[++i]);
      return true;
    }
    return false;
  }

  static void parse_option(CliArgs& args, const std::string& key, char* argv[], int& i, int argc) {
    static const std::vector<std::string> bool_flags = {
        "harmonic-only", "percussive-only", "with-residual", "hard-mask",
        "triads-only",   "no-hpss",         "with-seventh"};

    bool is_flag =
        std::find(bool_flags.begin(), bool_flags.end(), key) != bool_flags.end() ||
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
};

// ============================================================================
// Statistics Utility
// ============================================================================

struct Stats {
  float mean, std, min, max;

  static Stats compute(const std::vector<float>& v) {
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
};

// ============================================================================
// ANSI Color Codes (for bpm-detector style output)
// ============================================================================

namespace color {
constexpr const char* reset = "\033[0m";
constexpr const char* bold = "\033[1m";
constexpr const char* cyan = "\033[36m";
constexpr const char* green = "\033[32m";
constexpr const char* magenta = "\033[35m";
constexpr const char* yellow = "\033[33m";
constexpr const char* blue = "\033[34m";
constexpr const char* red = "\033[31m";
}  // namespace color

// ============================================================================
// Output Helpers
// ============================================================================

/// @brief Stage info for progress display.
struct StageInfo {
  int number;
  int total;
  const char* description;
};

/// @brief Get stage info from stage name.
StageInfo get_stage_info(const char* stage) {
  static const std::map<std::string, StageInfo> stages = {
      {"bpm", {1, 8, "Detecting BPM"}},
      {"key", {2, 8, "Detecting key"}},
      {"beats", {3, 8, "Detecting beats"}},
      {"chords", {4, 8, "Analyzing chords"}},
      {"sections", {5, 8, "Analyzing sections"}},
      {"timbre", {6, 8, "Analyzing timbre"}},
      {"dynamics", {7, 8, "Analyzing dynamics"}},
      {"rhythm", {8, 8, "Analyzing rhythm"}},
      {"complete", {8, 8, "Complete"}},
  };
  auto it = stages.find(stage);
  if (it != stages.end()) {
    return it->second;
  }
  return {0, 0, stage};
}

void progress_callback(float /*progress*/, const char* stage) {
  StageInfo info = get_stage_info(stage);
  if (info.number > 0) {
    std::cerr << "\r" << color::blue << "[" << info.number << "/" << info.total << "] "
              << info.description << "..." << color::reset << "                    " << std::flush;
  } else {
    std::cerr << "\r" << color::blue << stage << "..." << color::reset << "          " << std::flush;
  }
}

void clear_progress() {
  std::cerr << "\r                                                              \r" << std::flush;
}

std::string describe_level(float value, const char* low, const char* mid, const char* high) {
  if (value < 0.33f) return low;
  if (value < 0.67f) return mid;
  return high;
}

/// @brief Extract basename from a path.
std::string basename(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// ============================================================================
// Command Handler Type
// ============================================================================

using CommandHandler = std::function<int(const CliArgs&, const Audio&)>;

// ============================================================================
// Command Implementations
// ============================================================================

int cmd_version(const CliArgs& args) {
  if (args.json_output) {
    JsonBuilder().begin_object().kv("cli_version", "1.0.0").kv("lib_version", version()).end_object().print();
  } else {
    std::cout << "sonare-cli version 1.0.0\n";
    std::cout << "libsonare version " << version() << "\n";
  }
  return 0;
}

int cmd_info(const CliArgs& args, const Audio& audio) {
  float peak = 0.0f, rms_sum = 0.0f;
  for (size_t i = 0; i < audio.size(); ++i) {
    float val = std::abs(audio.data()[i]);
    peak = std::max(peak, val);
    rms_sum += audio.data()[i] * audio.data()[i];
  }
  float rms = std::sqrt(rms_sum / static_cast<float>(audio.size()));
  float peak_db = 20.0f * std::log10(std::max(peak, 1e-10f));
  float rms_db = 20.0f * std::log10(std::max(rms, 1e-10f));

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("path", args.input_file)
        .kv("duration", audio.duration())
        .kv("sample_rate", audio.sample_rate())
        .kv("samples", audio.size())
        .kv("peak_db", peak_db)
        .kv("rms_db", rms_db)
        .end_object()
        .print();
  } else {
    int mins = static_cast<int>(audio.duration()) / 60;
    int secs = static_cast<int>(audio.duration()) % 60;
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << mins << ":" << std::setfill('0')
              << std::setw(2) << secs << " (" << std::fixed << std::setprecision(1)
              << audio.duration() << "s)" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Sample Rate: " << audio.sample_rate() << " Hz"
              << color::reset << "\n";
    std::cout << "  " << color::blue << "> Samples: " << audio.size() << color::reset << "\n";
    std::cout << "  " << color::green << "> Peak Level: " << std::fixed << std::setprecision(1)
              << peak_db << " dB" << color::reset << "\n";
    std::cout << "  " << color::green << "> RMS Level: " << rms_db << " dB" << color::reset
              << "\n\n";
  }
  return 0;
}

int cmd_bpm(const CliArgs& args, const Audio& audio) {
  float bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().begin_object().kv("bpm", bpm).end_object().print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << bpm << " BPM" << color::reset << "\n\n";
  }
  return 0;
}

int cmd_key(const CliArgs& args, const Audio& audio) {
  Key key = quick::detect_key(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("root", static_cast<int>(key.root))
        .kv("mode", static_cast<int>(key.mode))
        .kv("confidence", key.confidence)
        .kv("name", key.to_string())
        .end_object()
        .print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::magenta << color::bold << "> Estimated Key : " << key.to_string()
              << "  (conf " << std::fixed << std::setprecision(1) << (key.confidence * 100.0f)
              << "%)" << color::reset << "\n\n";
  }
  return 0;
}

int cmd_beats(const CliArgs& args, const Audio& audio) {
  auto beats = quick::detect_beats(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(beats).print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << beats.size() << " beats" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Beat times:" << color::reset << "\n    ";
    for (size_t i = 0; i < beats.size(); ++i) {
      printf("%.2f", beats[i]);
      if (i < beats.size() - 1) std::cout << ", ";
      if ((i + 1) % 10 == 0) std::cout << "\n    ";
    }
    std::cout << "\n\n";
  }
  return 0;
}

int cmd_onsets(const CliArgs& args, const Audio& audio) {
  auto onsets = quick::detect_onsets(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(onsets).print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << onsets.size() << " onsets" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Onset times:" << color::reset << "\n    ";
    for (size_t i = 0; i < onsets.size(); ++i) {
      printf("%.2f", onsets[i]);
      if (i < onsets.size() - 1) std::cout << ", ";
      if ((i + 1) % 10 == 0) std::cout << "\n    ";
    }
    std::cout << "\n\n";
  }
  return 0;
}

int cmd_chords(const CliArgs& args, const Audio& audio) {
  ChordConfig config;
  config.min_duration = args.get_float("min-duration", 0.3f);
  config.threshold = args.get_float("threshold", 0.5f);
  config.use_triads_only = args.has("triads-only");
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  ChordAnalyzer analyzer(audio, config);
  const auto& chords = analyzer.chords();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("progression", analyzer.progression_pattern())
        .kv("count", chords.size())
        .key("chords")
        .begin_array();
    for (const auto& c : chords) {
      json.begin_object()
          .kv("name", c.to_string())
          .kv("root", static_cast<int>(c.root))
          .kv("start", c.start)
          .kv("end", c.end)
          .kv("confidence", c.confidence)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s, " << chords.size() << " chord changes" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Chord Progression: " << analyzer.progression_pattern()
              << color::reset << "\n";
    std::cout << "  " << color::blue << "> Chord Details:" << color::reset << "\n";
    for (size_t i = 0; i < chords.size(); ++i) {
      const auto& c = chords[i];
      printf("    %zu. %-8s (%.2fs - %.2fs, conf %.0f%%)\n", i + 1, c.to_string().c_str(), c.start,
             c.end, c.confidence * 100.0f);
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_sections(const CliArgs& args, const Audio& audio) {
  SectionConfig config;
  config.min_section_sec = args.get_float("min-duration", 4.0f);
  config.boundary_threshold = args.get_float("threshold", 0.3f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  SectionAnalyzer analyzer(audio, config);
  const auto& sections = analyzer.sections();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("form", analyzer.form())
        .kv("count", sections.size())
        .key("sections")
        .begin_array();
    for (const auto& s : sections) {
      json.begin_object()
          .kv("type", s.type_string())
          .kv("start", s.start)
          .kv("end", s.end)
          .kv("energy", s.energy_level)
          .kv("confidence", s.confidence)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structure: " << analyzer.form() << " (" << sections.size()
              << " sections)" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Section Details:" << color::reset << "\n";
    for (size_t i = 0; i < sections.size(); ++i) {
      const auto& s = sections[i];
      int start_mm = static_cast<int>(s.start) / 60;
      int start_ss = static_cast<int>(s.start) % 60;
      float duration = s.end - s.start;
      std::string energy_sym = (s.energy_level < 0.33f)   ? "low E"
                               : (s.energy_level < 0.67f) ? "mid E"
                                                          : "high E";
      printf("    %zu. %s (%02d:%02d, %.1fs, %s)\n", i + 1, s.type_string().c_str(), start_mm,
             start_ss, duration, energy_sym.c_str());
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_timbre(const CliArgs& args, const Audio& audio) {
  TimbreConfig config;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.n_mels = args.n_mels;

  TimbreAnalyzer analyzer(audio, config);
  const Timbre& t = analyzer.timbre();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("brightness", t.brightness)
        .kv("warmth", t.warmth)
        .kv("density", t.density)
        .kv("roughness", t.roughness)
        .kv("complexity", t.complexity)
        .end_object()
        .print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::magenta << "> Timbre Analysis:" << color::reset << "\n";
    printf("    Brightness: %.2f (%s)\n", t.brightness,
           describe_level(t.brightness, "dark", "neutral", "bright").c_str());
    printf("    Warmth:     %.2f (%s)\n", t.warmth,
           describe_level(t.warmth, "thin", "neutral", "warm").c_str());
    printf("    Density:    %.2f (%s)\n", t.density,
           describe_level(t.density, "sparse", "moderate", "rich").c_str());
    printf("    Roughness:  %.2f (%s)\n", t.roughness,
           describe_level(t.roughness, "smooth", "moderate", "rough").c_str());
    printf("    Complexity: %.2f (%s)\n", t.complexity,
           describe_level(t.complexity, "simple", "moderate", "complex").c_str());
    std::cout << "\n";
  }
  return 0;
}

int cmd_dynamics(const CliArgs& args, const Audio& audio) {
  DynamicsConfig config;
  config.window_sec = args.get_float("window-sec", 0.4f);
  config.hop_length = args.hop_length;

  DynamicsAnalyzer analyzer(audio, config);
  const Dynamics& d = analyzer.dynamics();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("peak_db", d.peak_db)
        .kv("rms_db", d.rms_db)
        .kv("dynamic_range_db", d.dynamic_range_db)
        .kv("crest_factor", d.crest_factor)
        .kv("loudness_range_db", d.loudness_range_db)
        .kv("is_compressed", d.is_compressed)
        .end_object()
        .print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Dynamics Analysis:" << color::reset << "\n";
    printf("    Peak Level:     %.1f dB\n", d.peak_db);
    printf("    RMS Level:      %.1f dB\n", d.rms_db);
    printf("    Dynamic Range:  %.1f dB\n", d.dynamic_range_db);
    printf("    Crest Factor:   %.1f dB\n", d.crest_factor);
    printf("    Loudness Range: %.1f LU\n", d.loudness_range_db);
    std::cout << "    Compression:    " << (d.is_compressed ? "Yes (compressed)" : "No (natural)")
              << "\n\n";
  }
  return 0;
}

int cmd_rhythm(const CliArgs& args, const Audio& audio) {
  RhythmConfig config;
  config.start_bpm = args.get_float("start-bpm", 120.0f);
  config.bpm_min = args.get_float("bpm-min", 60.0f);
  config.bpm_max = args.get_float("bpm-max", 200.0f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  RhythmAnalyzer analyzer(audio, config);
  const RhythmFeatures& r = analyzer.features();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .key("time_signature")
        .begin_object()
        .kv("numerator", r.time_signature.numerator)
        .kv("denominator", r.time_signature.denominator)
        .kv("confidence", r.time_signature.confidence)
        .end_object()
        .kv("bpm", analyzer.bpm())
        .kv("groove_type", r.groove_type)
        .kv("syncopation", r.syncopation)
        .kv("pattern_regularity", r.pattern_regularity)
        .kv("tempo_stability", r.tempo_stability)
        .end_object()
        .print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Rhythm Analysis:" << color::reset << "\n";
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << analyzer.bpm() << " BPM" << color::reset << "\n";
    printf("    Time Signature:    %d/%d (conf %.0f%%)\n", r.time_signature.numerator,
           r.time_signature.denominator, r.time_signature.confidence * 100.0f);
    printf("    Groove Type:       %s\n", r.groove_type.c_str());
    printf("    Syncopation:       %.2f (%s)\n", r.syncopation,
           r.syncopation < 0.3f ? "low" : (r.syncopation < 0.7f ? "moderate" : "high"));
    printf("    Pattern Regularity: %.2f (%s)\n", r.pattern_regularity,
           r.pattern_regularity > 0.7f ? "regular" : "irregular");
    printf("    Tempo Stability:   %.2f (%s)\n", r.tempo_stability,
           r.tempo_stability > 0.8f ? "stable" : "variable");
    std::cout << "\n";
  }
  return 0;
}

int cmd_melody(const CliArgs& args, const Audio& audio) {
  MelodyConfig config;
  config.fmin = args.get_float("fmin", 80.0f);
  config.fmax = args.get_float("fmax", 1000.0f);
  config.threshold = args.get_float("threshold", 0.1f);
  config.hop_length = args.hop_length;

  MelodyAnalyzer analyzer(audio, config);
  const MelodyContour& c = analyzer.contour();

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("has_melody", analyzer.has_melody())
        .kv("pitch_range_octaves", c.pitch_range_octaves)
        .kv("mean_frequency", c.mean_frequency)
        .kv("pitch_stability", c.pitch_stability)
        .kv("vibrato_rate", c.vibrato_rate)
        .kv("pitch_count", c.pitches.size())
        .end_object()
        .print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::cyan << "> Melody Analysis:" << color::reset << "\n";
    std::cout << "    Has Melody:      " << (analyzer.has_melody() ? "Yes" : "No") << "\n";
    printf("    Pitch Range:     %.2f octaves\n", c.pitch_range_octaves);
    printf("    Mean Frequency:  %.1f Hz\n", c.mean_frequency);
    printf("    Pitch Stability: %.2f\n", c.pitch_stability);
    printf("    Vibrato Rate:    %.1f Hz\n", c.vibrato_rate);
    printf("    Pitch Points:    %zu\n", c.pitches.size());
    std::cout << "\n";
  }
  return 0;
}

int cmd_boundaries(const CliArgs& args, const Audio& audio) {
  BoundaryConfig config;
  config.threshold = args.get_float("threshold", 0.3f);
  config.kernel_size = args.get_int("kernel-size", 64);
  config.peak_distance = args.get_float("min-distance", 2.0f);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  BoundaryDetector detector(audio, config);
  const auto& bounds = detector.boundaries();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("count", bounds.size()).key("boundaries").begin_array();
    for (const auto& b : bounds) {
      json.begin_object()
          .kv("time", b.time)
          .kv("frame", b.frame)
          .kv("strength", b.strength)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structural Boundaries (" << bounds.size() << " detected):"
              << color::reset << "\n";
    for (size_t i = 0; i < bounds.size(); ++i) {
      const auto& b = bounds[i];
      int mm = static_cast<int>(b.time) / 60;
      int ss = static_cast<int>(b.time) % 60;
      printf("    %zu. %02d:%02d (strength %.2f)\n", i + 1, mm, ss, b.strength);
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_analyze(const CliArgs& args, const Audio& audio) {
  MusicAnalyzerConfig config;
  config.use_triads_only = !args.has("with-seventh");
  config.use_hpss = !args.has("no-hpss");
  if (args.has("chroma-highpass")) {
    config.chroma_highpass_hz = args.get_float("chroma-highpass", 200.0f);
  }

  MusicAnalyzer analyzer(audio, config);
  if (!args.quiet && !args.json_output) {
    analyzer.set_progress_callback(progress_callback);
  }

  AnalysisResult r = analyzer.analyze();

  if (!args.quiet && !args.json_output) {
    clear_progress();
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("bpm", r.bpm)
        .kv("bpmConfidence", r.bpm_confidence)
        .key("key")
        .begin_object()
        .kv("root", static_cast<int>(r.key.root))
        .kv("mode", static_cast<int>(r.key.mode))
        .kv("confidence", r.key.confidence)
        .kv("name", r.key.to_string())
        .end_object()
        .key("timeSignature")
        .begin_object()
        .kv("numerator", r.time_signature.numerator)
        .kv("denominator", r.time_signature.denominator)
        .kv("confidence", r.time_signature.confidence)
        .end_object()
        .key("beats")
        .begin_array();
    for (const auto& b : r.beats) {
      json.begin_object().kv("time", b.time).kv("strength", b.strength).end_object();
    }
    json.end_array().key("chords").begin_array();
    for (const auto& c : r.chords) {
      json.begin_object()
          .kv("name", c.to_string())
          .kv("start", c.start)
          .kv("end", c.end)
          .kv("confidence", c.confidence)
          .end_object();
    }
    json.end_array().key("sections").begin_array();
    for (const auto& s : r.sections) {
      json.begin_object()
          .kv("type", s.type_string())
          .kv("start", s.start)
          .kv("end", s.end)
          .end_object();
    }
    json.end_array()
        .key("timbre")
        .begin_object()
        .kv("brightness", r.timbre.brightness)
        .kv("warmth", r.timbre.warmth)
        .kv("density", r.timbre.density)
        .kv("roughness", r.timbre.roughness)
        .kv("complexity", r.timbre.complexity)
        .end_object()
        .key("dynamics")
        .begin_object()
        .kv("dynamicRangeDb", r.dynamics.dynamic_range_db)
        .kv("loudnessRangeDb", r.dynamics.loudness_range_db)
        .kv("crestFactor", r.dynamics.crest_factor)
        .kv("isCompressed", r.dynamics.is_compressed)
        .end_object()
        .key("rhythm")
        .begin_object()
        .kv("syncopation", r.rhythm.syncopation)
        .kv("grooveType", r.rhythm.groove_type)
        .kv("patternRegularity", r.rhythm.pattern_regularity)
        .end_object()
        .kv("form", r.form)
        .end_object()
        .print();
  } else {
    // bpm-detector style output
    std::cout << "\n" << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";

    // Summary line
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s, BPM: " << r.bpm << ", Key: " << r.key.to_string()
              << color::reset << "\n";

    // Chord progression (if available)
    if (!r.chords.empty()) {
      std::cout << "  " << color::blue << "> Chord Progression: ";
      size_t max_chords = std::min(r.chords.size(), static_cast<size_t>(4));
      for (size_t i = 0; i < max_chords; ++i) {
        if (i > 0) std::cout << " -> ";
        std::cout << r.chords[i].to_string();
      }
      std::cout << color::reset << "\n";
    }

    // Structure
    if (!r.sections.empty()) {
      std::cout << "  " << color::blue << "> Structure: " << r.form << " (" << r.sections.size()
                << " sections)" << color::reset << "\n";

      // Section details
      std::cout << "  " << color::blue << "> Section Details (" << r.sections.size()
                << " sections):" << color::reset << "\n";

      for (size_t i = 0; i < r.sections.size(); ++i) {
        const auto& s = r.sections[i];
        int start_mm = static_cast<int>(s.start) / 60;
        int start_ss = static_cast<int>(s.start) % 60;
        float duration = s.end - s.start;
        int bars = static_cast<int>(std::round(duration / (4.0f * 60.0f / r.bpm)));

        std::string energy_sym = (s.energy_level < 0.33f)   ? "low E"
                                 : (s.energy_level < 0.67f) ? "mid E"
                                                            : "high E";

        printf("    %zu. %s (%02d:%02d, %dbars, %s)\n", i + 1, s.type_string().c_str(), start_mm,
               start_ss, bars, energy_sym.c_str());
      }
    }

    // Rhythm
    std::cout << "  " << color::blue << "> Rhythm: " << r.time_signature.numerator << "/"
              << r.time_signature.denominator << " time, " << r.rhythm.groove_type << " groove"
              << color::reset << "\n";

    // Timbre
    std::cout << "  " << color::magenta << "> Timbre: Brightness " << std::fixed
              << std::setprecision(1) << r.timbre.brightness << ", Warmth " << r.timbre.warmth
              << color::reset << "\n";

    // Dynamics
    std::cout << "  " << color::yellow << "> Dynamics: " << std::fixed << std::setprecision(1)
              << r.dynamics.dynamic_range_db << "dB range" << color::reset << "\n";

    // Final BPM and Key results
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << r.bpm << " BPM  (conf " << std::setprecision(1)
              << (r.bpm_confidence * 100.0f) << "%)" << color::reset << "\n";
    std::cout << "  " << color::magenta << color::bold << "> Estimated Key : " << r.key.to_string()
              << "  (conf " << std::fixed << std::setprecision(1) << (r.key.confidence * 100.0f)
              << "%)" << color::reset << "\n\n";
  }
  return 0;
}

// ============================================================================
// Processing Commands
// ============================================================================

int cmd_pitch_shift(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: pitch-shift requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("semitones")) {
    std::cerr << color::red << "Error: --semitones required" << color::reset << "\n";
    return 1;
  }

  float semitones = args.get_float("semitones", 0.0f);
  PitchShiftConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Pitch shifting by " << semitones << " semitones..."
              << color::reset << "\n";
  }

  Audio result = pitch_shift(audio, semitones, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("semitones", semitones)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_time_stretch(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: time-stretch requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("rate")) {
    std::cerr << color::red << "Error: --rate required" << color::reset << "\n";
    return 1;
  }

  float rate = args.get_float("rate", 1.0f);
  TimeStretchConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Time stretching with rate " << rate << "..." << color::reset << "\n";
  }

  Audio result = time_stretch(audio, rate, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("rate", rate)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_hpss(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: hpss requires output prefix (-o)" << color::reset << "\n";
    return 1;
  }

  HpssConfig config;
  config.kernel_size_harmonic = args.get_int("kernel-harmonic", 31);
  config.kernel_size_percussive = args.get_int("kernel-percussive", 31);
  config.use_soft_mask = !args.has("hard-mask");

  StftConfig stft{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Performing harmonic-percussive separation..." << color::reset << "\n";
  }

  std::string base = args.output_file;
  if (base.size() > 4 && base.substr(base.size() - 4) == ".wav") {
    base = base.substr(0, base.size() - 4);
  }

  auto save_audio = [](const std::string& path, const Audio& a) {
    save_wav(path, a.data(), a.size(), a.sample_rate());
  };

  if (args.has("harmonic-only")) {
    std::string path = base + ".wav";
    save_audio(path, harmonic(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved harmonic to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("harmonic", path).end_object().print();
  } else if (args.has("percussive-only")) {
    std::string path = base + ".wav";
    save_audio(path, percussive(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved percussive to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("percussive", path).end_object().print();
  } else if (args.has("with-residual")) {
    auto r = hpss_with_residual(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav",
                res = base + "_residual.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    save_audio(res, r.residual);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << ", " << res << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("harmonic", h)
          .kv("percussive", p)
          .kv("residual", res)
          .end_object()
          .print();
  } else {
    auto r = hpss(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder().begin_object().kv("harmonic", h).kv("percussive", p).end_object().print();
  }
  return 0;
}

// ============================================================================
// Feature Commands
// ============================================================================

int cmd_mel(const CliArgs& args, const Audio& audio) {
  MelConfig config;
  config.n_mels = args.n_mels;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.fmin = args.fmin;
  config.fmax = args.fmax > 0 ? args.fmax : static_cast<float>(audio.sample_rate()) / 2.0f;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  const float* data = mel.power_data();
  size_t total = static_cast<size_t>(mel.n_mels()) * static_cast<size_t>(mel.n_frames());

  float min_v = *std::min_element(data, data + total);
  float max_v = *std::max_element(data, data + total);
  float mean_v = std::accumulate(data, data + total, 0.0f) / static_cast<float>(total);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_mels", mel.n_mels())
        .kv("n_frames", mel.n_frames())
        .kv("duration", mel.duration())
        .kv("sample_rate", mel.sample_rate())
        .kv("hop_length", mel.hop_length())
        .key("stats")
        .begin_object()
        .kv("min", min_v)
        .kv("max", max_v)
        .kv("mean", mean_v)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Mel Spectrogram:\n";
    printf("  Shape:       %d bands x %d frames\n", mel.n_mels(), mel.n_frames());
    printf("  Duration:    %.2fs\n", mel.duration());
    printf("  Sample Rate: %d Hz\n", mel.sample_rate());
    printf("  Stats:       min=%.4f, max=%.4f, mean=%.4f\n", min_v, max_v, mean_v);
  }
  return 0;
}

int cmd_chroma(const CliArgs& args, const Audio& audio) {
  ChromaConfig config;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;

  Chroma chroma = Chroma::compute(audio, config);
  auto mean_energy = chroma.mean_energy();
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("n_chroma", chroma.n_chroma())
        .kv("n_frames", chroma.n_frames())
        .kv("duration", chroma.duration())
        .key("mean_energy")
        .begin_object();
    for (int i = 0; i < 12; ++i) json.kv(names[i], mean_energy[i]);
    json.end_object().end_object().print();
  } else {
    std::cout << "Chromagram:\n";
    printf("  Shape:    %d bins x %d frames\n", chroma.n_chroma(), chroma.n_frames());
    printf("  Duration: %.2fs\n", chroma.duration());
    std::cout << "\nMean Energy by Pitch Class:\n";
    float max_e = *std::max_element(mean_energy.begin(), mean_energy.end());
    for (int i = 0; i < 12; ++i) {
      int bar = static_cast<int>(mean_energy[i] / max_e * 20);
      printf("  %-2s: %.3f ", names[i], mean_energy[i]);
      for (int j = 0; j < bar; ++j) std::cout << "*";
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_spectral(const CliArgs& args, const Audio& audio) {
  StftConfig stft{args.n_fft, args.hop_length};
  Spectrogram spec = Spectrogram::compute(audio, stft);
  int sr = audio.sample_rate();

  auto centroid = spectral_centroid(spec, sr);
  auto bandwidth = spectral_bandwidth(spec, sr);
  auto rolloff = spectral_rolloff(spec, sr);
  auto flatness = spectral_flatness(spec);
  auto zcr = zero_crossing_rate(audio, args.n_fft, args.hop_length);
  auto rms = rms_energy(audio, args.n_fft, args.hop_length);

  Stats sc = Stats::compute(centroid), sb = Stats::compute(bandwidth), sr_s = Stats::compute(rolloff),
        sf = Stats::compute(flatness), sz = Stats::compute(zcr), se = Stats::compute(rms);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", centroid.size())
        .key("features")
        .begin_object()
        .key("centroid")
        .begin_object()
        .kv("mean", sc.mean)
        .kv("std", sc.std)
        .end_object()
        .key("bandwidth")
        .begin_object()
        .kv("mean", sb.mean)
        .kv("std", sb.std)
        .end_object()
        .key("rolloff")
        .begin_object()
        .kv("mean", sr_s.mean)
        .kv("std", sr_s.std)
        .end_object()
        .key("flatness")
        .begin_object()
        .kv("mean", sf.mean)
        .kv("std", sf.std)
        .end_object()
        .key("zcr")
        .begin_object()
        .kv("mean", sz.mean)
        .kv("std", sz.std)
        .end_object()
        .key("rms")
        .begin_object()
        .kv("mean", se.mean)
        .kv("std", se.std)
        .end_object()
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Spectral Features (" << centroid.size() << " frames):\n";
    printf("  Feature          Mean       Std        Min        Max\n");
    printf("  centroid         %-10.1f %-10.1f %-10.1f %.1f\n", sc.mean, sc.std, sc.min, sc.max);
    printf("  bandwidth        %-10.1f %-10.1f %-10.1f %.1f\n", sb.mean, sb.std, sb.min, sb.max);
    printf("  rolloff          %-10.1f %-10.1f %-10.1f %.1f\n", sr_s.mean, sr_s.std, sr_s.min,
           sr_s.max);
    printf("  flatness         %-10.4f %-10.4f %-10.4f %.4f\n", sf.mean, sf.std, sf.min, sf.max);
    printf("  zcr              %-10.4f %-10.4f %-10.4f %.4f\n", sz.mean, sz.std, sz.min, sz.max);
    printf("  rms              %-10.4f %-10.4f %-10.4f %.4f\n", se.mean, se.std, se.min, se.max);
  }
  return 0;
}

int cmd_pitch(const CliArgs& args, const Audio& audio) {
  PitchConfig config;
  config.fmin = args.get_float("fmin", 65.0f);
  config.fmax = args.get_float("fmax", 2093.0f);
  config.threshold = args.get_float("threshold", 0.3f);
  config.hop_length = args.hop_length;

  std::string algo = args.get_string("algorithm", "pyin");
  PitchResult result = (algo == "yin") ? yin_track(audio, config) : pyin(audio, config);

  int voiced = std::count(result.voiced_flag.begin(), result.voiced_flag.end(), true);
  float ratio = static_cast<float>(voiced) / static_cast<float>(result.n_frames());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("algorithm", algo)
        .kv("n_frames", result.n_frames())
        .kv("voiced_count", voiced)
        .kv("voiced_ratio", ratio)
        .kv("median_f0", result.median_f0())
        .kv("mean_f0", result.mean_f0())
        .end_object()
        .print();
  } else {
    std::cout << "Pitch Tracking (" << algo << "):\n";
    printf("  Frames:    %d\n", result.n_frames());
    printf("  Voiced:    %d (%.1f%%)\n", voiced, ratio * 100.0f);
    printf("  Median F0: %.1f Hz\n", result.median_f0());
    printf("  Mean F0:   %.1f Hz\n", result.mean_f0());
  }
  return 0;
}

int cmd_onset_env(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.n_fft;
  mel_config.hop_length = args.hop_length;
  mel_config.n_mels = args.n_mels;

  auto envelope = compute_onset_strength(audio, mel_config);

  auto max_it = std::max_element(envelope.begin(), envelope.end());
  float peak = *max_it;
  int peak_frame = static_cast<int>(std::distance(envelope.begin(), max_it));
  float peak_time = static_cast<float>(peak_frame * args.hop_length) / audio.sample_rate();
  float mean = std::accumulate(envelope.begin(), envelope.end(), 0.0f) /
               static_cast<float>(envelope.size());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", envelope.size())
        .kv("duration", audio.duration())
        .kv("hop_length", args.hop_length)
        .kv("peak_time", peak_time)
        .kv("peak_strength", peak)
        .kv("mean", mean)
        .end_object()
        .print();
  } else {
    std::cout << "Onset Strength Envelope:\n";
    printf("  Frames:        %zu\n", envelope.size());
    printf("  Duration:      %.2fs\n", audio.duration());
    printf("  Peak Time:     %.2fs\n", peak_time);
    printf("  Peak Strength: %.3f\n", peak);
    printf("  Mean:          %.3f\n", mean);
  }
  return 0;
}

int cmd_cqt(const CliArgs& args, const Audio& audio) {
  CqtConfig config;
  config.hop_length = args.hop_length;
  config.fmin = args.get_float("fmin", 32.7f);
  config.n_bins = args.get_int("n-bins", 84);
  config.bins_per_octave = args.get_int("bins-per-octave", 12);

  CqtResult result = cqt(audio, config);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_bins", result.n_bins())
        .kv("n_frames", result.n_frames())
        .kv("duration", result.duration())
        .kv("fmin", config.fmin)
        .kv("bins_per_octave", config.bins_per_octave)
        .end_object()
        .print();
  } else {
    float fmax = config.fmin * std::pow(2.0f, static_cast<float>(config.n_bins) /
                                                   static_cast<float>(config.bins_per_octave));
    int octaves = config.n_bins / config.bins_per_octave;
    std::cout << "Constant-Q Transform:\n";
    printf("  Shape:           %d bins x %d frames\n", result.n_bins(), result.n_frames());
    printf("  Frequency Range: %.1f - %.1f Hz (%d octaves)\n", config.fmin, fmax, octaves);
    printf("  Duration:        %.2fs\n", result.duration());
  }
  return 0;
}

// ============================================================================
// Command Registry
// ============================================================================

struct CommandInfo {
  std::string name;
  std::string description;
  CommandHandler handler;
  bool requires_audio;
};

const std::vector<CommandInfo>& get_commands() {
  static std::vector<CommandInfo> commands = {
      // Analysis
      {"analyze", "Full music analysis", cmd_analyze, true},
      {"bpm", "Detect BPM only", cmd_bpm, true},
      {"key", "Detect key only", cmd_key, true},
      {"beats", "Detect beat times", cmd_beats, true},
      {"onsets", "Detect onset times", cmd_onsets, true},
      {"chords", "Detect chord progression", cmd_chords, true},
      {"sections", "Detect song structure", cmd_sections, true},
      {"timbre", "Analyze timbral characteristics", cmd_timbre, true},
      {"dynamics", "Analyze dynamics/loudness", cmd_dynamics, true},
      {"rhythm", "Analyze rhythm features", cmd_rhythm, true},
      {"melody", "Track melody/pitch contour", cmd_melody, true},
      {"boundaries", "Detect structural boundaries", cmd_boundaries, true},
      // Processing
      {"pitch-shift", "Shift pitch by semitones", cmd_pitch_shift, true},
      {"time-stretch", "Time stretch audio", cmd_time_stretch, true},
      {"hpss", "Harmonic-percussive separation", cmd_hpss, true},
      // Features
      {"mel", "Compute mel spectrogram", cmd_mel, true},
      {"chroma", "Compute chromagram", cmd_chroma, true},
      {"spectral", "Compute spectral features", cmd_spectral, true},
      {"pitch", "Track pitch (YIN/pYIN)", cmd_pitch, true},
      {"onset-env", "Compute onset strength envelope", cmd_onset_env, true},
      {"cqt", "Compute Constant-Q Transform", cmd_cqt, true},
      // Utility
      {"info", "Show audio file information", cmd_info, true},
  };
  return commands;
}

const CommandInfo* find_command(const std::string& name) {
  for (const auto& cmd : get_commands()) {
    if (cmd.name == name) return &cmd;
  }
  return nullptr;
}

// ============================================================================
// Usage
// ============================================================================

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " <command> [options] <audio_file> [-o output]\n\n";

  std::cerr << "ANALYSIS COMMANDS:\n";
  for (const auto& cmd : get_commands()) {
    if (cmd.name == "pitch-shift") std::cerr << "\nPROCESSING COMMANDS:\n";
    if (cmd.name == "mel") std::cerr << "\nFEATURE COMMANDS:\n";
    if (cmd.name == "info") std::cerr << "\nUTILITY COMMANDS:\n";
    fprintf(stderr, "  %-14s %s\n", cmd.name.c_str(), cmd.description.c_str());
  }
  std::cerr << "  version        Show library version\n";

  std::cerr << "\nGLOBAL OPTIONS:\n"
            << "  --json             Output results in JSON format\n"
            << "  --quiet, -q        Suppress progress output\n"
            << "  --help, -h         Show help\n"
            << "  -o, --output       Output file path\n"
            << "  --n-fft <int>      FFT size (default: 2048)\n"
            << "  --hop-length <int> Hop length (default: 512)\n"
            << "\nExamples:\n"
            << "  " << prog << " analyze music.mp3\n"
            << "  " << prog << " bpm music.wav --json\n"
            << "  " << prog << " pitch-shift --semitones 3 input.wav -o output.wav\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  CliArgs args = ArgParser::parse(argc, argv);

  if (args.help) {
    print_usage(argv[0]);
    return 0;
  }

  if (args.command.empty()) {
    std::cerr << color::red << "Error: No command specified" << color::reset << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  // Version command (no audio needed)
  if (args.command == "version") {
    return cmd_version(args);
  }

  // Find command
  const CommandInfo* cmd = find_command(args.command);
  if (!cmd) {
    std::cerr << color::red << "Error: Unknown command '" << args.command << "'" << color::reset << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  // Check for audio file
  if (cmd->requires_audio && args.input_file.empty()) {
    std::cerr << color::red << "Error: Missing audio file" << color::reset << "\n\n";
    print_usage(argv[0]);
    return 1;
  }

  try {
    if (!args.quiet && !args.json_output) {
      std::cerr << color::blue << "Loading " << basename(args.input_file) << "..."
                << color::reset << std::flush;
    }

    auto [samples, sample_rate] = load_audio(args.input_file);
    if (samples.empty()) {
      std::cerr << "\n" << color::red << "Error: Failed to load audio file" << color::reset << "\n";
      return 1;
    }

    Audio audio = Audio::from_vector(std::move(samples), sample_rate);

    if (!args.quiet && !args.json_output) {
      std::cerr << "\r" << color::green << "Loaded " << std::fixed << std::setprecision(1)
                << audio.duration() << "s @ " << sample_rate << "Hz" << color::reset
                << "                    \n";
    }

    return cmd->handler(args, audio);

  } catch (const std::exception& e) {
    std::cerr << "\n" << color::red << "Error: " << e.what() << color::reset << "\n";
    return 1;
  }
}
