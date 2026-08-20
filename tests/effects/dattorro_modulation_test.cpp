/// @file dattorro_modulation_test.cpp
/// @brief Tank modulation contract for the Dattorro plate: the depth control
///        must reach both allpasses and must not degrade the signal as it rises.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "effects/reverb/dattorro_reverb.h"
#include "util/constants.h"

using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;

namespace {

using sonare::constants::kTwoPiD;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

std::vector<float> run_stereo(const DattorroReverbConfig& config, const std::vector<float>& input,
                              int channel) {
  DattorroReverb reverb(config);
  reverb.prepare(kSampleRate, kBlockSize);
  std::vector<float> left = input;
  std::vector<float> right = input;
  const int total = static_cast<int>(input.size());
  for (int offset = 0; offset + kBlockSize <= total; offset += kBlockSize) {
    float* channels[] = {left.data() + offset, right.data() + offset};
    reverb.process(channels, 2, kBlockSize);
  }
  return channel == 0 ? left : right;
}

std::vector<float> impulse(int length) {
  std::vector<float> signal(static_cast<size_t>(length), 0.0f);
  signal[0] = 1.0f;
  return signal;
}

std::vector<float> tone(int length, float hz) {
  std::vector<float> signal(static_cast<size_t>(length), 0.0f);
  for (int i = 0; i < length; ++i) {
    signal[static_cast<size_t>(i)] =
        0.5f * std::sin(static_cast<float>(kTwoPiD * hz * i / kSampleRate));
  }
  return signal;
}

/// Fraction of spectral energy sitting more than two octaves above a steady
/// input tone. The tank is linear, so without modulation a settled tone stays a
/// tone; smooth modulation adds sidebands a few Hz wide. Only a discontinuity
/// in the delay-line read can put energy up here.
double artefact_fraction(const std::vector<float>& tail, float tone_hz) {
  std::vector<float> windowed(tail.size());
  for (size_t i = 0; i < tail.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) /
                                          static_cast<double>(tail.size() - 1));
    windowed[i] = tail[i] * static_cast<float>(w);
  }
  sonare::FFT fft(static_cast<int>(windowed.size()));
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  fft.forward(windowed.data(), spectrum.data());

  const double bin_hz = kSampleRate / static_cast<double>(windowed.size());
  double total = 0.0;
  double far = 0.0;
  for (size_t bin = 1; bin < spectrum.size(); ++bin) {
    const double energy = static_cast<double>(std::norm(spectrum[bin]));
    total += energy;
    if (static_cast<double>(bin) * bin_hz > 4.0 * static_cast<double>(tone_hz)) far += energy;
  }
  return total > 0.0 ? far / total : 0.0;
}

}  // namespace

// A frozen LFO makes the two starting phases observable: at rate 0 each allpass
// holds a constant offset of depth*sin(phase), so if both phases are the same
// value AND that value is zero, the depth control does nothing at all. That is
// the state this pins - the control must not be inert.
//
// Scope, stated because it is narrower than it looks: this catches two phases
// that are equal *and zero*. Two phases set to the same non-zero value would
// still pass. Nothing on the public surface exposes the phases directly, and
// none of the stereo-width measures distinguish equal from quadrature phases,
// so this is the strongest deterministic assertion available here.
TEST_CASE("Tank modulation depth reaches the allpasses", "[effects][reverb][dattorro]") {
  constexpr int kLength = 48000;
  DattorroReverbConfig unmodulated;
  unmodulated.dry_wet = 1.0f;
  unmodulated.decay = 0.8f;
  unmodulated.mod_rate_hz = 0.0f;
  unmodulated.mod_depth_samples = 0.0f;

  DattorroReverbConfig modulated = unmodulated;
  modulated.mod_depth_samples = 24.0f;

  for (int channel = 0; channel < 2; ++channel) {
    CAPTURE(channel);
    const auto flat = run_stereo(unmodulated, impulse(kLength), channel);
    const auto moved = run_stereo(modulated, impulse(kLength), channel);
    REQUIRE(flat.size() == moved.size());

    size_t differing = 0;
    for (size_t i = 0; i < flat.size(); ++i) {
      if (flat[i] != moved[i]) ++differing;
    }
    // Both tank halves must respond; the two are cross-coupled, so a dead
    // offset on one side still shows up on both.
    REQUIRE(differing > flat.size() / 10);
  }
}

// The depth control must buy modulation without buying artefacts. Whole-sample
// read-pointer steps put broadband energy into the tail at a rate proportional
// to the depth, which makes the control worse as it is turned up.
TEST_CASE("Tank modulation depth does not add broadband artefacts", "[effects][reverb][dattorro]") {
  constexpr int kWarmup = 48000 * 2;
  constexpr int kAnalyze = 32768;
  constexpr float kToneHz = 1000.0f;
  const auto input = tone(kWarmup + kAnalyze, kToneHz);

  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.decay = 0.5f;
  config.mod_rate_hz = 2.0f;

  config.mod_depth_samples = 0.0f;
  const auto quiet = run_stereo(config, input, 0);
  const double baseline = artefact_fraction(
      std::vector<float>(quiet.begin() + kWarmup, quiet.begin() + kWarmup + kAnalyze), kToneHz);

  // The unmodulated tank is the floor this metric can reach at all, so the
  // sweep is judged against it rather than against an absolute number.
  for (float depth : {1.0f, 6.0f, 24.0f, 48.0f}) {
    CAPTURE(depth, baseline);
    config.mod_depth_samples = depth;
    const auto swept = run_stereo(config, input, 0);
    const double artefact = artefact_fraction(
        std::vector<float>(swept.begin() + kWarmup, swept.begin() + kWarmup + kAnalyze), kToneHz);
    CAPTURE(artefact);
    REQUIRE(artefact < baseline * 10.0);
  }
}
