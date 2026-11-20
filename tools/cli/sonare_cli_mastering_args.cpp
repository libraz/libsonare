#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "sonare_cli.h"
#include "sonare_cli_registry.h"
#include "util/json.h"

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

// Refuses a supplied --params key the named processor does not read.
//
// insert_param_names() builds the processor against an empty map and reports
// every key its config builder probed, so a supplied key outside that set took
// no effect at all. The list is sorted, which makes the membership test a binary
// search. A name with no published key set is left alone: only a realtime insert
// can be probed this way, so an empty list means "not an insert", not "reads nothing".
std::string unknown_processor_params_refusal(const std::string& processor,
                                             const std::string& text) {
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
  return "unknown --params key for " + processor + ": " + unknown;
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
  for (auto& leaf : mastering::api::chain_config_flat_params(chain_params_document(text))) {
    out.push_back({std::move(leaf.key), leaf.value});
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
    throw std::invalid_argument("--params key must be a repair.* field: " + param.key);
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
    throw std::invalid_argument(param.key + " names a stage --stages does not run");
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

/// Spellings the record used to take, refused with the key that replaced them.
constexpr std::pair<const char*, const char*> kStreamingPlatformRenamed[] = {
    {"targetLufs", "target_lufs"},
    {"ceilingDb", "ceiling_dbtp"},
    {"ceilingDbtp", "ceiling_dbtp"},
    {"ceiling_db", "ceiling_dbtp"},
};

float streaming_platform_number(const sonare::util::json::Value& entry, const std::string& where,
                                const char* key) {
  const sonare::util::json::Value& value = *entry.find(key);
  if (!value.is_number()) throw std::invalid_argument(where + "." + key + " must be a number");
  // The library's bound for a float field; JSON carries no infinity, so a large value is the case.
  if (std::fabs(value.as_number()) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw std::invalid_argument(where + "." + key + " must be finite");
  }
  return value.as_float();
}

// The texts are the library's own platform-record refusals, keyed by the record's path.
void check_streaming_platform_keys(const sonare::util::json::Value& entry,
                                   const std::string& where) {
  for (const auto& [key, unused] : entry.as_object()) {
    (void)unused;
    for (const auto& [old_key, new_key] : kStreamingPlatformRenamed) {
      if (key == old_key) {
        throw std::invalid_argument("unknown key " + where + "." + key + " (renamed to " + where +
                                    "." + new_key + ")");
      }
    }
    bool known = false;
    std::string expected_list;
    for (const char* expected : kStreamingPlatformKeys) {
      known = known || key == expected;
      expected_list += (expected_list.empty() ? "" : ", ") + std::string(expected);
    }
    if (!known) {
      throw std::invalid_argument("unknown key " + where + "." + key + " (expected " +
                                  expected_list + ")");
    }
  }
  for (const char* expected : kStreamingPlatformKeys) {
    if (entry.find(expected) == nullptr) {
      throw std::invalid_argument(where + "." + expected + " must be given");
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
      check_streaming_platform_keys(entry, where);
      StreamingPlatformSpec spec;
      const sonare::util::json::Value& name = *entry.find("name");
      if (!name.is_string()) throw std::invalid_argument(where + ".name must be a string");
      if (name.as_string().empty()) throw std::invalid_argument(where + ".name must not be empty");
      spec.name = name.as_string();
      spec.target_lufs = streaming_platform_number(entry, where, "target_lufs");
      spec.ceiling_dbtp = streaming_platform_number(entry, where, "ceiling_dbtp");
      platforms.push_back(std::move(spec));
    }
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
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}

#endif
