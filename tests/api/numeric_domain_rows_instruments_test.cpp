/// @file numeric_domain_rows_instruments_test.cpp
/// @brief The instrument partition of the numeric-domain table against its C entries.

#include "c_api/numeric_domain_rows_instruments.h"

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "numeric_domain_rows_support.h"

namespace {

using namespace sonare::test::numeric_domains;
using sonare_c_detail::kInstrumentDomainRows;
using sonare_c_detail::numeric_domain_table;

SonareError probe_metronome(const std::string& param, double value) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  SonareEngineMetronomeConfig config{};
  sonare_engine_metronome_config_init(&config);
  if (is_param(param, "config.beat_gain")) config.beat_gain = static_cast<float>(value);
  if (is_param(param, "config.accent_gain")) config.accent_gain = static_cast<float>(value);
  if (is_param(param, "config.click_samples")) config.click_samples = static_cast<int>(value);
  if (is_param(param, "config.click_sec")) config.click_sec = value;
  const SonareError err = sonare_engine_set_metronome(engine, &config);
  sonare_engine_destroy(engine);
  return err;
}

#if defined(SONARE_WITH_ARRANGEMENT)

template <typename C>
void set_builtin_field(C* c, const std::string& field, double value) {
  if (field == "gain") c->gain = static_cast<float>(value);
  if (field == "attack_ms") c->attack_ms = static_cast<float>(value);
  if (field == "decay_ms") c->decay_ms = static_cast<float>(value);
  if (field == "release_ms") c->release_ms = static_cast<float>(value);
  if (field == "sustain") c->sustain = static_cast<float>(value);
  if (field == "polyphony") c->polyphony = static_cast<int>(value);
}

template <typename C>
void set_sf2_field(C* c, const std::string& field, double value) {
  if (field == "gain") c->gain = static_cast<float>(value);
  if (field == "polyphony") c->polyphony = static_cast<int>(value);
  if (field == "prefer_model_for_modeled_families") {
    c->prefer_model_for_modeled_families = static_cast<int>(value);
  }
  if (field == "clear_bank_rig") c->clear_bank_rig = static_cast<int>(value);
  if (field == "gs_efx_realization") c->gs_efx_realization = static_cast<int>(value);
}

std::string field_of(const std::string& param) { return param.substr(param.rfind('.') + 1); }

SonareError probe_engine_builtin(const std::string& param, double value) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 32, 16) == SONARE_OK);
  SonareEngineBuiltinSynthConfig config{};
  sonare_engine_builtin_synth_config_init(&config);
  set_builtin_field(&config, field_of(param), value);
  const SonareError err = sonare_engine_set_builtin_instrument(engine, 5, &config);
  sonare_engine_destroy(engine);
  return err;
}

SonareError probe_engine_sf2(const std::string& param, double value) {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 64, 32, 16) == SONARE_OK);
  SonareEngineSf2InstrumentConfig config{};
  sonare_engine_sf2_instrument_config_init(&config);
  set_sf2_field(&config, field_of(param), value);
  const SonareError err = sonare_engine_set_sf2_instrument(engine, 5, &config);
  sonare_engine_destroy(engine);
  return err;
}

/// Runs @p bounce over a fresh empty project with the options a 64-frame render takes.
SonareError bounce_with(
    const std::function<SonareError(SonareProject*, const SonareProjectBounceOptions*, float**,
                                    size_t*, int*)>& bounce) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  SonareProjectBounceOptions options;
  sonare_project_bounce_options_init(&options);
  options.total_frames = 64;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;
  float* out = nullptr;
  size_t out_len = 0;
  int channels = 0;
  const SonareError err = bounce(project, &options, &out, &out_len, &channels);
  if (err == SONARE_OK) sonare_free_floats(out);
  sonare_project_destroy(project);
  return err;
}

SonareError probe_bounce_builtin(const std::string& param, double value) {
  SonareBuiltinInstrumentBinding binding{};
  sonare_builtin_synth_config_init(&binding.config);
  binding.destination_id = 2;
  set_builtin_field(&binding.config, field_of(param), value);
  return bounce_with([&](SonareProject* project, const SonareProjectBounceOptions* options,
                         float** out, size_t* out_len, int* channels) {
    return sonare_project_bounce_with_builtin_instruments(project, options, &binding, 1, out,
                                                          out_len, channels);
  });
}

SonareError probe_bounce_sf2(const std::string& param, double value) {
  SonareSf2InstrumentBinding binding{};
  sonare_sf2_instrument_config_init(&binding.config);
  binding.destination_id = 2;
  set_sf2_field(&binding.config, field_of(param), value);
  return bounce_with([&](SonareProject* project, const SonareProjectBounceOptions* options,
                         float** out, size_t* out_len, int* channels) {
    return sonare_project_bounce_with_sf2_instruments(project, options, &binding, 1, out, out_len,
                                                      channels);
  });
}

#endif  // SONARE_WITH_ARRANGEMENT

const std::map<std::string, Probe>& probes() {
  static const std::map<std::string, Probe> table = {
      {"engine_set_metronome", probe_metronome},
#if defined(SONARE_WITH_ARRANGEMENT)
      {"engine_set_builtin_instrument", probe_engine_builtin},
      {"engine_set_sf2_instrument", probe_engine_sf2},
      {"project_bounce_with_builtin_instruments", probe_bounce_builtin},
      {"project_bounce_with_sf2_instruments", probe_bounce_sf2},
#endif
  };
  return table;
}

/// The rows whose entry this build compiles; the instrument and bounce entries need arrangement.
std::vector<NumericDomainRow> built_rows() {
  std::vector<NumericDomainRow> rows;
  for (const NumericDomainRow& row : kInstrumentDomainRows) {
    if (probes().count(row.entry) != 0) rows.push_back(row);
  }
  return rows;
}

NumericDomainTable built_table() {
  static const std::vector<NumericDomainRow> rows = built_rows();
  return {"instruments", rows.data(), rows.size()};
}

}  // namespace

TEST_CASE("the instrument numeric-domain rows are sorted", "[numeric_domains][c_api]") {
  require_sorted(numeric_domain_table("instruments", kInstrumentDomainRows));
}

TEST_CASE("an instrument value outside its row's domain is refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_outside_refused(built_table(), probes());
}

TEST_CASE("an instrument value inside its row's domain is not refused under the row's subject",
          "[numeric_domains][c_api]") {
  require_inside_accepted(built_table(), probes());
}
