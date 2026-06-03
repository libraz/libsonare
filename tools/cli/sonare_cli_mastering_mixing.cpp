#include "sonare_cli.h"

#ifdef SONARE_WITH_MASTERING
std::vector<mastering::api::Param> parse_mastering_params(const std::string& text) {
  std::vector<mastering::api::Param> params;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos) continue;
    // Locale-independent parse: std::stod follows LC_NUMERIC, which DAW plugin
    // hosts sometimes set to e.g. de_DE (comma as decimal separator). Imbue the
    // classic locale so "1.5" always parses as 1.5 regardless of host locale.
    // Matches the policy in util/json.h.
    const std::string value_text = item.substr(eq + 1);
    std::istringstream ss(value_text);
    ss.imbue(std::locale::classic());
    double value = 0.0;
    ss >> value;
    if (!ss || ss.peek() != std::char_traits<char>::eof()) {
      throw std::invalid_argument("invalid numeric value for parameter '" + item.substr(0, eq) +
                                  "': " + value_text);
    }
    params.push_back({item.substr(0, eq), value});
  }
  return params;
}

std::string read_text_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::invalid_argument("cannot open config file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void print_chain_result_json(const mastering::api::MonoChainResult& result, const std::string& mode,
                             const std::string& output, const std::string& preset,
                             const std::vector<std::string>& explanation = {}) {
  JsonBuilder json;
  json.begin_object()
      .kv("mode", mode)
      .kv("input_lufs", result.input_lufs)
      .kv("output_lufs", result.output_lufs)
      .kv("applied_gain_db", result.applied_gain_db)
      .kv("output", output);
  if (!preset.empty()) json.kv("preset", preset);
  json.key("stages").begin_array();
  for (const auto& stage : result.stages) json.value(stage);
  json.end_array();
  if (!explanation.empty()) {
    json.key("explanation").begin_array();
    for (const auto& item : explanation) json.value(item);
    json.end_array();
  }
  json.end_object().print();
}

int cmd_mastering(const CliArgs& args, const Audio& audio) {
  const bool use_chain = args.has("preset") || args.has("config") || args.has("assistant");
  if (use_chain) {
    mastering::api::MasteringChainConfig chain_config;
    std::string mode = "config";
    std::string preset_name;
    std::vector<std::string> explanation;

    if (args.has("assistant")) {
      mastering::assistant::AssistantConfig assistant_config;
      assistant_config.target_lufs = args.get_float("target-lufs", -14.0f);
      assistant_config.ceiling_db = args.get_float("ceiling-db", -1.0f);
      assistant_config.enable_repair = args.has("enable-repair");
      auto suggestion = mastering::assistant::suggest_chain(audio, assistant_config);
      chain_config = std::move(suggestion.config);
      explanation = std::move(suggestion.explanation);
      mode = "assistant";
    } else if (args.has("config")) {
      chain_config =
          mastering::api::chain_config_from_json(read_text_file(args.get_string("config")));
      mode = "config";
    } else {
      preset_name = args.get_string("preset");
      chain_config = mastering::api::preset_config(mastering::api::preset_from_string(preset_name));
      mode = "preset";
    }

    const auto overrides = parse_mastering_params(args.get_string("params"));
    if (!overrides.empty()) {
      mastering::api::apply_chain_config_overrides(chain_config, overrides.data(),
                                                   overrides.size());
    }

    mastering::api::MasteringChain chain(std::move(chain_config));
    const auto result = chain.process_mono(audio.data(), audio.size(), audio.sample_rate());
    if (!args.output_file.empty()) {
      save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
               args.get_int("bits", 16));
    }

    if (args.json_output) {
      print_chain_result_json(result, mode, args.output_file, preset_name,
                              args.has("explain") ? explanation : std::vector<std::string>{});
    } else {
      std::cout << "\n"
                << color::cyan << color::bold << "Mastering Chain" << color::reset << "\n"
                << "  Mode:            " << mode << "\n";
      if (!preset_name.empty()) std::cout << "  Preset:          " << preset_name << "\n";
      std::cout << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
                << "\n"
                << "  Output LUFS:     " << result.output_lufs << "\n"
                << "  Applied Gain:    " << result.applied_gain_db << " dB\n"
                << "  Stages:          " << result.stages.size() << "\n";
      if (args.has("explain") && !explanation.empty()) {
        std::cout << "  Explanation:\n";
        for (const auto& item : explanation) std::cout << "    - " << item << "\n";
      }
      if (!args.output_file.empty()) std::cout << "  Output:          " << args.output_file << "\n";
      std::cout << "\n";
    }
    return 0;
  }

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

// True when `name` has no mono implementation and must run through the stereo
// entry point (stereo wideners / mid-side EQ / multiband — see
// named_processor_registry.cpp). The mono apply_named_processor() rejects these
// with an opaque INVALID_PARAMETER, so the CLI must route them to the stereo
// path (or give a clear diagnostic).
bool is_stereo_only_processor(const std::string& name) {
  const auto names = mastering::api::stereo_processor_names();
  return std::find(names.begin(), names.end(), name) != names.end();
}

// Stereo path for cmd_mastering_processor: the CLI carries a single mono buffer,
// so we feed it to BOTH channels, run the true-stereo processor, and downmix the
// processed L/R back to mono for the (mono-centric) CLI WAV writer. This makes
// stereo-only processors (stereo.imager, eq.midSide, multiband.*) reachable as a
// standalone CLI effect. Engaged by --stereo or auto-engaged for a stereo-only
// processor.
int run_mastering_processor_stereo(const CliArgs& args, const Audio& audio,
                                   const std::string& processor,
                                   const std::vector<mastering::api::Param>& params) {
  const std::vector<float> channel(audio.begin(), audio.end());
  const auto result = mastering::api::apply_named_processor_stereo(
      processor, channel.data(), channel.data(), audio.size(), audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    std::vector<float> mono(result.left.size(), 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
      const float r = i < result.right.size() ? result.right[i] : 0.0f;
      mono[i] = 0.5f * (result.left[i] + r);
    }
    save_wav(args.output_file, mono, result.sample_rate, args.get_int("bits", 16));
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("processor", processor)
        .kv("stereo", true)
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("latency_samples", result.latency_samples)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Mastering Processor (stereo)" << color::reset
              << "\n"
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

int cmd_mastering_processor(const CliArgs& args, const Audio& audio) {
  const std::string processor = args.get_string("processor");
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const auto params = parse_mastering_params(args.get_string("params"));
  // Route stereo-only processors (and an explicit --stereo request) through the
  // true-stereo entry point; otherwise they would fail with an opaque mono
  // INVALID_PARAMETER. The mono path stays the default for everything else.
  if (args.has("stereo") || is_stereo_only_processor(processor)) {
    return run_mastering_processor_stereo(args, audio, processor, params);
  }
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

int cmd_eq(const CliArgs& args, const Audio& audio) {
  std::vector<mastering::api::Param> params = parse_mastering_params(args.get_string("params"));
  if (args.get_string("params").empty()) {
    params.push_back({"band0.enabled", 1.0});
    params.push_back({"band0.type", static_cast<double>(args.get_int("type", 0))});
    params.push_back({"band0.frequencyHz", args.get_float("frequency-hz", 1000.0f)});
    params.push_back({"band0.gainDb", args.get_float("gain-db", 0.0f)});
    params.push_back({"band0.q", args.get_float("q", 1.0f)});
    params.push_back({"band0.coeffMode", static_cast<double>(args.get_int("coeff-mode", 0))});
    params.push_back({"band0.slopeDbOct", static_cast<double>(args.get_int("slope-db-oct", 12))});
    params.push_back({"band0.placement", static_cast<double>(args.get_int("placement", 0))});
    params.push_back({"band0.proportionalQ", args.has("proportional-q") ? 1.0 : 0.0});
    params.push_back({"band0.dynamic", args.has("dynamic") ? 1.0 : 0.0});
    params.push_back({"band0.thresholdDb", args.get_float("threshold-db", -24.0f)});
    params.push_back({"band0.autoThreshold", args.has("auto-threshold") ? 1.0 : 0.0});
    params.push_back({"band0.ratio", args.get_float("ratio", 2.0f)});
    params.push_back({"band0.rangeDb", args.get_float("range-db", -6.0f)});
    params.push_back({"band0.attackMs", args.get_float("attack-ms", 5.0f)});
    params.push_back({"band0.releaseMs", args.get_float("release-ms", 50.0f)});
    params.push_back({"band0.lookaheadMs", args.get_float("lookahead-ms", 0.0f)});
    params.push_back({"band0.sidechainFreqHz", args.get_float("sidechain-freq-hz", -1.0f)});
    params.push_back({"band0.sidechainQ", args.get_float("sidechain-q", 1.0f)});
    params.push_back({"phaseMode", static_cast<double>(args.get_int("phase-mode", 1))});
    params.push_back({"resolution", static_cast<double>(args.get_int("resolution", 0))});
    params.push_back({"autoGain", args.has("auto-gain") ? 1.0 : 0.0});
    params.push_back({"gainScale", args.get_float("gain-scale", 1.0f)});
    params.push_back({"outputGainDb", args.get_float("output-gain-db", 0.0f)});
    params.push_back({"outputPan", args.get_float("output-pan", 0.0f)});
  }
  const auto result = mastering::api::apply_named_processor(
      "eq.equalizer", audio.data(), audio.size(), audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
             args.get_int("bits", 16));
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("processor", "eq.equalizer")
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("latency_samples", result.latency_samples)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Equalizer" << color::reset << "\n"
              << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
              << "\n"
              << "  Output LUFS:     " << result.output_lufs << "\n"
              << "  Applied Gain:    " << result.applied_gain_db << " dB\n";
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

int cmd_mastering_pair_processor(const CliArgs& args, const Audio& audio) {
  const std::string processor = args.get_string("processor");
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const Audio reference = load_reference_audio_any_length(args, audio.sample_rate());
  const auto params = parse_mastering_params(args.get_string("params"));
  const auto result = mastering::api::apply_named_pair_processor(
      processor, audio.data(), reference.data(), audio.size(), reference.size(),
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
  const Audio reference = load_reference_audio_any_length(args, audio.sample_rate());
  const auto params = parse_mastering_params(args.get_string("params"));
  std::cout << mastering::api::analyze_named_pair(analysis, audio.data(), reference.data(),
                                                  audio.size(), reference.size(),
                                                  audio.sample_rate(), params)
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

#ifdef SONARE_WITH_MIXING
mixing::PanMode parse_pan_mode_option(const std::string& value) {
  std::string key = value;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (key == "balance") return mixing::PanMode::Balance;
  if (key == "stereopan" || key == "stereo-pan" || key == "pan") return mixing::PanMode::StereoPan;
  if (key == "dualpan" || key == "dual-pan") return mixing::PanMode::DualPan;
  throw std::invalid_argument("invalid pan mode: " + value);
}

int cmd_mixing_presets(const CliArgs& args, const Audio&) {
  const auto names = mixing::api::scene_preset_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_mixing_preset(const CliArgs& args, const Audio&) {
  const std::string preset_name = args.get_string("preset", "vocalReverbSend");
  const auto preset = mixing::api::scene_preset_from_string(preset_name);
  std::cout << mixing::api::scene_to_json(mixing::api::scene_preset(preset)) << "\n";
  return 0;
}

int cmd_mix(const CliArgs& args, const Audio& audio) {
  mixing::ChannelStrip strip;
  strip.set_input_trim_db(args.get_float("input-trim-db", 0.0f));
  strip.set_fader_db(args.get_float("fader-db", 0.0f));
  strip.set_pan(args.get_float("pan", 0.0f));
  strip.set_pan_mode(parse_pan_mode_option(args.get_string("pan-mode", "balance")));
  strip.set_width(args.get_float("width", 1.0f));
  strip.prepare(static_cast<double>(audio.sample_rate()), static_cast<int>(audio.size()));

  std::vector<float> left(audio.begin(), audio.end());
  std::vector<float> right(audio.begin(), audio.end());
  float* channels[] = {left.data(), right.data()};
  strip.process(channels, 2, static_cast<int>(audio.size()));

  std::vector<float> mono(audio.size(), 0.0f);
  for (size_t i = 0; i < mono.size(); ++i) {
    mono[i] = 0.5f * (left[i] + right[i]);
  }
  if (!args.output_file.empty()) {
    save_wav(args.output_file, mono, audio.sample_rate());
  }

  const auto meter = strip.meter_snapshot();
  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("sample_rate", audio.sample_rate())
        .kv("length", audio.size())
        .key("meter")
        .begin_object()
        .kv("peak_db_l", meter.peak_db[0])
        .kv("peak_db_r", meter.peak_db[1])
        .kv("rms_db_l", meter.rms_db[0])
        .kv("rms_db_r", meter.rms_db[1])
        .kv("correlation", meter.correlation)
        .kv("mono_compat_width", meter.mono_compat_width)
        .kv("likely_mono_compatible", meter.likely_mono_compatible)
        .kv("max_true_peak_db", meter.max_true_peak_db)
        .end_object()
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cout << "Mixed " << audio.size() << " samples";
    if (!args.output_file.empty()) std::cout << " -> " << args.output_file;
    std::cout << "\n";
    std::cout << "Correlation: " << meter.correlation
              << ", mono-compatible: " << (meter.likely_mono_compatible ? "yes" : "no") << "\n";
  }
  return 0;
}
#endif
