// Chain-level loudness and spectral metrics: integrated loudness, true peak,
// short-term loudness spread, and per-band energy delta measured before and
// after a mastering chain. Each case pins one metric to the degradation it is
// in the set to catch, and the gain case pins the other three to standing still
// while a real level change goes past them.
//
// The four are the same quantities tools/mastering-eval/metrics_chain.py
// reports, taken from the same library entry points. This is not a
// cross-language value pin: the meters are shared, so only the reductions
// around them are written twice.
#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "core/audio.h"
#include "mastering/api/result_types.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/match/reference_spectrum.h"
#include "metering/basic.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "rt/biquad_design.h"
#include "util/constants.h"

namespace {

using sonare::Audio;
using sonare::constants::kHalfPiD;
using sonare::constants::kTwoPiD;
using sonare::mastering::api::kMasteringReportBandCount;
using sonare::mastering::dynamics::Compressor;
using sonare::mastering::dynamics::CompressorConfig;
using sonare::mastering::dynamics::DetectorMode;
using sonare::rt::kLoudnessOffset;

constexpr int kSampleRate = 48000;
constexpr int kTruePeakOversample = 4;

// The block the compressor is prepared for and fed in; a whole 8 s buffer as one
// block is outside what a realtime processor is asked to accept.
constexpr int kBlockSize = 4096;

// The band centres the mastering report publishes: kMasteringReportBandCount
// steps of a geometric sweep from 20 Hz to Nyquist, sampled at each step's
// midpoint. Mirrored by metrics_chain.band_center_frequencies.
constexpr float kBandLowHz = 20.0f;

// A power-of-two gain scales every float sample exactly, so any movement a
// level-independent metric shows is the metric's own and not the probe's
// rounding. 20*log10(0.5).
constexpr float kExactGain = 0.5f;
constexpr double kExactGainDb = -6.020599913279624;

// The level readings follow the gain to the last place the float meters carry;
// the spread is reconstructed through a float32 log and back, which is where
// its own thousandth of a LU comes from.
constexpr double kLevelTolerance = 1e-3;
constexpr double kSpreadTolerance = 1e-4;

std::vector<float> interleave(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> out(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out[2 * i] = left[i];
    out[2 * i + 1] = right[i];
  }
  return out;
}

Audio as_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

float integrated_loudness(const std::vector<float>& samples) {
  return sonare::metering::lufs(as_audio(samples)).integrated_lufs;
}

float true_peak_dbtp(const std::vector<float>& samples) {
  return sonare::metering::true_peak_db(as_audio(samples), kTruePeakOversample);
}

/// Population standard deviation of the gated short-term loudness series, in LU.
/// NaN when fewer than two blocks survive, which includes any signal under
/// 3.1 s: short-term emits no partial window.
double short_term_spread(const std::vector<float>& samples) {
  const std::vector<float> series = sonare::metering::short_term_lufs(as_audio(samples));
  std::vector<double> gated;
  for (float value : series) {
    if (std::isfinite(value) && value >= sonare::metering::kLufsAbsoluteGate) {
      gated.push_back(static_cast<double>(value));
    }
  }
  if (gated.size() < 2) return std::numeric_limits<double>::quiet_NaN();
  const double mean =
      std::accumulate(gated.begin(), gated.end(), 0.0) / static_cast<double>(gated.size());
  double variance = 0.0;
  for (double value : gated) variance += (value - mean) * (value - mean);
  return std::sqrt(variance / static_cast<double>(gated.size()));
}

float interpolated_db(const sonare::mastering::match::ReferenceSpectrum& spectrum,
                      float frequency_hz) {
  const auto upper =
      std::lower_bound(spectrum.frequencies.begin(), spectrum.frequencies.end(), frequency_hz);
  if (upper == spectrum.frequencies.begin()) return spectrum.db.front();
  if (upper == spectrum.frequencies.end()) return spectrum.db.back();
  const size_t high = static_cast<size_t>(upper - spectrum.frequencies.begin());
  const float ratio = (frequency_hz - spectrum.frequencies[high - 1]) /
                      (spectrum.frequencies[high] - spectrum.frequencies[high - 1]);
  return spectrum.db[high - 1] + ratio * (spectrum.db[high] - spectrum.db[high - 1]);
}

std::array<float, kMasteringReportBandCount> band_levels_db(const std::vector<float>& samples) {
  const auto spectrum = sonare::mastering::match::reference_spectrum(as_audio(samples));
  const float high_hz = 0.5f * static_cast<float>(kSampleRate);
  const float low_hz = std::min(kBandLowHz, high_hz);
  std::array<float, kMasteringReportBandCount> levels{};
  for (size_t index = 0; index < levels.size(); ++index) {
    const float position =
        (static_cast<float>(index) + 0.5f) / static_cast<float>(kMasteringReportBandCount);
    levels[index] = interpolated_db(spectrum, low_hz * std::pow(high_hz / low_hz, position));
  }
  return levels;
}

float rms(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float value : samples) sum += static_cast<double>(value) * value;
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

std::vector<float> scaled(const std::vector<float>& samples, float gain) {
  std::vector<float> out(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) out[i] = samples[i] * gain;
  return out;
}

/// Per-band change in long-term level between a chain's input and its output,
/// after matching the output's RMS to the input's so a flat gain reads zero
/// everywhere and only the tonal shape survives.
std::array<float, kMasteringReportBandCount> band_energy_delta(
    const std::vector<float>& source, const std::vector<float>& processed) {
  const float source_rms = rms(source);
  const float processed_rms = rms(processed);
  const std::vector<float> matched =
      processed_rms > 0.0f ? scaled(processed, source_rms / processed_rms) : processed;
  const auto before = band_levels_db(source);
  const auto after = band_levels_db(matched);
  std::array<float, kMasteringReportBandCount> delta{};
  for (size_t index = 0; index < delta.size(); ++index) delta[index] = after[index] - before[index];
  return delta;
}

/// Two partials with a slow amplitude drift, so the short-term loudness series
/// has a width to shrink and the spectrum has something to cut.
std::vector<float> drifting_tones(double seconds) {
  const size_t count = static_cast<size_t>(seconds * kSampleRate);
  std::vector<float> samples(count);
  for (size_t i = 0; i < count; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double envelope = 0.30 + 0.25 * std::sin(kTwoPiD * t / 4.0);
    samples[i] = static_cast<float>(
        envelope * (std::sin(kTwoPiD * 220.0 * t) + 0.6 * std::sin(kTwoPiD * 587.33 * t)));
  }
  return samples;
}

/// Deterministic broadband noise: every band carries content, so a cut in one
/// of them is visible against a floor the rest of the spectrum does not sit on.
std::vector<float> broadband_noise(double seconds) {
  const size_t count = static_cast<size_t>(seconds * kSampleRate);
  std::vector<float> samples(count);
  uint32_t state = 0x13572468u;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1664525u + 1013904223u;
    samples[i] = 0.2f * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return samples;
}

std::vector<float> compressed(const std::vector<float>& samples, float threshold_db, float ratio) {
  CompressorConfig config;
  config.threshold_db = threshold_db;
  config.ratio = ratio;
  config.attack_ms = 5.0f;
  config.release_ms = 80.0f;
  config.detector = DetectorMode::Rms;
  Compressor compressor(config);
  compressor.prepare(static_cast<double>(kSampleRate), kBlockSize);
  std::vector<float> out = samples;
  for (size_t start = 0; start < out.size(); start += kBlockSize) {
    const int count = static_cast<int>(std::min<size_t>(kBlockSize, out.size() - start));
    float* channels[1] = {out.data() + start};
    compressor.process(channels, 1, count);
  }
  return out;
}

std::vector<float> band_cut(const std::vector<float>& samples, float frequency_hz, float q,
                            float gain_db) {
  sonare::rt::BiquadState filter;
  filter.set(sonare::rt::rbj_peak(
      sonare::rt::frequency_to_w0(frequency_hz, static_cast<double>(kSampleRate)), q, gain_db));
  std::vector<float> out(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) out[i] = filter.process(samples[i]);
  return out;
}

float band_center_hz(size_t index) {
  const float high_hz = 0.5f * static_cast<float>(kSampleRate);
  const float position =
      (static_cast<float>(index) + 0.5f) / static_cast<float>(kMasteringReportBandCount);
  return kBandLowHz * std::pow(high_hz / kBandLowHz, position);
}

}  // namespace

TEST_CASE("an output gain moves the two level readings by exactly the gain and nothing else",
          "[mastering][metrics]") {
  const std::vector<float> source = drifting_tones(8.0);
  const std::vector<float> processed = compressed(source, -24.0f, 4.0f);
  const std::vector<float> gained = scaled(processed, kExactGain);

  // Integrated loudness IS the level, so this is the check rather than a failure.
  REQUIRE(static_cast<double>(integrated_loudness(gained) - integrated_loudness(processed)) ==
          Catch::Approx(kExactGainDb).margin(kLevelTolerance));

  // True peak is a second level reading and follows the gain the same way. It is
  // in the set to be compared against a ceiling, not to be read as quality.
  REQUIRE(static_cast<double>(true_peak_dbtp(gained) - true_peak_dbtp(processed)) ==
          Catch::Approx(kExactGainDb).margin(kLevelTolerance));

  // The two that must not see a level change at all.
  REQUIRE(short_term_spread(gained) ==
          Catch::Approx(short_term_spread(processed)).margin(kSpreadTolerance));
  const auto delta = band_energy_delta(source, processed);
  const auto gained_delta = band_energy_delta(source, gained);
  for (size_t index = 0; index < delta.size(); ++index) {
    REQUIRE(gained_delta[index] == delta[index]);
  }

  // Non-vacuity: the same gain moved the level readings by 6 dB, so the two
  // invariant metrics were handed a genuinely different signal, and the spread
  // measured on it is a width rather than a degenerate zero.
  REQUIRE(short_term_spread(processed) > 0.1);
}

TEST_CASE("true peak sees an inter-sample overshoot the sample peak does not",
          "[mastering][metrics]") {
  // The same fs/4 tone at two sampling phases. On-peak, every sample lands on a
  // crest; at 45 degrees they straddle it and the sample peak reads 3 dB low
  // while the waveform between them is unchanged.
  const size_t count = static_cast<size_t>(kSampleRate);
  const float amplitude = 0.7953f;
  std::vector<float> on_peak(count);
  std::vector<float> between_samples(count);
  for (size_t i = 0; i < count; ++i) {
    const double phase = kHalfPiD * static_cast<double>(i);
    on_peak[i] = static_cast<float>(amplitude * std::cos(phase));
    between_samples[i] = static_cast<float>(amplitude * std::cos(phase + 0.5 * kHalfPiD));
  }

  const float on_peak_sample = sonare::metering::peak_db(as_audio(on_peak));
  const float between_sample = sonare::metering::peak_db(as_audio(between_samples));
  const float on_peak_true = true_peak_dbtp(on_peak);
  const float between_true = true_peak_dbtp(between_samples);

  // Same tone at the same level: the sample-peak meter disagrees by 3 dB purely
  // because of where the samples fell.
  REQUIRE(static_cast<double>(between_sample - on_peak_sample) ==
          Catch::Approx(-3.0103).margin(0.01));
  // The true-peak meter reads through it and puts both at the same level. The
  // margin is the meter's own: a quarter-Nyquist tone sits at the edge of the
  // oversampler's passband, where it reads a couple of tenths of a dB over the
  // continuous peak, and by a different couple for each sampling phase.
  REQUIRE(static_cast<double>(between_true - on_peak_true) == Catch::Approx(0.0).margin(0.35));

  // A ceiling between the two readings is the case the metric is in the set for:
  // a chain that limited on sample peak would call this output compliant.
  constexpr float kCeilingDbtp = -2.5f;
  REQUIRE(between_sample < kCeilingDbtp);
  REQUIRE(between_true > kCeilingDbtp);

  // The overshoot is a peak event and not a level change: the two phases carry
  // the same loudness, so the reading above is not loudness under another name.
  REQUIRE(static_cast<double>(integrated_loudness(between_samples) -
                              integrated_loudness(on_peak)) == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("compressing harder shrinks the short-term loudness spread", "[mastering][metrics]") {
  const std::vector<float> source = drifting_tones(8.0);
  const double untouched = short_term_spread(source);
  const double gentle = short_term_spread(compressed(source, -18.0f, 2.0f));
  const double heavy = short_term_spread(compressed(source, -40.0f, 20.0f));

  REQUIRE(untouched > gentle);
  REQUIRE(gentle > heavy);
  // The width has to collapse rather than merely drift, or the ordering above is
  // reading noise.
  REQUIRE(heavy < 0.5 * untouched);
}

TEST_CASE("the channel-summed short-term series rebuilds from the per-channel ones",
          "[mastering][metrics]") {
  // The library publishes the short-term series for a mono buffer only, so a
  // stereo feed's series is rebuilt from the two mono ones. Every block index
  // covers the same window in both channels and a stereo layout carries unit
  // BS.1770 channel weights, so the sum of the per-channel block energies is the
  // sum the multi-channel meter forms. Max-S is that meter's own reduction of
  // the series this rebuilds, which is what pins the arithmetic.
  const std::vector<float> left = drifting_tones(8.0);
  const std::vector<float> right = band_cut(left, 220.0f, 1.0f, -9.0f);

  const std::vector<float> left_series = sonare::metering::short_term_lufs(as_audio(left));
  const std::vector<float> right_series = sonare::metering::short_term_lufs(as_audio(right));
  REQUIRE(left_series.size() == right_series.size());
  REQUIRE(left_series.size() >= 2);

  const auto block_energy = [](float lufs) {
    return std::isfinite(lufs)
               ? std::pow(10.0, (static_cast<double>(lufs) - kLoudnessOffset) / 10.0)
               : 0.0;
  };
  double rebuilt_max = -std::numeric_limits<double>::infinity();
  for (size_t index = 0; index < left_series.size(); ++index) {
    const double energy = block_energy(left_series[index]) + block_energy(right_series[index]);
    if (energy > 0.0)
      rebuilt_max = std::max(rebuilt_max, kLoudnessOffset + 10.0 * std::log10(energy));
  }

  const std::vector<float> both = interleave(left, right);
  const float published =
      sonare::metering::lufs_interleaved(both.data(), left.size(), 2, kSampleRate)
          .max_short_term_lufs;
  REQUIRE(rebuilt_max == Catch::Approx(static_cast<double>(published)).margin(1e-4));

  // Non-vacuity: the sum is not either channel read on its own, so the agreement
  // above is the reconstruction's and not a coincidence of one dominant channel.
  const float left_max = *std::max_element(left_series.begin(), left_series.end());
  REQUIRE(rebuilt_max - static_cast<double>(left_max) > 0.5);
}

TEST_CASE("cutting one band moves that band's delta and leaves the distant ones alone",
          "[mastering][metrics]") {
  constexpr size_t kTargetBand = 20;
  constexpr float kCutDb = -12.0f;
  constexpr size_t kNeighbourhood = 3;

  const std::vector<float> source = broadband_noise(4.0);
  const std::vector<float> cut = band_cut(source, band_center_hz(kTargetBand), 4.0f, kCutDb);
  const auto delta = band_energy_delta(source, cut);

  // Third-octave smoothing spans about one band, so the cut lands on a small
  // neighbourhood rather than a single index; the claim is that it stays there.
  REQUIRE(delta[kTargetBand] < 0.5f * kCutDb);
  for (size_t index = 0; index < delta.size(); ++index) {
    const size_t distance = index > kTargetBand ? index - kTargetBand : kTargetBand - index;
    if (distance > kNeighbourhood) {
      REQUIRE(std::abs(delta[index]) < 1.0f);
    }
  }

  // Non-vacuity: the tolerance the distant bands pass is far below the movement
  // the cut band shows, so the bound is not one every band would clear.
  REQUIRE(std::abs(delta[kTargetBand]) > 6.0f);
}
