#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/json_schema.h"

namespace sonare::mastering::api {
namespace {

namespace json = util::json;

constexpr std::string_view kMultiband = "dynamics.multibandComp.";
constexpr std::string_view kBands = "dynamics.multibandComp.bands";
constexpr std::string_view kCutoffs = "dynamics.multibandComp.crossover.cutoffsHz";
constexpr std::string_view kScalarCutoffs = "dynamics.multibandComp.cutoff{i}Hz";
constexpr std::string_view kIndex = "{i}";
constexpr char kOverrides[] = "overrides";

std::string child(std::string_view path, std::string_view key) {
  return std::string(path) + "." + std::string(key);
}

void collect_values(const json::Value& value, const std::string& path, json::Object& leaves) {
  if (value.is_object()) {
    for (const auto& [key, member] : value.as_object()) {
      collect_values(member, child(path, key), leaves);
    }
  } else if (value.is_array()) {
    for (size_t i = 0; i < value.as_array().size(); ++i) {
      collect_values(value[i], child(path, std::to_string(i)), leaves);
    }
  } else {
    leaves.emplace(path, value);
  }
}

json::Value example(const json::Value& param) {
  const auto& choices = param["choices"];
  if (param["type"].as_string() == "enum" && choices.is_array()) {
    return choices[0]["name"];
  }
  return param["default"];
}

const json::Array& offline_params(const json::Value& processors, const std::string& id) {
  for (const auto& processor : processors.as_array()) {
    if (processor["id"].as_string() != id) continue;
    const auto* offline = processor.find("offlineParams");
    return (offline ? *offline : processor["params"]).as_array();
  }
  FAIL("Missing processor " << id);
  return processors.as_array();
}

std::string indexed_key(std::string name, std::string_view index) {
  const auto placeholder = name.find(kIndex);
  if (placeholder == std::string::npos) return child(name, index);
  name.replace(placeholder, kIndex.size(), index);
  return name;
}

struct CatalogKeys {
  json::Object concrete;
  std::set<std::string> grammar;
};

CatalogKeys catalog_keys() {
  const auto chain = json::parse_strict(chain_catalog_json());
  const auto processors = json::parse_strict(processor_catalog_json());
  CatalogKeys keys;
  const auto add = [&](const std::string& key, json::Value sample) {
    keys.concrete.emplace(key, std::move(sample));
    keys.grammar.insert(key);
  };
  std::set<std::string> list_roots;
  const json::Value* cutoff_list = nullptr;
  const json::Value* scalar_cutoffs = nullptr;
  for (const auto& list : chain["lists"].as_array()) {
    const auto& key = list["key"].as_string();
    REQUIRE(list_roots.insert(key).second);
    if (key == kCutoffs) cutoff_list = &list;
    if (key == kScalarCutoffs) scalar_cutoffs = &list;
  }
  REQUIRE(cutoff_list != nullptr);
  REQUIRE(scalar_cutoffs != nullptr);
  CHECK((*scalar_cutoffs)["minItems"].as_int() == (*cutoff_list)["minItems"].as_int());
  CHECK((*scalar_cutoffs)["maxItems"].as_int() == (*cutoff_list)["maxItems"].as_int());
  for (const auto& param : chain["params"].as_array()) {
    const auto& name = param["name"].as_string();
    if (!list_roots.count(name)) add(name, example(param));
  }
  std::set<std::string> list_descriptors;
  for (const auto& stage : chain["stages"].as_array()) {
    const auto& prefix = stage["prefix"].as_string();
    add(stage["enabledKey"].as_string(), json::Value(false));
    // Multiband construction uses chain parameters and lists for its stage.
    if (stage["processor"].is_null() || prefix + "." == kMultiband) continue;
    REQUIRE(stage["form"].as_string() == "offline");
    for (const auto& param : offline_params(processors, stage["processor"].as_string())) {
      add(child(prefix, param["name"].as_string()), example(param));
    }
  }
  for (const auto& list : chain["lists"].as_array()) {
    const auto& name = list["key"].as_string();
    const int size = list["maxItems"].as_int();
    const auto add_indexed = [&](std::string_view field, const json::Value& sample) {
      const auto at = [&](std::string_view index) {
        const auto key = indexed_key(name, index);
        return field.empty() ? key : child(key, field);
      };
      keys.grammar.insert(at(kIndex));
      for (int i = 0; i < size; ++i) {
        keys.concrete.emplace(at(std::to_string(i)), sample);
      }
    };
    if (list["processor"].is_null()) {
      for (const auto& param : chain["params"].as_array()) {
        if (param["name"].as_string() == name) {
          REQUIRE(param["type"].as_string() == "number");
          list_descriptors.insert(name);
          break;
        }
      }
      REQUIRE(list_descriptors.count(name) == 1);
      add_indexed("", nullptr);
    } else {
      REQUIRE(list["form"].as_string() == "offline");
      for (const auto& param : offline_params(processors, list["processor"].as_string())) {
        add_indexed(param["name"].as_string(), example(param));
      }
    }
  }
  return keys;
}

std::string mastering_source(const char* file) {
  const auto path = std::filesystem::path(SONARE_SOURCE_ROOT) / "src/mastering/api" / file;
  std::ifstream input(path);
  INFO(path.string());
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

template <typename Collect>
void each_match(const std::string& text, const std::regex& expression, Collect collect) {
  for (auto it = std::sregex_iterator(text.begin(), text.end(), expression);
       it != std::sregex_iterator(); ++it) {
    collect(*it);
  }
}

std::set<std::string> reader_keys() {
  const std::string source = mastering_source("chain_params.cpp");
  const auto begin = source.find("bool apply_repair_param(");
  const auto end = source.find("void require_float_representable(", begin);
  REQUIRE(begin != std::string::npos);
  REQUIRE(end != std::string::npos);
  const auto branches = source.substr(begin, end - begin);

  std::set<std::string> keys;
  const std::regex literal(R"rx(if \(key == "([^"]+)"\))rx");
  each_match(branches, literal, [&](const auto& match) { keys.insert(match[1]); });

  std::set<std::string> band_fields;
  const std::regex band_literal(R"rx(if \(field == "([^"]+)"\))rx");
  each_match(branches, band_literal, [&](const auto& match) { band_fields.insert(match[1]); });
  const std::regex crossover(R"rx(if \(rest == "([^"]+)"\))rx");
  each_match(branches, crossover, [&](const auto& match) {
    keys.insert(std::string(kMultiband) + "crossover." + match[1].str());
  });

  const auto field_source =
      std::regex_replace(mastering_source("param_field_tables.h"), std::regex("\\\\\n"), " ");
  const std::regex table(R"(#define (SONARE_FIELDS_\w+)\(X\)[ \t]+([^\n]+))");
  std::map<std::string, std::string> tables;
  each_match(field_source, table, [&](const auto& match) { tables.emplace(match[1], match[2]); });

  const std::regex block(R"(#define X\(jkey, member, meta\)([\s\S]*?)#undef X)");
  const std::regex prefixed(R"rx(if \(key == "([^"]+)" jkey\))rx");
  const std::regex invocation(R"((SONARE_FIELDS_\w+)\(X\))");
  const std::regex field(R"rx(\bX\("([^"]+)")rx");
  each_match(branches, block, [&](const auto& match) {
    const auto body = match[1].str();
    std::smatch prefix;
    const bool stage_field = std::regex_search(body, prefix, prefixed);
    REQUIRE((stage_field || body.find("if (field == jkey)") != std::string::npos));
    each_match(body, invocation, [&](const auto& use) {
      const auto& declaration = tables.at(use[1].str());
      REQUIRE_FALSE(std::regex_search(declaration, invocation));
      each_match(declaration, field, [&](const auto& key) {
        if (stage_field) {
          keys.insert(prefix[1].str() + key[1].str());
        } else {
          band_fields.insert(key[1]);
        }
      });
    });
  });
  REQUIRE_FALSE(band_fields.empty());
  REQUIRE(branches.find(R"(parse_indexed(rest, "cutoff", "Hz", &index))") != std::string::npos);
  REQUIRE(branches.find(R"(parse_band_field(rest, "band", &index, &field))") != std::string::npos);
  REQUIRE(source.find("if (is_cutoff_list_key(canonical[i], &index))") != std::string::npos);
  for (size_t band = 0; band < kMaxMultibandBands; ++band) {
    for (const auto& field_name : band_fields) {
      keys.insert(child(child(kBands, std::to_string(band)), field_name));
    }
  }
  for (size_t cutoff = 0; cutoff < kMaxMultibandBands - 1; ++cutoff) {
    keys.insert(child(kCutoffs, std::to_string(cutoff)));
    keys.insert(std::string(kMultiband) + "cutoff" + std::to_string(cutoff) + "Hz");
  }
  return keys;
}

std::optional<SonareParameterRefusal> probe(const MasteringChainConfig& base, json::Object params) {
  try {
    const auto bag = json::dump(json::Value(std::move(params)));
    (void)chain_config_overrides_from_json(base, chain_params_document(bag, kOverrides),
                                           chain_params_argument(kOverrides));
    return std::nullopt;
  } catch (const SonareParameterRefusal& refusal) {
    return refusal;
  }
}

}  // namespace

TEST_CASE("mastering chain catalog describes every override key",
          "[mastering][chain-overrides-keys]") {
  MasteringChainConfig base;
  auto& multiband = base.dynamics.multiband_comp.config;
  multiband.crossover.cutoffs_hz.resize(kMaxMultibandBands - 1);
  multiband.bands.resize(kMaxMultibandBands);
  constexpr float kCutoffSpacingHz = 100.0f;
  constexpr float kSidechainHz = 80.0f;
  for (size_t i = 0; i < multiband.crossover.cutoffs_hz.size(); ++i) {
    multiband.crossover.cutoffs_hz[i] = kCutoffSpacingHz * static_cast<float>(i + 1);
  }
  base.dynamics.compressor.config.sidechain_high_pass_hz = kSidechainHz;
  for (auto& band : multiband.bands) band.sidechain_high_pass_hz = kSidechainHz;

  json::Object values;
  const auto written = json::parse_strict(chain_config_to_json(base));
  for (const auto& [key, value] : written["params"].as_object()) {
    collect_values(value, key, values);
  }

  const auto published = catalog_keys();
  const auto& catalog = published.concrete;
  const auto reader = reader_keys();
  REQUIRE_FALSE(reader.empty());
  REQUIRE_FALSE(catalog.empty());

  json::Object candidates = catalog;
  for (const auto& key : reader) candidates.try_emplace(key, nullptr);
  std::string reader_only;
  std::string catalog_refused;
  std::string unprobed_reader;
  const std::regex scalar_cutoff(R"(dynamics\.multibandComp\.cutoff([0-9]+)Hz)");
  for (const auto& [key, fallback] : candidates) {
    json::Object params;
    std::smatch cutoff;
    if (const auto value = values.find(key); value != values.end()) {
      params.emplace(key, value->second);
      const auto cutoff_prefix = std::string(kCutoffs) + ".";
      if (key.compare(0, cutoff_prefix.size(), cutoff_prefix) == 0) {
        const auto index = std::stoul(key.substr(cutoff_prefix.size()));
        for (size_t i = 0; i < index; ++i) {
          const auto preceding = child(kCutoffs, std::to_string(i));
          params.emplace(preceding, values.at(preceding));
        }
      }
    } else if (std::regex_match(key, cutoff, scalar_cutoff)) {
      const auto value = values.find(child(kCutoffs, cutoff[1].str()));
      params.emplace(key, value == values.end() ? json::Value(kCutoffSpacingHz) : value->second);
    } else {
      params.emplace(key, fallback.is_null() ? json::Value(1.0) : fallback);
    }

    if (const auto refusal = probe(base, std::move(params))) {
      auto& failures = catalog.count(key) ? catalog_refused : unprobed_reader;
      failures += "\n  " + key + " (" + refusal->what() + ")";
    } else if (!catalog.count(key)) {
      reader_only += "\n  " + key;
    }
  }
  INFO("Accepted by the reader but missing from the catalog:" << reader_only);
  INFO("Published by the catalog but refused by the reader:" << catalog_refused);
  INFO("Reader branches with no accepted probe value:" << unprobed_reader);

  const std::string expected = json::schema::expected_keys(
      std::vector<std::string>(published.grammar.begin(), published.grammar.end()));
  REQUIRE_FALSE(expected.empty());
  for (const char* key :
       {"repair.denoise.gainFloor", "eq.tilt.tiltDB", "dynamics.multibandComp.slope",
        "dynamics.multibandComp.bands.01.ratio", "dynamics.multibandComp.cutoff01Hz",
        "saturation.ampSim.drive", "unknown.stage.key"}) {
    CAPTURE(key);
    const auto refusal = probe(base, {{key, json::Value(1.0)}});
    REQUIRE(refusal.has_value());
    CHECK(refusal->code() == ErrorCode::InvalidParameter);
    CHECK(refusal->form() == SonareParameterRefusal::Form::UnknownKey);
    CHECK(refusal->subject() == child(kOverrides, key));
    CHECK(refusal->expected() == expected);
    CHECK(refusal->what() == json::schema::unknown_key_expected(child(kOverrides, key), expected));
  }

  CHECK(catalog_refused.empty());
  CHECK(unprobed_reader.empty());
  CHECK(reader_only.empty());
}

TEST_CASE("mastering chain documents refuse undeclared keys before reading values",
          "[mastering][chain-overrides-keys]") {
  const MasteringChainConfig base;
  const std::string before = chain_config_to_json(base);
  const std::string& expected = chain_expected_keys();
  for (const std::string_view value : {"{}", "\"x\"", "null", "[]", "1"}) {
    for (const bool nested : {false, true}) {
      CAPTURE(value, nested);
      const std::string setting = "\"undeclared\":" + std::string(value);
      const std::string document = R"({"params":{"eq.tilt.tiltDb":2,)" +
                                   (nested ? "\"eq\":{\"tilt\":{" + setting + "}}" : setting) +
                                   "}}";
      const std::string path = nested ? "params.eq.tilt.undeclared" : "params.undeclared";
      std::optional<MasteringChainConfig> loaded;
      try {
        loaded = chain_config_overrides_from_json(base, document);
        CHECK_FALSE(loaded.has_value());
      } catch (const SonareParameterRefusal& refusal) {
        CHECK(refusal.code() == ErrorCode::InvalidFormat);
        CHECK(refusal.form() == SonareParameterRefusal::Form::UnknownKey);
        CHECK(refusal.subject() == path);
        CHECK(refusal.what() == json::schema::unknown_key_expected(path, expected));
        CHECK_FALSE(loaded.has_value());
      }
      CHECK(chain_config_to_json(base) == before);
    }
  }

  const auto empty_groups = chain_config_overrides_from_json(
      base, R"({"params":{"eq":{"tilt":{"$annotation":{"anything":null}}},"x-extra":[]}})");
  CHECK(chain_config_to_json(empty_groups) == before);
}

TEST_CASE("streaming mastering chain reports the expected keys for an undeclared parameter",
          "[mastering][chain-overrides-keys]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChain chain(config);
  chain.prepare(kInsertProbeSampleRate, kInsertProbeBlockSize, 1);
  const std::string before = chain_config_to_json(chain.config());
  const std::string key = "eq.tilt.undeclared";
  try {
    chain.set_parameter(key, 2.0);
    FAIL("expected an unknown-key refusal");
  } catch (const SonareParameterRefusal& refusal) {
    CHECK(refusal.code() == ErrorCode::InvalidParameter);
    CHECK(refusal.form() == SonareParameterRefusal::Form::UnknownKey);
    CHECK(refusal.subject() == key);
    CHECK(refusal.what() == json::schema::unknown_key_expected(key, chain_expected_keys()));
  }
  CHECK(chain_config_to_json(chain.config()) == before);
}

}  // namespace sonare::mastering::api
