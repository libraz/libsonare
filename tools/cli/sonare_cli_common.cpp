#include "sonare_cli.h"

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

std::string read_plain_text_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::invalid_argument("cannot open text file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
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

Audio load_reference_audio_any_length(const CliArgs& args, int expected_sample_rate) {
  const std::string path = args.get_string("reference");
  if (path.empty()) {
    throw std::invalid_argument("--reference is required");
  }
  auto [samples, sample_rate] = load_audio(path);
  if (sample_rate != expected_sample_rate) {
    throw std::invalid_argument("reference sample rate must match input sample rate");
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

std::vector<std::string> split_string(const std::string& text, char delimiter) {
  std::vector<std::string> values;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, delimiter)) {
    if (!item.empty()) values.push_back(item);
  }
  return values;
}

sonare::util::json::Value parse_cli_json_scalar(const std::string& raw) {
  try {
    return sonare::util::json::parse(raw);
  } catch (const std::exception&) {
    return sonare::util::json::Value(raw);
  }
}

void set_json_path(sonare::util::json::Value& root, const std::string& path,
                   sonare::util::json::Value value) {
  auto parts = split_string(path, '.');
  if (parts.empty()) throw std::invalid_argument("empty --set path");
  if (!root.is_object()) root = sonare::util::json::Object{};
  sonare::util::json::Value* cursor = &root;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    auto& object = cursor->as_object();
    auto it = object.find(parts[i]);
    if (it == object.end() || !it->second.is_object()) {
      it = object.insert_or_assign(parts[i], sonare::util::json::Object{}).first;
    }
    cursor = &it->second;
  }
  cursor->as_object()[parts.back()] = std::move(value);
}

// Maps the UI macro names (pitch/formant/space/intensity/output) to concrete
// dsp.* paths so `--set macros.X=...` from the CLI is convenient. This is
// intentionally CLI-only sugar; the core loader treats `dsp` as authoritative
// and never derives dsp from macros.
// Keep the mapping in sync with `_apply_voice_macro_override` in
// bindings/python/src/libsonare/cli.py.
void apply_voice_macro_override(sonare::util::json::Value& root, const std::string& path,
                                const sonare::util::json::Value& value) {
  if (!value.is_number()) return;
  const double number = value.as_number();
  if (path == "macros.pitch") {
    set_json_path(root, "dsp.retune.semitones", number);
  } else if (path == "macros.formant") {
    set_json_path(root, "dsp.formant.factor", number);
  } else if (path == "macros.space") {
    set_json_path(root, "dsp.reverb.mix", number);
  } else if (path == "macros.intensity") {
    set_json_path(root, "dsp.compressor.ratio", 1.0 + number * 4.0);
  } else if (path == "macros.output") {
    set_json_path(root, "dsp.outputGainDb", number);
  }
}

std::string find_voice_preset_in_pack(const std::string& pack_json, const std::string& preset_id) {
  const auto root = sonare::util::json::parse(pack_json);
  const auto* presets = root.find("presets");
  if (presets == nullptr || !presets->is_array()) {
    throw std::invalid_argument("preset pack must contain a presets array");
  }
  const sonare::util::json::Value* match = nullptr;
  for (const auto& item : presets->as_array()) {
    const auto* id = item.find("id");
    if (id == nullptr || !id->is_string()) continue;
    if (id->as_string() == preset_id) {
      if (match != nullptr)
        throw std::invalid_argument("duplicate preset id in preset pack: " + preset_id);
      match = &item;
    }
  }
  if (match == nullptr)
    throw std::invalid_argument("preset not found in preset pack: " + preset_id);
  return sonare::util::json::dump(*match);
}

std::string apply_voice_preset_sets(std::string config_text, const std::string& set_options) {
  if (set_options.empty()) return config_text;
  auto root = sonare::util::json::parse(config_text);
  for (const auto& assignment : split_string(set_options, ',')) {
    const auto eq = assignment.find('=');
    if (eq == std::string::npos || eq == 0) {
      throw std::invalid_argument("invalid --set assignment: " + assignment);
    }
    const std::string path = assignment.substr(0, eq);
    auto value = parse_cli_json_scalar(assignment.substr(eq + 1));
    set_json_path(root, path, value);
    apply_voice_macro_override(root, path, value);
  }
  return sonare::util::json::dump(root);
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
    return {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
            Mode::Lydian, Mode::Mixolydian, Mode::Locrian};
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
  print_float_values(
      args, power_to_db(values, args.get_float("ref", 1.0f), args.get_float("amin", 1e-10f),
                        args.get_float("top-db", 80.0f)));
  return 0;
}

int cmd_amplitude_to_db(const CliArgs& args, const Audio&) {
  auto values = require_float_values(args);
  print_float_values(
      args, amplitude_to_db(values, args.get_float("ref", 1.0f), args.get_float("amin", 1e-5f),
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
  print_float_values(args,
                     pcen(values, args.get_int("n-bins", 0), args.get_int("n-frames", 0), config));
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
