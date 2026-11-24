#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef SONARE_WITH_MASTERING
#include "mastering/api/presets.h"
#include "mastering/assistant/param_spelling.h"
#include "mastering/assistant/platform_targets.h"
#endif
#ifdef SONARE_WITH_MIXING_ASSISTANT
#include "mixing/assistant/config_from_params.h"
#endif
#include "sonare_cli.h"
#include "sonare_cli_project.h"
#include "sonare_cli_registry.h"
#include "util/refusal_predicates.h"

// The argv-level refusals the command validators call, and the parsers they share with the
// handlers. sonare_tests links this TU without the handler TUs, so nothing here may reach one.

std::string lowercase_cli_value(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
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

std::vector<Mode> parse_mode_list_option(const std::string& value) {
  const std::string key = lowercase_cli_value(value);
  if (key == "all" || key == "modal") {
    return {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
            Mode::Lydian, Mode::Mixolydian, Mode::Locrian};
  }
  if (key == "major-minor" || key == "majmin" || key == "diatonic") {
    return {Mode::Major, Mode::Minor};
  }

  std::vector<Mode> modes;
  for (const auto& item : split_string(value, ',')) {
    modes.push_back(parse_mode_option(item));
  }
  if (modes.empty()) {
    throw std::invalid_argument("--modes must contain at least one mode");
  }
  return modes;
}

KeyProfileType parse_key_profile_option(const std::string& value) {
  const std::string key = lowercase_cli_value(value);
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

#if defined(SONARE_WITH_MIXING) || defined(SONARE_WITH_MIXING_ASSISTANT)
/// The id a bare `--input` path stands for: the file name without its last extension, split at
/// the platform's path separator and leaving leading dots alone, as Python's splitext does.
std::string bare_path_id(const std::string& entry) {
  const std::string base = std::filesystem::path(entry).filename().string();
  const auto first = base.find_first_not_of('.');
  const auto dot = base.find_last_of('.');
  return first == std::string::npos || dot == std::string::npos || dot < first
             ? base
             : base.substr(0, dot);
}

#endif

#ifdef SONARE_WITH_MIXING
/// The first malformed `--input [ID=]WAV` entry of `mix`, or empty; decided from argv alone.
std::string mix_input_refusal(const CliArgs& args) {
  for (const std::string& entry : args.get_string_list("input")) {
    const auto separator = entry.find('=');
    if (separator == 0) return "--input strip id must not be empty: " + entry;
    const bool no_path =
        separator == std::string::npos ? entry.empty() : separator + 1 == entry.size();
    if (no_path) return "--input requires a file path: " + entry;
  }
  return {};
}
#endif

#if defined(SONARE_WITH_MASTERING) || defined(SONARE_WITH_MIXING_ASSISTANT)
namespace {

double read_assistant_number(const std::string& key, const std::string& text) {
  try {
    const double value = parse_double_strict("params", text);
    if (std::fabs(value) <= std::numeric_limits<float>::max()) return value;
  } catch (const std::invalid_argument&) {
    // Both assistants use the same refusal for a value the numeric transport cannot read.
  }
  throw SonareParameterRefusal::predicate(
      ErrorCode::InvalidParameter, cli_bag_subject("--params", key), "must be a finite number")
      .as_caller_spelled();
}

}  // namespace
#endif

#ifdef SONARE_WITH_MIXING_ASSISTANT
/// Split `[ID=]WAV` into its two halves. A bare path takes the file's own name
/// as the id, which is what the scene then addresses the strip by. Either half
/// may come out empty; suggest_mix_argument_refusal refuses those before any read.
void split_track_entry(const std::string& entry, std::string* id, std::string* path) {
  const auto separator = entry.find('=');
  if (separator == std::string::npos) {
    *path = entry;
    *id = bare_path_id(entry);
  } else {
    *id = entry.substr(0, separator);
    *path = entry.substr(separator + 1);
  }
}

namespace {

/// The assistant's accepted `--params` keys, camelCase only; the list
/// mix_assistant_config_from_params reads (config_from_params.h).
constexpr std::initializer_list<const char*> kAssistantParamKeys = {"targetTrackLufs",
                                                                    "suggestionStrength",
                                                                    "eqMaxCutDb",
                                                                    "mixBusHeadroomDbtp",
                                                                    "tempoBpm",
                                                                    "enableStructure",
                                                                    "enableGain",
                                                                    "enableBalance",
                                                                    "enableEq",
                                                                    "enableDynamics",
                                                                    "enableImage",
                                                                    "enableHighPass",
                                                                    "nFft",
                                                                    "hopLength"};

// The assistant's refusal for one `suggest-mix --params` entry, or an empty string.
std::string mix_assistant_value_refusal(const std::string& key, double value) {
  try {
    const mastering::api::Param param{key, value};
    mixing::assistant::mix_assistant_config_from_params(&param, 1);
  } catch (const SonareParameterRefusal& refusal) {
    return refusal
        .respelled(
            refusal.code(),
            [&](const std::string& subject) { return cli_bag_subject("--params", subject); }, true)
        .what();
  } catch (const SonareException& refusal) {
    return refusal.what();
  }
  return {};
}

}  // namespace

std::string suggest_mix_argument_refusal(const CliArgs& args) {
  const std::vector<std::string> entries = args.get_string_list("input");
  if (entries.empty()) return "suggest-mix requires at least one --input";
  for (const std::string& entry : entries) {
    std::string id;
    std::string path;
    split_track_entry(entry, &id, &path);
    if (id.empty()) return "--input track id must not be empty: " + entry;
    if (path.empty()) return "--input requires a file path: " + entry;
  }
  const std::string params = args.get_string("params");
  std::stringstream stream(params);
  std::string item;
  bool tempo_in_params = false;
  std::string value_refusal;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == item.size()) {
      return "invalid parameter entry: '" + item + "' (expected key=value)";
    }
    const std::string key = item.substr(0, eq);
    double value;
    try {
      value = read_assistant_number(key, item.substr(eq + 1));
    } catch (const SonareParameterRefusal& refusal) {
      return refusal.what();
    } catch (const std::invalid_argument& refusal) {
      return refusal.what();
    }
    // The library's own refusals of a key it does not read.
    try {
      const bool known = std::any_of(kAssistantParamKeys.begin(), kAssistantParamKeys.end(),
                                     [&key](const char* name) { return key == name; });
      if (!known) {
        mastering::assistant::refuse_unknown_param(
            key, std::vector<std::string>(kAssistantParamKeys.begin(), kAssistantParamKeys.end()));
      }
    } catch (const SonareParameterRefusal& refusal) {
      return refusal
          .respelled(
              refusal.code(),
              [&](const std::string& subject) { return cli_bag_subject("--params", subject); },
              true)
          .what();
    } catch (const std::exception& refusal) {
      return refusal.what();
    }
    if (key == "tempoBpm") tempo_in_params = true;
    if (value_refusal.empty()) value_refusal = mix_assistant_value_refusal(key, value);
  }
  // Naming it twice is a contradiction rather than a precedence question.
  if (args.has("tempo-bpm") && tempo_in_params) {
    return std::string("--tempo-bpm and ") + cli_bag_subject("--params", "tempoBpm") +
           "= set the same value";
  }
  return value_refusal;
}
#endif

#ifdef SONARE_WITH_MASTERING
namespace {

/// A named assistant option and the flat-param key it sets.
struct AssistantOption {
  const char* key;
  const char* option;
};

constexpr AssistantOption kAssistantOptions[] = {
    {"targetLufs", "target-lufs"},
    {"ceilingDbtp", "ceiling-dbtp"},
    {"enableRepair", "enable-repair"},
    {"preferStreamingSafe", "no-streaming-safe"},
    {"speechMonoAmount", "speech-mono-amount"},
    {"targetPlatform", "target-platform"},
    {"preset", "preset"},
};

// Refuses a `mastering-suggest --params` key the mastering assistant does not read.
void validate_assistant_param_key(const std::string& key) {
  for (const AssistantOption& named : kAssistantOptions) {
    if (key == named.key) return;
  }
  std::vector<std::string> accepted;
  for (const AssistantOption& named : kAssistantOptions) accepted.emplace_back(named.key);
  mastering::assistant::refuse_unknown_param(key, accepted);
}

void validate_assistant_param_value(const std::string& key, const std::string& value) {
  std::vector<std::string> tokens;
  if (key == "preset") {
    using namespace mastering::api;
    for (const auto& name : preset_names()) {
      if (preset_kind(preset_from_string(name)) == PresetKind::Mastering) tokens.push_back(name);
    }
  } else if (key == "targetPlatform") {
    tokens = mastering::assistant::platform_names();
  } else {
    read_assistant_number(key, value);
    return;
  }
  if (std::find(tokens.begin(), tokens.end(), value) != tokens.end()) return;
  std::string message = key + " must be one of ";
  const char* separator = "";
  for (const auto& token : tokens) {
    message += separator + token;
    separator = ", ";
  }
  throw SonareParameterRefusal::predicate(
      ErrorCode::InvalidParameter, cli_bag_subject("--params", key), message.substr(key.size() + 1))
      .as_caller_spelled();
}

}  // namespace

std::string assistant_params_refusal(const CliArgs& args) {
  const std::string text = args.get_string("params", "");
  if (std::string refusal = params_grammar_refusal(text); !refusal.empty()) return refusal;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    const std::string key = item.substr(0, eq);
    try {
      validate_assistant_param_key(key);
    } catch (const SonareParameterRefusal& refusal) {
      return refusal
          .respelled(
              refusal.code(),
              [](const std::string& subject) { return cli_bag_subject("--params", subject); }, true)
          .what();
    } catch (const std::exception& refusal) {
      return refusal.what();
    }
    for (const AssistantOption& named : kAssistantOptions) {
      if (key == named.key && args.has(named.option)) {
        return std::string("--") + named.option + " and " + cli_bag_subject("--params", key) +
               "= set the same value";
      }
    }
    try {
      validate_assistant_param_value(key, item.substr(eq + 1));
    } catch (const SonareParameterRefusal& refusal) {
      return refusal.what();
    } catch (const std::invalid_argument& refusal) {
      return refusal.what();
    }
  }
  return {};
}
#endif

#ifdef SONARE_WITH_ARRANGEMENT
std::vector<std::string> synth_preset_name_list() {
  const char* joined = sonare_synth_preset_names();
  return joined != nullptr && joined[0] != '\0' ? split_string(joined, '\n')
                                                : std::vector<std::string>{};
}

// Splits one `--audio <source_id>=FILE` assignment at the FIRST '=', so a path
// containing one stays intact. A source is addressed by id alone: its URI is
// unique too, but accepting either spelling would put two addresses on one
// binding, and the id is what the unresolved-source report already names.
void parse_audio_binding(const std::string& assignment, uint32_t* out_source_id,
                         std::string* out_path) {
  const size_t separator = assignment.find('=');
  if (separator == 0 || separator == std::string::npos || separator + 1 >= assignment.size()) {
    throw std::invalid_argument("argument --audio: must be <source_id>=FILE");
  }
  const long long source_id =
      parse_int64_text("argument --audio: <source_id>", assignment.substr(0, separator));
  if (source_id <= 0 || source_id > static_cast<long long>(UINT32_MAX)) {
    throw std::invalid_argument(
        "argument --audio: <source_id> " +
        sonare::util::refusal::integer_range_predicate(1.0, static_cast<double>(UINT32_MAX)));
  }
  *out_source_id = static_cast<uint32_t>(source_id);
  *out_path = assignment.substr(separator + 1);
}

std::string project_audio_binding_refusal(const CliArgs& args) {
  for (const auto& assignment : args.get_string_list("audio")) {
    uint32_t source_id = 0;
    std::string path;
    try {
      parse_audio_binding(assignment, &source_id, &path);
    } catch (const std::invalid_argument& refusal) {
      return refusal.what();
    }
  }
  return {};
}

std::string project_synth_refusal(const CliArgs& args, bool bare_flag_selects_gm) {
  if (!args.has("synth")) return {};
  const std::string requested = args.get_string("synth");
  if (bare_flag_selects_gm && (requested.empty() || requested == "true")) return {};
  if (find_cli_token(requested, synth_preset_name_list()).has_value()) return {};
  return "argument --synth: unknown synth preset '" + requested + "'";
}
#endif  // SONARE_WITH_ARRANGEMENT
