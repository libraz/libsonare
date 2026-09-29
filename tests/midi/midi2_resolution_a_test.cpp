/// @file midi2_resolution_a_test.cpp
/// @brief A MIDI 2.0 velocity between two 7-bit steps must land between their onsets in the
///        FM, Karplus-Strong, modal, additive and percussion engines.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/control_value.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"

namespace {

using sonare::midi::Velocity16;
using sonare::midi::synth::gm_fallback_drum_patch;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

constexpr double kSr = 48000.0;
constexpr int kWindow = static_cast<int>(0.05 * kSr);
constexpr uint8_t kStep = 64;

const NativeSynthPatch& melodic_patch(SynthEngineMode mode) {
  for (uint8_t program = 0; program < 128; ++program) {
    const NativeSynthPatch& p = gm_fallback_patch(0, program);
    if (p.mode == mode) return p;
  }
  FAIL("no GM program uses the requested engine");
  return gm_fallback_patch(0, 0);
}

/// Absolute peak of the first 50 ms; `Render` is `float(Velocity16)` returning the window peak.
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

TEST_CASE("fm velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  // A lone carrier isolates the velocity->level path; with a modulator the onset peak is not
  // monotonic in velocity because brightness moves with it.
  sonare::midi::synth::FmPatchParams params;
  params.algorithm = sonare::midi::synth::FmAlgorithm::kAdd2;
  params.ops[0].level = 0.5f;
  params.ops[0].vel_to_level = 1.0f;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::FmVoiceCore core;
    core.start(params, kSr, 60, v);
    float peak = 0.0f;
    for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
    return peak;
  });
}

TEST_CASE("karplus-strong velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kKarplusStrong).ks;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::ks_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::KsVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::ks_buffer_capacity(kSr));
    core.start(params, kSr, 60, v, 0x2001u);
    float peak = 0.0f;
    for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
    return peak;
  });
}

TEST_CASE("modal velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kModal).modal;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::ModalVoiceCore core;
    core.start(params, kSr, 60, v, 0x2002u);
    float peak = 0.0f;
    for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
    return peak;
  });
}

TEST_CASE("additive velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kAdditive).additive;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::AdditiveVoiceCore core;
    core.start(params, kSr, 60, v, 0x2003u);
    float peak = 0.0f;
    for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
    return peak;
  });
}

TEST_CASE("percussion velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = gm_fallback_drum_patch(38).percussion;
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::PercussionVoiceCore core;
    core.start(params, kSr, 38, v, 0x2004u);
    float peak = 0.0f;
    for (int i = 0; i < kWindow; ++i) peak = std::max(peak, std::fabs(core.render(1.0f)));
    return peak;
  });
}
