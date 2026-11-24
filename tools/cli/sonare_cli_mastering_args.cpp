#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "sonare_cli.h"
#include "sonare_cli_registry.h"
#include "util/json.h"
#include "util/json_schema.h"

// What the mastering and repair commands decide from the command line alone, so the
// registry's validators and the handlers read one implementation.
#ifdef SONARE_WITH_MASTERING
std::string params_grammar_refusal(const std::string& text) {
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == item.size()) {
      return "argument --params: must be a comma-separated list of key=value entries";
    }
  }
  return {};
}

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
      const std::string key = item.substr(0, eq);
      // An enum-valued key may be given by its name; the resolver also refuses an unknown one.
      if (resolve_names) {
        try {
          if (const auto named = mastering::api::mastering_enum_value(processor, key, value_text)) {
            params.push_back({key, *named});
            continue;
          }
        } catch (const SonareParameterRefusal& refusal) {
          throw refusal.respelled(
              refusal.code(), [&](const std::string& subject) {
                return cli_bag_subject("--params", subject);
              },
              true);
        }
      }
      throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter,
                                              cli_bag_subject("--params", key),
                                              "must be a finite number")
          .as_caller_spelled();
    }
    params.push_back({item.substr(0, eq), value});
  }
  return params;
}

// Refuses a supplied --params key the named processor does not read.
//
// insert_param_names() builds the processor against an empty map and reports
// every key its config builder probed. Offline-only processors use their own
// parameter reader, so the insert query applies only to registered inserts.
std::string unknown_processor_params_refusal(const std::string& processor,
                                             const std::string& text) {
  static const auto inserts = mastering::api::insert_factory_names();
  if (std::find(inserts.begin(), inserts.end(), processor) == inserts.end()) return {};
  const std::vector<std::string> known = mastering::api::insert_param_names(processor);
  if (known.empty()) return {};
  std::stringstream stream(text);
  std::string item;
  std::string unknown;
  while (std::getline(stream, item, ',')) {
    const std::string key = item.substr(0, item.find('='));
    if (std::binary_search(known.begin(), known.end(), key)) continue;
    if (!unknown.empty()) unknown += ", ";
    unknown += key;
  }
  if (unknown.empty()) return {};
  const auto separator = unknown.find(", ");
  const std::string key = unknown.substr(0, separator);
  const std::string expected = sonare::util::json::schema::expected_keys(known);
  return SonareParameterRefusal::unknown(
             ErrorCode::InvalidParameter, cli_bag_subject("--params", key), expected)
      .as_caller_spelled()
      .what();
}

namespace {

// The `--params` list as a chain document, so the core decides a choice-valued key
// (a name is resolved, a number refused) in its own words.
std::string chain_params_document(const std::string& text) {
  namespace json = sonare::util::json;
  json::Object params;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const auto eq = item.find('=');
    const std::string value_text = item.substr(eq + 1);
    std::istringstream ss(value_text);
    ss.imbue(std::locale::classic());
    double number = 0.0;
    ss >> number;
    const bool is_number = ss && ss.peek() == std::char_traits<char>::eof();
    params[item.substr(0, eq)] = is_number ? json::Value(number) : json::Value(value_text);
  }
  json::Object document;
  document.emplace("version", json::Value(2.0));
  document.emplace("params", json::Value(std::move(params)));
  return json::dump(json::Value(std::move(document)));
}

}  // namespace

std::string chain_params_refusal(const std::string& text) {
  try {
    mastering::api::chain_config_from_json(chain_params_document(text));
  } catch (const SonareParameterRefusal& refusal) {
    return refusal
        .respelled(refusal.code(),
                   [](const std::string& subject) {
                     return cli_bag_subject("--params", subject, "params");
                   },
                   true)
        .what();
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

std::vector<mastering::api::Param> parse_chain_params(const std::string& text) {
  if (const std::string refusal = params_grammar_refusal(text); !refusal.empty()) {
    throw std::invalid_argument(refusal);
  }
  std::vector<mastering::api::Param> out;
  try {
    for (auto& leaf : mastering::api::chain_config_flat_params(chain_params_document(text))) {
      out.push_back({std::move(leaf.key), leaf.value});
    }
  } catch (const SonareParameterRefusal& refusal) {
    throw refusal
        .respelled(refusal.code(),
                   [](const std::string& subject) {
                     return cli_bag_subject("--params", subject, "params");
                   },
                   true);
  }
  return out;
}

namespace {

/// `--params` on `repair` is documented as repair config overrides, so a key
/// reaching any other stage would let an override silently master -- refusing
/// it here is what makes that promise actual rather than aspirational.
void reject_non_repair_overrides(const std::vector<mastering::api::Param>& overrides) {
  for (const auto& param : overrides) {
    if (param.key.rfind("repair.", 0) == 0) continue;
    throw SonareParameterRefusal::predicate(
              ErrorCode::InvalidParameter, cli_bag_subject("--params", param.key),
              "must be a repair.* field")
        .as_caller_spelled();
  }
}

void reject_non_repair_param_keys(const std::string& text) {
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const std::string key = item.substr(0, item.find('='));
    if (key.rfind("repair.", 0) == 0) continue;
    throw SonareParameterRefusal::predicate(
              ErrorCode::InvalidParameter, cli_bag_subject("--params", key),
              "must be a repair.* field")
        .as_caller_spelled();
  }
}

/// The stage vocabulary `--stages` accepts; the chain, not this list, decides
/// the order they run in.
constexpr std::array<const char*, 6> kRepairStageNames = {"declip", "declick", "decrackle",
                                                          "dehum",  "denoise", "dereverb"};

/// Under `--stages`, a `--params` key reaching a stage the list leaves out
/// would tune nothing, so it is refused by name.
void reject_unlisted_stage_overrides(const std::vector<mastering::api::Param>& overrides,
                                     const std::vector<std::string>& stages) {
  for (const auto& param : overrides) {
    const std::string rest = param.key.substr(std::string("repair.").size());
    const std::string stage = rest.substr(0, rest.find('.'));
    if (std::find(kRepairStageNames.begin(), kRepairStageNames.end(), stage) ==
        kRepairStageNames.end()) {
      continue;
    }
    if (std::find(stages.begin(), stages.end(), stage) != stages.end()) continue;
    throw SonareParameterRefusal::predicate(
              ErrorCode::InvalidParameter, cli_bag_subject("--params", param.key),
              "names a stage --stages does not run")
        .as_caller_spelled();
  }
}

}  // namespace

/// Parses `--stages` into the stage names it lists, lower-cased: a closed set,
/// with an empty entry or a repeated stage refused rather than ignored.
std::vector<std::string> parse_repair_stages(const std::string& text) {
  std::vector<std::string> stages;
  std::string expected;
  for (const char* name : kRepairStageNames) {
    if (!expected.empty()) expected += ", ";
    expected += name;
  }
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = text.find(',', start);
    std::string entry =
        text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    const auto first = entry.find_first_not_of(" \t");
    const auto last = entry.find_last_not_of(" \t");
    entry = first == std::string::npos ? std::string() : entry.substr(first, last - first + 1);
    std::transform(entry.begin(), entry.end(), entry.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (entry.empty()) throw std::invalid_argument("--stages has an empty entry");
    if (std::find(kRepairStageNames.begin(), kRepairStageNames.end(), entry) ==
        kRepairStageNames.end()) {
      throw std::invalid_argument("--stages: unknown stage '" + entry + "' (expected " + expected +
                                  ")");
    }
    if (std::find(stages.begin(), stages.end(), entry) != stages.end()) {
      throw std::invalid_argument("--stages names " + entry + " twice");
    }
    stages.push_back(entry);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return stages;
}

std::string repair_argument_refusal(const CliArgs& args) {
  const bool has_preset = args.has("preset");
  const bool has_stages = args.has("stages");
  if (args.has("detect")) {
    // --detect only measures, so nothing that shapes or writes a repair applies.
    for (const char* name : {"output", "explain", "preset", "stages", "params", "bits"}) {
      if (args.has(name)) return std::string("--") + name + " cannot be combined with --detect";
    }
    return {};
  }
  if (!args.has("output")) return "--output is required unless --detect is given";
  if (has_stages && has_preset) return "--stages cannot be combined with --preset";
  // Why each stage was chosen exists only when the assistant chooses them.
  if (args.has("explain") && has_stages) return "--explain cannot be combined with --stages";
  if (args.has("explain") && has_preset) return "--explain cannot be combined with --preset";
  try {
    const std::vector<std::string> stages =
        has_stages ? parse_repair_stages(args.get_string("stages")) : std::vector<std::string>{};
    if (const std::string refusal = params_grammar_refusal(args.get_string("params"));
        !refusal.empty()) {
      return refusal;
    }
    reject_non_repair_param_keys(args.get_string("params"));
    const auto overrides = parse_chain_params(args.get_string("params"));
    reject_non_repair_overrides(overrides);
    if (const std::string refusal = chain_params_refusal(args.get_string("params"));
        !refusal.empty()) {
      return refusal;
    }
    if (has_stages) reject_unlisted_stage_overrides(overrides, stages);
  } catch (const std::exception& error) {
    return error.what();
  }
  return {};
}

namespace {

/// The record's keys; every one is required, matching the library's platform record.
constexpr const char* kStreamingPlatformKeys[] = {"name", "target_lufs", "ceiling_dbtp"};

float streaming_platform_number(const sonare::util::json::Value& entry, const std::string& where,
                                const char* key, bool cli_value) {
  const std::string internal = where + "." + key;
  const std::string subject = cli_value ? cli_bag_subject("--platforms", internal, "platforms")
                                        : internal;
  const sonare::util::json::Value& value = *entry.find(key);
  if (!value.is_number()) {
    throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, subject,
                                             "must be a number")
        .as_caller_spelled();
  }
  // JSON carries no infinity, so a large value is the case for the library float bound.
  if (std::fabs(value.as_number()) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject, "must be finite")
        .as_caller_spelled();
  }
  return value.as_float();
}

// The texts are the library's own platform-record refusals, keyed by the record's path.
void check_streaming_platform_keys(const sonare::util::json::Value& entry,
                                   const std::string& where, bool cli_value) {
  for (const auto& item : entry.as_object()) {
    const auto& key = item.first;
    if (std::find(std::begin(kStreamingPlatformKeys), std::end(kStreamingPlatformKeys), key) ==
        std::end(kStreamingPlatformKeys)) {
      const std::string expected_list =
          sonare::util::json::schema::expected_keys(std::vector<std::string>(
              std::begin(kStreamingPlatformKeys), std::end(kStreamingPlatformKeys)));
      const std::string internal = where + "." + key;
      const std::string subject =
          cli_value ? cli_bag_subject("--platforms", internal, "platforms") : internal;
      throw SonareParameterRefusal::unknown(ErrorCode::InvalidParameter, subject, expected_list)
          .as_caller_spelled();
    }
  }
  for (const char* expected : kStreamingPlatformKeys) {
    if (entry.find(expected) == nullptr) {
      const std::string internal = where + "." + expected;
      const std::string subject =
          cli_value ? cli_bag_subject("--platforms", internal, "platforms") : internal;
      throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                              "must be given")
          .as_caller_spelled();
    }
  }
}

}  // namespace

// Reads a platform document, refusing it in the words above; @p subject names where it came from.
std::vector<StreamingPlatformSpec> parse_streaming_platforms(const std::string& text,
                                                             const std::string& subject) {
  // Same document the Python CLI's --platforms takes, so one platform list
  // drives either front-end. An absent option is not an empty set: it selects
  // the built-in platform list, which is what the C entry point reads NULL/0 as.
  std::vector<StreamingPlatformSpec> platforms;
  if (text.empty()) return platforms;
  try {
    sonare::util::json::Value document;
    try {
      document = sonare::util::json::parse(text);
    } catch (const std::exception&) {
      document = sonare::util::json::Value();
    }
    const auto is_record_list = [&] {
      if (!document.is_array()) return false;
      for (const auto& entry : document.as_array()) {
        if (!entry.is_object()) return false;
      }
      return true;
    };
    if (!is_record_list()) throw std::invalid_argument("must be a JSON array of platform objects");
    for (const auto& entry : document.as_array()) {
      const std::string where = "platforms[" + std::to_string(platforms.size()) + "]";
      const bool cli_value = subject == "argument --platforms";
      check_streaming_platform_keys(entry, where, cli_value);
      StreamingPlatformSpec spec;
      const sonare::util::json::Value& name = *entry.find("name");
      const std::string name_subject =
          cli_value ? cli_bag_subject("--platforms", where + ".name", "platforms")
                    : where + ".name";
      if (!name.is_string()) {
        throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidParameter, name_subject,
                                                 "must be a string")
            .as_caller_spelled();
      }
      if (name.as_string().empty()) {
        throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, name_subject,
                                                "must not be empty")
            .as_caller_spelled();
      }
      spec.name = name.as_string();
      spec.target_lufs = streaming_platform_number(entry, where, "target_lufs", cli_value);
      spec.ceiling_dbtp = streaming_platform_number(entry, where, "ceiling_dbtp", cli_value);
      platforms.push_back(std::move(spec));
    }
  } catch (const SonareParameterRefusal& refusal) {
    if (subject == "argument --platforms") throw;
    throw std::invalid_argument(subject + ": " + refusal.what());
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(subject + ": " + error.what());
  }
  return platforms;
}

std::string streaming_platforms_refusal(const CliArgs& args) {
  // --platforms-file wins when both are given, so the text is then never read.
  if (args.has("platforms-file") || !args.has("platforms")) return {};
  try {
    (void)parse_streaming_platforms(args.get_string("platforms", ""), "argument --platforms");
  } catch (const SonareParameterRefusal& refusal) {
    return refusal.what();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}

#endif
