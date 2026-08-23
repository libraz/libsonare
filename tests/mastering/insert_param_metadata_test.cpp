// The host-facing parameter metadata every insert publishes: its type, its
// design default, and the range construction accepts.
//
// All three are DERIVED rather than declared — the type and the default come
// from the config builder's own accessors, the bounds are measured by handing
// candidate values to the same construction path a caller uses. Nothing here is
// a hand-maintained table, so what these cases pin is the derivation: that it
// covers every construction key, that it agrees with the config structs, and
// that the published range really is the range construction enforces.

#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/saturation/tape.h"
#include "mastering/stereo/imager.h"
#include "util/json.h"

namespace {

namespace json = sonare::util::json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::api::make_insert;

json::Array param_info(const std::string& name) {
  const json::Value parsed = json::parse_strict(insert_param_info_json(name));
  REQUIRE(parsed.is_array());
  return parsed.as_array();
}

const json::Value& field(const json::Value& parameter, const std::string& key) {
  const json::Value* value = parameter.find(key);
  REQUIRE(value != nullptr);
  return *value;
}

const json::Value* find_param(const json::Array& params, const std::string& name) {
  for (const json::Value& parameter : params) {
    if (field(parameter, "name").as_string() == name) return &parameter;
  }
  return nullptr;
}

// A one-key insert config carrying @p value under @p key, serialized the way a
// host would. Going through the JSON writer rather than std::to_string keeps a
// default that needs full float precision from being rounded on its way back in.
std::string one_param_json(const std::string& key, const json::Value& value) {
  json::Object params;
  params.emplace(key, value);
  return json::dump(json::Value(std::move(params)));
}

bool builds_with(const std::string& name, const std::string& key, const json::Value& value) {
  try {
    return make_insert(name, one_param_json(key, value)) != nullptr;
  } catch (...) {
    return false;
  }
}

}  // namespace

TEST_CASE("every insert publishes a default for every construction key it automates",
          "[mastering][catalog]") {
  // The per-band EQ surface is the bulk of the flat parameter set and is only
  // read when the caller supplies a band, so it is the part that silently
  // publishes nothing unless the builders declare their bands explicitly.
  for (const std::string& name : insert_factory_names()) {
    const std::vector<std::string> construction_keys = insert_param_names(name);
    const std::set<std::string> keys(construction_keys.begin(), construction_keys.end());
    for (const json::Value& parameter : param_info(name)) {
      const std::string key = field(parameter, "name").as_string();
      // A descriptor id with no construction key of the same name cannot have a
      // construction default, and correctly publishes none.
      if (keys.find(key) == keys.end()) continue;
      INFO(name << " parameter " << key);
      REQUIRE_FALSE(field(parameter, "default").is_null());
    }
  }
}

TEST_CASE("a published default is a value construction accepts", "[mastering][catalog]") {
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      const json::Value& fallback = field(parameter, "default");
      if (fallback.is_null()) continue;
      const std::string key = field(parameter, "name").as_string();
      INFO(name << " parameter " << key);
      REQUIRE(builds_with(name, key, fallback));
    }
  }
}

TEST_CASE("a published bound brackets the default and rejects the value beyond it",
          "[mastering][catalog]") {
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      const std::string key = field(parameter, "name").as_string();
      const json::Value& minimum = field(parameter, "min");
      const json::Value& maximum = field(parameter, "max");
      const json::Value& fallback = field(parameter, "default");
      INFO(name << " parameter " << key);

      if (!minimum.is_null() && !maximum.is_null()) {
        REQUIRE(minimum.as_number() <= maximum.as_number());
      }
      // The default has to sit inside the range the catalog publishes, or the
      // processor ships a configuration its own validation rejects.
      if (fallback.is_number()) {
        if (!minimum.is_null()) REQUIRE(fallback.as_number() >= minimum.as_number());
        if (!maximum.is_null()) REQUIRE(fallback.as_number() <= maximum.as_number());
      }
      // A bound is only worth publishing if it is enforced. One unit outside is
      // safely outside for every measured bound, whose resolution is far finer.
      if (!minimum.is_null()) {
        REQUIRE_FALSE(builds_with(name, key, json::Value(minimum.as_number() - 1.0)));
      }
      if (!maximum.is_null()) {
        REQUIRE_FALSE(builds_with(name, key, json::Value(maximum.as_number() + 1.0)));
      }
    }
  }
}

TEST_CASE("published defaults come from the config struct's own initializers",
          "[mastering][catalog]") {
  const sonare::mastering::dynamics::CompressorConfig compressor;
  const json::Array compressor_params = param_info("dynamics.compressor");
  REQUIRE(find_param(compressor_params, "thresholdDb")->find("default")->as_number() ==
          static_cast<double>(compressor.threshold_db));
  REQUIRE(find_param(compressor_params, "ratio")->find("default")->as_number() ==
          static_cast<double>(compressor.ratio));
  REQUIRE(find_param(compressor_params, "releaseMs")->find("default")->as_number() ==
          static_cast<double>(compressor.release_ms));
  // auto_makeup is the standing example of a boolean config field whose key does
  // not end in "Enabled": it must publish as a JSON boolean, not as 0.
  const json::Value* auto_makeup = find_param(compressor_params, "autoMakeup");
  REQUIRE(auto_makeup->find("type")->as_string() == "boolean");
  REQUIRE(auto_makeup->find("default")->is_bool());
  REQUIRE(auto_makeup->find("default")->as_bool() == compressor.auto_makeup);
  // A boolean cannot be out of range, so it carries no measured bounds.
  REQUIRE(auto_makeup->find("min")->is_null());
  REQUIRE(auto_makeup->find("max")->is_null());

  const sonare::mastering::saturation::TapeConfig tape;
  REQUIRE(find_param(param_info("saturation.tape"), "driveDb")->find("default")->as_number() ==
          static_cast<double>(tape.drive_db));

  const sonare::mastering::stereo::ImagerConfig imager;
  REQUIRE(find_param(param_info("stereo.imager"), "width")->find("default")->as_number() ==
          static_cast<double>(imager.width));
}

TEST_CASE("measured bounds reproduce the validation they were measured through",
          "[mastering][catalog]") {
  // Compressor::validate_config demands ratio >= 1 and non-negative timings, and
  // leaves thresholdDb and makeupGainDb open.
  const json::Array compressor = param_info("dynamics.compressor");
  REQUIRE(find_param(compressor, "ratio")->find("min")->as_number() == 1.0);
  REQUIRE(find_param(compressor, "ratio")->find("max")->is_null());
  REQUIRE(find_param(compressor, "attackMs")->find("min")->as_number() == 0.0);
  REQUIRE(find_param(compressor, "thresholdDb")->find("min")->is_null());
  REQUIRE(find_param(compressor, "makeupGainDb")->find("max")->is_null());
  // sidechainHpfHz is validated as strictly positive, and an exclusive bound is
  // published as the limit it excludes.
  REQUIRE(find_param(compressor, "sidechainHpfHz")->find("min")->as_number() == 0.0);
  REQUIRE_FALSE(builds_with("dynamics.compressor", "sidechainHpfHz", json::Value(0.0)));

  // The two-sided ranges the imager checks explicitly.
  const json::Array imager = param_info("stereo.imager");
  REQUIRE(find_param(imager, "width")->find("min")->as_number() == 0.0);
  REQUIRE(find_param(imager, "width")->find("max")->as_number() == 2.0);
  REQUIRE(find_param(imager, "decorrelationAmount")->find("max")->as_number() == 1.0);

  // A signed range, and an integer-valued parameter: the flat surface rounds
  // before the field sees it, so its bounds are whole numbers rather than the
  // midpoint between the last accepted and first rejected setting.
  REQUIRE(find_param(param_info("stereo.stereoBalance"), "balance")->find("min")->as_number() ==
          -1.0);
  const json::Array bitcrusher = param_info("saturation.bitcrusher");
  REQUIRE(find_param(bitcrusher, "bitDepth")->find("min")->as_number() == 1.0);
  REQUIRE(find_param(bitcrusher, "bitDepth")->find("max")->as_number() == 24.0);
}

TEST_CASE("declaring a band's parameters does not make its keys count as read",
          "[mastering][catalog]") {
  // The band readers are replayed against a throwaway map so the catalog learns
  // every band key without the live map probing it. A key the processor still
  // ignores must therefore still be reported as ignored.
  std::vector<std::string> ignored;
  REQUIRE(make_insert("eq.minimumPhase", R"({"band0.q":2.0})", &ignored) != nullptr);
  REQUIRE(ignored == std::vector<std::string>{"band0.q"});

  // And a band the caller does supply is read as before.
  ignored.clear();
  REQUIRE(make_insert("eq.minimumPhase", R"({"band0.frequencyHz":800,"band0.q":2.0})", &ignored) !=
          nullptr);
  REQUIRE(ignored.empty());
}
