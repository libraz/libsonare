/// @file singing_passivity_test.cpp
/// @brief The singing assembly (SingingPhonation: fold, implicit junction, tract,
///        lips) at the engine's own operating point: it phonates on the breath,
///        loses energy once the breath is cut -- through a moving vowel and a tap
///        closure as well -- and its junction converges on every sample.
///
/// Passivity alone is not evidence the assembly works, since a silent one passes
/// it trivially; each case first shows the fold oscillating, as valve_coupling_test
/// does for the bare valve.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/synth/singing_voice.h"
#include "singing_measure.h"

namespace {

using sonare::midi::synth::SingingPatchParams;
using sonare::midi::synth::SingingPhonation;
using sonare::test::singing::kRate;
using sonare::test::singing::span_rms;

constexpr float kNoteHz = 220.0f;
constexpr float kForce = 0.6f;

/// 512-sample block means never rise and no sample exceeds the start by more than 1e-6.
void require_passive(const std::vector<double>& energy) {
  const double start = energy.front();
  REQUIRE(start > 0.0);
  double previous = start * 2.0;
  for (std::size_t b = 0; (b + 1) * 512 <= energy.size(); ++b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
      const double e = energy[b * 512 + i];
      REQUIRE(e <= start * (1.0 + 1.0e-6));
      sum += e;
    }
    const double mean = sum / 512.0;
    INFO("block " << b << " mean " << mean << " previous " << previous);
    REQUIRE(mean <= previous);
    previous = mean;
  }
}

/// Phonates for half a second at the defaults and requires the oscillation.
void phonate(SingingPhonation& voice) {
  voice.configure_patch(kRate, kNoteHz, SingingPatchParams{});
  std::vector<float> out;
  for (int n = 0; n < kRate / 2; ++n) out.push_back(voice.process(kForce, 0.0f, 0.55f, 0.0f));
  REQUIRE(span_rms(out, out.size() - 4096, out.size()) > 2.0e-4);
}

}  // namespace

TEST_CASE("singing assembly loses energy once the breath is cut", "[midi][synth][singing]") {
  constexpr int kDecay = 9600;
  SECTION("held vowel") {
    SingingPhonation voice;
    phonate(voice);
    std::vector<double> energy;
    for (int n = 0; n < kDecay; ++n) {
      energy.push_back(voice.stored_energy());
      voice.process(0.0f, 0.0f, 0.55f, 0.0f);
    }
    require_passive(energy);
  }
  SECTION("through a tap closure and a moving vowel") {
    // The tongue tip closing to the area floor at the alveolar ridge and
    // opening again, where a junction's reflection approaches 1, while the
    // vowel's own constriction moves under it.
    SingingPhonation voice;
    phonate(voice);
    const SingingPatchParams defaults;
    std::vector<double> energy;
    for (int n = 0; n < kDecay; ++n) {
      if (n % 32 == 0) {
        const float t = static_cast<float>(n) / 1200.0f;
        const float closure = t < 1.0f ? t : t < 2.0f ? 1.0f : std::max(0.0f, 3.0f - t);
        voice.set_tap(defaults.tap_position, closure);
        voice.set_constriction(defaults.position + 0.3f * static_cast<float>(n) / kDecay,
                               defaults.brightness * (1.0f - 0.5f * closure));
      }
      energy.push_back(voice.stored_energy());
      voice.process(0.0f, 0.0f, 0.55f, 0.0f);
    }
    require_passive(energy);
  }
}

TEST_CASE("singing junction converges across the breath range", "[midi][synth][singing]") {
  // The convergence instrument at the engine's operating point: a cap hit
  // would mean the fold left the domain the junction's clamps admit.
  SingingPhonation voice;
  voice.configure_patch(kRate, kNoteHz, SingingPatchParams{});
  for (int n = 0; n < 2 * kRate; ++n) {
    const float force = static_cast<float>(n) / static_cast<float>(2 * kRate);
    voice.process(force, 0.0f, 0.55f, 0.0f);
  }
  const auto& stats = voice.junction().stats();
  INFO("solves " << stats.solves << ", max iterations " << stats.max_iterations
                 << ", max exit |du| " << stats.max_exit_step);
  REQUIRE(stats.solves == static_cast<std::uint64_t>(2 * kRate));
  REQUIRE(stats.cap_hits == 0);
}
