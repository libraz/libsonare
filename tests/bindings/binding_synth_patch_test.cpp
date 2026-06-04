/// @file binding_synth_patch_test.cpp
/// @brief NativeSynth C ABI surface: the preset catalog
///        (sonare_synth_preset_names / sonare_synth_preset_patch), the
///        patch-driven bounce (sonare_project_bounce_with_synth_instruments)
///        and the realtime engine entry (sonare_engine_set_synth_instrument).

#include "binding_project_parity_test_helpers.h"

namespace {

/// A project with one MIDI track routed to destination @p dest playing a held
/// note (on at 0, off at 2 ppq).
SonareProject* make_synth_project(uint32_t dest, uint8_t note = 60) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20900040u | (static_cast<uint32_t>(note) << 8);  // note-on, vel 64
  events[0].data1 = 0u;
  events[1].ppq = 2.0;
  events[1].data0 = 0x20800000u | (static_cast<uint32_t>(note) << 8);  // note-off
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, dest) == SONARE_OK);
  return project;
}

std::vector<float> bounce_synth(SonareProject* project, const SonareSynthPatch& patch,
                                uint32_t dest = 3) {
  SonareProjectBounceOptions options{};
  options.total_frames = 24000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = dest;
  binding.patch = patch;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  return result;
}

float peak_of(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float s : samples) peak = std::max(peak, std::abs(s));
  return peak;
}

}  // namespace

TEST_CASE("synth preset catalog lists every §E entry", "[project][synth_patch]") {
  const char* names = sonare_synth_preset_names();
  REQUIRE(names != nullptr);
#if defined(SONARE_WITH_ARRANGEMENT)
  const std::string joined = std::string("\n") + names + "\n";
  for (const char* expected : {"sine", "saw-lead", "square-lead", "sub-bass", "warm-pad", "e-piano",
                               "bell", "brass", "pluck", "electric-guitar", "harp", "marimba",
                               "glass", "organ", "drum-kit", "acoustic-piano"}) {
    INFO(expected);
    REQUIRE(joined.find(std::string("\n") + expected + "\n") != std::string::npos);
  }
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("synth preset patches round-trip through the versioned struct",
          "[project][synth_patch]") {
  SonareSynthPatch patch{};
  // warm-pad: a 7-osc subtractive supersaw.
  REQUIRE(sonare_synth_preset_patch("warm-pad", &patch) == SONARE_OK);
  REQUIRE(patch.struct_version == 1);
  REQUIRE(std::string(patch.preset) == "warm-pad");
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_SUBTRACTIVE);
  REQUIRE(patch.waveform == SONARE_SYNTH_OSC_SAW);
  REQUIRE(patch.unison == 7);
  REQUIRE(patch.stereo_spread > 0.0f);
  // e-piano selects the FM engine; electric-guitar the KS engine.
  REQUIRE(sonare_synth_preset_patch("e-piano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_FM);
  REQUIRE(sonare_synth_preset_patch("electric-guitar", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_KARPLUS_STRONG);
  REQUIRE(sonare_synth_preset_patch("acoustic-piano", &patch) == SONARE_OK);
  REQUIRE(patch.engine_mode == SONARE_SYNTH_ENGINE_PIANO);
  // Unknown names and NULL args are rejected.
  REQUIRE(sonare_synth_preset_patch("minimoog", &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch(nullptr, &patch) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_synth_preset_patch("sine", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("bounce_with_synth_instruments renders preset patches deterministically",
          "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // Every catalog preset must produce audio through the bounce.
  const char* names = sonare_synth_preset_names();
  std::string catalog(names);
  size_t pos = 0;
  while (pos < catalog.size()) {
    size_t next = catalog.find('\n', pos);
    if (next == std::string::npos) next = catalog.size();
    const std::string name = catalog.substr(pos, next - pos);
    pos = next + 1;
    SonareSynthPatch patch{};
    REQUIRE(sonare_synth_preset_patch(name.c_str(), &patch) == SONARE_OK);
    INFO(name);
    REQUIRE(peak_of(bounce_synth(project, patch)) > 0.0f);
  }

  // Determinism: bit-identical renders for a fixed patch.
  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &patch) == SONARE_OK);
  REQUIRE(bounce_synth(project, patch) == bounce_synth(project, patch));

  sonare_project_destroy(project);
}

TEST_CASE("synth patch field overrides shape the preset", "[project][synth_patch]") {
  SonareProject* project = make_synth_project(3);

  // A zero-init patch (no preset) is the default subtractive patch.
  SonareSynthPatch init{};
  const std::vector<float> plain = bounce_synth(project, init);
  REQUIRE(peak_of(plain) > 0.0f);

  // Overriding the filter audibly changes the render.
  SonareSynthPatch dark = init;
  dark.cutoff_hz = 300.0f;
  dark.resonance_q = 4.0f;
  const std::vector<float> filtered = bounce_synth(project, dark);
  REQUIRE(filtered != plain);

  // The mod matrix table is applied (vibrato via LFO1 -> pitch).
  SonareSynthPatch wobble = init;
  wobble.num_mod_routings = 1;
  wobble.mod_routings[0] = {3 /*lfo1*/, 1 /*pitchCents*/, 80.0f};
  wobble.lfo_rate_hz = 6.0f;
  REQUIRE(bounce_synth(project, wobble) != plain);

  // Invalid patches are rejected.
  SonareSynthPatch bad_version{};
  bad_version.struct_version = 2;
  SonareProjectBounceOptions options{};
  options.total_frames = 1024;
  SonareSynthInstrumentBinding binding{};
  binding.destination_id = 3;
  binding.patch = bad_version;
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSynthPatch bad_name{};
  std::strcpy(bad_name.preset, "no-such-preset");
  binding.patch = bad_name;
  REQUIRE(sonare_project_bounce_with_synth_instruments(project, &options, &binding, 1, &out,
                                                       &out_len) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("the drum-kit preset plays the GM drum map on any key", "[project][synth_patch]") {
  // Note 38 = acoustic snare in the GM map.
  SonareProject* project = make_synth_project(3, 38);
  SonareSynthPatch kit{};
  REQUIRE(sonare_synth_preset_patch("drum-kit", &kit) == SONARE_OK);
  REQUIRE(kit.engine_mode == SONARE_SYNTH_ENGINE_PERCUSSION);
  const std::vector<float> snare = bounce_synth(project, kit);
  REQUIRE(peak_of(snare) > 0.0f);
  REQUIRE(bounce_synth(project, kit) == snare);
  sonare_project_destroy(project);
}

TEST_CASE("sonare_engine synth instrument renders live MIDI input", "[c_api][synth_patch]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  SonareSynthPatch patch{};
  REQUIRE(sonare_synth_preset_patch("saw-lead", &patch) == SONARE_OK);
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, &patch) == SONARE_OK);
  size_t count = 0;
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_push_midi_note_on(engine, 7, 0, 0, 60, 100, -1) == SONARE_OK);
  std::vector<float> left(128, 0.0f);
  std::vector<float> right(128, 0.0f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE(sonare_engine_process(engine, channels, 2, 128) == SONARE_OK);
  float peak = 0.0f;
  for (float s : left) peak = std::max(peak, std::abs(s));
  for (float s : right) peak = std::max(peak, std::abs(s));
  REQUIRE(peak > 0.0f);

  // Invalid patches are rejected without disturbing the binding.
  SonareSynthPatch bad{};
  bad.struct_version = 9;
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_synth_instrument(engine, 7, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  sonare_engine_destroy(engine);
}

#endif  // SONARE_WITH_ARRANGEMENT
