#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "sonare_cli.h"
#include "util/json.h"

#ifdef SONARE_WITH_MASTERING
#include "mastering/assistant/config_from_params.h"

/// Rewrite one camelCase JSON key as snake_case ("gainToMatchDb" ->
/// "gain_to_match_db"). The analysis keys carry no acronym runs, so a plain
/// "uppercase starts a new word" rule is exact for them.
std::string json_key_to_snake_case(const std::string& key) {
  std::string out;
  out.reserve(key.size() + 4);
  for (char c : key) {
    const auto byte = static_cast<unsigned char>(c);
    if (std::isupper(byte)) {
      if (!out.empty()) out.push_back('_');
      out.push_back(static_cast<char>(std::tolower(byte)));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

sonare::util::json::Value json_keys_to_snake_case(const sonare::util::json::Value& value) {
  namespace json = sonare::util::json;
  if (value.is_object()) {
    json::Object out;
    for (const auto& [key, child] : value.as_object()) {
      out.emplace(json_key_to_snake_case(key), json_keys_to_snake_case(child));
    }
    return json::Value(std::move(out));
  }
  if (value.is_array()) {
    json::Array out;
    out.reserve(value.as_array().size());
    for (const auto& element : value.as_array()) {
      out.emplace_back(json_keys_to_snake_case(element));
    }
    return json::Value(std::move(out));
  }
  return value;
}

namespace {

/// The mastering analyses are core APIs, and every core JSON producer keys its
/// output in camelCase to match the Node / WASM object surfaces. CLI stdout is
/// snake_case throughout, so the pass-through commands re-key the payload here
/// instead of the core changing convention for one pair of callers. Re-dumping
/// through util::json also keeps the CLI's copy locale-independent.
std::string analysis_json_for_cli(const std::string& core_json) {
  namespace json = sonare::util::json;
  return json::dump(json_keys_to_snake_case(json::parse(core_json)));
}

/// The assistant's document, re-keyed except for the chain it suggests.
///
/// The chain is fed back to the library verbatim (`mastering --chain-config`), so the
/// names under it belong to the chain param schema rather than to CLI stdout:
/// re-keying them would produce a document the library then rejects. Only the
/// key naming it is renamed; everything beside it is measurement output and
/// follows the snake_case rule.
std::string suggestion_json_for_cli(const std::string& core_json) {
  namespace json = sonare::util::json;
  json::Value document = json::parse(core_json);
  json::Object out;
  for (const auto& [key, child] : document.as_object()) {
    if (key == "chainConfig") {
      out.emplace("chain_config", child);
    } else {
      out.emplace(json_key_to_snake_case(key), json_keys_to_snake_case(child));
    }
  }
  return json::dump(json::Value(std::move(out)));
}

}  // namespace

std::vector<mastering::api::Param> parse_mastering_params(const std::string& text,
                                                          bool resolve_names,
                                                          const std::string& processor) {
  if (const std::string refusal = params_grammar_refusal(text); !refusal.empty()) {
    throw std::invalid_argument(refusal);
  }
  std::vector<mastering::api::Param> params;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
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
      // An enum-valued key may be given by its name; the resolver also refuses an unknown one.
      if (resolve_names) {
        if (const auto named =
                mastering::api::mastering_enum_value(processor, item.substr(0, eq), value_text)) {
          params.push_back({item.substr(0, eq), *named});
          continue;
        }
      }
      throw std::invalid_argument("invalid numeric value for parameter '" + item.substr(0, eq) +
                                  "': " + value_text);
    }
    params.push_back({item.substr(0, eq), value});
  }
  return params;
}

std::string read_text_file(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (!read_binary_file(path, &bytes)) {
    throw std::invalid_argument("cannot open config file: " + path);
  }
  return std::string(bytes.begin(), bytes.end());
}

void append_mastering_loudness_summary_json(
    JsonBuilder& json, const mastering::api::MasteringLoudnessSummary& summary) {
  json.begin_object()
      .kv("integrated_lufs", summary.integrated_lufs)
      .kv("max_momentary_lufs", summary.max_momentary_lufs)
      .kv("max_short_term_lufs", summary.max_short_term_lufs)
      .kv("true_peak_dbtp", summary.true_peak_dbtp)
      .kv("loudness_range_lu", summary.loudness_range_lu)
      .end_object();
}

std::string mastering_report_json(const mastering::api::MasteringReport& report) {
  JsonBuilder json;
  json.begin_object().key("before");
  append_mastering_loudness_summary_json(json, report.before);
  json.key("after");
  append_mastering_loudness_summary_json(json, report.after);
  json.kv("applied_gain_db", report.applied_gain_db)
      .kv("max_gain_reduction_db", report.max_gain_reduction_db)
      .kv("loudness_target_limited", report.loudness_target_limited)
      .kv("loudness_gain_db", report.loudness_gain_db)
      .measured_array("band_energy_delta_db", "band_energy_delta_band",
                      report.band_energy_delta_db.data(), report.band_energy_delta_db.size())
      .end_object();
  return json.build();
}

void write_mastering_report(const std::string& path,
                            const mastering::api::MasteringReport& report) {
  const std::string payload = mastering_report_json(report) + '\n';
  if (!write_binary_file(path, reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
    throw SonareException(ErrorCode::EncodeFailed, "cannot write mastering report: " + path);
  }
}

// Templated on the result rather than taking MonoChainResult: the stereo result
// carries the same ChainMetrics and the same three loudness scalars, so one
// writer keeps the two payloads identical by construction.
template <typename ChainResult>
void print_chain_result_json(const ChainResult& result, const std::string& mode,
                             const std::string& output, const std::string& preset,
                             const std::vector<std::string>& explanation = {},
                             bool include_report_latency = false) {
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
  if (include_report_latency) json.kv("latency_samples", 0);
  json.end_object().print();
}

// The loudness payload, written from either the standalone facade result or the
// loudness-only chain's. `latency_samples` comes from the result on both: the
// facade documents it as always 0 and the chain never moves it off 0.
template <typename LoudnessResult>
void print_loudness_result_json(const LoudnessResult& result,
                                const mastering::maximizer::LoudnessOptimizeConfig& config,
                                int sample_rate, const std::string& output) {
  JsonBuilder()
      .begin_object()
      .kv("input_lufs", result.input_lufs)
      .kv("output_lufs", result.output_lufs)
      .kv("applied_gain_db", result.applied_gain_db)
      .kv("target_lufs", config.target_lufs)
      .kv("ceiling_dbtp", config.ceiling_dbtp)
      .kv("true_peak_oversample", config.true_peak_oversample)
      .kv("latency_samples", result.latency_samples)
      .kv("loudness_target_limited", result.loudness_target_limited)
      .kv("sample_rate", sample_rate)
      .kv("output", output)
      .end_object()
      .print();
}

/// The same values as text. `report` is empty when no report was written.
template <typename LoudnessResult>
void print_loudness_result_text(const LoudnessResult& result, const std::string& report,
                                const std::string& output) {
  std::cout << "\n"
            << color::cyan << color::bold << "Mastering" << color::reset << "\n"
            << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
            << "\n"
            << "  Output LUFS:     " << result.output_lufs << "\n"
            << "  Applied Gain:    " << result.applied_gain_db << " dB\n";
  if (!report.empty()) std::cout << "  Report:          " << report << "\n";
  if (!output.empty()) std::cout << "  Output:          " << output << "\n";
  std::cout << "\n";
}

namespace {

/// Build the mastering assistant's config from the command line.
/// @details `mastering --assistant` and `mastering-suggest` declare the same
///   named options, and `mastering-suggest` also takes them as `--params`. Both
///   routes reach the one flat-param builder, so the explicit-loudness flags and
///   the refusal of an unknown key come from the core. `targetPlatform` and
///   `preset` take a name: the index the C ABI carries is a transport detail.
///   A setting named by an option and by `--params` is a contradiction rather
///   than a precedence question.
mastering::assistant::AssistantConfig assistant_config_from_cli(const CliArgs& args,
                                                                const std::string& params_text) {
  std::vector<mastering::api::Param> params;
  std::string platform = args.get_string("target-platform", "streaming");
  std::string preset = catalog_token(args.get_string("preset"), mastering::api::preset_names());
  bool has_platform = args.has("target-platform");
  bool has_preset = args.has("preset");

  std::stringstream stream(params_text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    const std::string key = item.substr(0, eq);
    if (key == "targetPlatform") {
      platform = item.substr(eq + 1);
      has_platform = true;
    } else if (key == "preset") {
      preset = item.substr(eq + 1);
      has_preset = true;
    } else {
      const auto parsed = parse_mastering_params(item);
      params.insert(params.end(), parsed.begin(), parsed.end());
    }
  }

  if (args.has("target-lufs")) params.push_back({"targetLufs", args.get_float("target-lufs", 0)});
  if (args.has("ceiling-dbtp"))
    params.push_back({"ceilingDbtp", args.get_float("ceiling-dbtp", 0)});
  if (args.has("enable-repair")) params.push_back({"enableRepair", 1.0});
  if (args.has("no-streaming-safe")) params.push_back({"preferStreamingSafe", 0.0});
  if (args.has("speech-mono-amount")) {
    params.push_back({"speechMonoAmount", args.get_float("speech-mono-amount", 1.0f)});
  }

  auto config = mastering::assistant::assistant_config_from_params(params.data(), params.size());
  // Through the shared setter and parser, so a name is validated against the
  // same tables every other surface resolves it with.
  if (has_platform) mastering::assistant::set_target_platform(config, platform);
  if (has_preset) config.preset = mastering::api::preset_from_string(preset);
  return config;
}

}  // namespace

int cmd_mastering(const CliArgs& args, const Audio& audio) {
  // validate_mastering_selection() has settled the chain selector before the input was read.
  const bool has_config = args.has("chain-config");
  const bool has_assistant = args.has("assistant");
  const std::string params_text = args.get_string("params");

  // main() probed the channel count for this invocation; a two-channel input is
  // mastered as a stereo pair rather than through the mono downmix it was also
  // handed. Anything else keeps the mono path, including a surround input, which
  // main() has already warned is downmixed.
  const bool stereo_input = args.source_channels == 2;

  mastering::api::MasteringChainConfig chain_config;
  std::string mode = "chainConfig";
  std::string preset_name;
  std::vector<std::string> explanation;
  // The document's params as written; with --params appended they are one list.
  std::vector<mastering::api::Param> document_params;

  if (has_assistant) {
    // --params here overrides the suggested chain, so none of it reaches the
    // assistant's own config.
    const auto assistant_config = assistant_config_from_cli(args, "");
    auto suggestion = mastering::assistant::suggest_chain(audio, assistant_config);
    chain_config = std::move(suggestion.config);
    explanation = std::move(suggestion.explanation);
    mode = "assistant";
  } else if (has_config) {
    const std::string document = read_text_file(args.get_string("chain-config"));
    // Parsed only to apply the document's own refusals; the chain is built from the list.
    mastering::api::chain_config_from_json(document);
    const auto root = sonare::util::json::parse(document);
    if (!root.find("params")->as_object().empty()) {
      for (auto& flat : mastering::api::chain_config_flat_params(document)) {
        document_params.push_back({std::move(flat.key), flat.value});
      }
    }
    mode = "chainConfig";
  } else {
    preset_name = catalog_token(args.get_string("preset"), mastering::api::preset_names());
    chain_config = mastering::api::preset_config(mastering::api::preset_from_string(preset_name));
    mode = "preset";
  }

  const auto overrides = parse_chain_params(params_text);
  if (has_config) {
    document_params.insert(document_params.end(), overrides.begin(), overrides.end());
    chain_config =
        mastering::api::parse_chain_config_params(document_params.data(), document_params.size());
  } else if (!overrides.empty()) {
    mastering::api::apply_chain_config_overrides(chain_config, overrides.data(), overrides.size());
  }

  mastering::api::MasteringChain chain(std::move(chain_config));
  // One reporter for both channel counts, so the payload cannot drift between
  // them: the two results differ only in which buffers were just written.
  const auto report_chain_result = [&](const auto& result) {
    if (args.has("report")) {
      write_mastering_report(args.get_string("report"), result.report);
    }
    if (args.json_output) {
      print_chain_result_json(result, mode, args.output_file, preset_name,
                              args.has("explain") ? explanation : std::vector<std::string>{},
                              args.has("report"));
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
      if (!args.output_file.empty()) {
        std::cout << "  Output:          " << args.output_file << "\n";
      }
      std::cout << "\n";
    }
  };

  if (stereo_input) {
    const auto planes = load_stereo_planes(args, audio);
    const auto result = chain.process_stereo(planes.left.data(), planes.right.data(),
                                             planes.left.size(), audio.sample_rate());
    if (!args.output_file.empty()) {
      save_stereo_wav(args.output_file, result.left, result.right, result.sample_rate,
                      args.get_int("bits", 16));
    }
    report_chain_result(result);
  } else {
    const auto result = chain.process_mono(audio.data(), audio.size(), audio.sample_rate());
    if (!args.output_file.empty()) {
      save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
               args.get_int("bits", 16));
    }
    report_chain_result(result);
  }
  return 0;
}

int cmd_mastering_normalize_loudness(const CliArgs& args, const Audio& audio) {
  // main() probed the channel count; a two-channel input keeps its pair.
  const bool stereo_input = args.source_channels == 2;
  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = args.get_float("target-lufs", -14.0f);
  config.ceiling_dbtp = args.get_float("ceiling-dbtp", -1.0f);
  config.true_peak_oversample = args.get_int("true-peak-oversample", 4);

  // The standalone loudness facade predates the chain report and has no stereo
  // overload, so two requests route to the loudness-only chain instead: an
  // explicit --report, which needs the before/after payload only the chain
  // composes, and a stereo input. That is a route change rather than an
  // algorithm change -- on the same mono input the loudness-only chain and the
  // facade return bit-identical samples and the same input/output LUFS and
  // applied gain.
  if (args.has("report") || stereo_input) {
    mastering::api::MasteringChainConfig chain_config;
    chain_config.loudness.enabled = true;
    chain_config.loudness.target_lufs = config.target_lufs;
    chain_config.loudness.ceiling_dbtp = config.ceiling_dbtp;
    chain_config.loudness.true_peak_oversample = config.true_peak_oversample;
    mastering::api::MasteringChain chain(std::move(chain_config));
    const std::string report_path = args.has("report") ? args.get_string("report") : std::string{};
    const auto report_loudness_result = [&](const auto& result) {
      if (!report_path.empty()) write_mastering_report(report_path, result.report);
      if (args.json_output) {
        print_loudness_result_json(result, config, result.sample_rate, args.output_file);
      } else {
        print_loudness_result_text(result, report_path, args.output_file);
      }
    };

    if (stereo_input) {
      const auto planes = load_stereo_planes(args, audio);
      const auto result = chain.process_stereo(planes.left.data(), planes.right.data(),
                                               planes.left.size(), audio.sample_rate());
      if (!args.output_file.empty()) {
        save_stereo_wav(args.output_file, result.left, result.right, result.sample_rate,
                        args.get_int("bits", 16));
      }
      report_loudness_result(result);
    } else {
      const auto result = chain.process_mono(audio.data(), audio.size(), audio.sample_rate());
      if (!args.output_file.empty()) {
        save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
                 args.get_int("bits", 16));
      }
      report_loudness_result(result);
    }
    return 0;
  }

  const auto result = mastering::maximizer::loudness_optimize(audio, config);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.audio.data(), result.audio.size(), result.audio.sample_rate(),
             args.get_int("bits", 16));
  }

  if (args.json_output) {
    print_loudness_result_json(result, config, result.audio.sample_rate(), args.output_file);
  } else {
    print_loudness_result_text(result, std::string{}, args.output_file);
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

// Stereo path for cmd_mastering_processor. A two-channel source is re-read as a
// pair, so the processor acts on the image the file carries; a mono source is
// fed to both channels, which is what makes stereo-only processors
// (stereo.imager, eq.midSide, multiband.*) reachable as a standalone CLI effect
// at all. Either way the result is written as a stereo file: these processors
// exist to act on or create a difference between the channels, and folding the
// pair back to mono discards exactly what they produced.
int run_mastering_processor_stereo(const CliArgs& args, const Audio& audio,
                                   const std::string& processor,
                                   const std::vector<mastering::api::Param>& params) {
  std::vector<float> left;
  std::vector<float> right;
  if (args.source_channels == 2) {
    StereoPlanes planes = load_stereo_planes(args, audio);
    left = std::move(planes.left);
    right = std::move(planes.right);
  } else {
    left.assign(audio.begin(), audio.end());
    right = left;
  }
  const auto result = mastering::api::apply_named_processor_stereo(
      processor, left.data(), right.data(), left.size(), audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    save_stereo_wav(args.output_file, result.left, result.right, result.sample_rate,
                    args.get_int("bits", 16));
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
        .kv("sample_rate", result.sample_rate)
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
  std::vector<std::string> names = mastering::api::processor_names();
  for (auto& name : mastering::api::stereo_processor_names()) names.push_back(std::move(name));
  const std::string processor = catalog_token(args.get_string("processor"), names);
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const auto params = parse_mastering_params(args.get_string("params"), true, processor);
  // The file's own channel count and the library's own stereo-only set decide
  // this, with nothing in between. A two-channel source takes the stereo entry
  // point, which accepts every processor: whether a given one links its decision
  // across the pair or runs per channel is already settled per processor inside
  // the library, and downmixing first would take that choice away. A stereo-only
  // processor routes there from any source because the mono entry rejects it with
  // an opaque INVALID_PARAMETER. Everything else stays mono.
  if (args.source_channels == 2 || is_stereo_only_processor(processor)) {
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
        .kv("stereo", false)
        .kv("input_lufs", result.input_lufs)
        .kv("output_lufs", result.output_lufs)
        .kv("applied_gain_db", result.applied_gain_db)
        .kv("latency_samples", result.latency_samples)
        .kv("sample_rate", result.sample_rate)
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
  const std::string params_text = args.get_string("params");
  std::vector<mastering::api::Param> params = parse_mastering_params(params_text);
  if (params_text.empty()) {
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
    params.push_back({"band0.detectorDelayMs", args.get_float("detector-delay-ms", 0.0f)});
    params.push_back({"band0.sidechainFreqHz", args.get_float("sidechain-freq-hz", -1.0f)});
    params.push_back({"band0.sidechainQ", args.get_float("sidechain-q", 1.0f)});
    params.push_back({"phaseMode", static_cast<double>(args.get_int("phase-mode", 1))});
    params.push_back({"resolution", static_cast<double>(args.get_int("resolution", 0))});
    params.push_back({"autoGain", args.has("auto-gain") ? 1.0 : 0.0});
    params.push_back({"gainScale", args.get_float("gain-scale", 1.0f)});
    params.push_back({"outputGainDb", args.get_float("output-gain-db", 0.0f)});
    params.push_back({"outputPan", args.get_float("output-pan", 0.0f)});
  }
  // A two-channel source is equalized as a pair, as mastering-processor does,
  // so --placement and --output-pan act on the image the file carries.
  const bool stereo = args.source_channels == 2;
  const int bits = args.get_int("bits", 16);
  const auto report = [&](const auto& result) {
    if (args.json_output) {
      JsonBuilder()
          .begin_object()
          .kv("processor", "eq.equalizer")
          .kv("input_lufs", result.input_lufs)
          .kv("output_lufs", result.output_lufs)
          .kv("applied_gain_db", result.applied_gain_db)
          .kv("latency_samples", result.latency_samples)
          .kv("sample_rate", result.sample_rate)
          .kv("output", args.output_file)
          .kv("stereo", stereo)
          .end_object()
          .print();
    } else {
      std::cout << "\n"
                << color::cyan << color::bold << "Equalizer" << color::reset << "\n"
                << "  Input LUFS:      " << std::fixed << std::setprecision(2) << result.input_lufs
                << "\n"
                << "  Output LUFS:     " << result.output_lufs << "\n"
                << "  Applied Gain:    " << result.applied_gain_db << " dB\n";
      if (!args.output_file.empty()) {
        std::cout << "  Output:          " << args.output_file << "\n";
      }
      std::cout << "\n";
    }
  };
  if (stereo) {
    const StereoPlanes planes = load_stereo_planes(args, audio);
    const auto result = mastering::api::apply_named_processor_stereo(
        "eq.equalizer", planes.left.data(), planes.right.data(), planes.left.size(),
        audio.sample_rate(), params);
    if (!args.output_file.empty()) {
      save_stereo_wav(args.output_file, result.left, result.right, result.sample_rate, bits);
    }
    report(result);
    return 0;
  }
  const auto result = mastering::api::apply_named_processor(
      "eq.equalizer", audio.data(), audio.size(), audio.sample_rate(), params);
  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
             bits);
  }
  report(result);
  return 0;
}

int cmd_mastering_processors(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::processor_names();
  return print_name_catalog(args, "processors", names);
}

int cmd_mastering_pair_processors(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::pair_processor_names();
  return print_name_catalog(args, "processors", names);
}

int cmd_mastering_pair_analyses(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::pair_analysis_names();
  return print_name_catalog(args, "analyses", names);
}

int cmd_mastering_stereo_analyses(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::stereo_analysis_names();
  return print_name_catalog(args, "analyses", names);
}

int cmd_mastering_presets(const CliArgs& args, const Audio&) {
  const auto names = mastering::api::preset_names();
  return print_name_catalog(args, "presets", names);
}

/// Print a C ABI JSON document on CLI stdout.
///
/// These two go through the C entry point, rather than the C++ API the commands
/// around them call, so that one serializer builds the document for either
/// front-end. Its keys arrive in the core's camelCase and are re-keyed here for
/// the same reason @ref analysis_json_for_cli exists: CLI stdout is snake_case
/// throughout, and the core does not change convention for one pair of callers.
int print_c_api_json(SonareError err, char* json) {
  if (err != SONARE_OK) {
    sonare_free_string(json);
    throw_c_failure(err, nullptr);
  }
  const std::string document(json);
  sonare_free_string(json);
  std::cout << analysis_json_for_cli(document) << "\n";
  return 0;
}

int cmd_mastering_profile(const CliArgs& args, const Audio& audio) {
  const auto parsed = parse_mastering_params(args.get_string("params"));
  std::vector<SonareMasteringParam> params;
  params.reserve(parsed.size());
  for (const auto& param : parsed) params.push_back({param.key.c_str(), param.value});

  char* json = nullptr;
  const SonareError err = sonare_mastering_audio_profile(
      audio.data(), audio.size(), audio.sample_rate(), params.data(), params.size(), &json);
  return print_c_api_json(err, json);
}

int cmd_mastering_streaming(const CliArgs& args, const Audio& audio) {
  // A platform list is optional: NULL/0 selects the built-in one, which is what
  // an absent --platforms means here rather than an empty set. `specs` outlives
  // the call, so the borrowed name pointers stay valid. --platforms-file wins
  // over --platforms when both are given, as it does on the Python CLI.
  const std::string platforms_file = args.get_string("platforms-file");
  const auto specs =
      platforms_file.empty()
          ? parse_streaming_platforms(args.get_string("platforms", ""), "argument --platforms")
          : parse_streaming_platforms(read_text_file(platforms_file), "--platforms-file");
  std::vector<SonareStreamingPlatform> platforms;
  platforms.reserve(specs.size());
  for (const auto& spec : specs) {
    platforms.push_back({spec.name.c_str(), spec.target_lufs, spec.ceiling_dbtp});
  }

  char* json = nullptr;
  const SonareError err = sonare_mastering_streaming_preview(
      audio.data(), audio.size(), audio.sample_rate(),
      platforms.empty() ? nullptr : platforms.data(), platforms.size(), &json);
  return print_c_api_json(err, json);
}

int cmd_mastering_pair_processor(const CliArgs& args, const Audio& audio) {
  const std::string processor =
      catalog_token(args.get_string("processor"), mastering::api::pair_processor_names());
  if (processor.empty()) {
    std::cerr << color::red << "Error: --processor is required" << color::reset << "\n";
    return 1;
  }
  const Audio reference = load_pair_reference_audio(args);
  const auto params = parse_mastering_params(args.get_string("params"));
  const auto result = mastering::api::apply_named_pair_processor(
      processor, audio.data(), reference.data(), audio.size(), reference.size(),
      audio.sample_rate(), reference.sample_rate(), params);
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
  const std::string analysis =
      catalog_token(args.get_string("analysis"), mastering::api::pair_analysis_names());
  if (analysis.empty()) {
    std::cerr << color::red << "Error: --analysis is required" << color::reset << "\n";
    return 1;
  }
  const Audio reference = load_pair_reference_audio(args);
  const auto params = parse_mastering_params(args.get_string("params"));
  std::cout << analysis_json_for_cli(mastering::api::analyze_named_pair(
                   analysis, audio.data(), reference.data(), audio.size(), reference.size(),
                   audio.sample_rate(), reference.sample_rate(), params))
            << "\n";
  return 0;
}

int cmd_mastering_stereo_analyze(const CliArgs& args, const Audio& audio) {
  const std::string analysis =
      catalog_token(args.get_string("analysis"), mastering::api::stereo_analysis_names());
  if (analysis.empty()) {
    std::cerr << color::red << "Error: --analysis is required" << color::reset << "\n";
    return 1;
  }
  const Audio right = load_reference_audio(args, audio.sample_rate(), audio.size());
  const auto params = parse_mastering_params(args.get_string("params"));
  std::cout << analysis_json_for_cli(mastering::api::analyze_named_stereo(
                   analysis, audio.data(), right.data(), audio.size(), audio.sample_rate(), params))
            << "\n";
  return 0;
}

int cmd_mastering_suggest(const CliArgs& args, const Audio& audio) {
  const auto config = assistant_config_from_cli(args, args.get_string("params"));
  const auto suggestion = mastering::assistant::suggest_chain(audio, config);

  const std::string config_out = args.get_string("config-out");
  if (!config_out.empty()) {
    // Through the chain serializer on the config already in hand: that is the
    // form `mastering --chain-config` parses, and re-deriving it from the
    // printed document would re-key what the library must read back verbatim.
    const std::string payload = mastering::api::chain_config_to_json(suggestion.config) + '\n';
    if (!write_binary_file(config_out, reinterpret_cast<const uint8_t*>(payload.data()),
                           payload.size())) {
      throw SonareException(ErrorCode::EncodeFailed, "cannot write chain config: " + config_out);
    }
  }
  std::cout << suggestion_json_for_cli(mastering::assistant::assistant_result_to_json(suggestion))
            << "\n";
  return 0;
}
#endif
