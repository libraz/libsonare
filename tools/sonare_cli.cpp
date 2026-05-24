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
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/meter/lufs.h"
#include "analysis/music_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/audio_io.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "effects/hpss.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/silence.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#ifdef SONARE_WITH_MASTERING
#include "mastering/api/named_processor.h"
#include "mastering/maximizer/loudness_optimize.h"
#endif
#include "cli_support.h"
#include "quick.h"
#include "sonare.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/vector_normalize.h"

using namespace sonare;

using CommandHandler = std::function<int(const CliArgs&, const Audio&)>;

std::vector<float> parse_float_list(const std::string& text) {
  std::vector<float> values;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) values.push_back(std::stof(item));
  }
  if (values.empty()) throw std::invalid_argument("--values must contain at least one number");
  return values;
}

std::vector<int> parse_int_list(const std::string& text) {
  std::vector<int> values;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) values.push_back(std::stoi(item));
  }
  if (values.empty()) throw std::invalid_argument("--values must contain at least one integer");
  return values;
}

PitchClass parse_pitch_class_option(const std::string& value) {
  static const std::map<std::string, PitchClass> names = {
      {"C", PitchClass::C},   {"C#", PitchClass::Cs}, {"DB", PitchClass::Cs},
      {"D", PitchClass::D},   {"D#", PitchClass::Ds}, {"EB", PitchClass::Ds},
      {"E", PitchClass::E},   {"F", PitchClass::F},   {"F#", PitchClass::Fs},
      {"GB", PitchClass::Fs}, {"G", PitchClass::G},   {"G#", PitchClass::Gs},
      {"AB", PitchClass::Gs}, {"A", PitchClass::A},   {"A#", PitchClass::As},
      {"BB", PitchClass::As}, {"B", PitchClass::B},
  };
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  auto it = names.find(key);
  if (it == names.end()) {
    throw std::invalid_argument("invalid pitch class: " + value);
  }
  return it->second;
}

Mode parse_mode_option(const std::string& value) {
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "major" || key == "maj") return Mode::Major;
  if (key == "minor" || key == "min" || key == "m") return Mode::Minor;
  if (key == "dorian") return Mode::Dorian;
  if (key == "phrygian") return Mode::Phrygian;
  if (key == "lydian") return Mode::Lydian;
  if (key == "mixolydian") return Mode::Mixolydian;
  if (key == "locrian") return Mode::Locrian;
  throw std::invalid_argument("invalid mode: " + value);
}

std::vector<Mode> parse_mode_list_option(const std::string& value) {
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "all" || key == "modal") {
    return {Mode::Major,    Mode::Minor,      Mode::Dorian,     Mode::Phrygian,
            Mode::Lydian,   Mode::Mixolydian, Mode::Locrian};
  }
  if (key == "major-minor" || key == "majmin" || key == "diatonic") {
    return {Mode::Major, Mode::Minor};
  }

  std::vector<Mode> modes;
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) {
      modes.push_back(parse_mode_option(item));
    }
  }
  if (modes.empty()) {
    throw std::invalid_argument("--modes must contain at least one mode");
  }
  return modes;
}

KeyProfileType parse_key_profile_option(const std::string& value) {
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "ks" || key == "krumhansl" || key == "krumhansl-schmuckler") {
    return KeyProfileType::KrumhanslSchmuckler;
  }
  if (key == "temperley") return KeyProfileType::Temperley;
  if (key == "shaath" || key == "keyfinder") return KeyProfileType::Shaath;
  if (key == "faraldo-edmt" || key == "edmt") return KeyProfileType::FaraldoEDMT;
  if (key == "faraldo-edma" || key == "edma") return KeyProfileType::FaraldoEDMA;
  if (key == "faraldo-edmm" || key == "edmm") return KeyProfileType::FaraldoEDMM;
  if (key == "bellman-budge" || key == "bellman" || key == "budge") {
    return KeyProfileType::BellmanBudge;
  }
  throw std::invalid_argument("invalid key profile: " + value);
}

void print_float_values(const CliArgs& args, const std::vector<float>& values) {
  if (args.json_output) {
    JsonBuilder().float_array(values).print();
    return;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) std::cout << ",";
    std::cout << values[i];
  }
  std::cout << "\n";
}

void print_int_values(const CliArgs& args, const std::vector<int>& values) {
  JsonBuilder json;
  if (args.json_output) {
    json.begin_array();
    for (int value : values) json.value(value);
    json.end_array().print();
    return;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) std::cout << ",";
    std::cout << values[i];
  }
  std::cout << "\n";
}

std::vector<float> require_float_values(const CliArgs& args) {
  if (!args.has("values")) throw std::invalid_argument("--values required");
  return parse_float_list(args.get_string("values"));
}

std::vector<int> require_int_values(const CliArgs& args) {
  if (!args.has("values")) throw std::invalid_argument("--values required");
  return parse_int_list(args.get_string("values"));
}

// ============================================================================
// Command Implementations
// ============================================================================

int cmd_version(const CliArgs& args) {
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("cli_version", "1.0.0")
        .kv("lib_version", version())
        .end_object()
        .print();
  } else {
    std::cout << "sonare-cli version 1.0.0\n";
    std::cout << "libsonare version " << version() << "\n";
  }
  return 0;
}

int cmd_system_info(const CliArgs& args) {
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .key("cpu")
        .begin_object()
        .kv("logical_cores", system_info::logical_cores())
        .kv("physical_cores", system_info::physical_cores())
        .end_object()
        .key("memory")
        .begin_object()
        .kv("total_gb",
            static_cast<float>(system_info::total_memory_bytes()) / (1024.0f * 1024.0f * 1024.0f))
        .kv("available_gb", static_cast<float>(system_info::available_memory_bytes()) /
                                (1024.0f * 1024.0f * 1024.0f))
        .end_object()
        .key("parallel")
        .begin_object()
        .kv("enabled", system_info::parallel_enabled())
        .kv("workers", system_info::parallel_workers())
        .kv("strategy", system_info::parallel_strategy())
        .end_object()
        .end_object()
        .print();
  } else {
    float total_gb =
        static_cast<float>(system_info::total_memory_bytes()) / (1024.0f * 1024.0f * 1024.0f);
    float avail_gb =
        static_cast<float>(system_info::available_memory_bytes()) / (1024.0f * 1024.0f * 1024.0f);

    std::cout << color::cyan << color::bold << "System Information" << color::reset << "\n";
    std::cout << "  CPU Cores: " << system_info::logical_cores() << " logical, "
              << system_info::physical_cores() << " physical\n";
    printf("  Memory: %.1f GB total, %.1f GB available\n", total_gb, avail_gb);
    std::cout << "\n"
              << color::green << color::bold << "Parallel Configuration" << color::reset << "\n";
    std::cout << "  Parallel Enabled: " << (system_info::parallel_enabled() ? "yes" : "no") << "\n";
    std::cout << "  Workers: " << system_info::parallel_workers() << "\n";
    std::cout << "  Strategy: " << system_info::parallel_strategy() << "\n";
  }
  return 0;
}

int cmd_frames_to_samples(const CliArgs& args, const Audio&) {
  const int frames = args.get_int("frames", 0);
  const int hop_length = args.get_int("hop-length", args.hop_length);
  const int n_fft = args.get_int("n-fft", 0);
  const int samples = frames_to_samples(frames, hop_length, n_fft);
  if (args.json_output) {
    JsonBuilder().begin_object().kv("samples", samples).end_object().print();
  } else {
    std::cout << samples << "\n";
  }
  return 0;
}

int cmd_samples_to_frames(const CliArgs& args, const Audio&) {
  const int samples = args.get_int("samples", 0);
  const int hop_length = args.get_int("hop-length", args.hop_length);
  const int n_fft = args.get_int("n-fft", 0);
  const int frames = samples_to_frames(samples, hop_length, n_fft);
  if (args.json_output) {
    JsonBuilder().begin_object().kv("frames", frames).end_object().print();
  } else {
    std::cout << frames << "\n";
  }
  return 0;
}

int cmd_power_to_db(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, power_to_db(values, args.get_float("ref", 1.0f),
                                       args.get_float("amin", 1e-10f),
                                       args.get_float("top-db", 80.0f)));
  return 0;
}

int cmd_amplitude_to_db(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, amplitude_to_db(values, args.get_float("ref", 1.0f),
                                           args.get_float("amin", 1e-5f),
                                           args.get_float("top-db", 80.0f)));
  return 0;
}

int cmd_db_to_power(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, db_to_power(values, args.get_float("ref", 1.0f)));
  return 0;
}

int cmd_db_to_amplitude(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, db_to_amplitude(values, args.get_float("ref", 1.0f)));
  return 0;
}

int cmd_frame_signal(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const int frame_length = args.get_int("frame-length", args.n_fft);
  const int hop_length = args.get_int("hop-length", args.hop_length);
  auto frames = frame(values, frame_length, hop_length);
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", frame_count(values.size(), frame_length, hop_length))
        .key("frames")
        .float_array(frames)
        .end_object()
        .print();
  } else {
    print_float_values(args, frames);
  }
  return 0;
}

int cmd_pad_center(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, pad_center(values, static_cast<size_t>(args.get_int("size", 0)),
                                      args.get_float("pad-value", 0.0f)));
  return 0;
}

int cmd_fix_length(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(args, fix_length(values, static_cast<size_t>(args.get_int("size", 0)),
                                      args.get_float("pad-value", 0.0f)));
  return 0;
}

int cmd_fix_frames(const CliArgs& args, const Audio&) {
  auto values = require_int_values(args);
  print_int_values(args, fix_frames(values, args.get_int("x-min", 0), args.get_int("x-max", -1),
                                    !args.has("no-pad")));
  return 0;
}

int cmd_peak_pick(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_int_values(args, peak_pick(values, args.get_int("pre-max", 1), args.get_int("post-max", 1),
                                   args.get_int("pre-avg", 1), args.get_int("post-avg", 1),
                                   args.get_float("delta", 0.0f), args.get_int("wait", 0)));
  return 0;
}

int cmd_vector_normalize(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  const int norm_type = args.get_int("norm-type", 0);
  NormType norm = NormType::Inf;
  if (norm_type == 1) norm = NormType::L1;
  if (norm_type == 2) norm = NormType::L2;
  if (norm_type == 3) norm = NormType::Power;
  print_float_values(args, normalize(values, norm, args.get_float("threshold", 1e-12f)));
  return 0;
}

int cmd_pcen(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  PcenConfig config;
  config.sr = args.get_int("sample-rate", config.sr);
  config.hop_length = args.get_int("hop-length", config.hop_length);
  config.time_constant = args.get_float("time-constant", config.time_constant);
  config.gain = args.get_float("gain", config.gain);
  config.bias = args.get_float("bias", config.bias);
  config.power = args.get_float("power", config.power);
  config.eps = args.get_float("eps", config.eps);
  print_float_values(args, pcen(values, args.get_int("n-bins", 0), args.get_int("n-frames", 0),
                                config));
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << color::bold << "> Estimated BPM : " << std::fixed
              << std::setprecision(2) << bpm << " BPM" << color::reset << "\n\n";
  }
  return 0;
}

int cmd_key(const CliArgs& args, const Audio& audio) {
  KeyConfig config;
  config.n_fft = args.n_fft == 2048 ? 4096 : args.n_fft;
  config.hop_length = args.hop_length;
  config.use_hpss = args.has("use-hpss") || args.has("hpss");
  config.loudness_weighted = args.has("loudness-weighted");
  config.high_pass_hz = args.get_float("high-pass-hz", 0.0f);
  if (args.has("genre-hint")) {
    config.genre_hint = args.get_string("genre-hint");
  }
  if (args.has("profile")) {
    config.profile_type = parse_key_profile_option(args.get_string("profile"));
  }
  if (args.has("modes")) {
    config.modes = parse_mode_list_option(args.get_string("modes"));
  }

  auto candidates =
      quick::detect_key_candidates(audio.data(), audio.size(), audio.sample_rate(), config);
  Key key = candidates.empty() ? quick::detect_key(audio.data(), audio.size(), audio.sample_rate())
                               : candidates.front().key;
  int candidate_count = 0;
  if (args.has("candidates")) {
    const std::string value = args.get_string("candidates");
    candidate_count = (value == "true") ? 5 : std::max(0, std::stoi(value));
    candidate_count = std::min(candidate_count, static_cast<int>(candidates.size()));
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("root", static_cast<int>(key.root))
        .kv("mode", static_cast<int>(key.mode))
        .kv("confidence", key.confidence)
        .kv("name", key.to_string());
    if (candidate_count > 0) {
      json.key("candidates").begin_array();
      for (int i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[static_cast<size_t>(i)];
        json.begin_object()
            .kv("root", static_cast<int>(candidate.key.root))
            .kv("mode", static_cast<int>(candidate.key.mode))
            .kv("confidence", candidate.key.confidence)
            .kv("name", candidate.key.to_string())
            .kv("correlation", candidate.correlation)
            .end_object();
      }
      json.end_array();
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::magenta << color::bold << "> Estimated Key : " << key.to_string()
              << "  (conf " << std::fixed << std::setprecision(1) << (key.confidence * 100.0f)
              << "%)" << color::reset << "\n\n";
    if (candidate_count > 0) {
      std::cout << "  " << color::blue << "> Key candidates:" << color::reset << "\n";
      for (int i = 0; i < candidate_count; ++i) {
        const auto& candidate = candidates[static_cast<size_t>(i)];
        std::cout << "    " << std::setw(2) << (i + 1) << ". " << candidate.key.to_string()
                  << "  corr " << std::fixed << std::setprecision(3) << candidate.correlation
                  << "  conf " << std::setprecision(1) << (candidate.key.confidence * 100.0f)
                  << "%\n";
      }
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_beats(const CliArgs& args, const Audio& audio) {
  auto beats = quick::detect_beats(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(beats).print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << beats.size() << " beats" << color::reset
              << "\n";
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

int cmd_downbeats(const CliArgs& args, const Audio& audio) {
  auto downbeats = quick::detect_downbeats(audio.data(), audio.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder().float_array(downbeats).print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << downbeats.size() << " downbeats"
              << color::reset << "\n";
    std::cout << "  " << color::blue << "> Downbeat times:" << color::reset << "\n    ";
    for (size_t i = 0; i < downbeats.size(); ++i) {
      printf("%.2f", downbeats[i]);
      if (i < downbeats.size() - 1) std::cout << ", ";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::green << "> Detected " << onsets.size() << " onsets" << color::reset
              << "\n";
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
  config.chroma_method = args.has("nnls") ? ChromaMethod::NNLS : ChromaMethod::STFT;
  config.use_hmm = args.has("use-hmm");
  config.hmm_beam_width = args.get_int("hmm-beam-width", config.hmm_beam_width);
  config.detect_inversions = args.has("detect-inversions");
  config.use_key_context = args.has("key-context");
  if (config.use_key_context) {
    config.key_root = parse_pitch_class_option(args.get_string("key-root", "C"));
    config.key_mode = parse_mode_option(args.get_string("key-mode", "major"));
  }

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
          .kv("bass", static_cast<int>(c.bass))
          .kv("start", c.start)
          .kv("end", c.end)
          .kv("confidence", c.confidence)
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s, " << chords.size() << " chord changes" << color::reset
              << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Duration: " << std::fixed << std::setprecision(1)
              << audio.duration() << "s" << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structure: " << analyzer.form() << " ("
              << sections.size() << " sections)" << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Structural Boundaries (" << bounds.size()
              << " detected):" << color::reset << "\n";
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

  if (!args.quiet && !args.json_output) {
    int workers = system_info::parallel_workers();
    if (system_info::parallel_enabled()) {
      std::cerr << color::green << "Parallel analysis: " << workers << " workers ("
                << system_info::parallel_strategy() << ")" << color::reset << "\n";
    }
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
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";

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
    std::cerr << color::red << "Error: pitch-shift requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("semitones")) {
    std::cerr << color::red << "Error: --semitones required" << color::reset << "\n";
    return 1;
  }

  float semitones = args.get_float("semitones", 0.0f);
  PitchShiftConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Pitch shifting by " << semitones << " semitones..." << color::reset
              << "\n";
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
    std::cerr << color::red << "Error: time-stretch requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("rate")) {
    std::cerr << color::red << "Error: --rate required" << color::reset << "\n";
    return 1;
  }

  float rate = args.get_float("rate", 1.0f);
  TimeStretchConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Time stretching with rate " << rate << "..." << color::reset
              << "\n";
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
    std::cerr << color::blue << "Performing harmonic-percussive separation..." << color::reset
              << "\n";
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
      std::cerr << color::green << "Saved: " << h << ", " << p << ", " << res << color::reset
                << "\n";
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

int cmd_preemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: preemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = preemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_deemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: deemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = deemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_trim_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  std::vector<float> input(audio.begin(), audio.end());
  TrimResult result = sonare::trim(input, top_db, args.n_fft, args.hop_length);

  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.audio.data(), result.audio.size(), audio.sample_rate());
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("start_sample", result.start_sample)
        .kv("end_sample", result.end_sample)
        .kv("samples", result.audio.size())
        .kv("top_db", top_db)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "Silence Trim:\n";
    printf("  Range:   %d - %d\n", result.start_sample, result.end_sample);
    printf("  Samples: %zu\n", result.audio.size());
    if (!args.output_file.empty()) std::cout << "  Output:  " << args.output_file << "\n";
  }
  return 0;
}

int cmd_split_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  std::vector<float> input(audio.begin(), audio.end());
  auto ranges = sonare::split(input, top_db, args.n_fft, args.hop_length);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_array();
    for (const auto& range : ranges) {
      json.begin_object()
          .kv("start_sample", range.first)
          .kv("end_sample", range.second)
          .end_object();
    }
    json.end_array().print();
  } else {
    std::cout << "Non-silent intervals: " << ranges.size() << "\n";
    for (const auto& range : ranges) {
      printf("  %d - %d\n", range.first, range.second);
    }
  }
  return 0;
}

#ifdef SONARE_WITH_MASTERING
std::vector<mastering::api::Param> parse_mastering_params(const std::string& text) {
  std::vector<mastering::api::Param> params;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos) continue;
    params.push_back({item.substr(0, eq), std::stod(item.substr(eq + 1))});
  }
  return params;
}

int cmd_mastering(const CliArgs& args, const Audio& audio) {
  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = args.get_float("target-lufs", -14.0f);
  config.ceiling_db = args.get_float("ceiling-db", -1.0f);
  config.true_peak_oversample = args.get_int("true-peak-oversample", 4);

  const auto result = mastering::maximizer::loudness_optimize(audio, config);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.audio.data(), result.audio.size(), result.audio.sample_rate(),
             args.get_int("bits", 16));
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("target_lufs", config.target_lufs)
        .kv("ceiling_db", config.ceiling_db)
        .kv("true_peak_oversample", config.true_peak_oversample)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Mastering" << color::reset << "\n"
              << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
              << "\n"
              << "  Output LUFS:     " << result.output_lufs << "\n"
              << "  Applied Gain:    " << result.applied_gain_db << " dB\n";
    if (!args.output_file.empty()) {
      std::cout << "  Output:          " << args.output_file << "\n";
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_mastering_processor(const CliArgs& args, const Audio& audio) {
  const std::string processor = args.get_string("processor");
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const auto params = parse_mastering_params(args.get_string("params"));
  const auto result = mastering::api::apply_named_processor(processor, audio.data(), audio.size(),
                                                            audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
             args.get_int("bits", 16));
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("processor", processor)
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("latency_samples", result.latency_samples)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Mastering Processor" << color::reset << "\n"
              << "  Processor:       " << processor << "\n"
              << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
              << "\n"
              << "  Output LUFS:     " << result.output_lufs << "\n"
              << "  Applied Gain:    " << result.applied_gain_db << " dB\n"
              << "  Latency:         " << result.latency_samples << " samples\n";
    if (!args.output_file.empty()) std::cout << "  Output:          " << args.output_file << "\n";
    std::cout << "\n";
  }
  return 0;
}

int cmd_mastering_processors(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::processor_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("processors").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_mastering_pair_processors(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::pair_processor_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("processors").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_mastering_pair_analyses(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::pair_analysis_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("analyses").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_mastering_stereo_analyses(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::stereo_analysis_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("analyses").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

Audio load_reference_audio(const CliArgs& args, int expected_sample_rate, size_t expected_size) {
  const std::string path = args.get_string("reference");
  if (path.empty()) {
    throw std::invalid_argument("--reference is required");
  }
  auto [samples, sample_rate] = load_audio(path);
  if (sample_rate != expected_sample_rate) {
    throw std::invalid_argument("reference sample rate must match input sample rate");
  }
  if (samples.size() != expected_size) {
    throw std::invalid_argument("reference length must match input length");
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

int cmd_mastering_pair_processor(const CliArgs& args, const Audio& audio) {
  const std::string processor = args.get_string("processor");
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const Audio reference = load_reference_audio(args, audio.sample_rate(), audio.size());
  const auto params = parse_mastering_params(args.get_string("params"));
  const auto result = mastering::api::apply_named_pair_processor(
      processor, audio.data(), reference.data(), audio.size(), audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
             args.get_int("bits", 16));
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("processor", processor)
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("latency_samples", result.latency_samples)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Mastering Pair Processor" << color::reset << "\n"
              << "  Processor:       " << processor << "\n"
              << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
              << "\n"
              << "  Output LUFS:     " << result.output_lufs << "\n"
              << "  Applied Gain:    " << result.applied_gain_db << " dB\n"
              << "  Latency:         " << result.latency_samples << " samples\n";
    if (!args.output_file.empty()) std::cout << "  Output:          " << args.output_file << "\n";
    std::cout << "\n";
  }
  return 0;
}

int cmd_mastering_pair_analyze(const CliArgs& args, const Audio& audio) {
  const std::string analysis = args.get_string("analysis");
  if (analysis.empty()) {
    std::cerr << color::red << "Error: --analysis is required" << color::reset << "\n";
    return 1;
  }
  const Audio reference = load_reference_audio(args, audio.sample_rate(), audio.size());
  const auto params = parse_mastering_params(args.get_string("params"));
  std::cout << mastering::api::analyze_named_pair(analysis, audio.data(), reference.data(),
                                                  audio.size(), audio.sample_rate(), params)
            << "\n";
  return 0;
}

int cmd_mastering_stereo_analyze(const CliArgs& args, const Audio& audio) {
  const std::string analysis = args.get_string("analysis");
  if (analysis.empty()) {
    std::cerr << color::red << "Error: --analysis is required" << color::reset << "\n";
    return 1;
  }
  const Audio right = load_reference_audio(args, audio.sample_rate(), audio.size());
  const auto params = parse_mastering_params(args.get_string("params"));
  std::cout << mastering::api::analyze_named_stereo(analysis, audio.data(), right.data(),
                                                    audio.size(), audio.sample_rate(), params)
            << "\n";
  return 0;
}
#endif

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

int cmd_tonnetz(const CliArgs& args, const Audio& audio) {
  ChromaConfig config;
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  Chroma chroma = Chroma::compute(audio, config);
  auto values = tonnetz(chroma);
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_dims", 6)
        .kv("n_frames", chroma.n_frames())
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Tonnetz:\n";
    printf("  Shape: %d dims x %d frames\n", 6, chroma.n_frames());
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std,
           stats.min, stats.max);
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

  Stats sc = Stats::compute(centroid), sb = Stats::compute(bandwidth),
        sr_s = Stats::compute(rolloff), sf = Stats::compute(flatness), sz = Stats::compute(zcr),
        se = Stats::compute(rms);

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
  float mean =
      std::accumulate(envelope.begin(), envelope.end(), 0.0f) / static_cast<float>(envelope.size());

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

int cmd_tempogram(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.hop_length;
  config.win_length = args.get_int("win-length", 384);
  auto values = tempogram(audio, config);
  const int n_frames = config.win_length > 0 ? static_cast<int>(values.size()) / config.win_length : 0;
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("win_length", config.win_length)
        .kv("n_frames", n_frames)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Tempogram:\n";
    printf("  Shape: %d lags x %d frames\n", config.win_length, n_frames);
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std,
           stats.min, stats.max);
  }
  return 0;
}

int cmd_plp(const CliArgs& args, const Audio& audio) {
  PlpConfig config;
  config.sr = audio.sample_rate();
  config.hop_length = args.hop_length;
  config.tempo_min = args.get_float("tempo-min", 30.0f);
  config.tempo_max = args.get_float("tempo-max", 300.0f);
  config.win_length = args.get_int("win-length", 384);
  auto values = plp(audio, config);
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("n_frames", values.size())
        .kv("tempo_min", config.tempo_min)
        .kv("tempo_max", config.tempo_max)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Predominant Local Pulse:\n";
    printf("  Frames: %zu\n", values.size());
    printf("  Stats:  mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std,
           stats.min, stats.max);
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

int cmd_acoustic(const CliArgs& args, const Audio& audio) {
  AcousticConfig config;
  config.n_octave_bands = args.get_int("n-bands", config.n_octave_bands);
  config.min_decay_db = args.get_float("min-decay-db", config.min_decay_db);
  config.noise_floor_margin_db =
      args.get_float("noise-floor-margin-db", config.noise_floor_margin_db);

  const bool use_ir = args.has("ir");
  AcousticParameters p = use_ir ? analyze_impulse_response(audio, config)
                                : detect_acoustic(audio, config);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("rt60", p.rt60)
        .kv("edt", p.edt)
        .kv("c50", p.c50)
        .kv("c80", p.c80)
        .kv("d50", p.d50)
        .kv("confidence", p.confidence)
        .kv("is_blind", p.is_blind)
        .key("rt60_bands")
        .float_array(p.rt60_bands)
        .key("edt_bands")
        .float_array(p.edt_bands)
        .key("c50_bands")
        .float_array(p.c50_bands)
        .key("c80_bands")
        .float_array(p.c80_bands)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::blue << "> Acoustic Analysis ("
              << (p.is_blind ? "blind" : "impulse response") << "):" << color::reset << "\n";
    printf("    RT60:       %.3f s\n", p.rt60);
    printf("    EDT:        %.3f s\n", p.edt);
    printf("    C50:        %.2f dB\n", p.c50);
    printf("    C80:        %.2f dB\n", p.c80);
    printf("    D50:        %.2f\n", p.d50);
    printf("    Confidence: %.2f\n", p.confidence);
    if (!p.rt60_bands.empty()) {
      std::cout << "    RT60 bands: ";
      for (size_t i = 0; i < p.rt60_bands.size(); ++i) {
        if (i > 0) std::cout << ", ";
        printf("%.3f", p.rt60_bands[i]);
      }
      std::cout << "\n";
    }
    std::cout << "\n";
  }
  return 0;
}

int cmd_onset_envelope(const CliArgs& args, const Audio& audio) {
  MelConfig mel_config;
  mel_config.n_fft = args.n_fft;
  mel_config.hop_length = args.hop_length;
  mel_config.n_mels = args.n_mels;

  auto envelope = compute_onset_strength(audio, mel_config, OnsetConfig());
  Stats stats = Stats::compute(envelope);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("count", envelope.size())
        .kv("hop_length", args.hop_length)
        .kv("duration", audio.duration())
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .key("values")
        .float_array(envelope)
        .end_object()
        .print();
  } else {
    std::cout << "Onset Strength Envelope:\n";
    printf("  Frames: %zu\n", envelope.size());
    printf("  Stats:  mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_fourier_tempogram(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.hop_length;
  config.win_length = args.get_int("win-length", config.win_length);

  auto values = fourier_tempogram(audio, config);
  const int n_bins = config.win_length / 2 + 1;
  const int n_frames = n_bins > 0 ? static_cast<int>(values.size()) / n_bins : 0;
  Stats stats = Stats::compute(values);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("win_length", config.win_length)
        .kv("n_bins", n_bins)
        .kv("n_frames", n_frames)
        .key("stats")
        .begin_object()
        .kv("mean", stats.mean)
        .kv("std", stats.std)
        .kv("min", stats.min)
        .kv("max", stats.max)
        .end_object()
        .end_object()
        .print();
  } else {
    std::cout << "Fourier Tempogram:\n";
    printf("  Shape: %d bins x %d frames\n", n_bins, n_frames);
    printf("  Stats: mean=%.4f, std=%.4f, min=%.4f, max=%.4f\n", stats.mean, stats.std, stats.min,
           stats.max);
  }
  return 0;
}

int cmd_tempogram_ratio(const CliArgs& args, const Audio& audio) {
  TempogramConfig config;
  config.hop_length = args.hop_length;
  config.win_length = args.get_int("win-length", config.win_length);

  auto tempogram_data = tempogram(audio, config);
  static const std::vector<float> factors = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
  auto ratios = tempogram_ratio(tempogram_data, config.win_length, audio.sample_rate(),
                                config.hop_length, factors);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("win_length", config.win_length).key("ratios").begin_array();
    for (size_t i = 0; i < ratios.size(); ++i) {
      json.begin_object()
          .kv("factor", factors[i])
          .kv("value", ratios[i])
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "Tempogram Ratio:\n";
    for (size_t i = 0; i < ratios.size(); ++i) {
      printf("  factor %.1f: %.4f\n", factors[i], ratios[i]);
    }
  }
  return 0;
}

int cmd_nnls_chroma(const CliArgs& args, const Audio& audio) {
  NnlsChromaConfig config;
  config.cqt.hop_length = args.hop_length;

  Chroma chroma = nnls_chroma(audio, config);
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
    std::cout << "NNLS Chromagram:\n";
    printf("  Shape:    %d bins x %d frames\n", chroma.n_chroma(), chroma.n_frames());
    printf("  Duration: %.2fs\n", chroma.duration());
    std::cout << "\nMean Energy by Pitch Class:\n";
    float max_e = *std::max_element(mean_energy.begin(), mean_energy.end());
    if (max_e <= 0.0f) max_e = 1.0f;
    for (int i = 0; i < 12; ++i) {
      int bar = static_cast<int>(mean_energy[i] / max_e * 20);
      printf("  %-2s: %.3f ", names[i], mean_energy[i]);
      for (int j = 0; j < bar; ++j) std::cout << "*";
      std::cout << "\n";
    }
  }
  return 0;
}

int cmd_lufs(const CliArgs& args, const Audio& audio) {
  using namespace sonare::analysis::meter;
  LufsConfig config;
  LufsResult result = lufs(audio, config);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("integrated_lufs", result.integrated_lufs)
        .kv("momentary_lufs", result.momentary_lufs)
        .kv("short_term_lufs", result.short_term_lufs)
        .kv("loudness_range", result.loudness_range);
    if (args.has("series")) {
      json.key("momentary_series").float_array(momentary_lufs(audio, config));
      json.key("short_term_series").float_array(short_term_lufs(audio, config));
    }
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << basename(args.input_file) << color::reset << "\n";
    std::cout << "  " << color::yellow << "> Loudness (LUFS):" << color::reset << "\n";
    printf("    Integrated:    %.2f LUFS\n", result.integrated_lufs);
    printf("    Momentary:     %.2f LUFS\n", result.momentary_lufs);
    printf("    Short-term:    %.2f LUFS\n", result.short_term_lufs);
    printf("    Loudness Range: %.2f LU\n", result.loudness_range);
    std::cout << "\n";
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
      {"downbeats", "Detect downbeat times", cmd_downbeats, true},
      {"onsets", "Detect onset times", cmd_onsets, true},
      {"chords", "Detect chord progression", cmd_chords, true},
      {"sections", "Detect song structure", cmd_sections, true},
      {"timbre", "Analyze timbral characteristics", cmd_timbre, true},
      {"dynamics", "Analyze dynamics/loudness", cmd_dynamics, true},
      {"rhythm", "Analyze rhythm features", cmd_rhythm, true},
      {"melody", "Track melody/pitch contour", cmd_melody, true},
      {"boundaries", "Detect structural boundaries", cmd_boundaries, true},
      {"acoustic", "Analyze room acoustics (RT60/EDT/clarity)", cmd_acoustic, true},
      {"lufs", "Measure loudness (LUFS / loudness range)", cmd_lufs, true},
      // Processing
      {"pitch-shift", "Shift pitch by semitones", cmd_pitch_shift, true},
      {"time-stretch", "Time stretch audio", cmd_time_stretch, true},
      {"hpss", "Harmonic-percussive separation", cmd_hpss, true},
      {"preemphasis", "Apply pre-emphasis filtering", cmd_preemphasis, true},
      {"deemphasis", "Apply de-emphasis filtering", cmd_deemphasis, true},
      {"trim-silence", "Trim leading/trailing silence", cmd_trim_silence, true},
      {"split-silence", "List non-silent intervals", cmd_split_silence, true},
#ifdef SONARE_WITH_MASTERING
      {"mastering", "Apply mastering loudness/true-peak processing", cmd_mastering, true},
      {"mastering-processor", "Apply a named mastering processor", cmd_mastering_processor, true},
      {"mastering-pair-processor", "Apply a two-input mastering processor",
       cmd_mastering_pair_processor, true},
      {"mastering-pair-analyze", "Run a two-input mastering analysis", cmd_mastering_pair_analyze,
       true},
      {"mastering-stereo-analyze", "Run a stereo mastering analysis", cmd_mastering_stereo_analyze,
       true},
      {"mastering-processors", "List named mastering processors", cmd_mastering_processors, false},
      {"mastering-pair-processors", "List two-input mastering processors",
       cmd_mastering_pair_processors, false},
      {"mastering-pair-analyses", "List two-input mastering analyses",
       cmd_mastering_pair_analyses, false},
      {"mastering-stereo-analyses", "List stereo mastering analyses",
       cmd_mastering_stereo_analyses, false},
#endif
      // Features
      {"mel", "Compute mel spectrogram", cmd_mel, true},
      {"chroma", "Compute chromagram", cmd_chroma, true},
      {"tonnetz", "Compute tonal centroid features", cmd_tonnetz, true},
      {"spectral", "Compute spectral features", cmd_spectral, true},
      {"pitch", "Track pitch (YIN/pYIN)", cmd_pitch, true},
      {"onset-env", "Compute onset strength envelope", cmd_onset_env, true},
      {"onset-envelope", "Compute onset strength envelope (full array)", cmd_onset_envelope, true},
      {"tempogram", "Compute onset tempogram", cmd_tempogram, true},
      {"fourier-tempogram", "Compute Fourier tempogram", cmd_fourier_tempogram, true},
      {"tempogram-ratio", "Compute tempogram ratio features", cmd_tempogram_ratio, true},
      {"plp", "Compute predominant local pulse", cmd_plp, true},
      {"nnls-chroma", "Compute NNLS chromagram", cmd_nnls_chroma, true},
      {"cqt", "Compute Constant-Q Transform", cmd_cqt, true},
      // Utility
      {"frames-to-samples", "Convert frame index to sample index", cmd_frames_to_samples, false},
      {"samples-to-frames", "Convert sample index to frame index", cmd_samples_to_frames, false},
      {"power-to-db", "Convert power values to dB", cmd_power_to_db, false},
      {"amplitude-to-db", "Convert amplitude values to dB", cmd_amplitude_to_db, false},
      {"db-to-power", "Convert dB values to power", cmd_db_to_power, false},
      {"db-to-amplitude", "Convert dB values to amplitude", cmd_db_to_amplitude, false},
      {"frame-signal", "Frame a value sequence", cmd_frame_signal, false},
      {"pad-center", "Pad a value sequence symmetrically", cmd_pad_center, false},
      {"fix-length", "Pad or trim a value sequence", cmd_fix_length, false},
      {"fix-frames", "Pad and clamp frame indices", cmd_fix_frames, false},
      {"peak-pick", "Pick local peaks from a value sequence", cmd_peak_pick, false},
      {"vector-normalize", "Normalize a value sequence", cmd_vector_normalize, false},
      {"pcen", "Apply per-channel energy normalization", cmd_pcen, false},
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
  std::cerr << "  system-info    Show system and parallel configuration\n";

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

  if (args.command == "system-info") {
    return cmd_system_info(args);
  }

  // Find command
  const CommandInfo* cmd = find_command(args.command);
  if (!cmd) {
    std::cerr << color::red << "Error: Unknown command '" << args.command << "'" << color::reset
              << "\n\n";
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
    if (!cmd->requires_audio) {
      return cmd->handler(args, Audio{});
    }

    if (!args.quiet && !args.json_output) {
      std::cerr << color::blue << "Loading " << basename(args.input_file) << "..." << color::reset
                << std::flush;
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
