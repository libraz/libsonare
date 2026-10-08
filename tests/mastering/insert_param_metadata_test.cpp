// The host-facing parameter metadata every insert publishes: its type, its
// design default, the range construction accepts, and the unit, axis scale,
// display range and sibling bounds its reader declares.
//
// The type, default and bounds are DERIVED — the type and the default come
// from the config builder's own accessors, the bounds are measured by handing
// candidate values to the same construction path a caller uses. What these
// cases pin is the derivation: that it covers every construction key, that it
// agrees with the config structs, and that the published range really is the
// range construction enforces. The unit, scale, display range and sibling
// bounds are declared at the read site; the cases pin that every numeric key
// declares them consistently and that a declared bound is one construction
// enforces.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/param_field_tables.h"
#include "mastering/api/processor_params.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/multiband/multiband_dynamic_eq.h"
#include "mastering/saturation/tape.h"
#include "mastering/stereo/imager.h"
#include "rt/processor_base.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

#ifdef SONARE_WITH_FX
#include "effects/delay/stereo_delay.h"
#include "effects/filter/vowel_filter.h"
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
using sonare::mastering::api::insert_param_info_json_at_rate;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::api::insert_probe_params;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::repair_param_info_json;

json::Array repair_param_info(const std::string& name) {
  const json::Value parsed = json::parse_strict(repair_param_info_json(name));
  REQUIRE(parsed.is_array());
  return parsed.as_array();
}

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

// Whether the insert builds with @p key at @p value and prepares at @p sample_rate.
bool prepares_at(const std::string& name, const std::string& key, double value,
                 double sample_rate) {
  try {
    const std::unique_ptr<sonare::rt::ProcessorBase> processor =
        make_insert(name, probe_json(name, key, json::Value(value)));
    if (processor == nullptr) return false;
    processor->prepare(sample_rate, sonare::mastering::api::kInsertProbeBlockSize);
    return true;
  } catch (...) {
    return false;
  }
}

json::Array param_info_at(const std::string& name, double sample_rate) {
  const json::Value parsed = json::parse_strict(insert_param_info_json_at_rate(name, sample_rate));
  REQUIRE(parsed.is_array());
  return parsed.as_array();
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
  (void)declared_names<sonare::effects::modulation::DelayInterpolation>();
  (void)declared_names<sonare::effects::modulation::RotaryModel>();
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
          std::vector<std::string>{"guitar4x12", "bass8x10", "guitar1x12Combo", "guitar2x12Open"});
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

#if SONARE_BUILD_FX
TEST_CASE("coupled GS TYPE selectors leave their pair-dependent domain open",
          "[mastering][catalog]") {
  const json::Array params = param_info("effects.gsEfx");
  const json::Value* type_msb = find_param(params, "typeMsb");
  const json::Value* type_lsb = find_param(params, "typeLsb");
  REQUIRE(type_msb != nullptr);
  REQUIRE(type_lsb != nullptr);

  // The valid values of either byte depend on its companion. A scalar probe
  // against the Thru companion cannot publish a bound or a choices list for it.
  for (const json::Value* selector : {type_msb, type_lsb}) {
    INFO(field(*selector, "name").as_string());
    CHECK(field(*selector, "min").is_null());
    CHECK(field(*selector, "max").is_null());
    CHECK(field(*selector, "choices").is_null());
  }

  // These documented non-Thru pairs must remain constructible even though a
  // single-key probe cannot measure their joint domain.
  REQUIRE(make_insert("effects.gsEfx", R"({"typeMsb":1,"typeLsb":16})") != nullptr);
  REQUIRE(make_insert("effects.gsEfx", R"({"typeMsb":2,"typeLsb":12})") != nullptr);
}
#endif

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

TEST_CASE("a bound builds exactly when it is not flagged exclusive", "[mastering][catalog]") {
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      const std::string key = field(parameter, "name").as_string();
      INFO(name << " parameter " << key);
      for (const char* side : {"min", "max"}) {
        const json::Value& bound = field(parameter, side);
        const bool exclusive =
            field(parameter, std::string(std::string(side) + "Exclusive").c_str()).as_bool();
        if (bound.is_null()) {
          REQUIRE_FALSE(exclusive);
          continue;
        }
        INFO(side << " " << bound.as_number());
        REQUIRE(builds_with(name, key, bound) == !exclusive);
        if (exclusive) {
          // A value strictly inside the limit builds, so the flag is the whole
          // story and the interval beyond the limit is not itself rejected.
          const double inside =
              bound.as_number() + (std::string(side) == "min" ? 1.0 : -1.0) *
                                      std::max(1.0e-4 * std::fabs(bound.as_number()), 1.0e-6);
          REQUIRE(builds_with(name, key, json::Value(inside)));
        }
      }
    }
  }
}

TEST_CASE("an EQ band ceiling follows the processing rate's Nyquist", "[mastering][catalog]") {
  const json::Array parametric = param_info("eq.parametric");
  const json::Value* frequency = find_param(parametric, "band0.frequencyHz");
  REQUIRE(frequency != nullptr);
  REQUIRE(field(*frequency, "maxRelativeTo").as_string() == "nyquist");
  REQUIRE(field(*frequency, "maxExclusive").as_bool());
  REQUIRE(field(*frequency, "max").as_number() == 24000.0);

  // The ceiling is the lower of `max` and the host's Nyquist: a band the probe
  // rate accepts is refused once the insert is prepared lower.
  const std::string below = R"({"band0.frequencyHz":12000})";
  const std::string above = R"({"band0.frequencyHz":20000})";
  auto prepared = make_insert("eq.parametric", above);
  REQUIRE(prepared != nullptr);
  REQUIRE_NOTHROW(prepared->prepare(48000.0, sonare::mastering::api::kInsertProbeBlockSize));
  auto narrow = make_insert("eq.parametric", above);
  REQUIRE_THROWS(narrow->prepare(22050.0, sonare::mastering::api::kInsertProbeBlockSize));
  auto fits = make_insert("eq.parametric", below);
  REQUIRE_NOTHROW(fits->prepare(32000.0, sonare::mastering::api::kInsertProbeBlockSize));

  // No other kind of key follows the rate.
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      if (field(parameter, "maxRelativeTo").is_null()) continue;
      INFO(name << " " << field(parameter, "name").as_string());
      REQUIRE(field(parameter, "maxRelativeTo").as_string() == "nyquist");
      REQUIRE(field(parameter, "unit").as_string() == "Hz");
      REQUIRE(field(parameter, "maxExclusive").as_bool());
    }
  }
  const json::Array compressor = param_info("dynamics.compressor");
  REQUIRE(field(*find_param(compressor, "ratio"), "maxRelativeTo").is_null());
  REQUIRE(field(*find_param(compressor, "makeupGainDb"), "maxRelativeTo").is_null());
}

TEST_CASE("a rate-specific descriptor publishes the ceiling accepted at that rate",
          "[mastering][catalog]") {
  const std::string rate_less = insert_param_info_json("eq.parametric");
  const std::string key = "band0.frequencyHz";

  // Below the probe rate the Nyquist is the ceiling, and it is exclusive.
  const json::Array at_44100 = param_info_at("eq.parametric", 44100.0);
  const json::Value* narrow = find_param(at_44100, key);
  REQUIRE(narrow != nullptr);
  CHECK(field(*narrow, "maxRelativeTo").as_string() == "nyquist");
  CHECK(field(*narrow, "max").as_number() == 22050.0);
  CHECK(field(*narrow, "maxExclusive").as_bool());
  CHECK_FALSE(prepares_at("eq.parametric", key, 22050.0, 44100.0));
  CHECK(prepares_at("eq.parametric", key, 22049.0, 44100.0));

  // Above the build rate the insert's own cap holds, so the published ceiling
  // is what builds and prepares there, not the host's Nyquist.
  const json::Array at_96000 = param_info_at("eq.parametric", 96000.0);
  const json::Value* wide = find_param(at_96000, key);
  REQUIRE(wide != nullptr);
  const double ceiling = field(*wide, "max").as_number();
  INFO("eq.parametric band0.frequencyHz max at 96000 = " << ceiling);
  CHECK(ceiling == 24000.0);
  CHECK(field(*wide, "maxExclusive").as_bool() ==
        !prepares_at("eq.parametric", key, ceiling, 96000.0));
  CHECK(prepares_at("eq.parametric", key, ceiling * (1.0 - 1.0e-4), 96000.0));
  CHECK_FALSE(prepares_at("eq.parametric", key, ceiling * (1.0 + 1.0e-4), 96000.0));

  // Only the rate-following ceilings move; the rest of the descriptor is the
  // rate-less one.
  const json::Array base = param_info("eq.parametric");
  REQUIRE(at_44100.size() == base.size());
  for (size_t index = 0; index < base.size(); ++index) {
    if (field(base[index], "name").as_string() == key) continue;
    if (!field(base[index], "maxRelativeTo").is_null()) continue;
    CHECK(json::dump(at_44100[index]) == json::dump(base[index]));
  }
  const json::Array compressor = param_info_at("dynamics.compressor", 44100.0);
  CHECK(json::dump(json::Value(compressor)) ==
        json::dump(json::Value(param_info("dynamics.compressor"))));

  // The rate-less answer neither changes nor depends on an earlier rate query.
  CHECK(insert_param_info_json("eq.parametric") == rate_less);
  CHECK(insert_param_info_json_at_rate("eq.parametric", 44100.0) ==
        insert_param_info_json_at_rate("eq.parametric", 44100.0));
}

TEST_CASE("the rate-specific descriptor query refuses a rate outside the supported range",
          "[mastering][catalog]") {
  CHECK(sonare_mastering_insert_param_info_at_rate("eq.parametric", 0) == nullptr);
  CHECK(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_mastering_insert_param_info_at_rate("eq.parametric", -48000) == nullptr);
  CHECK(sonare_mastering_insert_param_info_at_rate("eq.parametric", 100000000) == nullptr);

  const char* at_rate = sonare_mastering_insert_param_info_at_rate("eq.parametric", 44100);
  REQUIRE(at_rate != nullptr);
  CHECK(std::string(at_rate) == insert_param_info_json_at_rate("eq.parametric", 44100.0));
  const char* unknown = sonare_mastering_insert_param_info_at_rate("no.such.insert", 44100);
  REQUIRE(unknown != nullptr);
  CHECK(std::string(unknown) == "[]");
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
  // Compressor::validate_config demands ratio >= 1 and non-negative timings,
  // leaves thresholdDb open, and bounds makeupGainDb where its float gain overflows.
  const json::Array compressor = param_info("dynamics.compressor");
  REQUIRE(find_param(compressor, "ratio")->find("min")->as_number() == 1.0);
  REQUIRE(find_param(compressor, "ratio")->find("max")->is_null());
  REQUIRE(find_param(compressor, "attackMs")->find("min")->as_number() == 0.0);
  REQUIRE(find_param(compressor, "thresholdDb")->find("min")->is_null());
  REQUIRE(find_param(compressor, "makeupGainDb")->find("min")->is_null());
  const double makeup_max = find_param(compressor, "makeupGainDb")->find("max")->as_number();
  REQUIRE(makeup_max > 770.0);
  REQUIRE(makeup_max < 771.0);
  // sidechainHpfHz is validated as strictly positive, and an exclusive bound is
  // published as the limit it excludes, flagged as such.
  REQUIRE(find_param(compressor, "sidechainHpfHz")->find("min")->as_number() == 0.0);
  REQUIRE(find_param(compressor, "sidechainHpfHz")->find("minExclusive")->as_bool());
  REQUIRE_FALSE(builds_with("dynamics.compressor", "sidechainHpfHz", json::Value(0.0)));
  // An inclusive bound carries a false flag, and an absent bound never an exclusive one.
  REQUIRE_FALSE(find_param(compressor, "attackMs")->find("minExclusive")->as_bool());
  REQUIRE_FALSE(find_param(compressor, "thresholdDb")->find("minExclusive")->as_bool());
  REQUIRE_FALSE(find_param(compressor, "thresholdDb")->find("maxExclusive")->as_bool());

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
  REQUIRE(find_param(bitcrusher, "bitDepth")->find("min")->as_number() == 2.0);
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

namespace {

unsigned int descriptor_id(const sonare::rt::ProcessorBase& processor, const std::string& key) {
  for (const auto& descriptor : processor.parameter_descriptors()) {
    if (descriptor.key == key) return descriptor.id;
  }
  FAIL("no descriptor " << key);
  return 0;
}

// Settled output RMS of a 60 Hz tone, which sits inside the lowest crossover band.
float settled_low_band_rms(sonare::rt::ProcessorBase& processor) {
  constexpr int kRate = 48000;
  constexpr int kBlock = 512;
  processor.prepare(kRate, kBlock);
  double sum = 0.0;
  int counted = 0;
  for (int block = 0; block < 60; ++block) {
    std::vector<float> left(kBlock);
    for (int i = 0; i < kBlock; ++i) {
      const double t = static_cast<double>(block * kBlock + i) / kRate;
      left[static_cast<size_t>(i)] =
          static_cast<float>(0.25 * std::sin(2.0 * sonare::constants::kPiD * 60.0 * t));
    }
    std::vector<float> right = left;
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, kBlock);
    if (block >= 40) {
      for (const float sample : left) sum += static_cast<double>(sample) * sample;
      counted += kBlock;
    }
  }
  return static_cast<float>(std::sqrt(sum / counted));
}

std::string static_cut_json(int slot) {
  const std::string prefix = "\"band0.dyn" + std::to_string(slot) + ".";
  return "{" + prefix + "frequencyHz\":60," + prefix + "staticGainDb\":-12," + prefix + "q\":1," +
         prefix + "ratio\":1," + prefix + "thresholdDb\":0}";
}

}  // namespace

TEST_CASE("sparse multiband dynamic EQ sub-bands keep the slot their keys name",
          "[mastering][catalog]") {
  using sonare::mastering::multiband::MultibandDynamicEq;
  auto dense = make_insert("multiband.dynamicEq", static_cut_json(0));
  auto sparse = make_insert("multiband.dynamicEq", static_cut_json(2));
  REQUIRE(dense != nullptr);
  REQUIRE(sparse != nullptr);

  const auto& sparse_bands = dynamic_cast<MultibandDynamicEq&>(*sparse).config().bands[0];
  REQUIRE(sparse_bands.size() == 3);
  REQUIRE_FALSE(sparse_bands[0].enabled);
  REQUIRE_FALSE(sparse_bands[1].enabled);
  REQUIRE(sparse_bands[2].enabled);
  REQUIRE(sparse_bands[2].static_gain_db == -12.0f);

  const float cut = settled_low_band_rms(*dense);
  REQUIRE(settled_low_band_rms(*sparse) == Catch::Approx(cut).margin(1.0e-5));

  // The absent dyn0 slot must not reach the supplied dyn2 filter.
  REQUIRE(sparse->set_parameter(descriptor_id(*sparse, "band0.dyn0.staticGainDb"), 6.0f));
  REQUIRE(settled_low_band_rms(*sparse) == Catch::Approx(cut).margin(1.0e-5));
  REQUIRE(sparse->set_parameter(descriptor_id(*sparse, "band0.dyn0.staticGainDb"), 0.0f));

  // The dyn2 descriptor reaches the dyn2 filter, as dyn0 does in the dense layout.
  REQUIRE(dense->set_parameter(descriptor_id(*dense, "band0.dyn0.staticGainDb"), 0.0f));
  REQUIRE(sparse->set_parameter(descriptor_id(*sparse, "band0.dyn2.staticGainDb"), 0.0f));
  const float open = settled_low_band_rms(*dense);
  REQUIRE(open > cut * 2.0f);
  REQUIRE(settled_low_band_rms(*sparse) == Catch::Approx(open).margin(1.0e-5));
  REQUIRE(dynamic_cast<MultibandDynamicEq&>(*sparse).config().bands[0][2].static_gain_db == 0.0f);

  // A gap between supplied slots keeps both at their own index.
  auto gapped = make_insert("multiband.dynamicEq",
                            R"({"band0.dyn0.frequencyHz":40,"band0.dyn2.frequencyHz":80})");
  REQUIRE(gapped != nullptr);
  const auto& gapped_bands = dynamic_cast<MultibandDynamicEq&>(*gapped).config().bands[0];
  REQUIRE(gapped_bands.size() == 3);
  REQUIRE(gapped_bands[0].frequency_hz == 40.0f);
  REQUIRE_FALSE(gapped_bands[1].enabled);
  REQUIRE(gapped_bands[2].frequency_hz == 80.0f);
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

namespace {

// The closed unit vocabulary the catalog schema publishes.
const std::set<std::string>& unit_vocabulary() {
  static const std::set<std::string> units = {
      "dB",       "dBFS",      "LUFS",  "Hz",      "ms",   "s",     "samples",
      "m",        "cm",        "deg",   "percent", "degC", "V",     "inPerSec",
      "dBPerOct", "semitones", "cents", "ratio",   "bits", "count", "none"};
  return units;
}

// What is wrong with a descriptor list, one line per defect. Takes the parsed
// list rather than an insert name so a hand-built list can show each rule bites.
std::vector<std::string> descriptor_defects(const json::Array& params) {
  std::vector<std::string> defects;
  std::set<std::string> names;
  for (const json::Value& parameter : params) names.insert(field(parameter, "name").as_string());
  for (const json::Value& parameter : params) {
    const std::string name = field(parameter, "name").as_string();
    const json::Value& unit = field(parameter, "unit");
    if (field(parameter, "type").as_string() == "number") {
      if (!unit.is_string()) {
        defects.push_back(name + ": numeric parameter declares no unit");
      } else if (unit_vocabulary().count(unit.as_string()) == 0) {
        defects.push_back(name + ": unit outside the vocabulary");
      }
    } else if (!unit.is_null()) {
      defects.push_back(name + ": non-numeric parameter carries a unit");
    }
    const std::string scale = field(parameter, "scale").as_string();
    if (scale != "linear" && scale != "log") defects.push_back(name + ": unknown scale");

    const json::Value& ui_min = field(parameter, "uiMin");
    const json::Value& ui_max = field(parameter, "uiMax");
    const json::Value& min = field(parameter, "min");
    const json::Value& max = field(parameter, "max");
    for (const json::Value* ui : {&ui_min, &ui_max}) {
      if (ui->is_null()) continue;
      const bool min_open = field(parameter, "minExclusive").as_bool();
      const bool max_open = field(parameter, "maxExclusive").as_bool();
      if (min.is_number() &&
          (ui->as_number() < min.as_number() || (min_open && ui->as_number() == min.as_number()))) {
        defects.push_back(name + ": display range below the accepted range");
      }
      if (max.is_number() &&
          (ui->as_number() > max.as_number() || (max_open && ui->as_number() == max.as_number()))) {
        defects.push_back(name + ": display range above the accepted range");
      }
    }
    if (ui_min.is_number() && ui_max.is_number() && ui_min.as_number() > ui_max.as_number()) {
      defects.push_back(name + ": display range is inverted");
    }

    for (const json::Value& dependency : field(parameter, "dependsOn").as_array()) {
      const std::string sibling = field(dependency, "key").as_string();
      const std::string relation = field(dependency, "relation").as_string();
      if (sibling == name || names.count(sibling) == 0) {
        defects.push_back(name + ": dependsOn names no sibling");
      }
      if (relation != "lt" && relation != "le" && relation != "gt" && relation != "ge") {
        defects.push_back(name + ": dependsOn relation is unknown");
      }
    }
  }
  return defects;
}

json::Array parse_descriptors(const std::string& text) {
  const json::Value parsed = json::parse_strict(text);
  REQUIRE(parsed.is_array());
  return parsed.as_array();
}

// One descriptor in the writer's shape, with @p overrides spliced over the valid defaults.
std::string descriptor_text(const std::string& overrides) {
  return R"([{"name":"a","id":null,"rtSafe":false,"type":"number","min":0,"max":10,)"
         R"("minExclusive":false,"maxExclusive":false,"maxRelativeTo":null,"default":1,)"
         R"("unit":"Hz","uiMin":null,"uiMax":null,"scale":"linear","choices":null,"slot":null,)"
         R"("dependsOn":[]})" +
         overrides + "]";
}

}  // namespace

TEST_CASE("every numeric parameter declares a unit, a scale and a consistent display range",
          "[mastering][catalog]") {
  size_t numeric = 0;
  std::vector<std::string> defects;
  for (const std::string& name : insert_factory_names()) {
    const json::Array params = param_info(name);
    for (const json::Value& parameter : params) {
      if (field(parameter, "type").as_string() == "number") ++numeric;
    }
    for (const std::string& defect : descriptor_defects(params)) {
      defects.push_back(name + " " + defect);
    }
  }
  CHECK(numeric > 1000);
  INFO(defects.size() << " defects, first: " << (defects.empty() ? "" : defects.front()));
  CHECK(defects.empty());
}

TEST_CASE("the descriptor check reports each defect it exists to catch", "[mastering][catalog]") {
  REQUIRE(descriptor_defects(parse_descriptors(descriptor_text(""))).empty());

  const auto defects_of = [](const std::string& from, const std::string& to) {
    std::string text = descriptor_text("");
    const size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return descriptor_defects(parse_descriptors(text));
  };
  CHECK(defects_of(R"("unit":"Hz")", R"("unit":null)").size() == 1);
  CHECK(defects_of(R"("unit":"Hz")", R"("unit":"furlongs")").size() == 1);
  CHECK(defects_of(R"("unit":"Hz")", R"("unit":"none")").empty());
  CHECK(defects_of(R"("scale":"linear")", R"("scale":"sideways")").size() == 1);
  CHECK(defects_of(R"("uiMin":null)", R"("uiMin":-1)").size() == 1);
  CHECK(defects_of(R"("uiMax":null)", R"("uiMax":11)").size() == 1);
  CHECK(defects_of(R"("uiMax":null)", R"("uiMax":10)").empty());
  CHECK(
      defects_of(
          R"("maxExclusive":false,"maxRelativeTo":null,"default":1,"unit":"Hz","uiMin":null,"uiMax":null)",
          R"("maxExclusive":true,"maxRelativeTo":null,"default":1,"unit":"Hz","uiMin":null,"uiMax":10)")
          .size() == 1);
  CHECK(defects_of(R"("uiMin":null,"uiMax":null)", R"("uiMin":6,"uiMax":5)").size() == 1);
  CHECK(defects_of(R"("dependsOn":[])", R"("dependsOn":[{"key":"b","relation":"le"}])").size() ==
        1);
  CHECK(defects_of(R"("dependsOn":[])", R"("dependsOn":[{"key":"a","relation":"le"}])").size() ==
        1);
  CHECK(defects_of(R"("type":"number")", R"("type":"boolean")").size() == 1);
}

TEST_CASE("declared units follow what each reader declared, not the key's spelling",
          "[mastering][catalog]") {
  const auto unit_of = [](const std::string& insert, const std::string& key) {
    const json::Array params = param_info(insert);
    const json::Value* parameter = find_param(params, key);
    REQUIRE(parameter != nullptr);
    return field(*parameter, "unit").is_null() ? std::string("null")
                                               : field(*parameter, "unit").as_string();
  };
  const auto scale_of = [](const std::string& insert, const std::string& key) {
    const json::Array params = param_info(insert);
    const json::Value* parameter = find_param(params, key);
    REQUIRE(parameter != nullptr);
    return field(*parameter, "scale").as_string();
  };
  CHECK(unit_of("dynamics.compressor", "thresholdDb") == "dB");
  CHECK(unit_of("dynamics.compressor", "ratio") == "ratio");
  CHECK(unit_of("dynamics.compressor", "attackMs") == "ms");
  CHECK(unit_of("dynamics.compressor", "sidechainHpfHz") == "Hz");
  CHECK(unit_of("maximizer.truePeakLimiter", "ceilingDb") == "dBFS");
  CHECK(unit_of("maximizer.truePeakLimiter", "oversampleFactor") == "ratio");
  CHECK(unit_of("eq.linearPhase", "fftSize") == "samples");
  CHECK(unit_of("eq.parametric", "band0.slopeDbOct") == "dBPerOct");
  CHECK(unit_of("eq.parametric", "band0.q") == "none");
  CHECK(unit_of("saturation.tape", "speedIps") == "inPerSec");
  CHECK(unit_of("saturation.tube", "biasV") == "V");
  CHECK(unit_of("saturation.bitcrusher", "bitDepth") == "bits");
  CHECK(unit_of("saturation.bitcrusher", "mix") == "none");
  CHECK(unit_of("stereo.binaural", "azimuthDeg") == "deg");
  CHECK(unit_of("stereo.autoPan", "phase") == "none");

  CHECK(scale_of("dynamics.compressor", "attackMs") == "log");
  CHECK(scale_of("dynamics.compressor", "thresholdDb") == "linear");
  CHECK(scale_of("dynamics.deesser", "frequencyHz") == "log");
  CHECK(scale_of("multiband.compressor", "cutoff0Hz") == "log");
  CHECK(scale_of("dynamics.compressor", "ratio") == "linear");

#ifdef SONARE_WITH_FX
  CHECK(unit_of("effects.reverb.room", "lengthM") == "m");
  CHECK(unit_of("effects.reverb.room", "airHumidityPercent") == "percent");
  CHECK(unit_of("effects.reverb.room", "airTemperatureC") == "degC");
  CHECK(unit_of("effects.reverb.fdn", "decaySec") == "s");
  CHECK(unit_of("effects.modulation.pitchShifter", "cents") == "cents");
  CHECK(unit_of("effects.modulation.pitchShifter", "semitones") == "semitones");
  CHECK(unit_of("effects.reverb.plate", "modDepthSamples") == "samples");
  CHECK(unit_of("saturation.ampSim", "micDistanceCm") == "cm");
#endif
}

namespace {

struct DeclaredDependency {
  std::string insert;
  std::string key;
  std::string sibling;
  std::string relation;
};

std::vector<DeclaredDependency> declared_dependencies() {
  std::vector<DeclaredDependency> found;
  for (const std::string& name : insert_factory_names()) {
    for (const json::Value& parameter : param_info(name)) {
      for (const json::Value& dependency : field(parameter, "dependsOn").as_array()) {
        found.push_back({name, field(parameter, "name").as_string(),
                         field(dependency, "key").as_string(),
                         field(dependency, "relation").as_string()});
      }
    }
  }
  return found;
}

bool has_dependency(const std::vector<DeclaredDependency>& all, const std::string& insert,
                    const std::string& key, const std::string& sibling,
                    const std::string& relation) {
  for (const DeclaredDependency& entry : all) {
    if (entry.insert == insert && entry.key == key && entry.sibling == sibling &&
        entry.relation == relation) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("common controls publish a display range inside their accepted range",
          "[mastering][catalog]") {
  const auto range_of = [](const std::string& insert, const std::string& key) {
    const json::Array params = param_info(insert);
    const json::Value* parameter = find_param(params, key);
    REQUIRE(parameter != nullptr);
    REQUIRE(field(*parameter, "uiMin").is_number());
    REQUIRE(field(*parameter, "uiMax").is_number());
    return std::pair<double, double>(field(*parameter, "uiMin").as_number(),
                                     field(*parameter, "uiMax").as_number());
  };
  CHECK(range_of("dynamics.compressor", "thresholdDb") == std::pair<double, double>(-60, 0));
  CHECK(range_of("effects.modulation.chorus", "rateHz") == std::pair<double, double>(0, 10));
  // The gate refuses a threshold below its close threshold's default, so the window starts there.
  CHECK(range_of("dynamics.gate", "thresholdDb") == std::pair<double, double>(-50, 0));
}

TEST_CASE("sibling dependencies are declared for every coupled pair", "[mastering][catalog]") {
  const auto all = declared_dependencies();
  CHECK(has_dependency(all, "dynamics.gate", "closeThresholdDb", "thresholdDb", "le"));
  CHECK(has_dependency(all, "dynamics.gate", "thresholdDb", "closeThresholdDb", "ge"));
  CHECK(has_dependency(all, "maximizer.adaptiveRelease", "minReleaseMs", "maxReleaseMs", "le"));
  CHECK(has_dependency(all, "maximizer.adaptiveRelease", "maxReleaseMs", "minReleaseMs", "ge"));
  CHECK(has_dependency(all, "maximizer.adaptiveRelease", "crestLow", "crestHigh", "le"));
  CHECK(has_dependency(all, "maximizer.adaptiveRelease", "crestHigh", "crestLow", "ge"));
  CHECK(has_dependency(all, "spectral.spectralShaper", "frequencyHz", "highFrequencyHz", "lt"));
  CHECK(has_dependency(all, "spectral.spectralShaper", "highFrequencyHz", "frequencyHz", "gt"));
  CHECK(has_dependency(all, "eq.linearPhase", "kernelSize", "fftSize", "le"));
  CHECK(has_dependency(all, "eq.equalizer", "kernelSize", "fftSize", "le"));
  CHECK(has_dependency(all, "multiband.compressor", "cutoff0Hz", "cutoff1Hz", "lt"));
  CHECK(has_dependency(all, "multiband.compressor", "cutoff1Hz", "cutoff0Hz", "gt"));
  CHECK(has_dependency(all, "saturation.multibandExciter", "cutoff1Hz", "cutoff0Hz", "gt"));
#ifdef SONARE_WITH_FX
  CHECK(has_dependency(all, "effects.reverb.room", "sourceX", "lengthM", "le"));
  CHECK(has_dependency(all, "effects.reverb.room", "listenerY", "widthM", "le"));
  CHECK(has_dependency(all, "effects.reverb.room", "sourceZ", "heightM", "le"));
#endif
#ifdef SONARE_HAVE_ACOUSTIC
  CHECK(has_dependency(all, "effects.acoustic.roomMorph", "listenerX", "lengthM", "le"));
#endif
}

TEST_CASE("a declared sibling bound is one construction enforces", "[mastering][catalog]") {
  // Every declaration is held to the validation it describes: with the sibling
  // at its default, a value on the wrong side of it is refused when the insert
  // is built.
  size_t checked = 0;
  std::vector<std::string> unenforced;
  // One representative per distinct pair: the indexed keys of a crossover and the
  // processors sharing a pair read it through the same builder.
  std::set<std::string> seen;
  const auto collapsed = [](std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char c) { return std::isdigit(c) != 0; }),
               text.end());
    return text;
  };
  for (const DeclaredDependency& entry : declared_dependencies()) {
    if (!seen.insert(collapsed(entry.key) + collapsed(entry.sibling) + entry.relation).second) {
      continue;
    }
    const json::Array params = param_info(entry.insert);
    const json::Value* sibling = find_param(params, entry.sibling);
    REQUIRE(sibling != nullptr);
    const json::Value& fallback = field(*sibling, "default");
    if (!fallback.is_number()) continue;
    const bool upper = entry.relation == "lt" || entry.relation == "le";
    const double violating = fallback.as_number() + (upper ? 1.0 : -1.0);

    json::Object body;
    for (const auto& param : insert_probe_params(entry.insert, entry.key, 0.0)) {
      if (param.key != entry.key && param.key != entry.sibling) {
        body.emplace(param.key, json::Value(param.value));
      }
    }
    body.emplace(entry.sibling, fallback);
    body.emplace(entry.key, json::Value(violating));
    ++checked;
    bool refused = false;
    try {
      auto processor = make_insert(entry.insert, json::dump(json::Value(std::move(body))));
      refused = processor == nullptr;
    } catch (...) {
      refused = true;
    }
    if (!refused) {
      unenforced.push_back(entry.insert + " " + entry.key + " " + entry.relation + " " +
                           entry.sibling);
    }
  }
  CHECK(checked >= 12);
  INFO(unenforced.size() << " unenforced, first: " << (unenforced.empty() ? "" : unenforced[0]));
  CHECK(unenforced.empty());
}

TEST_CASE("a linear-phase kernel longer than its FFT is refused when the insert is built",
          "[mastering][catalog]") {
  for (const char* name : {"eq.linearPhase", "eq.equalizer"}) {
    INFO(name);
    CHECK_THROWS(make_insert(name, R"({"fftSize":1024,"kernelSize":2047})"));
    CHECK_THROWS(make_insert(name, R"({"fftSize":1024,"kernelSize":1025})"));
    CHECK(make_insert(name, R"({"fftSize":1024,"kernelSize":1023})") != nullptr);
  }
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
  require_a_key_per_config_field<sonare::effects::filter::VowelFilterConfig>(
      "effects.filter.vowel");
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
    float depth;
    float never_read;
  };
  CHECK(insert_param_names("effects.modulation.phaser").size() !=
        sonare::mastering::api::detail::field_count<PhaserConfigPlusOne>());
}
#endif

namespace {

const std::vector<std::string>& repair_stage_ids() {
  static const std::vector<std::string> ids = {
      "repair.declick",    "repair.declip",           "repair.decrackle",
      "repair.dehum",      "repair.denoiseClassical", "repair.dereverbClassical",
      "repair.trimSilence"};
  return ids;
}

// A noise burst long enough for every stage's analysis geometry at the bounds below.
std::vector<float> repair_probe_signal() {
  std::vector<float> samples(8192);
  unsigned state = 12345u;
  for (float& sample : samples) {
    state = state * 1664525u + 1013904223u;
    sample = 0.1f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return samples;
}

// Whether the offline named path accepts @p key = @p value on stage @p id.
bool repair_accepts(const std::string& id, const std::string& key, double value) {
  static const std::vector<float> samples = repair_probe_signal();
  try {
    (void)sonare::mastering::api::apply_named_processor(
        id, samples.data(), samples.size(), 48000,
        std::vector<sonare::mastering::api::Param>{{key, value}});
    return true;
  } catch (const sonare::SonareException&) {
    return false;
  }
}

}  // namespace

TEST_CASE("every repair stage publishes declared, finite-ranged parameters",
          "[mastering][catalog]") {
  // Power-of-two sizes and the hop that follows them are accepted on a set with gaps, which a
  // bisection cannot bound: they publish no limit, as eq.linearPhase's fftSize does.
  const std::set<std::string> gapped = {"nFft", "hopLength"};
  std::vector<std::string> defects;
  size_t numeric = 0;
  for (const std::string& id : repair_stage_ids()) {
    const json::Array params = repair_param_info(id);
    REQUIRE_FALSE(params.empty());
    for (const std::string& defect : descriptor_defects(params))
      defects.push_back(id + " " + defect);
    for (const json::Value& parameter : params) {
      const std::string name = field(parameter, "name").as_string();
      CHECK(field(parameter, "id").is_null());
      CHECK_FALSE(field(parameter, "rtSafe").as_bool());
      if (field(parameter, "type").as_string() != "number") continue;
      ++numeric;
      if (gapped.count(name) != 0) continue;
      INFO(id << " " << name);
      CHECK(field(parameter, "min").is_number());
      CHECK(field(parameter, "max").is_number());
    }
  }
  CHECK(numeric >= 40);
  INFO(defects.size() << " defects, first: " << (defects.empty() ? "" : defects.front()));
  CHECK(defects.empty());
}

TEST_CASE("each repair bound is published and enforced by the stage", "[mastering][catalog]") {
  struct Bound {
    const char* id;
    const char* key;
    double max;
  };
  const std::vector<Bound> bounds = {
      {"repair.declick", "threshold", 10.0},
      {"repair.declick", "neighborRatio", 100.0},
      {"repair.declick", "maxClickSamples", 512.0},
      {"repair.declick", "lpcOrder", 2048.0},
      {"repair.declick", "residualRatio", 1000.0},
      {"repair.declip", "lpcOrder", 2048.0},
      {"repair.declip", "iterations", 8.0},
      {"repair.decrackle", "threshold", 1000.0},
      {"repair.decrackle", "levels", 24.0},
      {"repair.dehum", "fundamentalHz", 5000.0},
      {"repair.dehum", "q", 100.0},
      {"repair.dehum", "searchRangeHz", 100.0},
      {"repair.dehum", "frameSize", 16384.0},
      {"repair.dehum", "pllBandwidth", 1.0},
      {"repair.denoiseClassical", "reductionDb", 120.0},
      {"repair.dereverbClassical", "t60Sec", 10.0},
      {"repair.dereverbClassical", "lateDelayMs", 500.0},
      {"repair.trimSilence", "threshold", 1.0},
      {"repair.trimSilence", "paddingSamples", 960000.0},
      {"repair.trimSilence", "gateLufs", 0.0},
      {"repair.trimSilence", "windowMs", 10000.0},
  };
  for (const Bound& bound : bounds) {
    INFO(bound.id << " " << bound.key);
    const json::Array params = repair_param_info(bound.id);
    const json::Value* parameter = find_param(params, bound.key);
    REQUIRE(parameter != nullptr);
    REQUIRE(field(*parameter, "max").is_number());
    CHECK(field(*parameter, "max").as_number() == Catch::Approx(bound.max));
    CHECK_FALSE(field(*parameter, "maxExclusive").as_bool());
    CHECK(repair_accepts(bound.id, bound.key, bound.max));
    CHECK_FALSE(repair_accepts(bound.id, bound.key, bound.max * 1.01 + 1.0));
  }
}

TEST_CASE("a repair sibling bound is one the stage enforces", "[mastering][catalog]") {
  size_t checked = 0;
  for (const std::string& id : repair_stage_ids()) {
    for (const json::Value& parameter : repair_param_info(id)) {
      const std::string key = field(parameter, "name").as_string();
      for (const json::Value& dependency : field(parameter, "dependsOn").as_array()) {
        const std::string sibling = field(dependency, "key").as_string();
        const std::string relation = field(dependency, "relation").as_string();
        const json::Array params = repair_param_info(id);
        const json::Value* other = find_param(params, sibling);
        REQUIRE(other != nullptr);
        REQUIRE(field(*other, "default").is_number());
        const bool upper = relation == "lt" || relation == "le";
        const double violating = field(*other, "default").as_number() + (upper ? 1.0 : -1.0);
        INFO(id << " " << key << " " << relation << " " << sibling);
        CHECK_FALSE(repair_accepts(id, key, violating));
        ++checked;
      }
    }
  }
  CHECK(checked >= 4);
}

TEST_CASE("the catalog marks which repair stages can run causally", "[mastering][catalog]") {
  const json::Value catalog = json::parse_strict(sonare::mastering::api::processor_catalog_json());
  std::set<std::string> causal;
  std::set<std::string> acausal;
  for (const json::Value& entry : catalog.as_array()) {
    REQUIRE(field(entry, "causal").is_bool());
    const std::string id = field(entry, "id").as_string();
    (field(entry, "causal").as_bool() ? causal : acausal).insert(id);
    if (id.rfind("repair.", 0) == 0) CHECK_FALSE(field(entry, "params").as_array().empty());
  }
  CHECK(acausal.count("repair.declick") == 1);
  CHECK(acausal.count("repair.declip") == 1);
  CHECK(acausal.count("repair.trimSilence") == 1);
  for (const char* id : {"repair.decrackle", "repair.dehum", "repair.denoiseClassical",
                         "repair.dereverbClassical", "dynamics.compressor"}) {
    CHECK(causal.count(id) == 1);
  }
}
