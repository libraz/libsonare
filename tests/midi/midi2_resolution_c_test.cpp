/// @file midi2_resolution_c_test.cpp
/// @brief A MIDI 2.0 velocity between two 7-bit steps must land between their onsets in the
///        reed, brass, flute, free reed and vocal engines.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/control_value.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"

namespace {

using sonare::midi::Velocity16;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

constexpr double kSr = 48000.0;
constexpr int kWindow = static_cast<int>(0.05 * kSr);
constexpr uint8_t kStep = 64;
constexpr uint8_t kNote = 60;
// The reed patch's onset peak falls with velocity at middle C, so it is measured an octave up.
constexpr uint8_t kReedNote = 72;

const NativeSynthPatch& melodic_patch(SynthEngineMode mode) {
  for (uint8_t program = 0; program < 128; ++program) {
    const NativeSynthPatch& p = gm_fallback_patch(0, program);
    if (p.mode == mode) return p;
  }
  FAIL("no GM program uses the requested engine");
  return gm_fallback_patch(0, 0);
}

/// Absolute peak of the first 50 ms of a started core.
template <class Core>
float window_peak(Core& core) {
  float peak = 0.0f;
  for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
  return peak;
}

/// `render` maps a Velocity16 to the first-50 ms peak; the midpoint must land strictly between.
template <class Render>
void require_strictly_between(Render render) {
  const Velocity16 lo = Velocity16::from7(kStep);
  const Velocity16 hi = Velocity16::from7(kStep + 1);
  const Velocity16 mid = Velocity16::from_raw(static_cast<uint16_t>((lo.raw + hi.raw) / 2));
  const float p_lo = render(lo);
  const float p_mid = render(mid);
  const float p_hi = render(hi);
  INFO("peak lo=" << p_lo << " mid=" << p_mid << " hi=" << p_hi);
  REQUIRE(p_lo > 0.0f);
  REQUIRE(p_lo < p_mid);
  REQUIRE(p_mid < p_hi);
}

}  // namespace

TEST_CASE("reed velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kReed).reed;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::reed_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::ReedVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::reed_buffer_capacity(kSr));
    core.start(params, kSr, kReedNote, v, 0x4001u);
    return window_peak(core);
  });
}

TEST_CASE("brass velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kBrass).brass;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::brass_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::BrassVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::brass_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x4002u);
    return window_peak(core);
  });
}

TEST_CASE("flute velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kFlute).flute;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::flute_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::FluteVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::flute_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x4003u);
    return window_peak(core);
  });
}

TEST_CASE("free reed velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kFreeReed).free_reed;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::FreeReedVoiceCore core;
    core.start(params, kSr, kNote, v, 0x4004u);
    return window_peak(core);
  });
}

TEST_CASE("vocal velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kVocal).vocal;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::VocalVoiceCore core;
    core.start(params, kSr, kNote, v, 0x4005u);
    return window_peak(core);
  });
}
