/// @file binding_project_soundfont_test.cpp
/// @brief SoundFont C ABI tests: sonare_project_load_soundfont / preset count /
///        bounce manifest / bounce_with_sf2_instruments, and the realtime
///        engine SF2 entry (sonare_engine_load_soundfont +
///        sonare_engine_set_sf2_instrument with live MIDI input).

#include "binding_project_parity_test_helpers.h"
#include "support/sf2_builder.h"

namespace {

// In-memory SF2 fixture: a looped sine preset at (0, 0), a second melodic
// preset at (0, 1) and a bank-128 drum kit at (128, 0). Program (0, 2) is
// deliberately NOT covered so manifest fallback reporting is observable.
std::vector<uint8_t> make_sf2_bytes() {
  sonare::test::Sf2Builder b;

  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] =
        0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  sonare::test::Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = sine_id;
  const int melodic = b.add_instrument("melodic", {looped});

  sonare::test::Sf2Builder::ZoneSpec pz0;
  pz0.target = melodic;
  b.add_preset("Piano 1", 0, 0, {pz0});

  sonare::test::Sf2Builder::ZoneSpec pz1;
  pz1.target = melodic;
  b.add_preset("Piano 2", 0, 1, {pz1});

  sonare::test::Sf2Builder::ZoneSpec dz;
  dz.target = melodic;
  b.add_preset("Standard Kit", 128, 0, {dz});

  return b.build();
}

// MIDI 1.0 channel-voice UMP word (MT=2, group 0).
constexpr uint32_t midi1_word(uint8_t status, uint8_t channel, uint8_t data1, uint8_t data2) {
  return (0x2u << 28) | (static_cast<uint32_t>(status & 0x0Fu) << 20) |
         (static_cast<uint32_t>(channel & 0x0Fu) << 16) |
         (static_cast<uint32_t>(data1 & 0x7Fu) << 8) | static_cast<uint32_t>(data2 & 0x7Fu);
}

SonareMidiEventPod pod(double ppq, uint32_t word) {
  SonareMidiEventPod e{};
  e.ppq = ppq;
  e.data0 = word;
  e.data1 = 0;
  return e;
}

}  // namespace

TEST_CASE("sonare_project_load_soundfont parses, replaces and clears", "[project][sf2]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  size_t count = 999;
  REQUIRE(sonare_project_soundfont_preset_count(project, &count) == SONARE_OK);
  REQUIRE(count == 0);

  // Invalid arguments / malformed bytes.
  const std::vector<uint8_t> sf2 = make_sf2_bytes();
  REQUIRE(sonare_project_load_soundfont(nullptr, sf2.data(), sf2.size()) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_load_soundfont(project, nullptr, 16) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_load_soundfont(project, sf2.data(), 0) == SONARE_ERROR_INVALID_PARAMETER);
  const uint8_t garbage[8] = {'n', 'o', 't', ' ', 'a', 's', 'f', '2'};
  REQUIRE(sonare_project_load_soundfont(project, garbage, sizeof(garbage)) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_error_message() != nullptr);
  REQUIRE(std::strlen(sonare_last_error_message()) > 0);
  // A failed load leaves the previous state untouched (still no soundfont).
  REQUIRE(sonare_project_soundfont_preset_count(project, &count) == SONARE_OK);
  REQUIRE(count == 0);

  REQUIRE(sonare_project_load_soundfont(project, sf2.data(), sf2.size()) == SONARE_OK);
  REQUIRE(sonare_project_soundfont_preset_count(project, &count) == SONARE_OK);
  REQUIRE(count == 3);

  // A malformed re-load keeps the loaded soundfont.
  REQUIRE(sonare_project_load_soundfont(project, garbage, sizeof(garbage)) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_soundfont_preset_count(project, &count) == SONARE_OK);
  REQUIRE(count == 3);

  REQUIRE(sonare_project_clear_soundfont(project) == SONARE_OK);
  REQUIRE(sonare_project_soundfont_preset_count(project, &count) == SONARE_OK);
  REQUIRE(count == 0);

  sonare_project_destroy(project);
}

TEST_CASE("sonare_project_soundfont_manifest reports per-program backends", "[project][sf2]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);

  // Channel 0: default program 0 note, then program 2 (uncovered) note, then
  // a variation-bank (CC0=8) program 1 note (falls back to capital tone 1).
  // Channel 9: a drum note (bank 128).
  std::vector<SonareMidiEventPod> events;
  events.push_back(pod(0.0, midi1_word(0x9, 0, 60, 64)));
  events.push_back(pod(0.5, midi1_word(0x8, 0, 60, 0)));
  events.push_back(pod(1.0, midi1_word(0xC, 0, 2, 0)));  // program change -> 2
  events.push_back(pod(1.5, midi1_word(0x9, 0, 62, 64)));
  events.push_back(pod(2.0, midi1_word(0x8, 0, 62, 0)));
  events.push_back(pod(2.5, midi1_word(0xB, 0, 0, 8)));  // CC0 bank MSB = 8
  events.push_back(pod(2.6, midi1_word(0xC, 0, 1, 0)));  // program change -> 1
  events.push_back(pod(3.0, midi1_word(0x9, 0, 64, 64)));
  events.push_back(pod(3.5, midi1_word(0x8, 0, 64, 0)));
  events.push_back(pod(4.0, midi1_word(0x9, 9, 36, 100)));
  events.push_back(pod(4.5, midi1_word(0x8, 9, 36, 0)));
  REQUIRE(sonare_project_set_midi_events(project, clip, events.data(), events.size()) == SONARE_OK);

  // Without a soundfont everything is a synth fallback.
  size_t total = 0;
  REQUIRE(sonare_project_soundfont_manifest(project, nullptr, 0, &total) == SONARE_OK);
  REQUIRE(total == 4);

  const std::vector<uint8_t> sf2 = make_sf2_bytes();
  REQUIRE(sonare_project_load_soundfont(project, sf2.data(), sf2.size()) == SONARE_OK);

  std::vector<SonareSf2ProgramStatus> manifest(8);
  REQUIRE(sonare_project_soundfont_manifest(project, manifest.data(), manifest.size(), &total) ==
          SONARE_OK);
  REQUIRE(total == 4);
  manifest.resize(total);

  // First-use order: (ch0 bank0 prog0), (ch0 bank0 prog2), (ch0 bank8 prog1),
  // (ch9 bank128 prog0).
  REQUIRE(manifest[0].channel == 0);
  REQUIRE(manifest[0].bank == 0);
  REQUIRE(manifest[0].program == 0);
  REQUIRE(manifest[0].backend == SONARE_SOURCE_BACKEND_SF2);
  REQUIRE(std::string(manifest[0].preset_name) == "Piano 1");

  REQUIRE(manifest[1].channel == 0);
  REQUIRE(manifest[1].bank == 0);
  REQUIRE(manifest[1].program == 2);
  REQUIRE(manifest[1].backend == SONARE_SOURCE_BACKEND_SYNTH);
  REQUIRE(std::string(manifest[1].preset_name).empty());

  // Variation bank 8 is not in the soundfont: GS falls back to bank 0.
  REQUIRE(manifest[2].channel == 0);
  REQUIRE(manifest[2].bank == 8);
  REQUIRE(manifest[2].program == 1);
  REQUIRE(manifest[2].backend == SONARE_SOURCE_BACKEND_SF2);
  REQUIRE(std::string(manifest[2].preset_name) == "Piano 2");

  REQUIRE(manifest[3].channel == 9);
  REQUIRE(manifest[3].bank == 128);
  REQUIRE(manifest[3].program == 0);
  REQUIRE(manifest[3].backend == SONARE_SOURCE_BACKEND_SF2);
  REQUIRE(std::string(manifest[3].preset_name) == "Standard Kit");

  // Truncated write still reports the total.
  SonareSf2ProgramStatus first{};
  REQUIRE(sonare_project_soundfont_manifest(project, &first, 1, &total) == SONARE_OK);
  REQUIRE(total == 4);
  REQUIRE(first.bank == 0);
  REQUIRE(first.backend == SONARE_SOURCE_BACKEND_SF2);

  sonare_project_destroy(project);
}

TEST_CASE("bounce_with_sf2_instruments renders the loaded SoundFont", "[project][sf2]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[2] = {pod(0.0, midi1_word(0x9, 0, 60, 100)),
                                  pod(2.0, midi1_word(0x8, 0, 60, 0))};
  REQUIRE(sonare_project_set_midi_events(project, clip, events, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, 5) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = 12000;
  options.block_size = 128;
  options.num_channels = 2;
  options.sample_rate = 48000;

  SonareSf2InstrumentBinding binding{};
  binding.destination_id = 5;

  // Binding an SF2 instrument without a loaded soundfont is an invalid state.
  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out,
                                                     &out_len) == SONARE_ERROR_INVALID_STATE);

  const std::vector<uint8_t> sf2 = make_sf2_bytes();
  REQUIRE(sonare_project_load_soundfont(project, sf2.data(), sf2.size()) == SONARE_OK);

  // An unknown struct version is rejected.
  binding.config.struct_version = 99;
  REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out,
                                                     &out_len) == SONARE_ERROR_INVALID_PARAMETER);
  binding.config.struct_version = 0;

  REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out,
                                                     &out_len) == SONARE_OK);
  REQUIRE(out != nullptr);
  REQUIRE(out_len == static_cast<size_t>(options.total_frames) * 2);
  float peak = 0.0f;
  for (size_t i = 0; i < out_len; ++i) peak = std::max(peak, std::abs(out[i]));
  REQUIRE(peak > 0.01f);

  // Deterministic: a second bounce is bit-identical.
  float* out2 = nullptr;
  size_t out2_len = 0;
  REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, &binding, 1, &out2,
                                                     &out2_len) == SONARE_OK);
  REQUIRE(out2_len == out_len);
  REQUIRE(std::memcmp(out, out2, out_len * sizeof(float)) == 0);
  sonare_free_floats(out2);

  // Zero bindings bounce silently (same contract as the built-in synth path).
  float* silent = nullptr;
  size_t silent_len = 0;
  REQUIRE(sonare_project_bounce_with_sf2_instruments(project, &options, nullptr, 0, &silent,
                                                     &silent_len) == SONARE_OK);
  float silent_peak = 0.0f;
  for (size_t i = 0; i < silent_len; ++i) silent_peak = std::max(silent_peak, std::abs(silent[i]));
  REQUIRE(silent_peak == 0.0f);
  sonare_free_floats(silent);
  sonare_free_floats(out);

  sonare_project_destroy(project);
}

TEST_CASE("sonare_engine SF2 instrument renders live MIDI input", "[c_api][sf2]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000.0, 128, 16, 16) == SONARE_OK);

  const std::vector<uint8_t> sf2 = make_sf2_bytes();
  SonareEngineSf2InstrumentConfig config{};

#if defined(SONARE_WITH_ARRANGEMENT)
  // Binding before a soundfont is loaded is an invalid state.
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 7, &config) == SONARE_ERROR_INVALID_STATE);

  const uint8_t garbage[4] = {'b', 'a', 'd', '!'};
  REQUIRE(sonare_engine_load_soundfont(engine, garbage, sizeof(garbage)) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_load_soundfont(engine, sf2.data(), sf2.size()) == SONARE_OK);

  config.struct_version = 99;
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 7, &config) == SONARE_ERROR_INVALID_PARAMETER);
  config.struct_version = 0;
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 7, &config) == SONARE_OK);
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

  // Rebinding the same destination with the built-in synth replaces the player.
  SonareEngineBuiltinSynthConfig synth{};
  REQUIRE(sonare_engine_set_builtin_instrument(engine, 7, &synth) == SONARE_OK);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 1);

  REQUIRE(sonare_engine_clear_midi_instrument(engine, 7) == SONARE_OK);
  REQUIRE(sonare_engine_midi_instrument_count(engine, &count) == SONARE_OK);
  REQUIRE(count == 0);
#else
  REQUIRE(sonare_engine_load_soundfont(engine, sf2.data(), sf2.size()) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_engine_set_sf2_instrument(engine, 7, &config) == SONARE_ERROR_NOT_SUPPORTED);
#endif

  sonare_engine_destroy(engine);
}
