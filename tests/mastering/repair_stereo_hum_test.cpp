/// @file repair_stereo_hum_test.cpp
/// @brief Detection, validation and stereo behaviour of dehum/decrackle/trim_silence.
///
/// The hum fixtures reproduce two items of the restoration corpus
/// (tools/mastering-eval/corpus.py) in the tree -- a 50 Hz series over a sine
/// bed and a 60 Hz series over a chord bed, at the levels the manifest records
/// -- so the detector is measured against quantities a generator planted rather
/// than against numbers this implementation produced. The corpus carries no
/// crackle item, so that fixture plants its own ramp of deviations here and the
/// expected counts are computed from the ramp, not read off a run.
///
/// The golden digests pin the mono outputs against the build that preceded the
/// seam extraction, so the decrackle refactor is bit-identical rather than
/// merely plausible.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "mastering/api/audio_utils.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/trim_silence.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::repair;

namespace {

constexpr int kSampleRate = 48000;
/// The corpus writes 24-bit WAVs and measures every planted quantity after the
/// round trip, so a fixture that skips the grid is a different signal.
constexpr double kQuantizeFull = 8388607.0;
constexpr double kQuantizeScale = 8388608.0;

float quantize24(double value) {
  const double clamped = std::min(1.0, std::max(-1.0, value));
  return static_cast<float>(std::nearbyint(clamped * kQuantizeFull) / kQuantizeScale);
}

std::vector<float> quantize(const std::vector<double>& values) {
  std::vector<float> out(values.size());
  for (size_t i = 0; i < values.size(); ++i) out[i] = quantize24(values[i]);
  return out;
}

std::vector<double> sine_bed(size_t frames, double phase, double amp = 0.5,
                             double frequency = 440.0) {
  std::vector<double> out(frames);
  for (size_t i = 0; i < frames; ++i) {
    out[i] = amp * std::sin(constants::kTwoPiD * frequency * static_cast<double>(i) / kSampleRate +
                            phase);
  }
  return out;
}

std::vector<double> chord_bed(size_t frames, double side) {
  const std::array<double, 3> partials = {220.0, 220.0 * std::pow(2.0, 4.0 / 12.0),
                                          220.0 * std::pow(2.0, 7.0 / 12.0)};
  std::vector<double> out(frames, 0.0);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    double sum = 0.0;
    for (size_t p = 0; p < partials.size(); ++p) {
      sum += 0.28 * std::sin(constants::kTwoPiD * partials[p] * t + side * static_cast<double>(p));
    }
    out[i] = sum * (0.7 + 0.3 * std::sin(constants::kTwoPiD * 0.5 * t));
  }
  return out;
}

/// The corpus hum planter: a series at stated dBFS amplitudes, phase 0.23k.
void plant_hum(std::vector<double>& bed, double fundamental_hz,
               const std::vector<double>& levels_db) {
  for (size_t i = 0; i < bed.size(); ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    for (size_t k = 0; k < levels_db.size(); ++k) {
      const double index = static_cast<double>(k + 1);
      bed[i] += std::pow(10.0, levels_db[k] / 20.0) *
                std::sin(constants::kTwoPiD * fundamental_hz * index * t + 0.23 * index);
    }
  }
}

// --------------------------------------------------------------- hum fixtures

/// The corpus "sine_hum50" item: one second of 440 Hz at 0.5, plus a 50 Hz
/// series at the four levels the manifest lists.
const std::vector<double> kPlantedHum50Db = {-26.0, -32.0, -38.0, -44.0};
constexpr double kPlantedHum50Hz = 50.0;

/// The corpus "chord_hum60" item: a 60 Hz series over the chord bed.
const std::vector<double> kPlantedHum60Db = {-24.0, -30.0, -36.0};
constexpr double kPlantedHum60Hz = 60.0;

std::vector<float> hum50_clean(double phase) { return quantize(sine_bed(48000, phase)); }

std::vector<float> hum50_fixture(double phase) {
  std::vector<double> bed = sine_bed(48000, phase);
  plant_hum(bed, kPlantedHum50Hz, kPlantedHum50Db);
  return quantize(bed);
}

std::vector<float> hum60_clean() { return quantize(chord_bed(48000, 0.11)); }

std::vector<float> hum60_fixture() {
  std::vector<double> bed = chord_bed(48000, 0.11);
  plant_hum(bed, kPlantedHum60Hz, kPlantedHum60Db);
  return quantize(bed);
}

/// Every hum harmonic and the 440 Hz bed complete a whole number of cycles in a
/// one-second frame, so the projection basis is orthogonal to the bed and the
/// only residue left is the 24-bit grid: -144 dBFS against the weakest planted
/// harmonic at -44 dBFS is 8.7e-5 dB of level error. A thousandth of a dB is
/// three orders above that and still four orders under the margin the harmonic
/// count uses.
constexpr float kOrthogonalLevelToleranceDb = 0.001f;

/// The chord partials sit at 220, 277.18 and 329.63 Hz, none of them a whole
/// number of cycles per frame, so each leaks into the harmonic grid. A
/// rectangular window's leakage at an offset of d bins is bounded by A/(pi*d);
/// summed over the three partials against the weakest planted harmonic that
/// bounds the level error at 1.9 dB.
constexpr float kChordLeakageToleranceDb = 2.0f;

/// The search evaluates seventeen candidates across the window, so a planted
/// fundamental at its centre is a grid point the search returns exactly. What is
/// left is float representation of that grid point: 50 Hz carries a float32 ulp
/// of 3.8e-6 Hz, four orders under this.
constexpr double kGridPointToleranceHz = 1.0e-4;

// ----------------------------------------------------------- crackle fixture

/// Deviations planted on a ramp so a threshold sweep has a computable answer:
/// the count at threshold t is the number of planted amplitudes above it, and
/// the local median a one-sample spike is measured against is a neighbouring bed
/// sample, which on a 440 Hz sine at 0.10 moves by under 0.006 per sample -- an
/// order under the 0.02 spacing of the ramp.
constexpr size_t kCracklePositions[] = {311,   1487,  2903,  4111,  5677,  7013,  8429,  9901,
                                        11243, 12689, 14051, 15473, 16907, 18229, 19681, 21013,
                                        22447, 23879, 25211, 26653, 28087, 29419, 30853, 32287};
constexpr size_t kPlantedCrackle = sizeof(kCracklePositions) / sizeof(kCracklePositions[0]);
constexpr size_t kCrackleFrames = 33000;
constexpr double kCrackleBase = 0.20;
constexpr double kCrackleStep = 0.02;

double planted_crackle_amplitude(size_t index) {
  return kCrackleBase + kCrackleStep * static_cast<double>(index);
}

/// How many planted deviations exceed @p threshold. The detector's answer is
/// this number, computed from the planting recipe rather than from a run.
size_t planted_crackle_above(float threshold) {
  size_t count = 0;
  for (size_t k = 0; k < kPlantedCrackle; ++k) {
    if (planted_crackle_amplitude(k) > static_cast<double>(threshold)) ++count;
  }
  return count;
}

std::vector<float> crackle_clean(double phase) {
  return quantize(sine_bed(kCrackleFrames, phase, 0.10));
}

std::vector<float> crackle_fixture(double phase) {
  std::vector<double> bed = sine_bed(kCrackleFrames, phase, 0.10);
  for (size_t k = 0; k < kPlantedCrackle; ++k) {
    const double amplitude = planted_crackle_amplitude(k);
    bed[kCracklePositions[k]] += (k % 2 == 0 ? amplitude : -amplitude);
  }
  return quantize(bed);
}

// ------------------------------------------------------------- trim fixtures

std::vector<float> gated_fixture(size_t first, size_t last_exclusive, double frequency,
                                 double amp = 0.4) {
  std::vector<double> bed(48000, 0.0);
  for (size_t i = first; i < last_exclusive; ++i) {
    bed[i] = amp * std::sin(constants::kTwoPiD * frequency * static_cast<double>(i) / kSampleRate);
  }
  return quantize(bed);
}

/// What the stereo trim measures on: the same mix the repair helpers build.
std::vector<float> downmix(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> mono(left.size());
  for (size_t i = 0; i < left.size(); ++i) mono[i] = 0.5f * (left[i] + right[i]);
  return mono;
}

// -------------------------------------------------------------------- metrics

Audio view(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), kSampleRate);
}

std::vector<float> to_vector(const Audio& audio) {
  return std::vector<float>(audio.data(), audio.data() + audio.size());
}

/// FNV-1a over the raw sample bits: an exact identity, not a tolerance.
uint32_t digest(const Audio& audio) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < audio.size(); ++i) {
    uint32_t bits = 0;
    const float value = audio[i];
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (bits >> (8 * byte)) & 0xffu;
      hash *= 16777619u;
    }
  }
  return hash;
}

uint32_t digest(const std::vector<float>& samples) { return digest(view(samples)); }

double rmse(const std::vector<float>& got, const std::vector<float>& want) {
  double sum = 0.0;
  for (size_t i = 0; i < got.size(); ++i) {
    const double error = static_cast<double>(got[i]) - static_cast<double>(want[i]);
    sum += error * error;
  }
  return std::sqrt(sum / static_cast<double>(got.size()));
}

/// RMS of (error_left - error_right): what a defect present in both channels
/// leaves behind once the two sides have been treated differently. This is the
/// quantity a shared decision exists to remove; per-channel RMSE cannot see it.
double image_error(const std::vector<float>& left, const std::vector<float>& right,
                   const std::vector<float>& clean_left, const std::vector<float>& clean_right) {
  double sum = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    const double difference = (static_cast<double>(left[i]) - clean_left[i]) -
                              (static_cast<double>(right[i]) - clean_right[i]);
    sum += difference * difference;
  }
  return std::sqrt(sum / static_cast<double>(left.size()));
}

template <typename Repair>
std::pair<std::vector<float>, std::vector<float>> shared_mono_transfer(
    const std::vector<float>& left, const std::vector<float>& right, Repair&& repair) {
  std::vector<float> a = left;
  std::vector<float> b = right;
  mastering::api::detail::apply_shared_mono_transfer_repair(a, b, kSampleRate, repair);
  return {a, b};
}

template <typename Repair>
std::pair<std::vector<float>, std::vector<float>> independent(const std::vector<float>& left,
                                                              const std::vector<float>& right,
                                                              Repair&& repair) {
  std::vector<float> a = left;
  std::vector<float> b = right;
  mastering::api::detail::apply_independent_repair(a, b, kSampleRate, repair);
  return {a, b};
}

const DehumConfig kHum50Fixed{50.0f, 4, 20.0f, false, 2.0f, 0.25f, 2048, 0.01f};
const DehumConfig kHum50Adaptive{50.0f, 4, 20.0f, true, 2.0f, 0.25f, 2048, 0.01f};
const DecrackleConfig kMedian{0.25f, DecrackleMode::Median, 4};
const DecrackleConfig kWavelet{0.08f, DecrackleMode::WaveletShrinkage, 4};
const TrimSilenceConfig kTrim{0.001f, 0, TrimSilenceMode::Peak, -60.0f, 400.0f};

}  // namespace

// ---------------------------------------------------------------- validation

TEST_CASE("Dehum validation rejects the non-finite values the old form let past",
          "[repair][stereo][hum]") {
  const std::vector<float> samples = hum50_fixture(0.0);
  const Audio audio = view(samples);

  // Every one of these was accepted by the preceding build. An infinite
  // fundamental compared greater than zero, so the pass became a silent no-op:
  // its output digest equalled the input's.
  DehumConfig infinite_fundamental = kHum50Fixed;
  infinite_fundamental.fundamental_hz = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(dehum(audio, infinite_fundamental));
  REQUIRE_THROWS(detect_hum(samples.data(), samples.size(), kSampleRate, infinite_fundamental));

  DehumConfig infinite_q = kHum50Fixed;
  infinite_q.q = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(dehum(audio, infinite_q));

  // NaN in a one-sided comparison falls on the accepting branch: with either of
  // these the tracker never refreshed its coefficients and the adaptive pass
  // silently produced the fixed cascade's output instead.
  DehumConfig nan_adaptation = kHum50Adaptive;
  nan_adaptation.adaptation = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(dehum(audio, nan_adaptation));

  DehumConfig nan_bandwidth = kHum50Adaptive;
  nan_bandwidth.pll_bandwidth = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS(dehum(audio, nan_bandwidth));

  DehumConfig infinite_bandwidth = kHum50Adaptive;
  infinite_bandwidth.pll_bandwidth = std::numeric_limits<float>::infinity();
  REQUIRE_THROWS(dehum(audio, infinite_bandwidth));

  for (float value :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    DehumConfig bad_range = kHum50Adaptive;
    bad_range.search_range_hz = value;
    REQUIRE_THROWS(dehum(audio, bad_range));
  }

  // The reported harmonic levels are a fixed-length array, so the cascade can no
  // longer be asked for more harmonics than the report can carry.
  DehumConfig too_many = kHum50Fixed;
  too_many.harmonics = kDehumMaxHarmonics + 1;
  REQUIRE_THROWS(dehum(audio, too_many));
  DehumConfig at_the_cap = kHum50Fixed;
  at_the_cap.harmonics = kDehumMaxHarmonics;
  REQUIRE_NOTHROW(dehum(audio, at_the_cap));

  REQUIRE_NOTHROW(dehum(audio, kHum50Fixed));
  REQUIRE_THROWS(detect_hum(samples.data(), samples.size(), 0, kHum50Fixed));
}

TEST_CASE("Decrackle validation rejects the mode and threshold that fell through",
          "[repair][stereo][hum]") {
  const std::vector<float> samples = crackle_fixture(0.0);
  const Audio audio = view(samples);

  // An infinite threshold was accepted in both modes. In median mode nothing
  // ever exceeded it and the output digest equalled the input's; in wavelet mode
  // it was indistinguishable from the default, because the Bayes threshold is
  // the smaller of the two on this material.
  for (const DecrackleConfig& base : {kMedian, kWavelet}) {
    DecrackleConfig infinite = base;
    infinite.threshold = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS(decrackle(audio, infinite));
    DecrackleConfig not_a_number = base;
    not_a_number.threshold = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(decrackle(audio, not_a_number));
  }

  // An out-of-range mode used to fall into the median branch and report success.
  DecrackleConfig unknown_mode = kMedian;
  unknown_mode.mode = static_cast<DecrackleMode>(7);
  REQUIRE_THROWS(decrackle(audio, unknown_mode));
  REQUIRE_THROWS(detect_crackle(samples.data(), samples.size(), kSampleRate, unknown_mode));

  DecrackleConfig no_levels = kWavelet;
  no_levels.levels = 0;
  REQUIRE_THROWS(decrackle(audio, no_levels));

  REQUIRE_NOTHROW(decrackle(audio, kMedian));
  REQUIRE_THROWS(detect_crackle(samples.data(), samples.size(), 0, kMedian));
}

TEST_CASE("Trim validation rejects a padding that wraps the padded tail", "[repair][stereo][hum]") {
  const std::vector<float> samples = gated_fixture(9600, 38400, 440.0);
  const Audio audio = view(samples);

  // padding_samples is a size_t with no upper bound, and the tail is computed as
  // last + padding_samples. Before this check the largest value wrapped: on this
  // fixture it returned [0, 38399) -- one sample SHORTER than the untrimmed tail
  // it was asked to extend past the end.
  TrimSilenceConfig wrapping = kTrim;
  wrapping.padding_samples = std::numeric_limits<size_t>::max();
  REQUIRE_THROWS(trim_silence(audio, wrapping));
  REQUIRE_THROWS(detect_trim_range(samples.data(), samples.size(), kSampleRate, wrapping));

  // The cap is where the sum CAN wrap, not where it does for a given input: one
  // past it still returned the right range on a fixture this short. The bound is
  // the length-independent one, so a long buffer cannot walk into the case above.
  TrimSilenceConfig past_the_cap = kTrim;
  past_the_cap.padding_samples = kMaxTrimPaddingSamples + 1;
  REQUIRE_THROWS(trim_silence(audio, past_the_cap));

  // At the cap the sum still fits, and a padding past the buffer simply keeps
  // everything, which is what the field means.
  TrimSilenceConfig at_the_cap = kTrim;
  at_the_cap.padding_samples = kMaxTrimPaddingSamples;
  const TrimRange kept = detect_trim_range(samples.data(), samples.size(), kSampleRate, at_the_cap);
  REQUIRE(kept.first == 0);
  REQUIRE(kept.last_exclusive == samples.size());
}

// ----------------------------------------------------------------- detection

TEST_CASE("Hum detection recovers the corpus fundamental and harmonic levels",
          "[repair][stereo][hum]") {
  const std::vector<float> left = hum50_fixture(0.0);
  const std::vector<float> right = hum50_fixture(0.35);

  for (const std::vector<float>& channel : {left, right}) {
    const HumDetection detected =
        detect_hum(channel.data(), channel.size(), kSampleRate, kHum50Fixed);
    REQUIRE_THAT(detected.fundamental_hz, WithinAbs(kPlantedHum50Hz, kGridPointToleranceHz));
    REQUIRE(detected.harmonics == static_cast<int>(kPlantedHum50Db.size()));
    for (size_t k = 0; k < kPlantedHum50Db.size(); ++k) {
      CAPTURE(k);
      REQUIRE_THAT(detected.harmonic_dbfs[k],
                   WithinAbs(kPlantedHum50Db[k], kOrthogonalLevelToleranceDb));
    }
    // Nothing was planted past the fourth, and the slots say so with the dB
    // floor rather than with the zero a struct starts at -- which would read as
    // full scale.
    for (int k = static_cast<int>(kPlantedHum50Db.size()); k < kDehumMaxHarmonics; ++k) {
      CAPTURE(k);
      REQUIRE(detected.harmonic_dbfs[k] == constants::kFloorDb);
    }
  }

  const std::vector<float> chord = hum60_fixture();
  const DehumConfig hum60{60.0f, 3};
  const HumDetection detected = detect_hum(chord.data(), chord.size(), kSampleRate, hum60);
  REQUIRE_THAT(detected.fundamental_hz, WithinAbs(kPlantedHum60Hz, kGridPointToleranceHz));
  REQUIRE(detected.harmonics == static_cast<int>(kPlantedHum60Db.size()));
  for (size_t k = 0; k < kPlantedHum60Db.size(); ++k) {
    CAPTURE(k);
    REQUIRE_THAT(detected.harmonic_dbfs[k],
                 WithinAbs(kPlantedHum60Db[k], kChordLeakageToleranceDb));
  }
}

TEST_CASE("Hum detection measures the input rather than echoing the configuration",
          "[repair][stereo][hum]") {
  const std::vector<float> samples = hum50_fixture(0.0);

  // The configured fundamental is wrong by a hertz and the planted one is inside
  // the search window. A detector that followed config.adaptive -- false here --
  // or that simply passed its argument back would answer 49.
  DehumConfig mistuned = kHum50Fixed;
  mistuned.fundamental_hz = 49.0f;
  const HumDetection found = detect_hum(samples.data(), samples.size(), kSampleRate, mistuned);
  REQUIRE_THAT(found.fundamental_hz, WithinAbs(kPlantedHum50Hz, kGridPointToleranceHz));
  REQUIRE(found.fundamental_hz != mistuned.fundamental_hz);
  REQUIRE(found.harmonics == static_cast<int>(kPlantedHum50Db.size()));

  // Outside the window the search cannot reach the planted tone, and the answer
  // pins to the window edge rather than pretending: 60 +/- 2 never covers 50.
  DehumConfig out_of_band = kHum50Fixed;
  out_of_band.fundamental_hz = 60.0f;
  const HumDetection missed = detect_hum(samples.data(), samples.size(), kSampleRate, out_of_band);
  REQUIRE(missed.harmonics == 0);
  REQUIRE(missed.fundamental_prominence < found.fundamental_prominence);
}

TEST_CASE("Hum prominence separates planted hum from the same bed without it",
          "[repair][stereo][hum]") {
  const std::vector<float> dirty_sine = hum50_fixture(0.0);
  const std::vector<float> clean_sine = hum50_clean(0.0);
  const std::vector<float> dirty_chord = hum60_fixture();
  const std::vector<float> clean_chord = hum60_clean();
  const DehumConfig hum60{60.0f, 3};

  const float dirty_sine_prominence =
      detect_hum(dirty_sine.data(), dirty_sine.size(), kSampleRate, kHum50Fixed)
          .fundamental_prominence;
  const float clean_sine_prominence =
      detect_hum(clean_sine.data(), clean_sine.size(), kSampleRate, kHum50Fixed)
          .fundamental_prominence;
  const float dirty_chord_prominence =
      detect_hum(dirty_chord.data(), dirty_chord.size(), kSampleRate, hum60).fundamental_prominence;
  const float clean_chord_prominence =
      detect_hum(clean_chord.data(), clean_chord.size(), kSampleRate, hum60).fundamental_prominence;

  // The field's contract is that a search finding no peak reads near 1. A
  // maximum over a median of seventeen candidates does not reach exactly 1 on
  // material that has none, so the claim is an order of magnitude between the
  // two, not a threshold on either. Measured: 13.0 on the sine pair, 15.1 on the
  // chord pair.
  CAPTURE(dirty_sine_prominence, clean_sine_prominence);
  CAPTURE(dirty_chord_prominence, clean_chord_prominence);
  REQUIRE(clean_sine_prominence < 3.0f);
  REQUIRE(clean_chord_prominence < 3.0f);
  REQUIRE(dirty_sine_prominence > 10.0f * clean_sine_prominence);
  REQUIRE(dirty_chord_prominence > 10.0f * clean_chord_prominence);

  // A clean bed also reports no harmonics, so the two halves of the detection
  // agree rather than one carrying the verdict alone.
  REQUIRE(detect_hum(clean_sine.data(), clean_sine.size(), kSampleRate, kHum50Fixed).harmonics ==
          0);
  REQUIRE(detect_hum(clean_chord.data(), clean_chord.size(), kSampleRate, hum60).harmonics == 0);
}

TEST_CASE("Crackle detection counts the planted deviations at every threshold",
          "[repair][stereo][hum]") {
  const std::vector<float> samples = crackle_fixture(0.0);
  REQUIRE(planted_crackle_above(0.15f) == kPlantedCrackle);

  for (float threshold : {0.15f, 0.25f, 0.35f, 0.45f, 0.55f, 0.65f, 0.67f}) {
    DecrackleConfig config = kMedian;
    config.threshold = threshold;
    const CrackleDetection detected =
        detect_crackle(samples.data(), samples.size(), kSampleRate, config);
    const size_t expected = planted_crackle_above(threshold);
    CAPTURE(threshold, expected);
    REQUIRE(detected.sample_count == expected);
    // Both are the count divided by the input length in float32, so the bound is
    // that division's rounding: a relative 1.2e-7 on a fraction near 7e-4 and on
    // a rate near 35 leaves 8.7e-11 and 4.2e-6.
    REQUIRE_THAT(
        detected.sample_fraction,
        WithinAbs(static_cast<double>(expected) / static_cast<double>(kCrackleFrames), 1.0e-9));
    REQUIRE_THAT(detected.per_second,
                 WithinAbs(static_cast<double>(expected) * kSampleRate / kCrackleFrames, 1.0e-4));

    // The repair acts on exactly what the detector counted: the two read one
    // criterion, so a cheaper detector cannot drift away from the repair.
    DecrackleReport report;
    decrackle(view(samples), config, &report);
    REQUIRE(report.replaced_samples == expected);
    REQUIRE(report.detected.sample_count == expected);
  }

  // The sweep runs the count from all of them to none of them, so a detector
  // stuck on any single value fails somewhere along it.
  REQUIRE(planted_crackle_above(0.67f) == 0);
}

TEST_CASE("Crackle detection reads the median criterion in wavelet mode too",
          "[repair][stereo][hum]") {
  const std::vector<float> samples = crackle_fixture(0.0);
  DecrackleConfig wavelet = kWavelet;
  wavelet.threshold = 0.25f;
  DecrackleConfig median = kMedian;

  // Detection does not read the mode: shrinkage never decides that a sample is
  // crackle, so the median deviation is this module's only definition of it.
  REQUIRE(detect_crackle(samples.data(), samples.size(), kSampleRate, wavelet).sample_count ==
          detect_crackle(samples.data(), samples.size(), kSampleRate, median).sample_count);
}

// --------------------------------------------------- mono output is preserved

TEST_CASE("Decrackle mono output survives the Haar seam extraction unchanged",
          "[repair][stereo][hum]") {
  // Digests taken from the build that preceded the split of the forward
  // transform, the shrinkage and the inverse into separate steps.
  const std::vector<float> left = crackle_fixture(0.0);
  const std::vector<float> right = crackle_fixture(0.35);
  REQUIRE(digest(left) == 0xc09020d7u);
  REQUIRE(digest(right) == 0x6e4ae955u);
  REQUIRE(digest(decrackle(view(left), kMedian)) == 0x7262ef87u);
  REQUIRE(digest(decrackle(view(right), kMedian)) == 0xd7a14dd5u);
  REQUIRE(digest(decrackle(view(left), kWavelet)) == 0xdc4b2183u);
  REQUIRE(digest(decrackle(view(right), kWavelet)) == 0x33135618u);
}

TEST_CASE("Dehum and trim mono output are unchanged by the report and stereo work",
          "[repair][stereo][hum]") {
  const std::vector<float> left = hum50_fixture(0.0);
  const std::vector<float> right = hum50_fixture(0.35);
  REQUIRE(digest(left) == 0x4214376du);
  REQUIRE(digest(right) == 0x399640edu);
  REQUIRE(digest(dehum(view(left), kHum50Fixed)) == 0xd6be71c3u);
  REQUIRE(digest(dehum(view(right), kHum50Fixed)) == 0x2c0f05e4u);
  REQUIRE(digest(dehum(view(left), kHum50Adaptive)) == 0xe65b6560u);
  REQUIRE(digest(dehum(view(right), kHum50Adaptive)) == 0x5a343eccu);

  const std::vector<float> chord = hum60_fixture();
  REQUIRE(digest(chord) == 0x67b9b392u);
  REQUIRE(digest(dehum(view(chord), DehumConfig{60.0f, 3, 20.0f})) == 0x6d434fe1u);

  const std::vector<float> gated = gated_fixture(9600, 38400, 440.0);
  REQUIRE(digest(gated) == 0xe922ae85u);
  const Audio trimmed = trim_silence(view(gated), kTrim);
  REQUIRE(digest(trimmed) == 0x9954e335u);
  REQUIRE(trimmed.size() == 28799);
}

TEST_CASE("Dehum reports what the cascade did without moving the output", "[repair][stereo][hum]") {
  const std::vector<float> samples = hum50_fixture(0.0);

  DehumReport fixed_report;
  REQUIRE(digest(dehum(view(samples), kHum50Fixed, &fixed_report)) == 0xd6be71c3u);
  REQUIRE(fixed_report.notched_harmonics == kHum50Fixed.harmonics);
  REQUIRE(fixed_report.applied_fundamental_hz == kHum50Fixed.fundamental_hz);
  // Without tracking the frequency cannot move, and the zero is that
  // measurement rather than an unfilled field.
  REQUIRE(fixed_report.fundamental_drift_hz == 0.0f);
  REQUIRE(fixed_report.detected.harmonics == static_cast<int>(kPlantedHum50Db.size()));

  DehumReport adaptive_report;
  REQUIRE(digest(dehum(view(samples), kHum50Adaptive, &adaptive_report)) == 0xe65b6560u);
  REQUIRE(adaptive_report.fundamental_drift_hz > 0.0f);
  // The tracker is clamped to the search window, so the drift cannot exceed it.
  REQUIRE(adaptive_report.fundamental_drift_hz <= kHum50Adaptive.search_range_hz);

  // A cascade whose upper harmonics fall past Nyquist reports the ones it
  // reached, which is the only place a caller can learn the cascade was cut
  // short. 16 harmonics of 3 kHz run past 24 kHz from the eighth.
  DehumConfig high{3000.0f, kDehumMaxHarmonics, 20.0f};
  DehumReport clipped;
  dehum(view(samples), high, &clipped);
  REQUIRE(clipped.notched_harmonics == 7);
  REQUIRE(clipped.notched_harmonics < high.harmonics);
}

TEST_CASE("Decrackle reports the shrinkage the wavelet mode performed", "[repair][stereo][hum]") {
  const std::vector<float> samples = crackle_fixture(0.0);

  DecrackleReport report;
  decrackle(view(samples), kWavelet, &report);

  // The detail band count follows from the input length and the level count:
  // each level halves, discarding an odd tail sample.
  size_t expected_details = 0;
  size_t active = kCrackleFrames;
  for (int level = 0; level < kWavelet.levels && active >= 2; ++level) {
    expected_details += active / 2;
    active /= 2;
  }
  REQUIRE(report.detail_coefficients == expected_details);
  REQUIRE(report.shrunk_coefficients > 0);
  REQUIRE(report.shrunk_coefficients < report.detail_coefficients);

  // The level-0 detail of a sine at amplitude A is A*sin(w/2) times a cosine, so
  // the MAD estimate is that amplitude times the median of |cos|, scaled by the
  // Gaussian constant the shrinkage uses. The planted spikes are 24 samples in
  // 16500 and do not move a median.
  const double omega = constants::kTwoPiD * 440.0 / kSampleRate;
  const double detail_amplitude = 0.10 * std::sin(0.5 * omega);
  const double predicted_sigma = detail_amplitude * constants::kInvSqrt2D / 0.67448975;
  CAPTURE(report.noise_sigma, predicted_sigma);
  REQUIRE(std::abs(report.noise_sigma - predicted_sigma) < 0.01 * predicted_sigma);

  // A threshold under the Bayes estimate is the binding one, so lowering it
  // shrinks fewer coefficients. Nothing else in the report may move with it.
  DecrackleConfig gentle = kWavelet;
  gentle.threshold = 1.0e-4f;
  DecrackleReport gentle_report;
  decrackle(view(samples), gentle, &gentle_report);
  REQUIRE(gentle_report.shrunk_coefficients < report.shrunk_coefficients);
  REQUIRE(gentle_report.detail_coefficients == report.detail_coefficients);
  REQUIRE(gentle_report.noise_sigma == report.noise_sigma);

  // Median mode fills the other half of the report and leaves these at zero,
  // which the caller reads against the mode it asked for.
  DecrackleReport median_report;
  decrackle(view(samples), kMedian, &median_report);
  REQUIRE(median_report.detail_coefficients == 0);
  REQUIRE(median_report.shrunk_coefficients == 0);
  REQUIRE(median_report.replaced_samples > 0);
  REQUIRE(report.replaced_samples == 0);
}

TEST_CASE("Trim reports what it dropped from each end", "[repair][stereo][hum]") {
  const std::vector<float> samples = gated_fixture(9600, 38400, 440.0);
  TrimReport report;
  const Audio trimmed = trim_silence(view(samples), kTrim, &report);

  REQUIRE(report.removed_head_samples == report.range.first);
  REQUIRE(report.removed_tail_samples == samples.size() - report.range.last_exclusive);
  REQUIRE(report.removed_head_samples + trimmed.size() + report.removed_tail_samples ==
          samples.size());
  REQUIRE(report.removed_head_samples > 0);
  REQUIRE(report.removed_tail_samples > 0);
}

// ---------------------------------------------------------------- stereo link

TEST_CASE("Stereo entrypoints with identical channels equal the mono path bit for bit",
          "[repair][stereo][hum]") {
  const std::vector<float> hum = hum50_fixture(0.0);
  const Audio hum_audio = view(hum);
  for (const DehumConfig& config : {kHum50Fixed, kHum50Adaptive}) {
    const uint32_t mono = digest(dehum(hum_audio, config));
    const DehumStereoResult linked = dehum_stereo(hum_audio, hum_audio, config);
    REQUIRE(digest(linked.left) == mono);
    REQUIRE(digest(linked.right) == mono);
    REQUIRE(linked.left_report.applied_fundamental_hz ==
            linked.right_report.applied_fundamental_hz);
  }

  const std::vector<float> crackle = crackle_fixture(0.0);
  const Audio crackle_audio = view(crackle);
  for (const DecrackleConfig& config : {kMedian, kWavelet}) {
    const uint32_t mono = digest(decrackle(crackle_audio, config));
    const DecrackleStereoResult linked = decrackle_stereo(crackle_audio, crackle_audio, config);
    REQUIRE(digest(linked.left) == mono);
    REQUIRE(digest(linked.right) == mono);
  }

  const std::vector<float> gated = gated_fixture(9600, 38400, 440.0);
  const Audio gated_audio = view(gated);
  const uint32_t mono_trim = digest(trim_silence(gated_audio, kTrim));
  const TrimSilenceStereoResult trimmed = trim_silence_stereo(gated_audio, gated_audio, kTrim);
  REQUIRE(digest(trimmed.left) == mono_trim);
  REQUIRE(digest(trimmed.right) == mono_trim);

  // A pair of different lengths is not a pair, and every stereo entrypoint says
  // so rather than truncating to the shorter channel.
  const std::vector<float> shorter_hum(hum.begin(), hum.end() - 1);
  const std::vector<float> shorter_crackle(crackle.begin(), crackle.end() - 1);
  const std::vector<float> shorter_gated(gated.begin(), gated.end() - 1);
  REQUIRE_THROWS(dehum_stereo(hum_audio, view(shorter_hum), kHum50Fixed));
  REQUIRE_THROWS(decrackle_stereo(crackle_audio, view(shorter_crackle), kMedian));
  REQUIRE_THROWS(trim_silence_stereo(gated_audio, view(shorter_gated), kTrim));
}

TEST_CASE("A shared tracked fundamental beats the shared mono transfer on a hum pair",
          "[repair][stereo][hum]") {
  // The same 50 Hz series at two levels. The quieter side's own search is the
  // one the programme material can pull, which is the case the shared tracker
  // exists for.
  const std::vector<float> clean_left = hum50_clean(0.0);
  const std::vector<float> clean_right = hum50_clean(0.35);
  std::vector<double> bed_left = sine_bed(48000, 0.0);
  std::vector<double> bed_right = sine_bed(48000, 0.35);
  plant_hum(bed_left, kPlantedHum50Hz, kPlantedHum50Db);
  plant_hum(bed_right, kPlantedHum50Hz, {-34.0, -40.0, -46.0, -52.0});
  const std::vector<float> dirty_left = quantize(bed_left);
  const std::vector<float> dirty_right = quantize(bed_right);
  const auto repair = [&](const Audio& in) { return dehum(in, kHum50Adaptive); };

  const auto transfer = shared_mono_transfer(dirty_left, dirty_right, repair);
  const auto split = independent(dirty_left, dirty_right, repair);
  const DehumStereoResult linked =
      dehum_stereo(view(dirty_left), view(dirty_right), kHum50Adaptive);
  const std::vector<float> linked_left = to_vector(linked.left);
  const std::vector<float> linked_right = to_vector(linked.right);

  const double untreated_image = image_error(dirty_left, dirty_right, clean_left, clean_right);
  const double transfer_image =
      image_error(transfer.first, transfer.second, clean_left, clean_right);
  const double split_image = image_error(split.first, split.second, clean_left, clean_right);
  const double linked_image = image_error(linked_left, linked_right, clean_left, clean_right);
  CAPTURE(untreated_image, transfer_image, split_image, linked_image);

  // The transfer applies one broadband per-sample gain, which cannot express a
  // notch: it leaves the pair further apart than it found them.
  REQUIRE(transfer_image > untreated_image);
  REQUIRE(linked_image < transfer_image);
  REQUIRE(linked_image < split_image);
  REQUIRE(linked_image < untreated_image);

  // Per-channel fidelity beats the transfer on both sides as well, so the image
  // was not bought with distortion.
  REQUIRE(rmse(linked_left, clean_left) < rmse(transfer.first, clean_left));
  REQUIRE(rmse(linked_right, clean_right) < rmse(transfer.second, clean_right));

  // The link acted, and this is where it shows without differencing the audio:
  // the two cascades sat on one frequency, while tracking the channels apart put
  // them a third of a hertz from each other -- an eighth of the notch's own
  // bandwidth at q = 20.
  REQUIRE(linked.left_report.applied_fundamental_hz == linked.right_report.applied_fundamental_hz);
  DehumReport alone_left;
  DehumReport alone_right;
  dehum(view(dirty_left), kHum50Adaptive, &alone_left);
  dehum(view(dirty_right), kHum50Adaptive, &alone_right);
  CAPTURE(alone_left.applied_fundamental_hz, alone_right.applied_fundamental_hz);
  REQUIRE(alone_left.applied_fundamental_hz != alone_right.applied_fundamental_hz);
  // The shared estimate is not one channel's answer imposed on the other: it
  // lies between the two the channels reach alone.
  REQUIRE(linked.left_report.applied_fundamental_hz > alone_left.applied_fundamental_hz);
  REQUIRE(linked.left_report.applied_fundamental_hz < alone_right.applied_fundamental_hz);
}

TEST_CASE("Per-channel decrackle beats the shared mono transfer on a crackle pair",
          "[repair][stereo][hum]") {
  const std::vector<float> clean_left = crackle_clean(0.0);
  const std::vector<float> clean_right = crackle_clean(0.35);
  const std::vector<float> dirty_left = crackle_fixture(0.0);
  const std::vector<float> dirty_right = crackle_fixture(0.35);
  const auto repair = [&](const Audio& in) { return decrackle(in, kMedian); };

  const auto transfer = shared_mono_transfer(dirty_left, dirty_right, repair);
  const auto split = independent(dirty_left, dirty_right, repair);
  const DecrackleStereoResult linked =
      decrackle_stereo(view(dirty_left), view(dirty_right), kMedian);
  const std::vector<float> linked_left = to_vector(linked.left);
  const std::vector<float> linked_right = to_vector(linked.right);

  // Per-channel is the frozen strategy here, and it is what the chain already
  // does: this entrypoint adds the reports and the length contract, not a
  // different sound. The digests say so.
  REQUIRE(digest(linked_left) == digest(split.first));
  REQUIRE(digest(linked_right) == digest(split.second));

  const double untreated_image = image_error(dirty_left, dirty_right, clean_left, clean_right);
  const double transfer_image =
      image_error(transfer.first, transfer.second, clean_left, clean_right);
  const double linked_image = image_error(linked_left, linked_right, clean_left, clean_right);
  CAPTURE(untreated_image, transfer_image, linked_image);

  // The same deviations are planted in both channels, so before repair the image
  // barely moves -- only the 24-bit grid separates the sides. The transfer
  // invents a shift five orders larger; treating the channels apart does not.
  REQUIRE(transfer_image > 1.0e5 * untreated_image);
  REQUIRE(linked_image < transfer_image);
  REQUIRE(rmse(linked_left, clean_left) < rmse(transfer.first, clean_left));
  REQUIRE(rmse(linked_right, clean_right) < rmse(transfer.second, clean_right));

  // Each channel reports its own work, and the reports are not copies.
  REQUIRE(linked.left_report.replaced_samples == planted_crackle_above(kMedian.threshold));
  REQUIRE(linked.right_report.replaced_samples == planted_crackle_above(kMedian.threshold));
  const DecrackleStereoResult wavelet_pair =
      decrackle_stereo(view(dirty_left), view(dirty_right), kWavelet);
  REQUIRE(wavelet_pair.left_report.shrunk_coefficients !=
          wavelet_pair.right_report.shrunk_coefficients);
}

TEST_CASE("Trim cuts a stereo pair to the union of its channels' ranges", "[repair][stereo][hum]") {
  // The right channel starts later and ends earlier than the left. A symmetric
  // pair cannot tell a shared range from two per-channel ones, since both answer
  // the same thing. This one only shows that one range reaches both channels --
  // which rule produced it is what the two cases below separate.
  const std::vector<float> left = gated_fixture(9600, 38400, 440.0);
  const std::vector<float> right = gated_fixture(14400, 33600, 330.0);

  const TrimSilenceStereoResult trimmed = trim_silence_stereo(view(left), view(right), kTrim);
  const TrimRange left_alone = detect_trim_range(left.data(), left.size(), kSampleRate, kTrim);
  const TrimRange right_alone = detect_trim_range(right.data(), right.size(), kSampleRate, kTrim);

  REQUIRE(trimmed.left_range.first == left_alone.first);
  REQUIRE(trimmed.right_range.first == right_alone.first);
  REQUIRE(trimmed.left_range.first != trimmed.right_range.first);

  // One range cuts both channels and it is the union of the two: the pair keeps
  // whatever either channel alone calls signal.
  REQUIRE(trimmed.report.range.first == std::min(left_alone.first, right_alone.first));
  REQUIRE(trimmed.report.range.last_exclusive ==
          std::max(left_alone.last_exclusive, right_alone.last_exclusive));
  REQUIRE(trimmed.left.size() == trimmed.right.size());
  REQUIRE(trimmed.left.size() == trimmed.report.range.last_exclusive - trimmed.report.range.first);

  // Unioning the ranges is not the same as cutting each channel by its own: that
  // returns two buffers of different lengths, which is no longer a stereo signal.
  const auto split =
      independent(left, right, [&](const Audio& in) { return trim_silence(in, kTrim); });
  REQUIRE(split.first.size() != split.second.size());
  REQUIRE(trimmed.left.size() == split.first.size());

  // The other stereo strategy in the tree cannot host a trim at all: it derives
  // a per-sample transfer ratio and rejects a repair that changed the length.
  REQUIRE_THROWS(
      shared_mono_transfer(left, right, [&](const Audio& in) { return trim_silence(in, kTrim); }));
}

TEST_CASE("Trim keeps an antiphase head that a downmix would read as silence",
          "[repair][stereo][hum]") {
  // The head is the same material inverted on the right, so the downmix is
  // exactly zero there while neither channel is. This is the fixture that
  // separates the two rules: a symmetric pair, or one differing only in how long
  // each channel is silent, answers the same under both.
  const std::vector<float> left = gated_fixture(9600, 38400, 440.0);
  std::vector<float> right = left;
  for (size_t i = 0; i < 19200; ++i) right[i] = -right[i];

  const std::vector<float> mixed = downmix(left, right);
  REQUIRE(std::all_of(mixed.begin() + 9600, mixed.begin() + 19200,
                      [](float sample) { return sample == 0.0f; }));
  REQUIRE(std::any_of(left.begin() + 9600, left.begin() + 19200,
                      [](float sample) { return std::abs(sample) > kTrim.threshold; }));

  const TrimRange left_alone = detect_trim_range(left.data(), left.size(), kSampleRate, kTrim);
  const TrimRange right_alone = detect_trim_range(right.data(), right.size(), kSampleRate, kTrim);
  const TrimSilenceStereoResult trimmed = trim_silence_stereo(view(left), view(right), kTrim);

  // Both channels carry material from 9600, so the pair is cut from 9600. A rule
  // reading the mix would cut from 19200 and destroy 9600 samples that are at
  // full level in both channels.
  REQUIRE(left_alone.first == right_alone.first);
  REQUIRE(trimmed.report.range.first == left_alone.first);
  REQUIRE(trimmed.report.range.first < 19200);
  REQUIRE(trimmed.report.range.last_exclusive == left_alone.last_exclusive);

  // A pair that cancels everywhere is still a pair of full-level channels, and
  // it survives intact. Its downmix is zero from end to end, so the rejected rule
  // would have returned two empty buffers.
  std::vector<float> inverted = left;
  for (float& sample : inverted) sample = -sample;
  const TrimSilenceStereoResult opposed = trim_silence_stereo(view(left), view(inverted), kTrim);
  REQUIRE(opposed.left.size() == left_alone.last_exclusive - left_alone.first);
  REQUIRE(opposed.right.size() == opposed.left.size());
  const std::vector<float> all_cancelled = downmix(left, inverted);
  REQUIRE(std::all_of(all_cancelled.begin(), all_cancelled.end(),
                      [](float sample) { return sample == 0.0f; }));

  // Emptying both channels is reserved for a pair that carries nothing, which is
  // the one case where the two rules agree.
  const std::vector<float> silence(left.size(), 0.0f);
  const TrimSilenceStereoResult nothing = trim_silence_stereo(view(silence), view(silence), kTrim);
  REQUIRE(nothing.left.size() == 0);
  REQUIRE(nothing.right.size() == 0);
  REQUIRE(nothing.report.range.first >= nothing.report.range.last_exclusive);
}

TEST_CASE("Trim keeps a lead-in only one channel carries", "[repair][stereo][hum]") {
  // A quiet lead-in on the left alone, loud enough to pass the gate there and
  // not after halving. The union keeps it; reading the mix would cut it, and the
  // material it would cut is not quiet in the channel that has it.
  const std::vector<float> body = gated_fixture(9600, 38400, 440.0);
  const std::vector<float> lead_in = gated_fixture(4800, 9600, 440.0, 0.0015);
  std::vector<float> left = body;
  for (size_t i = 0; i < left.size(); ++i) left[i] += lead_in[i];
  const std::vector<float> right = gated_fixture(9600, 38400, 330.0);

  const std::vector<float> mixed = downmix(left, right);
  REQUIRE(std::any_of(left.begin() + 4800, left.begin() + 9600,
                      [](float sample) { return std::abs(sample) > kTrim.threshold; }));
  REQUIRE(std::none_of(mixed.begin() + 4800, mixed.begin() + 9600,
                       [](float sample) { return std::abs(sample) > kTrim.threshold; }));

  const TrimRange left_alone = detect_trim_range(left.data(), left.size(), kSampleRate, kTrim);
  const TrimRange right_alone = detect_trim_range(right.data(), right.size(), kSampleRate, kTrim);
  const TrimSilenceStereoResult trimmed = trim_silence_stereo(view(left), view(right), kTrim);

  REQUIRE(left_alone.first < right_alone.first);
  REQUIRE(trimmed.report.range.first == std::min(left_alone.first, right_alone.first));
  REQUIRE(trimmed.report.range.first == left_alone.first);
  REQUIRE(trimmed.report.range.first < 9600);
  REQUIRE(trimmed.left.size() == trimmed.right.size());

  // Where the two rules part: the mix starts the cut at the body instead.
  const TrimRange from_downmix = detect_trim_range(mixed.data(), mixed.size(), kSampleRate, kTrim);
  REQUIRE(from_downmix.first >= 9600);
  REQUIRE(from_downmix.first > trimmed.report.range.first);
}
