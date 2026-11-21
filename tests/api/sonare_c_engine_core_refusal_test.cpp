/// @file sonare_c_engine_core_refusal_test.cpp
/// @brief The engine C ABI refuses a caller argument with a ruled text: the parameter as the
///        public header spells it, then its predicate.

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "util/refusal_predicates.h"

namespace {

// Binds a patch-only NativeSynth (no GM programs, no sample bank) on a destination.
SonareError set_synth_patch(SonareEngine* engine, uint32_t destination_id,
                            const SonareSynthPatch* patch) {
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = destination_id;
  binding.patch = *patch;
  return sonare_engine_set_synth_instrument(engine, &binding);
}

}  // namespace

namespace {

namespace refusal = sonare::util::refusal;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string last_message() { return sonare_last_error_message(); }

/// "<subject> <predicate>", the shape every domain refusal takes.
std::string refused(const std::string& subject, const std::string& predicate) {
  return subject + " " + predicate;
}

std::string int_at_least_1() {
  return refusal::integer_range_predicate(1, std::numeric_limits<int>::max());
}

SonareEngine* make_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16, 0) == SONARE_OK);
  return engine;
}

SonareParameterInfo valid_parameter(uint32_t id) {
  SonareParameterInfo info{};
  info.id = id;
  info.min_value = 0.0f;
  info.max_value = 1.0f;
  info.default_value = 0.5f;
  info.rt_safe = 1;
  info.default_curve = 0;
  return info;
}

SonareEngineClip valid_clip(const float* const* channels) {
  SonareEngineClip clip;
  sonare_engine_clip_init(&clip);
  clip.id = 1;
  clip.track_id = 1;
  clip.channels = channels;
  clip.num_channels = 1;
  clip.num_samples = 64;
  clip.gain = 1.0f;
  return clip;
}

}  // namespace

TEST_CASE("engine lifecycle refusals name the parameter", "[c_api][engine_refusal]") {
  CHECK(sonare_engine_create(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  CHECK(sonare_engine_prepare(nullptr, 48000.0, 128, 16, 16, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  const std::string rate = refusal::integer_range_predicate(8000, 384000);
  CHECK(sonare_engine_prepare(engine, 1000.0, 128, 16, 16, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("sample_rate", rate));
  CHECK(sonare_engine_prepare(engine, 48000.0, -1, 16, 16, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("max_block_size", refusal::integer_range_predicate(1, 1 << 20)));
  CHECK(sonare_engine_prepare(engine, 48000.0, 128, 65537, 16, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("command_capacity", refusal::integer_range_predicate(0, 65536)));
  CHECK(sonare_engine_prepare(engine, 48000.0, 128, 16, 16385, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("telemetry_capacity", refusal::integer_range_predicate(0, 16384)));
  CHECK(sonare_engine_prepare(engine, 48000.0, 128, 16, 16, 65) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("max_channels", refusal::integer_range_predicate(1, 64)));
  CHECK(sonare_engine_prepare(engine, 48000.0, 128, 16, 16, -1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("max_channels", refusal::integer_range_predicate(1, 64)));
  sonare_engine_destroy(engine);
}

TEST_CASE("engine transport refusals name the parameter", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_play(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");

  CHECK(sonare_engine_seek_sample(engine, -1, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "timeline_sample must be an integer of at least 0");

  CHECK(sonare_engine_seek_ppq(engine, -1.0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("ppq", refusal::range_predicate(0.0, 1.0e12)));
  CHECK(sonare_engine_seek_ppq(engine, kNaN, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("ppq", refusal::range_predicate(0.0, 1.0e12)));

  CHECK(sonare_engine_set_tempo(engine, 0.0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("bpm", refusal::interval_predicate(0.0, false, 100000.0, true)));

  CHECK(sonare_engine_set_time_signature(engine, 0, 4) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("numerator", int_at_least_1()));
  CHECK(sonare_engine_set_time_signature(engine, 4, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("denominator", int_at_least_1()));

  int64_t sample = 0;
  CHECK(sonare_engine_sample_at_ppq(engine, 1.0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_sample must not be NULL");
  CHECK(sonare_engine_sample_at_ppq(engine, -1.0, &sample) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("ppq", refusal::range_predicate(0.0, 1.0e12)));

  CHECK(sonare_engine_count_in_end_sample(engine, -1, 1, &sample) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "start_sample must be an integer of at least 0");
  CHECK(sonare_engine_count_in_end_sample(engine, 0, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_sample must not be NULL");
  // Two refused arguments: the earlier one in the header is named, and the out is cleared first.
  CHECK(sonare_engine_sample_at_ppq(engine, -1.0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("ppq", refusal::range_predicate(0.0, 1.0e12)));
  sample = 7;
  CHECK(sonare_engine_count_in_end_sample(engine, -1, 1, &sample) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sample == 0);

  CHECK(sonare_engine_set_loop(nullptr, 0.0, 1.0, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  sonare_engine_destroy(engine);
}

TEST_CASE("engine tempo and signature segment refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_tempo_segments(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "segments must not be NULL when segment_count is not 0");

  SonareProjectTempoSegment tempo{};
  tempo.bpm = 120.0;
  tempo.start_ppq = -1.0;
  CHECK(sonare_engine_set_tempo_segments(engine, &tempo, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("segments[0].start_ppq", refusal::range_predicate(0.0, 1.0e12)));
  tempo.start_ppq = 0.0;
  tempo.bpm = 0.0;
  CHECK(sonare_engine_set_tempo_segments(engine, &tempo, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("segments[0].bpm", refusal::interval_predicate(0.0, false, 100000.0, true)));
  tempo.bpm = 120.0;
  tempo.end_bpm = -1.0;
  CHECK(sonare_engine_set_tempo_segments(engine, &tempo, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("segments[0].end_bpm", refusal::range_predicate(0.0, 100000.0)));

  // A segment after the first names its position.
  SonareProjectTempoSegment two_tempo[2]{};
  two_tempo[0].bpm = 120.0;
  two_tempo[1].bpm = 0.0;
  two_tempo[1].start_ppq = 4.0;
  CHECK(sonare_engine_set_tempo_segments(engine, two_tempo, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("segments[1].bpm", refusal::interval_predicate(0.0, false, 100000.0, true)));

  CHECK(sonare_engine_set_time_signature_segments(engine, nullptr, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "segments must not be NULL when segment_count is not 0");
  SonareProjectTimeSignatureSegment sig{};
  sig.start_ppq = 0.0;
  sig.numerator = 0;
  sig.denominator = 4;
  CHECK(sonare_engine_set_time_signature_segments(engine, &sig, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("segments[0].numerator", int_at_least_1()));
  sig.numerator = 4;
  sig.denominator = 0;
  CHECK(sonare_engine_set_time_signature_segments(engine, &sig, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("segments[0].denominator", int_at_least_1()));
  sonare_engine_destroy(engine);
}

TEST_CASE("engine parameter registry refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  const std::string float_domain = refusal::kFloatRangePredicate;

  CHECK(sonare_engine_add_parameter(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "info must not be NULL");

  SonareParameterInfo info = valid_parameter(5);
  info.min_value = std::nanf("");
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("info.min_value", float_domain));
  info = valid_parameter(5);
  info.max_value = std::nanf("");
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("info.max_value", float_domain));
  info = valid_parameter(5);
  info.default_value = std::nanf("");
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("info.default_value", float_domain));
  info = valid_parameter(5);
  info.min_value = 2.0f;
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "info.max_value must be at least info.min_value");
  info = valid_parameter(5);
  info.default_curve = 9;
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("info.default_curve", refusal::one_of_predicate(std::array<int, 4>{0, 1, 2, 3})));
  info = valid_parameter(0x4D580000u);
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "info.id must not be a reserved engine parameter id");
  info = valid_parameter(0);
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "info.id must be an integer in [1, 4294967295]");
  info = valid_parameter(5);
  REQUIRE(sonare_engine_add_parameter(engine, &info) == SONARE_OK);
  CHECK(sonare_engine_add_parameter(engine, &info) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "info.id must be unique among registered parameters");

  size_t count = 0;
  CHECK(sonare_engine_parameter_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");
  CHECK(count == 0);

  SonareParameterInfo out{};
  CHECK(sonare_engine_parameter_info_by_index(engine, 99, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "index must be less than the number of registered parameters");
  CHECK(sonare_engine_parameter_info_by_index(engine, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
  CHECK(sonare_engine_parameter_info(engine, 0x1234u, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "id must be the id of a parameter in this engine");
  sonare_engine_destroy(engine);
}

TEST_CASE("engine automation lane and parameter refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareParameterInfo registered = valid_parameter(5);
  REQUIRE(sonare_engine_add_parameter(engine, &registered) == SONARE_OK);
  SonareParameterInfo not_rt = valid_parameter(7);
  not_rt.rt_safe = 0;
  REQUIRE(sonare_engine_add_parameter(engine, &not_rt) == SONARE_OK);

  CHECK(sonare_engine_set_automation_lane(engine, 5, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "points must not be NULL when point_count is not 0");

  // A reserved mixer id in a non-master lane names no strip.
  const uint32_t no_strip = 0x4D580100u;
  CHECK(sonare_engine_set_automation_lane(engine, no_strip, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_id must be the id of a mixer strip's parameter in this engine");
  CHECK(sonare_engine_set_automation_lane(engine, 7, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_id must name a realtime-safe parameter");

  SonareAutomationPoint point{};
  point.ppq = kNaN;
  CHECK(sonare_engine_set_automation_lane(engine, 5, &point, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "points[0].ppq must be finite");
  point.ppq = 0.0;
  point.value = std::nanf("");
  CHECK(sonare_engine_set_automation_lane(engine, 5, &point, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("points[0].value", refusal::kFloatRangePredicate));
  point.value = 0.0f;
  point.curve_to_next = 9;
  CHECK(sonare_engine_set_automation_lane(engine, 5, &point, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("points[0].curve_to_next",
                                  refusal::one_of_predicate(std::array<int, 4>{0, 1, 2, 3})));

  CHECK(sonare_engine_set_parameter(engine, no_strip, 0.5f, 0, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_id must be the id of a mixer strip's parameter in this engine");
  CHECK(sonare_engine_set_parameter(engine, 7, 0.5f, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "param_id must name a realtime-safe parameter");
  CHECK(sonare_engine_set_parameter(engine, 5, std::nanf(""), 0, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("value", refusal::kFloatRangePredicate));
  CHECK(sonare_engine_set_parameter(engine, 5, std::nanf(""), 0, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("value", refusal::kFloatRangePredicate));
  CHECK(sonare_engine_set_param_smoothing_ms(engine, -1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("smoothing_ms", refusal::at_least_predicate(0.0)));
  sonare_engine_destroy(engine);
}

TEST_CASE("engine marker refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_markers(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "markers must not be NULL when marker_count is not 0");

  SonareEngineMarker markers[2]{};
  markers[0].id = 0;
  CHECK(sonare_engine_set_markers(engine, markers, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("markers[0].id", refusal::integer_range_predicate(
                                                       1, std::numeric_limits<uint32_t>::max())));
  markers[0].id = 1;
  markers[1].id = 1;
  CHECK(sonare_engine_set_markers(engine, markers, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "markers[1].id must be unique across markers");
  markers[1].id = 2;
  markers[1].ppq = -1.0;
  CHECK(sonare_engine_set_markers(engine, markers, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("markers[1].ppq", refusal::at_least_predicate(0.0)));

  markers[1].ppq = 4.0;
  REQUIRE(sonare_engine_set_markers(engine, markers, 2) == SONARE_OK);

  SonareEngineMarker out{};
  CHECK(sonare_engine_marker_by_index(engine, 9, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "index must be less than the number of markers");
  CHECK(sonare_engine_marker(engine, 77, &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "id must be the id of a marker in this engine");
  CHECK(sonare_engine_marker(engine, 1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");

  CHECK(sonare_engine_set_loop_from_markers(engine, 77, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "start_marker_id must be the id of a marker in this engine");
  CHECK(sonare_engine_set_loop_from_markers(engine, 1, 77) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "end_marker_id must be the id of a marker in this engine");
  CHECK(sonare_engine_set_loop_from_markers(engine, 2, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "the marker end_marker_id names must be after the marker "
        "start_marker_id names");
  sonare_engine_destroy(engine);
}

TEST_CASE("engine metronome, monitor and offline priming refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_engine_set_metronome(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config must not be NULL");
  SonareEngineMetronomeConfig config;
  sonare_engine_metronome_config_init(&config);
  config.beat_gain = -1.0f;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("config.beat_gain", refusal::at_least_predicate(0.0)));
  config.beat_gain = 1.0f;
  config.accent_gain = -1.0f;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("config.accent_gain", refusal::at_least_predicate(0.0)));
  config.accent_gain = 1.0f;
  config.click_samples = 384001;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("config.click_samples", refusal::integer_range_predicate(1, 384000)));
  config.click_samples = 0;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("config.click_samples", refusal::integer_range_predicate(1, 384000)));
  config.click_samples = SONARE_ENGINE_METRONOME_CLICK_FROM_SEC;
  config.click_sec = 2.0;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("config.click_sec", refusal::interval_predicate(0.0, false, 1.0, true)));
  config.click_sec = 0.0;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("config.click_sec", refusal::interval_predicate(0.0, false, 1.0, true)));
  config.click_sec = 0.002;
  config.struct_size = 0;
  CHECK(sonare_engine_set_metronome(engine, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config.struct_size must be set by sonare_engine_metronome_config_init");

  CHECK(sonare_engine_set_track_monitor_mode(engine, 0,
                                             static_cast<SonareEngineTrackMonitorMode>(9),
                                             0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("mode", refusal::one_of_predicate(std::array<int, 3>{0, 1, 2})));

  CHECK(sonare_engine_prime_offline_parameters(engine, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("num_channels", int_at_least_1()));
  CHECK(sonare_engine_prime_offline_parameters(engine, 1, -1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("block_size", refusal::integer_range_predicate(
                                                    0, std::numeric_limits<int>::max())));
  CHECK(sonare_engine_prime_offline_parameters(engine, 1000, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("num_channels must be an integer in [1, ", 0) == 0);
  CHECK(sonare_engine_prime_offline_parameters(engine, 1, 1 << 20) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().rfind("block_size must be an integer in [1, ", 0) == 0);

  CHECK(sonare_engine_graph_latency_samples_q8(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_latency_q8 must not be NULL");
  CHECK(sonare_engine_max_block_size(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_max_block_size must not be NULL");
  sonare_engine_destroy(engine);
}

TEST_CASE("engine clip refusals name the clip field", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  const float samples[64] = {};
  const float* channels[1] = {samples};
  const std::string int64_at_least_0 = "must be an integer of at least 0";

  CHECK(sonare_engine_set_clips(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clips must not be NULL when clip_count is not 0");
  CHECK(sonare_engine_upsert_clip(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip must not be NULL");
  CHECK(sonare_engine_clip_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");

  SonareEngineClip clip = valid_clip(nullptr);
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip.channels must not be NULL");

  const float* null_channel[1] = {nullptr};
  clip = valid_clip(null_channel);
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip.channels[0] must not be NULL");

  clip = valid_clip(channels);
  clip.num_channels = 0;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clip.num_channels", int_at_least_1()));
  clip = valid_clip(channels);
  clip.num_samples = 0;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip.num_samples must be an integer of at least 1");
  clip = valid_clip(channels);
  clip.start_ppq = -1.0;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clip.start_ppq", refusal::range_predicate(0.0, 1.0e12)));
  clip = valid_clip(channels);
  clip.clip_offset_samples = 64;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("clip.clip_offset_samples", refusal::integer_range_predicate(0, 63)));
  clip = valid_clip(channels);
  clip.gain = -1.0f;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clip.gain", refusal::at_least_predicate(0.0)));
  clip = valid_clip(channels);
  clip.fade_in_samples = -1;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clip.fade_in_samples", int64_at_least_0));
  clip = valid_clip(channels);
  clip.fade_out_samples = -1;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clip.fade_out_samples", int64_at_least_0));
  clip = valid_clip(channels);
  clip.warp_mode = 9;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("clip.warp_mode", refusal::one_of_predicate(std::array<int, 4>{0, 1, 2, 3})));
  clip = valid_clip(channels);
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  clip.loop = 1;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip.loop must be 0 when clip.warp_mode is tempoSync");
  for (const int64_t bad_length : {int64_t{0}, int64_t{-2}}) {
    clip = valid_clip(channels);
    clip.length_samples = bad_length;
    CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_message() == "clip.length_samples must be an integer of at least 1");
  }
  clip = valid_clip(channels);
  clip.struct_size = 0;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip.struct_size must be set by sonare_engine_clip_init");

  clip = valid_clip(channels);
  clip.warp_anchor_count = 2;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "clip.warp_anchors must not be NULL when clip.warp_anchor_count is not 0");

  SonareEngineWarpAnchor anchors[2] = {{0.0, 0.0}, {10.0, 10.0}};
  clip = valid_clip(channels);
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_REPITCH;
  clip.loop = 1;
  clip.warp_anchors = anchors;
  clip.warp_anchor_count = 2;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "clip.loop must be 0 for a repitch or timeStretch clip when clip.warp_anchor_count is "
        "at least 2");

  clip = valid_clip(channels);
  anchors[0].warp_sample = -1.0;
  clip.warp_anchors = anchors;
  clip.warp_anchor_count = 2;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("clip.warp_anchors[0].warp_sample", refusal::at_least_predicate(0.0)));
  anchors[0] = {0.0, -1.0};
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("clip.warp_anchors[0].source_sample", refusal::at_least_predicate(0.0)));
  anchors[0] = {5.0, 5.0};
  anchors[1] = {5.0, 10.0};
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "clip.warp_anchors[1].warp_sample and clip.warp_anchors[1].source_sample must be "
        "greater than those of clip.warp_anchors[0]");

  // A tempo-sync clip whose anchors reach past the source holds no usable span.
  anchors[0] = {0.0, 0.0};
  anchors[1] = {10.0, 1000.0};
  clip.warp_mode = SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  CHECK(sonare_engine_upsert_clip(engine, &clip) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "clip.warp_anchors must describe non-empty spans inside the source for a tempo-sync clip");

  // A clip inside a set names its position.
  SonareEngineClip clip_set[2] = {valid_clip(channels), valid_clip(channels)};
  clip_set[1].gain = -1.0f;
  CHECK(sonare_engine_set_clips(engine, clip_set, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("clips[1].gain", refusal::at_least_predicate(0.0)));
  clip_set[1] = valid_clip(null_channel);
  CHECK(sonare_engine_set_clips(engine, clip_set, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clips[1].channels[0] must not be NULL");
  clip_set[1] = valid_clip(channels);
  clip_set[1].struct_size = 0;
  CHECK(sonare_engine_set_clips(engine, clip_set, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clips[1].struct_size must be set by sonare_engine_clip_init");

  CHECK(sonare_engine_remove_clip(engine, 99) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "clip_id must be the id of a clip in this engine");
  sonare_engine_destroy(engine);
}

TEST_CASE("clip page provider refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();

  CHECK(sonare_clip_page_provider_create(1, 64, 32, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_provider must not be NULL");

  SonareClipPageProvider* provider = nullptr;
  CHECK(sonare_clip_page_provider_create(0, 64, 32, &provider) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("num_channels", refusal::integer_range_predicate(1, 64)));
  CHECK(sonare_clip_page_provider_create(1, 0, 32, &provider) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "num_samples must be an integer of at least 1");
  CHECK(sonare_clip_page_provider_create(1, 64, 0, &provider) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("page_frames", refusal::integer_range_predicate(1, int64_t{1} << 24)));
  CHECK(sonare_clip_page_provider_create(1, int64_t{1} << 40, 1, &provider) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "num_samples must span at most " + std::to_string(1 << 20) +
                              " pages of page_frames frames");

  REQUIRE(sonare_clip_page_provider_create(1, 64, 32, &provider) == SONARE_OK);
  const float page[32] = {};
  const float* channels[1] = {page};
  CHECK(sonare_clip_page_provider_supply(nullptr, 0, channels, 1, 32) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "provider must not be NULL");
  CHECK(sonare_clip_page_provider_supply(provider, 5, channels, 1, 32) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("page_index", refusal::integer_range_predicate(0, 1)));
  CHECK(sonare_clip_page_provider_supply(provider, 0, nullptr, 1, 32) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels must not be NULL");
  CHECK(sonare_clip_page_provider_supply(provider, 0, channels, 2, 32) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("num_channels", refusal::one_of_predicate(std::array<int, 1>{1})));
  CHECK(sonare_clip_page_provider_supply(provider, 0, channels, 1, 5) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("frames", refusal::one_of_predicate(std::array<int, 1>{32})));
  const float* null_channel[1] = {nullptr};
  CHECK(sonare_clip_page_provider_supply(provider, 0, null_channel, 1, 32) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "channels[0] must not be NULL");

  CHECK(sonare_clip_page_provider_clear(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "provider must not be NULL");
  CHECK(sonare_clip_page_provider_clear(provider, 5) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("page_index", refusal::integer_range_predicate(0, 1)));
  sonare_clip_page_provider_destroy(provider);

  SonareClipPageRequest request{};
  int has_request = 0;
  CHECK(sonare_engine_pop_clip_page_request(engine, nullptr, &has_request) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_request must not be NULL");
  CHECK(sonare_engine_pop_clip_page_request(engine, &request, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_has_request must not be NULL");

  CHECK(sonare_engine_set_warp_voice_capacity(engine, 65) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("voices", refusal::integer_range_predicate(0, 64)));
  CHECK(sonare_engine_warp_voice_capacity(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_voices must not be NULL");
  CHECK(sonare_engine_set_clip_page_prefetch_frames(engine, -1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "frames must be an integer of at least 0");
  CHECK(sonare_engine_clip_page_prefetch_frames(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_frames must not be NULL");
  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("engine controller profile refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  patch.engine_mode = SONARE_SYNTH_ENGINE_REED;
  REQUIRE(set_synth_patch(engine, 3, &patch) == SONARE_OK);

  CHECK(sonare_engine_set_controller_profile(engine, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "preset_name must not be NULL");
  CHECK(sonare_engine_set_controller_profile(engine, 3, "") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "preset_name must not be empty");
  CHECK(sonare_engine_set_controller_profile(engine, 3, "no-such-preset") ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "preset_name must name a controller profile preset");

  CHECK(sonare_engine_bind_controller(engine, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "binding must not be NULL");

  SonareControllerBinding binding{};
  binding.input = SONARE_CONTROLLER_INPUT_CONTROL_CHANGE;
  binding.axis = SONARE_CONTROLLER_AXIS_EXCITATION;
  binding.lo = 0.0f;
  binding.hi = 1.0f;
  binding.curve = 1.0f;

  SonareControllerBinding bad = binding;
  bad.input = 9;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("binding.input", refusal::one_of_predicate(std::array<int, 5>{0, 1, 2, 3, 4})));
  bad = binding;
  bad.axis = 20;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.axis", refusal::kDeclaredValuePredicate));
  bad = binding;
  bad.index = 200;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.index", refusal::integer_range_predicate(0, 127)));
  bad = binding;
  bad.lo = std::nanf("");
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.lo", refusal::kFloatRangePredicate));
  bad = binding;
  bad.hi = std::nanf("");
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.hi", refusal::kFloatRangePredicate));
  bad = binding;
  bad.curve = 0.0f;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.curve", refusal::greater_than_predicate(0.0)));
  bad = binding;
  bad.axis = SONARE_CONTROLLER_AXIS_NONE;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "binding.axis must not be none");
  bad = binding;
  bad.input = SONARE_CONTROLLER_INPUT_POLY_PRESSURE;
  bad.axis = SONARE_CONTROLLER_AXIS_LOUDNESS;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "binding.axis must be an excitation axis when binding.input is "
        "polyPressure");

  // A destination with no instrument bound.
  CHECK(sonare_engine_bind_controller(engine, 9, &binding) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "destination_id must be the id of an instrument destination in this "
        "engine");
  CHECK(sonare_engine_clear_controller_bindings(engine, 9) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "destination_id must be the id of an instrument destination in this "
        "engine");

  size_t count = 0;
  CHECK(sonare_engine_controller_binding_count(engine, 3, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_count must not be NULL");
  CHECK(sonare_engine_controller_velocity_meaningful(engine, 3, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_meaningful must not be NULL");
  CHECK(count == 0);

  const std::string dimension = refusal::one_of_predicate(std::array<int, 3>{0, 1, 2});
  CHECK(sonare_engine_set_controller_note_tracking(engine, 3, 7, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("dimension", dimension));
  CHECK(sonare_engine_set_controller_note_tracking(engine, 3, 0, 9) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        refused("tracking", refusal::one_of_predicate(std::array<int, 4>{0, 1, 2, 3})));
  int tracking = 0;
  CHECK(sonare_engine_controller_note_tracking(engine, 3, 7, &tracking) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("dimension", dimension));
  CHECK(sonare_engine_controller_note_tracking(engine, 3, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out_tracking must not be NULL");

  // Header order: fields in member order, the whole binding before the destination lookup,
  // dimension before the out-parameter, and every out cleared before the first refusal.
  bad = binding;
  bad.index = 200;
  bad.axis = 20;
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.index", refusal::integer_range_predicate(0, 127)));
  bad = binding;
  bad.axis = SONARE_CONTROLLER_AXIS_NONE;
  bad.lo = std::nanf("");
  CHECK(sonare_engine_bind_controller(engine, 3, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "binding.axis must not be none");
  bad = binding;
  bad.curve = 0.0f;
  CHECK(sonare_engine_bind_controller(engine, 9, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("binding.curve", refusal::greater_than_predicate(0.0)));
  CHECK(sonare_engine_controller_note_tracking(engine, 3, 7, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == refused("dimension", dimension));
  tracking = 2;
  CHECK(sonare_engine_controller_note_tracking(nullptr, 3, 0, &tracking) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(tracking == SONARE_NOTE_TRACKING_LAST);
  int meaningful = 1;
  CHECK(sonare_engine_controller_velocity_meaningful(nullptr, 3, &meaningful) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(meaningful == 0);
  count = 5;
  CHECK(sonare_engine_controller_binding_count(nullptr, 3, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
  sonare_engine_destroy(engine);
}

TEST_CASE("engine project timeline refusals", "[c_api][engine_refusal]") {
  SonareEngine* engine = make_engine();
  CHECK(sonare_engine_apply_project_timeline(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "engine must not be NULL");
  CHECK(sonare_engine_apply_project_timeline(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "timeline must not be NULL");
  sonare_engine_destroy(engine);
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
