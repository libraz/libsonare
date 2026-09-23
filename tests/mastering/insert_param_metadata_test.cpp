// The host-facing parameter metadata every insert publishes: its type, its
// design default, and the range construction accepts.
//
// All three are DERIVED rather than declared — the type and the default come
// from the config builder's own accessors, the bounds are measured by handing
// candidate values to the same construction path a caller uses. Nothing here is
// a hand-maintained table, so what these cases pin is the derivation: that it
// covers every construction key, that it agrees with the config structs, and
// that the published range really is the range construction enforces.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/processor_params.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/saturation/tape.h"
#include "mastering/stereo/imager.h"
#include "rt/processor_base.h"
#include "support/schema_paths.h"
#include "util/json.h"

#ifdef SONARE_WITH_FX
#include "effects/delay/stereo_delay.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/ring_modulator.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#endif

namespace {

namespace json = sonare::util::json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::api::insert_probe_params;
using sonare::mastering::api::make_insert;

json::Array param_info(const std::string& name) {
  const json::Value parsed = json::parse_strict(insert_param_info_json(name));
  REQUIRE(parsed.is_array());
  return parsed.as_array();
}

// The key is taken as a pointer rather than as const std::string&: every caller
// passes a literal, and a reference parameter would bind to a temporary string
// that GCC then reports as the possible referent of the returned reference.
const json::Value& field(const json::Value& parameter, const char* key) {
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

// The config the catalog measures @p key through, carrying @p value, serialized
// the way a host would. Going through the JSON writer rather than std::to_string
// keeps a default that needs full float precision from being rounded on its way
// back in.
std::string probe_json(const std::string& name, const std::string& key, const json::Value& value) {
  json::Object params;
  for (const auto& param : insert_probe_params(name, key, 0.0)) {
    if (param.key != key) params.emplace(param.key, json::Value(param.value));
  }
  params.emplace(key, value);
  return json::dump(json::Value(std::move(params)));
}

bool builds_with(const std::string& name, const std::string& key, const json::Value& value) {
  try {
    return make_insert(name, probe_json(name, key, value)) != nullptr;
  } catch (...) {
    return false;
  }
}

// Build, then prepare at the catalog's probe rate: some processors refuse a
// setting only once they know the rate they run at.
bool prepares_with(const std::string& name, const std::string& key, double value) {
  try {
    const std::unique_ptr<sonare::rt::ProcessorBase> processor =
        make_insert(name, probe_json(name, key, json::Value(value)));
    if (processor == nullptr) return false;
    processor->prepare(sonare::mastering::api::kInsertProbeSampleRate,
                       sonare::mastering::api::kInsertProbeBlockSize);
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<double> choice_values(const json::Value& parameter) {
  std::vector<double> values;
  for (const json::Value& choice : field(parameter, "choices").as_array()) {
    values.push_back(field(choice, "value").as_number());
  }
  return values;
}

std::vector<std::string> choice_names(const json::Value& parameter) {
  std::vector<std::string> names;
  for (const json::Value& choice : field(parameter, "choices").as_array()) {
    names.push_back(field(choice, "name").as_string());
  }
  return names;
}

#ifdef SONARE_WITH_FX
// Compares an insert's construction keys against the arity of the config struct
// behind it. The two sides come from different places — the keys from probing
// the factory, the arity from the struct's own declaration — so neither can be
// corrected into agreement with the other.
template <typename Config>
void require_a_key_per_config_field(const std::string& name, std::size_t unexposed = 0) {
  const std::vector<std::string> keys = insert_param_names(name);
  INFO(name);
  // A name the factory does not know, and one whose feature is off, both return
  // an empty list — which would agree with any config without the factory being
  // asked anything at all.
  REQUIRE_FALSE(keys.empty());
  CHECK(keys.size() + unexposed == sonare::mastering::api::detail::field_count<Config>());
}
#endif

}  // namespace

TEST_CASE("the parameter info lists every key construction reads", "[mastering][catalog]") {
  std::vector<std::string> missing;
  for (const std::string& name : insert_factory_names()) {
    std::set<std::string> listed;
    for (const json::Value& parameter : param_info(name)) {
      listed.insert(field(parameter, "name").as_string());
    }
    for (const std::string& key : insert_param_names(name)) {
      if (listed.count(key) == 0) missing.push_back(name + " " + key);
    }
  }
  INFO(missing.size() << " construction keys missing; first: "
                      << (missing.empty() ? std::string() : missing.front()));
  REQUIRE(missing.empty());
}

TEST_CASE("automation targets come first in id order, then construction-only keys by name",
          "[mastering][catalog]") {
  std::size_t construction_only = 0;
  for (const std::string& name : insert_factory_names()) {
    INFO(name);
    const std::unique_ptr<sonare::rt::ProcessorBase> processor = make_insert(name, "{}");
    REQUIRE(processor != nullptr);
    const auto descriptors = processor->parameter_descriptors();
    const json::Array params = param_info(name);
    REQUIRE(params.size() >= descriptors.size());
    std::set<std::string> descriptor_keys;
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      INFO("entry " << index);
      REQUIRE(field(params[index], "name").as_string() == descriptors[index].key);
      REQUIRE(field(params[index], "id").as_number() == static_cast<double>(descriptors[index].id));
      descriptor_keys.insert(descriptors[index].key);
    }
    std::string previous;
    for (std::size_t index = descriptors.size(); index < params.size(); ++index) {
      const std::string key = field(params[index], "name").as_string();
      INFO("entry " << index << " " << key);
      REQUIRE(field(params[index], "id").is_null());
      REQUIRE(field(params[index], "rtSafe").as_bool() == false);
      REQUIRE(descriptor_keys.count(key) == 0);
      if (index > descriptors.size()) REQUIRE(previous < key);
      previous = key;
      ++construction_only;
    }
  }
  // Every insert listing only its automation targets passes the loop above.
  REQUIRE(construction_only > 0);
}

TEST_CASE("choices list exactly the values construction accepts", "[mastering][catalog][.][slow]") {
  std::size_t with_choices = 0;
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      const std::string key = field(parameter, "name").as_string();
      const std::string type = field(parameter, "type").as_string();
      INFO(name << " parameter " << key);
      if (field(parameter, "choices").is_null()) {
        REQUIRE(type != "enum");
        continue;
      }
      ++with_choices;
      REQUIRE(type != "boolean");
      REQUIRE(field(parameter, "min").is_null());
      REQUIRE(field(parameter, "max").is_null());
      const std::vector<double> values = choice_values(parameter);
      const std::vector<std::string> names = choice_names(parameter);
      REQUIRE_FALSE(values.empty());
      REQUIRE(std::is_sorted(values.begin(), values.end()));
      REQUIRE(std::adjacent_find(values.begin(), values.end()) == values.end());
      REQUIRE(std::set<std::string>(names.begin(), names.end()).size() == names.size());
      const json::Value& fallback = field(parameter, "default");
      if (fallback.is_number()) {
        REQUIRE(std::find(values.begin(), values.end(), fallback.as_number()) != values.end());
      }
      for (const double value : values) {
        INFO("choice " << value);
        REQUIRE(prepares_with(name, key, value));
      }
      // Every other value up to one past the largest listed must be refused: a
      // declared value the processor rejects is left out rather than listed.
      if (type != "enum") continue;
      for (double value = 0.0; value <= values.back() + 1.0; value += 1.0) {
        if (std::find(values.begin(), values.end(), value) != values.end()) continue;
        INFO("unlisted " << value);
        REQUIRE_FALSE(prepares_with(name, key, value));
      }
    }
  }
  REQUIRE(with_choices > 0);
}

TEST_CASE("choices leave out what the processor refuses and name an integer set by value",
          "[mastering][catalog]") {
  const json::Array exciter = param_info("saturation.exciter");
  const json::Value* aliasing = find_param(exciter, "aliasing");
  REQUIRE(aliasing != nullptr);
  REQUIRE(field(*aliasing, "type").as_string() == "enum");
  REQUIRE(choice_names(*aliasing) == std::vector<std::string>{"none", "oversample4x"});
  REQUIRE(choice_values(*aliasing) == std::vector<double>{0.0, 3.0});

  // A linear-phase EQ has no all-pass response to realize, and says so at prepare;
  // a build alone accepts it.
  const json::Array linear_phase = param_info("eq.linearPhase");
  const json::Value* band_type = find_param(linear_phase, "band0.type");
  REQUIRE(band_type != nullptr);
  REQUIRE(field(*band_type, "type").as_string() == "enum");
  const std::vector<std::string> band_types = choice_names(*band_type);
  REQUIRE(std::find(band_types.begin(), band_types.end(), "peak") != band_types.end());
  REQUIRE(std::find(band_types.begin(), band_types.end(), "allPass") == band_types.end());
  REQUIRE(builds_with("eq.linearPhase", "band0.type", json::Value(9.0)));

  // A whole-number control whose accepted set has holes: [1, 8] would invite a 3.
  const json::Array tube = param_info("saturation.tube");
  const json::Value* oversample = find_param(tube, "oversampleFactor");
  REQUIRE(oversample != nullptr);
  REQUIRE(field(*oversample, "type").as_string() == "number");
  REQUIRE(choice_values(*oversample) == std::vector<double>{1.0, 2.0, 4.0, 8.0});
  REQUIRE(choice_names(*oversample) == std::vector<std::string>{"1", "2", "4", "8"});
  REQUIRE(field(*oversample, "min").is_null());
  REQUIRE(field(*oversample, "max").is_null());
}

namespace {

// Names unique within the enum, and every declared value inside the scan with
// room to spare, so a value the scan cannot reach is a failure here first.
template <typename Enum>
std::vector<std::string> declared_names() {
  namespace detail = sonare::mastering::api::detail;
  const std::vector<detail::EnumChoice> choices = detail::enum_choices<Enum>();
  std::vector<std::string> names;
  for (const detail::EnumChoice& choice : choices) names.push_back(choice.name);
  REQUIRE_FALSE(choices.empty());
  REQUIRE(choices.back().value <= detail::kEnumOrdinalScanLimit - 1);
  REQUIRE(std::set<std::string>(names.begin(), names.end()).size() == names.size());
  return names;
}

}  // namespace

TEST_CASE("every enum a flat parameter selects has unique names within the scan",
          "[mastering][catalog]") {
  namespace m = sonare::mastering;
  (void)declared_names<sonare::rt::AliasingControl>();
  (void)declared_names<m::dynamics::DetectorMode>();
  (void)declared_names<m::saturation::WaveshaperCurve>();
  (void)declared_names<m::final::DitherType>();
  (void)declared_names<m::saturation::QuantizerMode>();
  (void)declared_names<sonare::effects::modulation::PhaserMixMode>();
  (void)declared_names<sonare::effects::modulation::PreFilterMode>();
  (void)declared_names<m::multiband::CrossoverMode>();
  (void)declared_names<m::eq::LinearPhaseEqConfig::Resolution>();
  (void)declared_names<m::eq::PultecComponentModel>();
  (void)declared_names<m::eq::StereoPlacement>();
  (void)declared_names<m::eq::PhaseMode>();
  (void)declared_names<m::eq::BiquadCoeffMode>();
  (void)declared_names<m::multiband::SaturationType>();
  (void)declared_names<m::saturation::AmpModel>();
  (void)declared_names<m::saturation::AmpTopology>();
  (void)declared_names<m::saturation::MicModel>();

  // The spelling rule on its awkward cases: an acronym, a `k` before a digit or
  // an acronym, and an interior capital.
  REQUIRE(declared_names<m::multiband::CrossoverSlope>() ==
          std::vector<std::string>{"lr2", "lr4", "lr8"});
  REQUIRE(declared_names<m::saturation::PowerTube>() ==
          std::vector<std::string>{"6l6", "el34", "el84", "6v6"});
  REQUIRE(declared_names<m::saturation::CabModel>() ==
          std::vector<std::string>{"guitar4x12", "bass8x10"});
  REQUIRE(declared_names<m::eq::CutFilterSlope>().front() == "db12PerOct");
  REQUIRE(declared_names<m::eq::EqBandType>().back() == "allPass");
  REQUIRE(declared_names<m::dynamics::DetectorMode>().back() == "logRms");
}

TEST_CASE("every insert publishes a default for every construction key it automates",
          "[mastering][catalog]") {
  // The per-band EQ surface is the bulk of the flat parameter set and is only
  // read when the caller supplies a band, so it is the part that silently
  // publishes nothing unless the builders declare their bands explicitly.
  //
  // A few keys have no fallback, because absence means something else: a
  // cutoff beyond the default split adds a band, and a reverb's decaySec (or
  // the plate's preDelayMs) is used only when supplied. A string or an array
  // rides the JSON side-channel and has no numeric default at all.
  const std::set<std::string> no_fallback = {"cutoff2Hz", "cutoff3Hz", "cutoff4Hz", "cutoff5Hz",
                                             "cutoff6Hz", "cutoff7Hz", "decaySec",  "preDelayMs"};
  for (const std::string& name : insert_factory_names()) {
    const std::vector<std::string> construction_keys = insert_param_names(name);
    const std::set<std::string> keys(construction_keys.begin(), construction_keys.end());
    for (const json::Value& parameter : param_info(name)) {
      const std::string key = field(parameter, "name").as_string();
      const std::string type = field(parameter, "type").as_string();
      // A descriptor id with no construction key of the same name cannot have a
      // construction default, and correctly publishes none.
      if (keys.find(key) == keys.end()) continue;
      INFO(name << " parameter " << key);
      if (type == "string" || type == "array") {
        REQUIRE(field(parameter, "default").is_null());
        continue;
      }
      if (no_fallback.count(key) != 0) continue;
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
  // every band key without the live map probing it. A crossover band the
  // default split does not create is declared that way, so its key must still
  // be reported as ignored.
  std::vector<std::string> ignored;
  REQUIRE(make_insert("multiband.compressor", R"({"band5.ratio":2.0})", &ignored) != nullptr);
  REQUIRE(ignored == std::vector<std::string>{"band5.ratio"});

  // And once the crossover has enough cutoffs for that band, it is read.
  ignored.clear();
  REQUIRE(make_insert("multiband.compressor",
                      R"({"cutoff0Hz":100,"cutoff1Hz":300,"cutoff2Hz":1000,"cutoff3Hz":3000,)"
                      R"("cutoff4Hz":8000,"band5.ratio":2.0})",
                      &ignored) != nullptr);
  REQUIRE(ignored.empty());
}

TEST_CASE("any one key of an EQ band makes the band exist", "[mastering][catalog]") {
  // One rule across every EQ family: a band a caller addresses by any of its
  // keys is built, so no supplied band key is dropped for lack of a frequency.
  for (const char* config : {R"({"band0.q":2.0})", R"({"band2.enabled":true})"}) {
    for (const char* name : {"eq.parametric", "eq.minimumPhase", "eq.linearPhase"}) {
      std::vector<std::string> ignored;
      REQUIRE(make_insert(name, config, &ignored) != nullptr);
      INFO(name << " " << config);
      REQUIRE(ignored.empty());
    }
  }
  std::vector<std::string> ignored;
  REQUIRE(make_insert("eq.dynamic", R"({"band0.ratio":3.0})", &ignored) != nullptr);
  REQUIRE(ignored.empty());
  REQUIRE(make_insert("eq.midSide", R"({"sideBand1.q":2.0})", &ignored) != nullptr);
  REQUIRE(ignored.empty());
  REQUIRE(make_insert("multiband.dynamicEq", R"({"band1.dyn2.ratio":3.0})", &ignored) != nullptr);
  REQUIRE(ignored.empty());
}

TEST_CASE("the parameter info schema list matches what the writer emits", "[mastering][catalog]") {
  // Every insert, not one: the writer emits the same eight keys per descriptor
  // regardless of the processor, so a list derived from a single sample would
  // pass while describing nothing about the rest.
  std::set<std::string> actual;
  for (const auto& name : insert_factory_names()) {
    const auto paths = sonare::test::schema_paths_of(insert_param_info_json(name));
    actual.insert(paths.begin(), paths.end());
  }
  const auto& expected_paths = sonare::mastering::api::insert_param_info_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE_FALSE(actual.empty());
  REQUIRE(actual == expected);
}

TEST_CASE("the processor catalog schema list matches what the writer emits",
          "[mastering][catalog]") {
  const auto actual =
      sonare::test::schema_paths_of(sonare::mastering::api::processor_catalog_json());
  const auto& expected_paths = sonare::mastering::api::processor_catalog_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE(actual == expected);

  // The params interior is the parameter info schema under a prefix. Both lists
  // are written out literally so a reader outside this language can parse them,
  // which is exactly what lets the two copies drift; this is what stops them.
  std::set<std::string> prefixed;
  for (const auto& path : sonare::mastering::api::insert_param_info_schema_paths()) {
    prefixed.insert("[].params" + path);
  }
  std::set<std::string> interior;
  for (const auto& path : expected) {
    if (path.rfind("[].params[]", 0) == 0) interior.insert(path);
  }
  REQUIRE(interior == prefixed);

  std::set<std::string> slot_prefixed;
  for (const auto& path : sonare::mastering::api::insert_slot_info_schema_paths()) {
    slot_prefixed.insert("[].slots" + path);
  }
  std::set<std::string> slot_interior;
  for (const auto& path : expected) {
    if (path.rfind("[].slots[]", 0) == 0) slot_interior.insert(path);
  }
  REQUIRE(slot_interior == slot_prefixed);
}

TEST_CASE("the slot info schema list matches what the writer emits", "[mastering][catalog]") {
  std::set<std::string> actual;
  for (const auto& name : insert_factory_names()) {
    const auto paths =
        sonare::test::schema_paths_of(sonare::mastering::api::insert_slot_info_json(name));
    actual.insert(paths.begin(), paths.end());
  }
  const auto& expected_paths = sonare::mastering::api::insert_slot_info_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE_FALSE(actual.empty());
  REQUIRE(actual == expected);
}

TEST_CASE("a slotted key supplied the way its slot's rule says is read", "[mastering][catalog]") {
  // The published rule is the one construction applies: supplying the key
  // alone, plus the cutoffs its crossover band needs, reaches the processor.
  // Without those cutoffs a band past the default split is not built, which is
  // what keeps minCrossoverCutoffs from being inflated.
  const int default_cutoffs =
      static_cast<int>(sonare::mastering::multiband::CrossoverConfig{}.cutoffs_hz.size());
  size_t slotted = 0;
  size_t gated_by_crossover = 0;
  for (const auto& name : insert_factory_names()) {
    const json::Value slots_value =
        json::parse_strict(sonare::mastering::api::insert_slot_info_json(name));
    const json::Array& slots = slots_value.as_array();
    for (const json::Value& parameter : param_info(name)) {
      if (field(parameter, "slot").is_null()) continue;
      const std::string key = field(parameter, "name").as_string();
      const json::Value& fallback = field(parameter, "default");
      if (!fallback.is_number() && !fallback.is_bool()) continue;
      ++slotted;
      INFO(name << " " << key);
      std::vector<std::string> ignored;
      REQUIRE(make_insert(name, probe_json(name, key, fallback), &ignored) != nullptr);
      REQUIRE(ignored.empty());

      int required = 0;
      for (std::string slot = field(parameter, "slot").as_string(); !slot.empty();) {
        std::string parent;
        for (const json::Value& entry : slots) {
          if (field(entry, "name").as_string() != slot) continue;
          required =
              std::max(required, static_cast<int>(field(entry, "minCrossoverCutoffs").as_number()));
          if (!field(entry, "parent").is_null()) parent = field(entry, "parent").as_string();
        }
        slot = parent;
      }
      if (required <= default_cutoffs) continue;
      ++gated_by_crossover;
      json::Object alone;
      alone.emplace(key, fallback);
      ignored.clear();
      REQUIRE(make_insert(name, json::dump(json::Value(std::move(alone))), &ignored) != nullptr);
      REQUIRE(ignored == std::vector<std::string>{key});
    }
  }
  REQUIRE(slotted > 0);
  REQUIRE(gated_by_crossover > 0);
}

#ifdef SONARE_WITH_FX
TEST_CASE("every effects-insert config field has a construction key", "[mastering][catalog]") {
  // A config field with no key is unreachable from every binding, and nothing
  // else here detects it: the DSP tests build the config struct directly and the
  // catalog only publishes keys someone already wrote. The mastering processors
  // are guarded at compile time by SONARE_ASSERT_TABLE_COVERS, which pairs a
  // field table with its struct. These have no table — insert_factory.cpp spells
  // their keys by hand — so the equivalent question is asked of the factory's
  // own behaviour instead.
  //
  // The population is every effects insert whose keys stand one to one with its
  // config's fields. The reverbs are outside it and an `unexposed` count would
  // misdescribe them in both directions: they probe alias keys for one field
  // (`damping` / `hfDamping`), derive one field from another key (`decaySec` ->
  // `decay`), and carry nested members that one field spans several keys of (a
  // room's dimensions, its endpoints, its air). Counting keys answers nothing
  // there, so they are guarded at compile time instead, by the arity assertions
  // beside their builders in insert_factory.cpp.
  namespace modulation = sonare::effects::modulation;
  require_a_key_per_config_field<modulation::PhaserConfig>("effects.modulation.phaser");
  require_a_key_per_config_field<modulation::RotaryConfig>("effects.modulation.rotary");
  require_a_key_per_config_field<modulation::ChorusConfig>("effects.modulation.chorus");
  require_a_key_per_config_field<modulation::FlangerConfig>("effects.modulation.flanger");
  require_a_key_per_config_field<modulation::PitchShifterConfig>("effects.modulation.pitchShifter");
  require_a_key_per_config_field<modulation::EnsembleConfig>("effects.modulation.ensemble");
  require_a_key_per_config_field<modulation::WahConfig>("effects.modulation.wah");
  require_a_key_per_config_field<modulation::AutoWahConfig>("effects.modulation.autoWah");
  require_a_key_per_config_field<modulation::RingModulatorConfig>(
      "effects.modulation.ringModulator");
  require_a_key_per_config_field<sonare::effects::delay::StereoDelayConfig>("effects.delay.stereo");

  // The same comparison against a struct carrying one field the factory does not
  // read must fail. A local specimen rather than a real config: a control naming
  // a live defect stops being one the day it is fixed.
  struct PhaserConfigPlusOne {
    float rate_hz;
    float min_hz;
    float max_hz;
    int stages;
    float dry_wet;
    float feedback;
    modulation::PhaserMixMode mix_mode;
    float never_read;
  };
  CHECK(insert_param_names("effects.modulation.phaser").size() !=
        sonare::mastering::api::detail::field_count<PhaserConfigPlusOne>());
}
#endif
