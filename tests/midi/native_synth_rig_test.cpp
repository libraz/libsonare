/// @file native_synth_rig_test.cpp
/// @brief NativeSynth's default rig under GM program resolution: the electric
///        guitars come out amplified once a host wires an insert factory, a
///        program the bank binds nothing to renders exactly as it did without
///        one, and the result agrees with the Sf2Player model floor, which
///        runs the same voice through the same rig.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(SONARE_WITH_MASTERING)

#include "core/fft.h"
#include "mastering/api/insert_factory.h"
#include "midi/instrument.h"
#include "midi/midi_event.h"
#include "midi/part_rig.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::GsEfxStageFactory;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::event;
using sonare::test::StereoRender;

constexpr double kRate = 48000.0;
constexpr int kFrames = 48000;
constexpr uint8_t kNote = 52;
constexpr uint8_t kVelocity = 110;

GsEfxStageFactory factory() {
  return [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
}

/// Offline GM playback with the factory wired, as a project bounce runs it.
NativeSynthConfig native_config(bool bound) {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.use_gm_programs = true;
  cfg.insert_factory = factory();
  cfg.bank_rig_binding = bound;
  cfg.realize_efx_inline = true;
  return cfg;
}

Sf2PlayerConfig sf2_config() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = factory();
  cfg.realize_efx_inline = true;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  return cfg;
}

template <typename Player>
StereoRender play(Player& player, uint8_t program) {
  player.prepare(kRate, 256);
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  return sonare::test::render_stereo(player, kFrames);
}

StereoRender render_native(const NativeSynthConfig& cfg, uint8_t program) {
  NativeSynth synth(cfg);
  return play(synth, program);
}

StereoRender render_sf2(uint8_t program) {
  Sf2Player player(sf2_config());
  return play(player, program);
}

/// Energy above @p split_hz as a share of the whole, in dB, over the settled
/// part of the render (0.1 s on), on the mid signal.
double share_above_db(const StereoRender& r, double split_hz = 2000.0) {
  constexpr int kFft = 4096;
  sonare::FFT fft(kFft);
  fft.prepare(true, false, false);
  std::vector<float> frame(kFft);
  std::vector<std::complex<float>> spectrum(kFft / 2 + 1);
  const size_t split_bin = static_cast<size_t>(split_hz * kFft / kRate);
  double above = 0.0;
  double total = 0.0;
  for (size_t start = static_cast<size_t>(kRate / 10); start + kFft <= r.left.size();
       start += kFft) {
    for (int i = 0; i < kFft; ++i) {
      const double w = 0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * i / kFft);
      const size_t at = start + static_cast<size_t>(i);
      frame[static_cast<size_t>(i)] = static_cast<float>(w * 0.5 * (r.left[at] + r.right[at]));
    }
    fft.forward(frame.data(), spectrum.data());
    for (size_t bin = 1; bin < spectrum.size(); ++bin) {
      const double power = std::norm(spectrum[bin]);
      total += power;
      if (bin >= split_bin) above += power;
    }
  }
  REQUIRE(total > 0.0);
  return 10.0 * std::log10(std::max(above, 1.0e-30) / total);
}

double rms_db(const StereoRender& r) {
  double acc = 0.0;
  for (size_t i = 0; i < r.left.size(); ++i) {
    acc +=
        static_cast<double>(r.left[i]) * r.left[i] + static_cast<double>(r.right[i]) * r.right[i];
  }
  return 10.0 * std::log10(acc / static_cast<double>(2 * r.left.size()));
}

}  // namespace

TEST_CASE("native: the bank rig amplifies programs 29 and 30 once a factory is wired",
          "[midi][native][rig]") {
  const StereoRender crunch = render_native(native_config(true), 29);
  const StereoRender lead = render_native(native_config(true), 30);
  const StereoRender crunch_di = render_native(native_config(false), 29);
  const StereoRender lead_di = render_native(native_config(false), 30);
  // Both programs voice the same DI; the rig is what tells them apart.
  REQUIRE(crunch_di.left == lead_di.left);
  REQUIRE(crunch.left != lead.left);
  const double crunch_gain = share_above_db(crunch) - share_above_db(crunch_di);
  const double lead_gain = share_above_db(lead) - share_above_db(lead_di);
  INFO("crunch +" << crunch_gain << " dB, lead +" << lead_gain << " dB above 2 kHz");
  REQUIRE(crunch_gain >= 15.0);
  REQUIRE(lead_gain >= 15.0);
}

TEST_CASE("native: a program the bank binds nothing to is untouched by the factory",
          "[midi][native][rig]") {
  NativeSynthConfig bare = native_config(true);
  bare.insert_factory = nullptr;
  const StereoRender without = render_native(bare, 0);
  const StereoRender bound = render_native(native_config(true), 0);
  const StereoRender unbound = render_native(native_config(false), 0);
  REQUIRE(bound.left == unbound.left);
  REQUIRE(bound.right == unbound.right);
  REQUIRE(bound.left == without.left);
  REQUIRE(bound.right == without.right);
}

TEST_CASE("native: the rig, the voice and the halo match the Sf2Player model floor",
          "[midi][native][rig]") {
  for (const uint8_t program : {uint8_t{29}, uint8_t{30}, uint8_t{31}}) {
    CAPTURE(program);
    const StereoRender native = render_native(native_config(true), program);
    const StereoRender sf2 = render_sf2(program);
    const double native_share = share_above_db(native);
    const double sf2_share = share_above_db(sf2);
    INFO("rms native " << rms_db(native) << " sf2 " << rms_db(sf2));
    INFO("share native " << native_share << " sf2 " << sf2_share);
    REQUIRE(std::abs(rms_db(native) - rms_db(sf2)) <= 0.5);
    REQUIRE(std::abs(native_share - sf2_share) <= 1.0);
  }
}

TEST_CASE("native: a rigged part's lanes sum to process()", "[midi][native][rig]") {
  constexpr int kSamples = 1000;
  constexpr uint32_t kTrack = 7;
  NativeSynth reference(native_config(true));
  NativeSynth laned(native_config(true));
  reference.prepare(kRate, 256);
  laned.prepare(kRate, 256);
  for (NativeSynth* synth : {&reference, &laned}) {
    synth->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 29)));
    synth->on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, 0)));
  }
  reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity)));
  reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 100)));
  const StereoRender ref = sonare::test::render_stereo(reference, kSamples);

  MidiEvent guitar = event(sonare::midi::make_midi1_note_on(0, 0, kNote, kVelocity));
  guitar.source_track_id = kTrack;
  laned.on_event(0, guitar);
  laned.on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 60, 100)));
  std::vector<float> fallback_l(kSamples, 0.0f);
  std::vector<float> fallback_r(kSamples, 0.0f);
  std::vector<float> lane_l(kSamples, 0.0f);
  std::vector<float> lane_r(kSamples, 0.0f);
  float* fallback[] = {fallback_l.data(), fallback_r.data()};
  float* lane[] = {lane_l.data(), lane_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {kTrack, lane}};
  REQUIRE(laned.process_source_tracks(outputs, 2, 2, kSamples));

  float peak = 0.0f;
  float lane_peak = 0.0f;
  float max_diff = 0.0f;
  for (size_t i = 0; i < static_cast<size_t>(kSamples); ++i) {
    peak = std::max({peak, std::fabs(ref.left[i]), std::fabs(ref.right[i])});
    lane_peak = std::max(lane_peak, std::fabs(lane_l[i]));
    max_diff = std::max(max_diff, std::fabs(fallback_l[i] + lane_l[i] - ref.left[i]));
    max_diff = std::max(max_diff, std::fabs(fallback_r[i] + lane_r[i] - ref.right[i]));
  }
  REQUIRE(peak > 0.0f);
  REQUIRE(lane_peak > 0.0f);
  INFO("max_diff/peak: " << (max_diff / peak));
  REQUIRE(max_diff <= 1.0e-6f * peak);
}

TEST_CASE("native: a rig's tail extends the instrument's", "[midi][native][rig]") {
  NativeSynth plain(native_config(true));
  plain.prepare(kRate, 256);
  NativeSynth delayed(native_config(true));
  sonare::midi::PartRig rig;
  rig.mode = sonare::midi::PartRigMode::kChain;
  rig.stages = {{"effects.delay.stereo", "{}"}};
  REQUIRE(delayed.set_part_rig(0, rig));
  delayed.prepare(kRate, 256);
  REQUIRE(delayed.tail_samples() > plain.tail_samples());
}

#endif  // SONARE_WITH_MASTERING
