/// @file sonare_c_engine_render_midi_refusal_test.cpp
/// @brief The text a refused argument leaves behind on the engine's offline render, telemetry
///        and MIDI entries.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include "host/midi_io.h"
#include "sonare_c_engine_test_helpers.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/synth_patch_common.h"
#endif

namespace {

std::string last_error() { return sonare_last_error_message(); }

// A prepared engine whose render and freeze entries accept two channels and blocks of 128.
SonareEngine* prepared_engine() {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 2) == SONARE_OK);
  return engine;
}

SonareEngineBounceOptions bounce_options() {
  SonareEngineBounceOptions options;
  sonare_engine_bounce_options_init(&options);
  options.total_frames = 64;
  options.num_channels = 2;
  return options;
}

SonareEngineFreezeOptions freeze_options() {
  SonareEngineFreezeOptions options;
  sonare_engine_freeze_options_init(&options);
  options.total_frames = 64;
  options.num_channels = 2;
  options.clip_id = 1;
  options.gain = 1.0f;
  return options;
}

}  // namespace

TEST_CASE("engine offline render refuses a NULL argument and an out-of-domain count by name",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  float left[16] = {};
  float right[16] = {};
  float* planes[] = {left, right};

  CHECK(sonare_engine_render_offline(nullptr, planes, 2, 16, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_render_offline(engine, nullptr, 2, 16, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL");
  CHECK(sonare_engine_render_offline(engine, planes, 0, 16, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "num_channels must be an integer in [1, 2147483647]");
  CHECK(sonare_engine_render_offline(engine, planes, 2, -1, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "total_frames must be an integer of at least 0");
  CHECK(sonare_engine_render_offline(engine, planes, 2, 16, -1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "block_size must be an integer in [0, 2147483647]");
  CHECK(sonare_engine_render_offline(engine, planes, 3, 16, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "num_channels must be an integer in [1, 2]");
  CHECK(sonare_engine_render_offline(engine, planes, 2, 16, 256, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "block_size must be an integer in [1, 128]");

  CHECK(sonare_engine_finish_offline_render(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  // A NULL options is ignored by the init functions.
  sonare_engine_bounce_options_init(nullptr);
  sonare_engine_freeze_options_init(nullptr);

  sonare_engine_destroy(engine);
}

TEST_CASE("engine bounce refuses each option by its member name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  SonareEngineBounceResult result{};
  const SonareEngineBounceOptions good = bounce_options();

  CHECK(sonare_engine_bounce_offline(nullptr, &good, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_bounce_offline(engine, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "options must not be NULL");
  CHECK(sonare_engine_bounce_offline(engine, &good, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL");

  auto refused = [&](auto edit) {
    SonareEngineBounceOptions options = good;
    edit(options);
    result = {};
    CHECK(sonare_engine_bounce_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
    return last_error();
  };
  CHECK(refused([](auto& o) { o.total_frames = 0; }) ==
        "options.total_frames must be an integer of at least 1");
  CHECK(refused([](auto& o) { o.struct_size = 0; }) ==
        "options.struct_size must be set by sonare_engine_bounce_options_init");
  CHECK(refused([](auto& o) { o.block_size = 0; }) ==
        "options.block_size must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.block_size = -2; }) ==
        "options.block_size must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.num_channels = 0; }) ==
        "options.num_channels must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.target_sample_rate = 0; }) ==
        "options.target_sample_rate must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.target_sample_rate = -2; }) ==
        "options.target_sample_rate must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.source_sample_rate = 0; }) ==
        "options.source_sample_rate must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.source_sample_rate = -2; }) ==
        "options.source_sample_rate must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.target_lufs = std::numeric_limits<float>::infinity(); }) ==
        "options.target_lufs must be finite");
  CHECK(refused([](auto& o) { o.total_frames = int64_t{1} << 40; }) ==
        "options.total_frames, options.num_channels, options.source_sample_rate and "
        "options.target_sample_rate give a result "
        "larger than the supported size");
  CHECK(refused([](auto& o) { o.dither = 4; }) == "options.dither must be one of 0, 1, 2, 3");
  CHECK(refused([](auto& o) { o.num_channels = 3; }) ==
        "options.num_channels must be one of 1, 2, 6, 8");
  CHECK(refused([](auto& o) { o.num_channels = 6; }) ==
        "options.num_channels must be an integer in [1, 2]");
  CHECK(refused([](auto& o) { o.block_size = 256; }) ==
        "options.block_size must be an integer in [1, 128]");
  CHECK(refused([](auto& o) { o.source_sample_rate = 44100; }) ==
        "options.source_sample_rate must equal the prepared sample rate 48000");
  // Members are refused in declaration order, and every option before a NULL out.
  CHECK(refused([](auto& o) {
          o.num_channels = 3;
          o.dither = 4;
        }) == "options.num_channels must be one of 1, 2, 6, 8");
  CHECK(refused([](auto& o) {
          o.dither = 4;
          o.dither_bits = 0;
        }) == "options.dither must be one of 0, 1, 2, 3");
  SonareEngineBounceOptions zero_frames = good;
  zero_frames.total_frames = 0;
  CHECK(sonare_engine_bounce_offline(engine, &zero_frames, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "options.total_frames must be an integer of at least 1");
#if defined(SONARE_WITH_MASTERING)
  for (const int bad_bits : {0, 1, 99}) {
    CHECK(refused([bad_bits](auto& o) {
            o.dither = 1;
            o.dither_bits = bad_bits;
          }) == "options.dither_bits must be an integer in [2, 32]");
  }
#endif
  // The word length is read only when a dither is chosen.
  {
    SonareEngineBounceOptions undithered = good;
    undithered.dither_bits = 0;
    SonareEngineBounceResult undithered_result{};
    CHECK(sonare_engine_bounce_offline(engine, &undithered, &undithered_result) == SONARE_OK);
    sonare_free_engine_bounce_result(&undithered_result);
  }

  sonare_engine_destroy(engine);
}

TEST_CASE("engine freeze refuses each option by its member name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  SonareEngineFreezeResult result{};
  const SonareEngineFreezeOptions good = freeze_options();

  CHECK(sonare_engine_freeze_offline(nullptr, &good, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_freeze_offline(engine, nullptr, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "options must not be NULL");
  CHECK(sonare_engine_freeze_offline(engine, &good, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL");

  auto refused = [&](auto edit) {
    SonareEngineFreezeOptions options = good;
    edit(options);
    result = {};
    CHECK(sonare_engine_freeze_offline(engine, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
    return last_error();
  };
  CHECK(refused([](auto& o) { o.total_frames = 0; }) ==
        "options.total_frames must be an integer of at least 1");
  CHECK(refused([](auto& o) { o.struct_size = 0; }) ==
        "options.struct_size must be set by sonare_engine_freeze_options_init");
  CHECK(refused([](auto& o) { o.block_size = 0; }) ==
        "options.block_size must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.block_size = -2; }) ==
        "options.block_size must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.num_channels = 0; }) ==
        "options.num_channels must be an integer in [1, 2147483647]");
  CHECK(refused([](auto& o) { o.start_ppq = std::nan(""); }) ==
        "options.start_ppq must be finite and at least 0");
  CHECK(refused([](auto& o) { o.gain = -1.0f; }) == "options.gain must be finite and at least 0");
  CHECK(refused([](auto& o) { o.total_frames = int64_t{1} << 40; }) ==
        "options.total_frames times options.num_channels exceeds the supported size");
  CHECK(refused([](auto& o) { o.num_channels = 3; }) ==
        "options.num_channels must be an integer in [1, 2]");
  CHECK(refused([](auto& o) { o.block_size = 256; }) ==
        "options.block_size must be an integer in [1, 128]");

  sonare_engine_destroy(engine);
}

TEST_CASE("engine offline entries on an unprepared engine record a text without a function prefix",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  float left[16] = {};
  float* planes[] = {left};
  const std::string not_prepared = "the engine has not been prepared";

  CHECK(sonare_engine_render_offline(engine, planes, 1, 16, 0, nullptr) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_error() == not_prepared);
  CHECK(sonare_engine_finish_offline_render(engine) == SONARE_ERROR_INVALID_STATE);
  CHECK(last_error() == not_prepared);

  SonareEngineBounceOptions bounce = bounce_options();
  bounce.source_sample_rate = 48000;
  SonareEngineBounceResult bounce_result{};
  CHECK(sonare_engine_bounce_offline(engine, &bounce, &bounce_result) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_error() == not_prepared);

  const SonareEngineFreezeOptions freeze = freeze_options();
  SonareEngineFreezeResult freeze_result{};
  CHECK(sonare_engine_freeze_offline(engine, &freeze, &freeze_result) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_error() == not_prepared);

  sonare_engine_destroy(engine);
}

TEST_CASE("engine telemetry drains refuse a NULL buffer by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  size_t count = 0;

  SonareEngineTelemetry telemetry[1] = {};
  CHECK(sonare_engine_drain_telemetry(nullptr, telemetry, 1, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_drain_telemetry(engine, telemetry, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "written must not be NULL");
  CHECK(sonare_engine_drain_telemetry(engine, nullptr, 1, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_records is not 0");

  SonareMeterTelemetryRecord meter[1] = {};
  CHECK(sonare_engine_drain_meter_telemetry(engine, meter, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_drain_meter_telemetry(engine, nullptr, 1, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_records is not 0");
  CHECK(sonare_engine_drain_scope_telemetry(engine, nullptr, 1, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_records is not 0");
  CHECK(sonare_engine_drain_scope_telemetry(engine, nullptr, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  // The record buffer precedes the count out-parameter in the header, so it is refused first.
  CHECK(sonare_engine_drain_telemetry(engine, nullptr, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_records is not 0");
  CHECK(sonare_engine_drain_meter_telemetry(engine, nullptr, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_records is not 0");

  CHECK(sonare_engine_configure_scope_telemetry(nullptr, 0, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");

  float gain_reduction[1] = {};
  CHECK(sonare_engine_meter_target_insert_gain_reduction(engine, 1, nullptr, 1, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_db must not be NULL when capacity is not 0");
  CHECK(sonare_engine_meter_target_insert_gain_reduction(engine, 1, gain_reduction, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_meter_target_insert_gain_reduction(engine, 1, nullptr, 1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_db must not be NULL when capacity is not 0");
#if defined(SONARE_WITH_MIXING)
  CHECK(sonare_engine_meter_target_insert_gain_reduction(engine, 0xDEADBEEFu, gain_reduction, 1,
                                                         &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "target_id must be the id of a meter target in this engine");
#endif

  sonare_engine_destroy(engine);
}

TEST_CASE("engine MIDI entries refuse a NULL argument by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  size_t count = 0;

  CHECK(sonare_engine_set_midi_clips(nullptr, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_set_midi_clips(engine, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "clips must not be NULL when clip_count is not 0");
  CHECK(sonare_engine_set_builtin_instrument(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config must not be NULL");
  CHECK(sonare_engine_set_synth_instrument(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "binding must not be NULL");
  CHECK(sonare_engine_set_sf2_instrument(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config must not be NULL");
  CHECK(sonare_engine_clear_midi_instrument(nullptr, 7) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_midi_instrument_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_midi_cc_binding_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_midi_input_pending_count(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_external_midi_dropped_count(engine, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");
  CHECK(sonare_engine_bind_midi_cc(engine, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "binding must not be NULL");
  CHECK(sonare_engine_set_midi_fx(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config_json must not be NULL");
  CHECK(sonare_engine_clear_midi_cc_bindings(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_clear_midi_fx(nullptr, 7) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_set_midi_input_source(nullptr, 7) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_clear_midi_input_source(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_push_midi_note_on(nullptr, 7, 0, 0, 60, 100, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_push_midi_input_note_on(nullptr, 0, 0, 60, 100, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_push_midi_panic(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_push_midi_ump(nullptr, 7, nullptr, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_push_midi_sysex(engine, 7, nullptr, 3, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "data must not be NULL");
  CHECK(sonare_engine_load_soundfont(engine, nullptr, 8) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "data must not be NULL");
  CHECK(sonare_engine_set_external_midi_clock_enabled(nullptr, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");

  int articulation = 0;
  CHECK(sonare_engine_articulation(engine, 7, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_articulation must not be NULL");
  CHECK(sonare_engine_articulation(nullptr, 7, 0, &articulation) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "engine must not be NULL");
  CHECK(sonare_engine_legato_fallback_count(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");

  SonareExternalMidiEvent events[8] = {};
  CHECK(sonare_engine_drain_external_midi(engine, nullptr, 8, &count) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL when max_events is not 0");
  CHECK(sonare_engine_drain_external_midi(engine, events, 8, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_count must not be NULL");

  sonare_engine_destroy(engine);
}

TEST_CASE("engine MIDI entries refuse an out-of-domain value by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();

  // Channel, controller, parameter id and range of a CC binding.
  SonareMidiCcBinding good{};
  good.cc_number = 1;
  good.channel = 0;
  good.kind = SONARE_MIDI_CC_CONTROL_CHANGE_7;
  good.param_id = 7;
  good.min_value = 0.0f;
  good.max_value = 1.0f;
  auto binding_refused = [&](auto edit) {
    SonareMidiCcBinding binding = good;
    edit(binding);
    CHECK(sonare_engine_bind_midi_cc(engine, &binding) == SONARE_ERROR_INVALID_PARAMETER);
    return last_error();
  };
  CHECK(binding_refused([](auto& b) { b.cc_number = 128; }) ==
        "binding.cc_number must be an integer in [0, 127]");
  CHECK(binding_refused([](auto& b) { b.channel = 16; }) ==
        "binding.channel must be an integer in [0, 15] or 255");
  CHECK(binding_refused([](auto& b) { b.kind = 4; }) == "binding.kind must be one of 0, 1, 2, 3");
  CHECK(binding_refused([](auto& b) { b.param_id = 0; }) ==
        "binding.param_id must be an integer in [1, 4294967295]");
  CHECK(binding_refused([](auto& b) { b.min_value = std::nanf(""); }) ==
        "binding.min_value must be finite and in [-3.4028235e38, 3.4028235e38]");
  CHECK(binding_refused([](auto& b) { b.param_id = 0x4D58FF00u; }) ==
        "binding.param_id must not be a reserved engine parameter id");
  CHECK(binding_refused([](auto& b) { b.max_value = -1.0f; }) ==
        "binding.max_value must be at least binding.min_value");
  CHECK(binding_refused([](auto& b) {
          b.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
          b.cc_number = 32;
          b.cc_lsb_number = 64;
        }) == "binding.cc_number must be an integer in [0, 31]");
  CHECK(binding_refused([](auto& b) {
          b.kind = SONARE_MIDI_CC_CONTROL_CHANGE_14;
          b.cc_number = 1;
          b.cc_lsb_number = 2;
        }) == "binding.cc_lsb_number must be binding.cc_number + 32");

  // Instrument parameter name and soundfont size.
  uint32_t id = 0;
  CHECK(sonare_engine_resolve_instrument_automation_id(engine, 7, nullptr, &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "param_name must not be NULL");
  CHECK(sonare_engine_resolve_instrument_automation_id(engine, 7, "", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "param_name must not be empty");
  CHECK(sonare_engine_resolve_instrument_automation_id(engine, 7, "gain", nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_id must not be NULL");
  const uint8_t byte = 0;
  CHECK(sonare_engine_load_soundfont(engine, &byte, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "size must be an integer of at least 1");

  // Articulation and part rig ordinals.
  CHECK(sonare_engine_set_articulation(engine, 7, 16, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "channel must be an integer in [0, 15]");
  CHECK(sonare_engine_set_articulation(engine, 7, 0, 5) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "articulation must be one of 0, 1, 2");
  int articulation = 0;
  CHECK(sonare_engine_articulation(engine, 7, 16, &articulation) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "channel must be an integer in [0, 15]");
  CHECK(sonare_engine_set_part_rig(engine, 7, 16, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "part must be an integer in [0, 15] or 255");
  CHECK(sonare_engine_set_part_rig(engine, 7, 0, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "mode must be one of 0, 1, 2");
  CHECK(sonare_engine_set_part_rig(engine, 7, 0, 2, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "inserts_json must not be NULL when mode is chain");
  CHECK(sonare_engine_set_part_rig(engine, 7, 0, 0, "[]") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "inserts_json must be NULL unless mode is chain");

  // Drain buffer shorter than one lowered record.
  constexpr size_t kLowered =
      std::extent<decltype(sonare::host::ExternalMidi1Lowered::messages)>::value;
  SonareExternalMidiEvent events[8] = {};
  size_t count = 0;
  if (kLowered > 1) {
    CHECK(sonare_engine_drain_external_midi(engine, events, 1, &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() ==
          "max_events must be 0 or an integer of at least " + std::to_string(kLowered));
  }

  sonare_engine_destroy(engine);
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("engine MIDI clips refuse each member by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  const SonareEngineMidiEvent event{0, 0x20906464u, 0, 0, 0, 1, 0, 0, 0};
  SonareEngineMidiClipSchedule good;
  sonare_engine_midi_clip_schedule_init(&good);
  good.gain = 1.0f;
  good.length_samples = 1024;
  good.events = &event;
  good.event_count = 1;

  auto refused = [&](auto edit) {
    SonareEngineMidiClipSchedule clip = good;
    edit(clip);
    CHECK(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_ERROR_INVALID_PARAMETER);
    return last_error();
  };
  CHECK(refused([](auto& c) { c.struct_size = 0; }) ==
        "clips[0].struct_size must be set by sonare_engine_midi_clip_schedule_init");
  CHECK(refused([](auto& c) { c.start_ppq = std::nan(""); }) ==
        "clips[0].start_ppq must be finite");
  CHECK(refused([](auto& c) { c.length_samples = 0; }) ==
        "clips[0].length_samples must be an integer of at least 1");
  CHECK(refused([](auto& c) { c.length_samples = -2; }) ==
        "clips[0].length_samples must be an integer of at least 1");
  CHECK(refused([](auto& c) {
          c.loop = 1;
          c.loop_length_samples = 0;
        }) == "clips[0].loop_length_samples must be an integer of at least 1");
  CHECK(refused([](auto& c) { c.loop = -1; }) ==
        "clips[0].loop must be an integer in [0, 2147483647]");
  CHECK(refused([](auto& c) { c.events = nullptr; }) ==
        "clips[0].events must not be NULL when clips[0].event_count is not 0");
  CHECK(refused([](auto& c) { c.gain = -1.0f; }) == "clips[0].gain must be finite and at least 0");
  CHECK(refused([](auto& c) { c.fade_in_samples = -1; }) ==
        "clips[0].fade_in_samples must be an integer of at least 0");
  CHECK(refused([](auto& c) { c.fade_out_samples = -1; }) ==
        "clips[0].fade_out_samples must be an integer of at least 0");
  CHECK(refused([](auto& c) {
          c.length_samples = SONARE_ENGINE_MIDI_CLIP_OPEN_ENDED;
          c.fade_out_samples = 16;
        }) == "clips[0].fade_out_samples must be 0 when clips[0].length_samples is open-ended");

  SonareEngineMidiEvent bad_group = event;
  bad_group.group = 16;
  CHECK(refused([&](auto& c) { c.events = &bad_group; }) ==
        "clips[0].events[0].group must be an integer in [0, 15]");
  SonareEngineMidiEvent bad_reserved = event;
  bad_reserved.reserved = 1;
  CHECK(refused([&](auto& c) { c.events = &bad_reserved; }) ==
        "clips[0].events[0].reserved must be 0");

  sonare_engine_destroy(engine);
}

TEST_CASE("engine instrument entries refuse an unknown destination and a bad ordinal",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  const std::string unknown_destination =
      "destination_id must be the id of a destination with a MIDI instrument in this engine";

  int articulation = 0;
  CHECK(sonare_engine_set_articulation(engine, 99, 0, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == unknown_destination);
  CHECK(sonare_engine_articulation(engine, 99, 0, &articulation) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == unknown_destination);
  size_t legato = 0;
  CHECK(sonare_engine_legato_fallback_count(engine, 99, &legato) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == unknown_destination);
  CHECK(sonare_engine_set_part_rig(engine, 99, 0, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == unknown_destination);

  SonareEngineBuiltinSynthConfig synth{};
  sonare_engine_builtin_synth_config_init(&synth);
  synth.gain = 0.5f;
  synth.waveform = 9;
  CHECK(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "config.waveform must be one of 0, 1, 2, 3");
  synth.waveform = 0;
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  uint32_t id = 0;
  CHECK(sonare_engine_resolve_instrument_automation_id(engine, 7, "noSuchParameter", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() ==
        "param_name must be the name of a parameter of the instrument bound to destination_id");
  CHECK(sonare_engine_resolve_instrument_automation_id(engine, 99, "gain", &id) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == unknown_destination);

  SonareEngineSf2InstrumentConfig sf2{};
  sonare_engine_sf2_instrument_config_init(&sf2);
  sf2.struct_size = 0;
  CHECK(sonare_engine_set_sf2_instrument(engine, 7, &sf2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() ==
        "config.struct_size must be set by sonare_engine_sf2_instrument_config_init");

  uint8_t data[1] = {0};
  CHECK(sonare_engine_push_midi_sysex(engine, 7, data, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "size must be an integer in [1, 512]");

  sonare_engine_destroy(engine);
}

TEST_CASE("engine built-in synth refuses each member by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  SonareEngineBuiltinSynthConfig synth{};
  sonare_engine_builtin_synth_config_init(&synth);
  CHECK(synth.struct_size == sizeof(SonareEngineBuiltinSynthConfig));
  const sonare::midi::BuiltinSynthConfig core_defaults{};
  CHECK(synth.waveform == static_cast<int>(core_defaults.waveform));
  CHECK(synth.gain == core_defaults.gain);
  CHECK(synth.attack_ms == core_defaults.attack_ms);
  CHECK(synth.decay_ms == core_defaults.decay_ms);
  CHECK(synth.sustain == core_defaults.sustain);
  CHECK(synth.release_ms == core_defaults.release_ms);
  CHECK(synth.polyphony == core_defaults.polyphony);
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);

  const auto refused = [&](const SonareEngineBuiltinSynthConfig& config, const std::string& text) {
    CHECK(sonare_engine_set_builtin_instrument(engine, 7, &config) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == text);
  };
  SonareEngineBuiltinSynthConfig bad = synth;
  bad.struct_size = static_cast<uint32_t>(sizeof(bad)) - 1;
  refused(bad, "config.struct_size must be set by sonare_engine_builtin_synth_config_init");
  bad = synth;
  bad.gain = 0.0f;
  refused(bad, "config.gain must be finite and in (0, 4]");
  bad = synth;
  bad.attack_ms = 0.0f;
  refused(bad, "config.attack_ms must be finite and in (0, 20000]");
  bad = synth;
  bad.decay_ms = 20001.0f;
  refused(bad, "config.decay_ms must be finite and in (0, 20000]");
  bad = synth;
  bad.sustain = 0.0f;
  refused(bad, "config.sustain must be finite and in (0, 1]");
  bad = synth;
  bad.release_ms = -1.0f;
  refused(bad, "config.release_ms must be finite and in (0, 20000]");
  bad = synth;
  bad.polyphony = 0;
  refused(bad, "config.polyphony must be an integer in [1, 64]");
  bad.polyphony = 65;
  refused(bad, "config.polyphony must be an integer in [1, 64]");

  sonare_engine_destroy(engine);
}

TEST_CASE("engine SF2 player refuses each member by name", "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();
  SonareEngineSf2InstrumentConfig sf2{};
  sonare_engine_sf2_instrument_config_init(&sf2);
  CHECK(sf2.struct_size == sizeof(SonareEngineSf2InstrumentConfig));
  const sonare::midi::synth::Sf2PlayerConfig core_defaults{};
  CHECK(sf2.gain == core_defaults.gain);
  CHECK(sf2.polyphony == core_defaults.polyphony);
  CHECK(sf2.prefer_model_for_modeled_families == 0);
  CHECK(sf2.clear_bank_rig == 0);
  CHECK(sf2.gs_efx_realization == 0);
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 7, &sf2) == SONARE_OK);

  const auto refused = [&](const SonareEngineSf2InstrumentConfig& config, const std::string& text) {
    CHECK(sonare_engine_set_sf2_instrument(engine, 7, &config) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(last_error() == text);
  };
  SonareEngineSf2InstrumentConfig bad = sf2;
  bad.struct_size = static_cast<uint32_t>(sizeof(bad)) - 1;
  refused(bad, "config.struct_size must be set by sonare_engine_sf2_instrument_config_init");
  bad = sf2;
  bad.gain = -0.5f;
  refused(bad, "config.gain must be finite and in [0, 4]");
  bad.gain = 4.5f;
  refused(bad, "config.gain must be finite and in [0, 4]");
  bad = sf2;
  bad.polyphony = 0;
  refused(bad, "config.polyphony must be an integer in [1, 64]");
  bad = sf2;
  bad.prefer_model_for_modeled_families = -1;
  refused(bad, "config.prefer_model_for_modeled_families must be an integer in [0, 2147483647]");
  bad = sf2;
  bad.clear_bank_rig = -1;
  refused(bad, "config.clear_bank_rig must be an integer in [0, 2147483647]");
  bad = sf2;
  bad.gs_efx_realization = 2;
  refused(bad, "config.gs_efx_realization must be an integer in [0, 1]");

  // Zero gain is silence, not the default.
  sf2.gain = 0.0f;
  CHECK(sonare_engine_set_sf2_instrument(engine, 7, &sf2) == SONARE_OK);

  sonare_engine_destroy(engine);
}

TEST_CASE("engine live MIDI pushes record the shared argument rule's text",
          "[c_api][engine_refusal]") {
  SonareEngine* engine = prepared_engine();

  CHECK(sonare_engine_push_midi_note_on(engine, 7, 16, 0, 60, 100, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "group must be an integer in [0, 15]");
  CHECK(sonare_engine_push_midi_cc(engine, 7, 0, 16, 1, 1, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "channel must be an integer in [0, 15]");
  CHECK(sonare_engine_push_midi_pitch_bend(engine, 7, 0, 0, 16384, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "bend14 must be an integer in [0, 16383]");
  CHECK(sonare_engine_push_midi_input_note_on(engine, 0, 0, 128, 100, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "note must be an integer in [0, 127]");
  CHECK(sonare_engine_push_midi_program(engine, 7, 0, 0, 0, 0, 1, 0, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "bank_msb and bank_lsb must be 0 when bank_valid is false");

  CHECK(sonare_engine_push_midi_input_note_on(engine, 0, 0, 60, 100, 0) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(last_error() == "the MIDI input source is not enabled");

  const uint32_t word = 0;
  CHECK(sonare_engine_push_midi_ump(engine, 7, &word, 0, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "words do not form a pushable UMP message");

  sonare_engine_destroy(engine);
}

#endif  // defined(SONARE_WITH_ARRANGEMENT)
