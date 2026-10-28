/// @file voice_psola_test.cpp
/// @brief Pitch accuracy, formant placement, aliasing, latency and allocation freedom of the
///        PSOLA shifter, fed the true f0 of synthetic voices as tracker-shaped estimates.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "editing/voice_changer/psola_shifter.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "voicelab/metrics.h"
#include "voicelab/synthetic_voice.h"

namespace {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kSemitonesPerOctave;
using sonare::editing::voice_changer::F0Estimate;
using sonare::editing::voice_changer::kMaxVoices;
using sonare::editing::voice_changer::PsolaShifter;
using sonare::editing::voice_changer::VoiceTarget;
using sonare::editing::voice_changer::VoiceTargets;
using sonare::voicelab::VoiceSegment;
namespace vl = sonare::voicelab;

constexpr float kF0MinHz = 70.0f;
constexpr float kLevelDb = -20.0f;
constexpr int kVowelA = 0;
constexpr int kVowelCount = 5;  ///< /a e i o u/
constexpr int kBlock = 128;
// Tracker conventions: hop of round(2 ms * sr / 3) analysis samples, decimation FIR group delay.
constexpr int kDecimation = 3;
constexpr double kHopS = 0.002;
constexpr int kGroupDelay = 24;
// Evaluator frames whose window reaches an onset, release or the output start are left out.
constexpr double kGuardS = 0.08;
constexpr double kEvaluatorHopS = 0.010;

int hop_input(double sr) {
  return kDecimation * static_cast<int>(std::lround(kHopS * sr / kDecimation));
}

double cents(double a, double b) { return kCentsPerOctave * std::log2(a / b); }

double semitone_ratio(int semitones) {
  return std::exp2(static_cast<double>(semitones) / kSemitonesPerOctave);
}

VoiceSegment steady(double seconds, float f0, float vibrato_cents = 0.0f, float vibrato_hz = 0.0f) {
  return {seconds, f0, f0, vibrato_cents, vibrato_hz, true, kVowelA, kLevelDb, 10.0f};
}

VoiceSegment steady_vowel(double seconds, float f0, int vowel) {
  return {seconds, f0, f0, 0.0f, 0.0f, true, vowel, kLevelDb, 10.0f};
}

/// How the estimates reach the shifter.
enum class Arrival {
  /// In the block that contains the estimate's centre.
  AtCentre,
  /// Only once the input has passed the mark it describes by the decimation delay and a hop.
  Delayed,
};

VoiceTarget relative(double ratio) {
  VoiceTarget t;
  t.ratio = static_cast<float>(ratio);
  t.formant_alpha = static_cast<float>(ratio);
  t.active = true;
  return t;
}

VoiceTarget absolute(double ratio, float formant_factor) {
  VoiceTarget t;
  t.ratio = static_cast<float>(ratio);
  t.formant_alpha = formant_factor;
  t.active = true;
  return t;
}

struct Shifted {
  std::vector<std::vector<float>> out;
  int latency = 0;
};

/// Streams `x` through a fresh shifter with one estimate per tracker hop taken from `truth`.
Shifted shift(const std::vector<float>& x, const std::vector<float>& truth, double sr,
              const std::vector<VoiceTarget>& voices, Arrival arrival) {
  PsolaShifter shifter;
  shifter.prepare(sr, kBlock, kF0MinHz);
  const int latency = shifter.latency_samples();
  const int hop = hop_input(sr);
  std::vector<F0Estimate> estimates;
  for (std::size_t c = 0; c < x.size(); c += static_cast<std::size_t>(hop)) {
    const float f0 = truth[c];
    estimates.push_back({f0, 1.0f, f0 > 0.0f, static_cast<std::int64_t>(c)});
  }
  const std::int64_t lag = arrival == Arrival::Delayed ? latency + kGroupDelay + hop : 0;

  VoiceTargets targets;
  targets.count = static_cast<int>(voices.size());
  for (std::size_t i = 0; i < voices.size(); ++i) targets.voices[i] = voices[i];
  Shifted result;
  result.latency = latency;
  result.out.assign(voices.size(), std::vector<float>(x.size(), 0.0f));
  std::size_t next = 0;
  for (std::size_t pos = 0; pos < x.size(); pos += kBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kBlock, x.size() - pos));
    const auto last = static_cast<std::int64_t>(pos) + n - 1;
    const std::size_t first = next;
    while (next < estimates.size() && estimates[next].centre_sample + lag <= last) ++next;
    shifter.push_estimates(estimates.data() + first, static_cast<int>(next - first));
    float* out[kMaxVoices];
    for (std::size_t i = 0; i < voices.size(); ++i) out[i] = result.out[i].data() + pos;
    shifter.process(x.data() + pos, targets, out, n);
  }
  return result;
}

/// Absolute cent errors of the evaluator's output f0 against `expected_hz`, interior frames only.
std::vector<double> pitch_errors(const std::vector<float>& y, double sr, int latency,
                                 double expected_hz) {
  const std::vector<float> track = vl::estimate_f0_track(y.data(), y.size(), sr, kEvaluatorHopS);
  std::vector<double> errors;
  const double guard = kGuardS * sr;
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double centre = static_cast<double>(i) * kEvaluatorHopS * sr;
    if (centre < latency + guard || centre > static_cast<double>(y.size()) - guard) continue;
    // An unvoiced frame counts as a gross error.
    errors.push_back(track[i] > 0.0f ? std::abs(cents(track[i], expected_hz)) : 1e9);
  }
  return errors;
}

double quantile(std::vector<double> v, double q) {
  REQUIRE_FALSE(v.empty());
  const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

/// Pooled frames: median <= 3 cent, p95 <= 15 cent, gross (> 50 cent) errors <= 1%.
void check_pitch_pool(const std::vector<double>& errors) {
  const auto gross = std::count_if(errors.begin(), errors.end(), [](double e) { return e > 50.0; });
  const double median = quantile(errors, 0.5);
  const double p95 = quantile(errors, 0.95);
  INFO("frames " << errors.size() << " median " << median << " p95 " << p95 << " gross " << gross);
  CHECK(median <= 3.0);
  CHECK(p95 <= 15.0);
  CHECK(static_cast<double>(gross) <= 0.01 * static_cast<double>(errors.size()));
}

/// The steady interior of a 0.5 s render: from latency + 100 ms to 30 ms before the end.
struct Span {
  std::size_t begin;
  std::size_t size;
};

Span steady_span(std::size_t n, double sr, int latency) {
  const auto begin = static_cast<std::size_t>(latency) + static_cast<std::size_t>(0.1 * sr);
  return {begin, n - static_cast<std::size_t>(0.03 * sr) - begin};
}

float median_f0(const float* y, std::size_t n, double sr) {
  const std::vector<float> track = vl::estimate_f0_track(y, n, sr, kEvaluatorHopS);
  std::vector<double> voiced;
  for (float f : track) {
    if (f > 0.0f) voiced.push_back(f);
  }
  if (voiced.empty()) return 0.0f;
  return static_cast<float>(quantile(voiced, 0.5));
}

/// Standard deviation in cents of the evaluator's f0 over the interior frames of `y`.
double f0_spread_cents(const float* y, std::size_t n, double sr) {
  constexpr double kHop = 0.005;
  const std::vector<float> track = vl::estimate_f0_track(y, n, sr, kHop);
  std::vector<double> c;
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double centre = static_cast<double>(i) * kHop * sr;
    if (centre < 0.06 * sr || centre > static_cast<double>(n) - 0.06 * sr) continue;
    if (track[i] > 0.0f) c.push_back(kCentsPerOctave * std::log2(track[i]));
  }
  REQUIRE(c.size() > 10);
  double mean = 0.0;
  for (double v : c) mean += v;
  mean /= static_cast<double>(c.size());
  double acc = 0.0;
  for (double v : c) acc += (v - mean) * (v - mean);
  return std::sqrt(acc / static_cast<double>(c.size()));
}

/// One formant measurement: input f0, shift in semitones, formant factor (absolute mode).
struct FormantCell {
  float f0;
  int semitones;
  float factor;
};

/// F1-F3 of every vowel lie within `tolerance` of the known formants times the factor.
void check_formant_cell(const FormantCell& cell, double tolerance) {
  constexpr double kSr = 48000.0;
  for (int vowel = 0; vowel < kVowelCount; ++vowel) {
    const std::array<float, 3> known = vl::vowel_formants_hz(vowel);
    std::vector<float> truth;
    const std::vector<float> x =
        vl::synthesize_voice({steady_vowel(0.5, cell.f0, vowel)}, kSr, &truth);
    const Shifted y = shift(x, truth, kSr, {absolute(semitone_ratio(cell.semitones), cell.factor)},
                            Arrival::AtCentre);
    const Span span = steady_span(x.size(), kSr, y.latency);
    const std::array<float, 3> formants =
        vl::estimate_formants(y.out[0].data() + span.begin, span.size, kSr);
    for (std::size_t k = 0; k < 3; ++k) {
      const double expected = known[k] * cell.factor;
      INFO("vowel " << vowel << " F" << (k + 1) << " measured " << formants[k] << " expected "
                    << expected);
      CHECK(std::abs(formants[k] / expected - 1.0) <= tolerance);
    }
  }
}

}  // namespace

TEST_CASE("PSOLA latency is ceil(sr / f0_min)", "[voice_psola]") {
  PsolaShifter shifter;
  shifter.prepare(48000.0, kBlock, 70.0f);
  CHECK(shifter.latency_samples() == 686);
  shifter.prepare(44100.0, kBlock, 70.0f);
  CHECK(shifter.latency_samples() == 630);
  shifter.prepare(48000.0, kBlock, 200.0f);
  CHECK(shifter.latency_samples() == 240);
}

TEST_CASE("PSOLA output f0 follows the commanded shift across the f0 and shift grid",
          "[voice_psola]") {
  // Input f0 x shift where the output lies in [50, 1400] Hz, relative formants (alpha = ratio);
  // the same grid again with estimates arriving a group delay and a hop late.
  constexpr double kSr = 48000.0;
  std::vector<double> at_centre;
  std::vector<double> delayed;
  for (const float f0 : {82.0f, 110.0f, 220.0f, 440.0f, 660.0f}) {
    std::vector<float> truth;
    const std::vector<float> x = vl::synthesize_voice({steady(0.4, f0)}, kSr, &truth);
    for (const int semitones : {-12, -7, -3, 0, 3, 7, 12}) {
      const double ratio = semitone_ratio(semitones);
      const double expected = f0 * ratio;
      if (expected < 50.0 || expected > 1400.0) continue;
      for (const Arrival arrival : {Arrival::AtCentre, Arrival::Delayed}) {
        const Shifted y = shift(x, truth, kSr, {relative(ratio)}, arrival);
        const std::vector<double> e = pitch_errors(y.out[0], kSr, y.latency, expected);
        REQUIRE_FALSE(e.empty());
        INFO("f0 " << f0 << " shift " << semitones << " worst " << quantile(e, 1.0));
        std::vector<double>& pool = arrival == Arrival::AtCentre ? at_centre : delayed;
        pool.insert(pool.end(), e.begin(), e.end());
      }
    }
  }
  {
    INFO("estimates at their centre");
    check_pitch_pool(at_centre);
  }
  {
    INFO("estimates delayed by the decimation group delay and a hop");
    check_pitch_pool(delayed);
  }
}

TEST_CASE("PSOLA pitch accuracy holds at 44.1 kHz", "[voice_psola]") {
  constexpr double kSr = 44100.0;
  std::vector<double> pool;
  for (const float f0 : {110.0f, 440.0f}) {
    std::vector<float> truth;
    const std::vector<float> x = vl::synthesize_voice({steady(0.4, f0)}, kSr, &truth);
    for (const int semitones : {-12, -7, 0, 7, 12}) {
      const double ratio = semitone_ratio(semitones);
      const Shifted y = shift(x, truth, kSr, {relative(ratio)}, Arrival::Delayed);
      const std::vector<double> e = pitch_errors(y.out[0], kSr, y.latency, f0 * ratio);
      pool.insert(pool.end(), e.begin(), e.end());
    }
  }
  check_pitch_pool(pool);
}

TEST_CASE("PSOLA voices each follow their own ratio", "[voice_psola]") {
  constexpr double kSr = 48000.0;
  std::vector<float> truth;
  const std::vector<float> x = vl::synthesize_voice({steady(0.4, 220.0f)}, kSr, &truth);
  const std::vector<double> ratios = {1.0, semitone_ratio(4), semitone_ratio(7), 0.5};
  std::vector<VoiceTarget> voices;
  for (double r : ratios) voices.push_back(relative(r));
  const Shifted y = shift(x, truth, kSr, voices, Arrival::Delayed);
  for (std::size_t i = 0; i < ratios.size(); ++i) {
    INFO("voice " << i << " ratio " << ratios[i]);
    check_pitch_pool(pitch_errors(y.out[i], kSr, y.latency, 220.0 * ratios[i]));
  }
}

// Outputs of 55 Hz and 220 Hz from a 110 Hz voice keep F1-F3 within 5%.
TEST_CASE("PSOLA absolute formants within 5%: 110 Hz down an octave", "[voice_psola]") {
  check_formant_cell({110.0f, -12, 1.0f}, 0.05);
}

TEST_CASE("PSOLA absolute formants within 5%: 110 Hz up an octave", "[voice_psola]") {
  check_formant_cell({110.0f, 12, 1.0f}, 0.05);
}

TEST_CASE("PSOLA absolute formants within 5%: 110 Hz down an octave, factor 1.2", "[voice_psola]") {
  check_formant_cell({110.0f, -12, 1.2f}, 0.05);
}

// Known limit of pitch-synchronous overlap-add when output harmonics fall between the scaled
// input harmonics: a two-period grain reads the envelope there as a phase-dependent sum of its
// neighbours, which moves a formant by up to about 15%. The 20% bound guards against regression.
TEST_CASE("PSOLA formants between scaled harmonics within 20%: 220 Hz down an octave",
          "[voice_psola]") {
  check_formant_cell({220.0f, -12, 1.0f}, 0.20);
}

TEST_CASE("PSOLA formants between scaled harmonics within 20%: 220 Hz down an octave, factor 1.2",
          "[voice_psola]") {
  check_formant_cell({220.0f, -12, 1.2f}, 0.20);
}

TEST_CASE("PSOLA formants between scaled harmonics within 20%: 110 Hz up an octave, factor 1.2",
          "[voice_psola]") {
  check_formant_cell({110.0f, 12, 1.2f}, 0.20);
}

TEST_CASE("PSOLA absolute formants above a 220 Hz output follow the stretched envelope",
          "[voice_psola]") {
  // 220 Hz up an octave: log-spectral distance to the input envelope stretched by the factor.
  constexpr double kSr = 48000.0;
  for (int vowel = 0; vowel < kVowelCount; ++vowel) {
    std::vector<float> truth;
    const std::vector<float> x =
        vl::synthesize_voice({steady_vowel(0.5, 220.0f, vowel)}, kSr, &truth);
    for (const float factor : {1.0f, 1.2f}) {
      const Shifted y = shift(x, truth, kSr, {absolute(2.0, factor)}, Arrival::AtCentre);
      const Span span = steady_span(x.size(), kSr, y.latency);
      // The input's steady span of the same length is the reference envelope.
      const auto ref_begin = static_cast<std::size_t>(0.1 * kSr);
      const float lsd = vl::log_spectral_distance_db(
          x.data() + ref_begin, y.out[0].data() + span.begin, span.size, kSr, factor);
      INFO("vowel " << vowel << " factor " << factor << " LSD " << lsd << " dB");
      CHECK(lsd <= 3.0f);
    }
  }
}

TEST_CASE("PSOLA +12 semitones keeps aliasing at most -50 dB", "[voice_psola]") {
  // 220 Hz /a/ up an octave, relative (alpha = 2) and absolute with formant_factor 1.2.
  for (const double sr : {48000.0, 44100.0}) {
    std::vector<float> truth;
    const std::vector<float> x = vl::synthesize_voice({steady(0.5, 220.0f)}, sr, &truth);
    for (const VoiceTarget& target : {relative(2.0), absolute(2.0, 1.2f)}) {
      const Shifted y = shift(x, truth, sr, {target}, Arrival::AtCentre);
      const Span span = steady_span(x.size(), sr, y.latency);
      const float* out = y.out[0].data() + span.begin;
      const float f0 = median_f0(out, span.size, sr);
      const float db = vl::inharmonic_energy_db(out, span.size, sr, f0);
      INFO("sr " << sr << " alpha " << target.formant_alpha << " f0 " << f0 << " inharmonic " << db
                 << " dB");
      CHECK(std::abs(cents(f0, 440.0)) <= 3.0);
      CHECK(db <= -50.0f);
    }
  }
}

TEST_CASE("PSOLA anti-alias tables suppress folding of a full-band source", "[voice_psola]") {
  // A sawtooth with partials up to 0.95 Nyquist, up an octave: with the fs / (2 alpha) tables the
  // off-harmonic energy reads about -36 dB (alpha 2) and -40 dB (alpha 1.2), reading through the
  // unfiltered table alone about -23 and -30 dB. The bound sits between the two.
  constexpr float kBoundDb = -33.0f;
  constexpr double kF0 = 220.0;
  for (const double sr : {48000.0, 44100.0}) {
    const auto n = static_cast<std::size_t>(0.5 * sr);
    std::vector<double> acc(n, 0.0);
    for (int k = 1; k * kF0 < 0.95 * 0.5 * sr; ++k) {
      const double w = sonare::constants::kTwoPiD * k * kF0 / sr;
      for (std::size_t i = 0; i < n; ++i) acc[i] += std::sin(w * static_cast<double>(i)) / k;
    }
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(0.05 * acc[i]);
    const std::vector<float> truth(n, static_cast<float>(kF0));
    for (const VoiceTarget& target : {relative(2.0), absolute(2.0, 1.2f)}) {
      const Shifted y = shift(x, truth, sr, {target}, Arrival::AtCentre);
      const Span span = steady_span(n, sr, y.latency);
      const float* out = y.out[0].data() + span.begin;
      const float f0 = median_f0(out, span.size, sr);
      const float db = vl::inharmonic_energy_db(out, span.size, sr, f0);
      INFO("sr " << sr << " alpha " << target.formant_alpha << " inharmonic " << db << " dB");
      CHECK(db <= kBoundDb);
    }
  }
}

TEST_CASE("PSOLA measured latency matches latency_samples()", "[voice_psola]") {
  // Alternating voiced and white-noise segments: waveform correlation within one sample at
  // ratio 1, envelope correlation within 48 samples at +-7 semitones.
  for (const double sr : {48000.0, 44100.0}) {
    std::vector<VoiceSegment> segments;
    for (int i = 0; i < 4; ++i) {
      segments.push_back({0.15, 220.0f, 220.0f, 0.0f, 0.0f, i % 2 == 0, kVowelA, kLevelDb, 5.0f});
    }
    std::vector<float> truth;
    const std::vector<float> x = vl::synthesize_voice(segments, sr, &truth);
    constexpr int kMaxLag = 1500;
    for (const Arrival arrival : {Arrival::AtCentre, Arrival::Delayed}) {
      const Shifted unity = shift(x, truth, sr, {relative(1.0)}, arrival);
      const int wave =
          vl::estimate_latency_samples(x.data(), unity.out[0].data(), x.size(), kMaxLag, false);
      INFO("sr " << sr << " delayed " << (arrival == Arrival::Delayed) << " reported "
                 << unity.latency << " waveform " << wave);
      CHECK(std::abs(wave - unity.latency) <= 1);
      // At ratio 1 the output is the delayed input; what remains sits at the voicing switches
      // (about -55 dB with estimates at their centre, -46 dB delayed).
      double signal = 0.0;
      double error = 0.0;
      const auto lag = static_cast<std::size_t>(unity.latency);
      for (std::size_t i = lag; i < x.size(); ++i) {
        const double d = unity.out[0][i] - x[i - lag];
        error += d * d;
        signal += static_cast<double>(x[i - lag]) * x[i - lag];
      }
      const double residual_db = 10.0 * std::log10(error / signal);
      INFO("residual " << residual_db << " dB");
      CHECK(residual_db <= -40.0);
      for (const int semitones : {-7, 7}) {
        const Shifted y = shift(x, truth, sr, {relative(semitone_ratio(semitones))}, arrival);
        const int env =
            vl::estimate_latency_samples(x.data(), y.out[0].data(), x.size(), kMaxLag, true);
        INFO("shift " << semitones << " envelope " << env);
        CHECK(std::abs(env - y.latency) <= 48);
      }
    }
  }
}

TEST_CASE("PSOLA passes unvoiced input through unshifted", "[voice_psola]") {
  // Unvoiced frames take no pitch change; with alpha = 1 the output is the delayed input.
  constexpr double kSr = 48000.0;
  std::vector<float> truth;
  const std::vector<float> x = vl::synthesize_voice(
      {{0.3, 0.0f, 0.0f, 0.0f, 0.0f, false, kVowelA, kLevelDb, 5.0f}}, kSr, &truth);
  const Shifted y = shift(x, truth, kSr, {absolute(semitone_ratio(7), 1.0f)}, Arrival::AtCentre);
  double signal = 0.0;
  double error = 0.0;
  for (std::size_t i = static_cast<std::size_t>(y.latency); i < x.size(); ++i) {
    const double d = y.out[0][i] - x[i - static_cast<std::size_t>(y.latency)];
    error += d * d;
    signal += static_cast<double>(x[i]) * x[i];
  }
  const double db = 10.0 * std::log10(error / signal);
  INFO("error " << db << " dB");
  CHECK(db <= -60.0);
}

TEST_CASE("PSOLA with absolute_weight 1 holds a fixed pitch through vibrato", "[voice_psola]") {
  // +-50 cent 5.5 Hz vibrato in, a fixed target out: output spread at most 3 cent.
  constexpr double kSr = 48000.0;
  constexpr float kVibratoCents = 50.0f;
  std::vector<float> truth;
  const std::vector<float> x =
      vl::synthesize_voice({steady(0.6, 220.0f, kVibratoCents, 5.5f)}, kSr, &truth);
  // The evaluator under-reads vibrato, so it must still read 0.7 of the input's true spread.
  const double input_spread = f0_spread_cents(x.data(), x.size(), kSr);
  INFO("input spread " << input_spread << " cent");
  CHECK(input_spread >= 0.7 * kVibratoCents / std::sqrt(2.0));
  for (const Arrival arrival : {Arrival::AtCentre, Arrival::Delayed}) {
    VoiceTarget target = absolute(1.0, 1.0f);
    target.target_hz = 261.63f;
    target.absolute_weight = 1.0f;
    const Shifted y = shift(x, truth, kSr, {target}, arrival);
    const auto begin = static_cast<std::size_t>(y.latency);
    const double spread = f0_spread_cents(y.out[0].data() + begin, x.size() - begin, kSr);
    const float f0 = median_f0(y.out[0].data() + begin, x.size() - begin, kSr);
    INFO("delayed " << (arrival == Arrival::Delayed) << " output spread " << spread << " cent, f0 "
                    << f0);
    CHECK(spread <= 3.0);
    CHECK(std::abs(cents(f0, 261.63)) <= 3.0);
  }
}

TEST_CASE("PSOLA push_estimates and process perform no heap allocation", "[voice_psola][rt]") {
  constexpr double kSr = 48000.0;
  std::vector<VoiceSegment> segments;
  for (int i = 0; i < 6; ++i) {
    segments.push_back({0.05, 180.0f, 240.0f, 30.0f, 5.0f, i % 2 == 0, i % 5, kLevelDb, 5.0f});
  }
  std::vector<float> truth;
  const std::vector<float> x = vl::synthesize_voice(segments, kSr, &truth);
  PsolaShifter shifter;
  shifter.prepare(kSr, kBlock, kF0MinHz);
  const int hop = hop_input(kSr);
  VoiceTargets targets;
  targets.count = kMaxVoices;
  for (int i = 0; i < kMaxVoices; ++i) {
    targets.voices[i] = relative(semitone_ratio(4 * i - 5));
    targets.voices[i].target_hz = 200.0f;
    targets.voices[i].absolute_weight = 0.5f * static_cast<float>(i % 2);
  }
  std::vector<float> storage(static_cast<std::size_t>(kMaxVoices * kBlock));
  float* out[kMaxVoices];
  for (int i = 0; i < kMaxVoices; ++i) out[i] = storage.data() + i * kBlock;
  std::vector<F0Estimate> pending(static_cast<std::size_t>(kBlock / hop + 2));

  const auto run_block = [&](std::size_t pos, std::int64_t* next_centre) {
    int count = 0;
    while (*next_centre <= static_cast<std::int64_t>(pos) + kBlock - 1) {
      const float f0 = truth[static_cast<std::size_t>(*next_centre)];
      pending[static_cast<std::size_t>(count++)] = {f0, 1.0f, f0 > 0.0f, *next_centre};
      *next_centre += hop;
    }
    shifter.push_estimates(pending.data(), count);
    shifter.process(x.data() + pos, targets, out, kBlock);
  };
  std::int64_t next_centre = 0;
  run_block(0, &next_centre);
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    for (std::size_t pos = kBlock; pos + kBlock <= x.size(); pos += kBlock) {
      // Voices drop out and come back, and the formant factor moves, while counting.
      targets.voices[2].active = (pos / kBlock) % 7 != 3;
      targets.voices[3].formant_alpha = 0.6f + 0.1f * static_cast<float>((pos / kBlock) % 9);
      run_block(pos, &next_centre);
    }
    allocations = guard.count();
  }
  CHECK(allocations == 0);
  for (float v : storage) REQUIRE(std::isfinite(v));
}
