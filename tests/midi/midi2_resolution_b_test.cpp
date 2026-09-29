/// @file midi2_resolution_b_test.cpp
/// @brief A MIDI 2.0 velocity between two 7-bit steps must land between their onsets in the
///        piano, pipe organ, bowed string, harpsichord and plucked string engines.

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

TEST_CASE("piano velocity between two 7-bit steps lands between their peaks", "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kPiano).piano;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::piano_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::PianoVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::piano_string_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x3001u);
    return window_peak(core);
  });
}

TEST_CASE("pipe organ velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kPipeOrgan).pipe_organ;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::pipe_organ_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::PipeOrganVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::pipe_organ_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x3002u);
    return window_peak(core);
  });
}

TEST_CASE("bowed string velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kBowedString).bowed_string;
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::bowed_string_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::BowedStringVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::bowed_string_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x3003u);
    return window_peak(core);
  });
}

TEST_CASE("harpsichord velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kHarpsichord).harpsichord;
  std::vector<float> slab(static_cast<size_t>(sonare::midi::synth::harpsichord_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::HarpsichordVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::harpsichord_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x3004u);
    return window_peak(core);
  });
}

TEST_CASE("plucked string velocity between two 7-bit steps lands between their peaks",
          "[midi][midi2]") {
  const auto& params = melodic_patch(SynthEngineMode::kPluckedString).plucked_string;
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::plucked_string_slab_capacity(kSr)));
  require_strictly_between([&](Velocity16 v) {
    sonare::midi::synth::PluckedStringVoiceCore core;
    core.attach(slab.data(), sonare::midi::synth::plucked_string_buffer_capacity(kSr));
    core.start(params, kSr, kNote, v, 0x3005u);
    return window_peak(core);
  });
}
