#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "midi/synth/envelope.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gm_fallback_tail_bounds.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/piano_resonance.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#include "util/constants.h"
#include "util/numeric_validation.h"

namespace {

using sonare::constants::kDefaultDawSampleRate;
using sonare::midi::synth::DahdsrEnvelope;
using sonare::midi::synth::EnvelopeTimeScales;
using sonare::midi::synth::GsToneMap;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

std::shared_ptr<Sf2File> make_release_fixture() {
  Sf2Builder builder;
  std::vector<float> samples(96, 0.25f);
  const int sample = builder.add_sample("loop", samples, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec instrument_global;
  instrument_global.gens.push_back({sonare::midi::synth::kGenReleaseVolEnv, 0});
  Sf2Builder::ZoneSpec instrument_zone;
  instrument_zone.gens.push_back({sonare::midi::synth::kGenSampleModes, 1});
  instrument_zone.target = sample;
  const int instrument =
      builder.add_instrument("release-instrument", {instrument_global, instrument_zone});

  Sf2Builder::ZoneSpec short_preset_zone;
  short_preset_zone.target = instrument;
  builder.add_preset("short-release", 0, 0, {short_preset_zone});

  Sf2Builder::ZoneSpec long_preset_zone;
  long_preset_zone.gens.push_back({sonare::midi::synth::kGenReleaseVolEnv, 1200});
  long_preset_zone.target = instrument;
  builder.add_preset("long-release", 0, 1, {long_preset_zone});

  const std::vector<uint8_t> bytes = builder.build();
  auto soundfont = std::make_shared<Sf2File>();
  REQUIRE(soundfont->parse(bytes.data(), bytes.size(), nullptr));
  return soundfont;
}

Sf2Player make_player(std::shared_ptr<Sf2File> soundfont = nullptr, bool synth_fallback = true) {
  Sf2PlayerConfig config;
  config.gain = 1.0f;
  config.synth_fallback = synth_fallback;
  config.dc_block = false;
#if defined(SONARE_MIDI_WITH_FX)
  config.effects.enable_reverb = false;
  config.effects.enable_chorus = false;
  config.effects.enable_delay = false;
#endif
  Sf2Player player(config);
  if (soundfont != nullptr) player.set_soundfont(std::move(soundfont));
  player.prepare(kDefaultDawSampleRate, 256);
  return player;
}

template <typename Instrument>
void send(Instrument& instrument, const sonare::midi::Ump& message) {
  instrument.on_event(0, sonare::test::event(message));
  REQUIRE(instrument.materialize_tail_probe());
}

}  // namespace

TEST_CASE("offline tails include only sounded fallback programs and reset cleanly",
          "[midi][offline_tail]") {
  const GsToneMap map = sonare::midi::synth::gs_effective_tone_map(0, 0);
  const int64_t bass_tail = sonare::midi::synth::gm_fallback_program_tail_samples(
      sonare::midi::synth::gm_fallback_patch(0, 33, map), kDefaultDawSampleRate,
      EnvelopeTimeScales{});

  Sf2Player player = make_player();
  const int live_tail = player.tail_samples();
  send(player, sonare::midi::make_midi1_program_change(0, 0, 33));
  send(player, sonare::midi::make_midi1_note_on(0, 0, 45, 100));
  CHECK(player.offline_tail_samples() == bass_tail);
  CHECK(player.tail_samples() == live_tail);

  send(player, sonare::midi::make_midi1_program_change(0, 0, 0));
  CHECK(player.offline_tail_samples() == bass_tail);

  player.reset();
  CHECK(player.offline_tail_samples() == 0);

  const int64_t piano_tail = sonare::midi::synth::gm_fallback_program_tail_samples(
      sonare::midi::synth::gm_fallback_patch(0, 0, map), kDefaultDawSampleRate,
      EnvelopeTimeScales{});
  const int64_t piano_body = sonare::numeric::ceil_sample_count(
      sonare::midi::synth::kPianoBodyRingS * kDefaultDawSampleRate);
  send(player, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  CHECK(player.offline_tail_samples() == piano_tail + piano_body);
  CHECK(player.tail_samples() == live_tail);
}

TEST_CASE("offline tails use the resolved fallback drum kit", "[midi][offline_tail]") {
  const GsToneMap map = sonare::midi::synth::gs_effective_tone_map(0, 0);
  const uint8_t standard = sonare::midi::synth::gm_fallback_drum_kit(0, map);
  const uint8_t tr808 = sonare::midi::synth::gm_fallback_drum_kit(25, map);
  const auto& kick = sonare::midi::synth::gm_fallback_drum_patch(36);
  const int64_t standard_tail = sonare::midi::synth::gm_fallback_drum_note_tail_samples(
      kick, standard, 36, kDefaultDawSampleRate, EnvelopeTimeScales{});
  const int64_t tr808_tail = sonare::midi::synth::gm_fallback_drum_note_tail_samples(
      kick, tr808, 36, kDefaultDawSampleRate, EnvelopeTimeScales{});
  REQUIRE(tr808_tail > standard_tail);

  Sf2Player player = make_player();
  send(player, sonare::midi::make_midi1_note_on(0, 9, 36, 100));
  CHECK(player.offline_tail_samples() == standard_tail);

  player.reset();
  send(player, sonare::midi::make_midi1_program_change(0, 9, 25));
  send(player, sonare::midi::make_midi1_note_on(0, 9, 36, 100));
  CHECK(player.offline_tail_samples() == tr808_tail);
}

TEST_CASE("offline tails follow sampled zone releases", "[midi][offline_tail]") {
  Sf2Player player = make_player(make_release_fixture(), false);
  const int64_t short_tail = DahdsrEnvelope::release_tail_samples(kDefaultDawSampleRate, 1000.0f);
  const int64_t long_tail = DahdsrEnvelope::release_tail_samples(kDefaultDawSampleRate, 2000.0f);

  send(player, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  CHECK(player.offline_tail_samples() == short_tail);

  send(player, sonare::midi::make_midi1_program_change(0, 0, 1));
  send(player, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  CHECK(player.offline_tail_samples() == long_tail);
  CHECK(player.tail_samples() == long_tail);
}

TEST_CASE("offline tails inherit NativeSynth program bounds", "[midi][offline_tail]") {
  NativeSynthConfig config;
  config.use_gm_programs = true;
  config.dc_block = false;
  NativeSynth synth(config);
  synth.prepare(kDefaultDawSampleRate, 256);

  send(synth, sonare::midi::make_midi1_program_change(0, 0, 33));
  send(synth, sonare::midi::make_midi1_note_on(0, 0, 45, 100));
  const int bass_tail = synth.tail_samples();
  CHECK(synth.offline_tail_samples() == bass_tail);

  synth.reset();
  send(synth, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  const int piano_tail = synth.tail_samples();
  CHECK(synth.offline_tail_samples() == piano_tail);
  CHECK(piano_tail > bass_tail);
}
