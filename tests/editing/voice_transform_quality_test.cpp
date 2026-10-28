/// @file voice_transform_quality_test.cpp
/// @brief Whole-chain quality of the voice transformer: latency, pitch accuracy, formant
///        placement, aliasing, stability over the pairwise configuration table, pitch correction,
///        harmony, MIDI notes, the vocoder and allocation freedom on the audio thread.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "editing/voice_changer/channel_vocoder.h"
#include "editing/voice_changer/isp_limiter.h"
#include "editing/voice_changer/realtime.h"
#include "support/alloc_guard.h"
#include "util/constants.h"
#include "util/exception.h"
#include "voicelab/metrics.h"
#include "voicelab/synthetic_voice.h"

namespace {

using namespace sonare::editing::voice_changer;
using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kTwoPiD;
using sonare::voicelab::VoiceSegment;
namespace vl = sonare::voicelab;

constexpr int kBlock = 128;
constexpr int kVowelA = 0;
constexpr int kVowelCount = 5;  ///< /a e i o u/
constexpr float kLevelDb = -20.0f;
// Evaluator frames whose window reaches an onset, a release or the output start are left out.
constexpr double kGuardS = 0.08;
constexpr double kEvaluatorHopS = 0.010;

double cents(double a, double b) { return kCentsPerOctave * std::log2(a / b); }

double semitone_ratio(double semitones) { return std::exp2(semitones / kSemitonesPerOctave); }

double midi_hz(double note) { return kA4Hz * std::exp2((note - kMidiA4) / kSemitonesPerOctave); }

VoiceSegment steady(double seconds, float f0, int vowel = kVowelA, float vibrato_cents = 0.0f,
                    float vibrato_hz = 0.0f) {
  return {seconds, f0, f0, vibrato_cents, vibrato_hz, true, vowel, kLevelDb, 10.0f};
}

/// Alternating voiced (220 Hz /a/) and equal-RMS white-noise segments with 5 ms edges.
std::vector<float> alternating(double sr, int segments, double segment_s) {
  std::vector<VoiceSegment> parts;
  for (int i = 0; i < segments; ++i) {
    parts.push_back({segment_s, 220.0f, 220.0f, 0.0f, 0.0f, i % 2 == 0, kVowelA, kLevelDb, 5.0f});
  }
  return vl::synthesize_voice(parts, sr);
}

/// Every stage except the voice stage set to transparent, the inter-sample-peak limiter on.
RealtimeVoiceChangerConfig transparent(float semitones = 0.0f) {
  RealtimeVoiceChangerConfig c;
  c.wet_mix = 1.0f;
  c.retune = {semitones, 1.0f, 0};
  c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  c.eq = {20.0f, 0.0f, 0.0f, 0.0f};
  c.gate = {-90.0f, 1.0f, 50.0f, 0.0f};
  c.compressor = {0.0f, 1.0f, 1.0f, 50.0f, 0.0f};
  c.deesser = {7000.0f, -6.0f, 1.0f, 0.0f};
  c.reverb.mix = 0.0f;
  c.limiter.ceiling_db = -1.0f;
  return c;
}

struct Rendered {
  std::vector<float> out;
  int latency = 0;
  std::uint32_t discards = 0;
  bool all_processed = true;
};

/// Streams `x` through a fresh mono changer. `before_block(changer, position)` runs ahead of each
/// block, so note events land on block boundaries as the note API specifies.
Rendered render(const RealtimeVoiceChangerConfig& config, const std::vector<float>& x, double sr,
                const std::vector<float>* carrier = nullptr,
                const std::function<void(RealtimeVoiceChanger&, std::size_t)>& before_block = {}) {
  RealtimeVoiceChanger changer(config);
  changer.prepare(sr, kBlock, 1);
  Rendered r;
  r.latency = changer.latency_samples();
  r.out.assign(x.size(), 0.0f);
  for (std::size_t pos = 0; pos < x.size(); pos += kBlock) {
    const int n = static_cast<int>(std::min<std::size_t>(kBlock, x.size() - pos));
    if (before_block) before_block(changer, pos);
    const bool processed =
        carrier != nullptr
            ? changer.process_block(x.data() + pos, carrier->data() + pos, r.out.data() + pos, n)
            : changer.process_block(x.data() + pos, r.out.data() + pos, n);
    r.all_processed = r.all_processed && processed;
  }
  r.discards = changer.non_finite_discard_count();
  return r;
}

double quantile(std::vector<double> v, double q) {
  REQUIRE_FALSE(v.empty());
  const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
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

/// The steady interior of a render: from latency + 100 ms to 30 ms before the end.
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

/// Standard deviation in cents of the evaluator's f0 over frames from `from_s` to 60 ms before
/// the end of `y`.
double f0_spread_cents(const float* y, std::size_t n, double sr, double from_s) {
  constexpr double kHop = 0.005;
  const std::vector<float> track = vl::estimate_f0_track(y, n, sr, kHop);
  std::vector<double> c;
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double centre = static_cast<double>(i) * kHop * sr;
    if (centre < from_s * sr || centre > static_cast<double>(n) - 0.06 * sr) continue;
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

/// Frequency of the strongest spectral line within +-60 cent of `near_hz` (Hann window,
/// parabolic interpolation of the log magnitude).
double line_near(const float* y, std::size_t n, double sr, double near_hz) {
  int n_fft = 1;
  while (n_fft < static_cast<int>(n)) n_fft *= 2;
  std::vector<float> frame(static_cast<std::size_t>(n_fft), 0.0f);
  for (std::size_t i = 0; i < n; ++i) {
    const double w =
        0.5 - 0.5 * std::cos(kTwoPiD * static_cast<double>(i) / static_cast<double>(n));
    frame[i] = static_cast<float>(w * y[i]);
  }
  sonare::FFT fft(n_fft);
  std::vector<std::complex<float>> spec(static_cast<std::size_t>(fft.n_bins()));
  fft.forward(frame.data(), spec.data());
  const double bin_hz = sr / n_fft;
  const int lo = std::max(1, static_cast<int>(near_hz * semitone_ratio(-0.6) / bin_hz));
  const int hi =
      std::min(fft.n_bins() - 2, static_cast<int>(near_hz * semitone_ratio(0.6) / bin_hz));
  int best = lo;
  for (int b = lo; b <= hi; ++b) {
    if (std::abs(spec[static_cast<std::size_t>(b)]) >
        std::abs(spec[static_cast<std::size_t>(best)])) {
      best = b;
    }
  }
  const auto log_mag = [&](int b) {
    return std::log(static_cast<double>(std::abs(spec[static_cast<std::size_t>(b)])) +
                    sonare::constants::kEpsilon);
  };
  const double a = log_mag(best - 1);
  const double m = log_mag(best);
  const double c = log_mag(best + 1);
  const double denom = a - 2.0 * m + c;
  const double delta = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
  return (static_cast<double>(best) + delta) * bin_hz;
}

/// A harmonic tone (1/k partials up to 4 kHz) whose pitch follows `midi_at(t)`.
std::vector<float> pitched_tone(double sr, double seconds,
                                const std::function<double(double)>& midi_at) {
  const auto n = static_cast<std::size_t>(seconds * sr);
  std::vector<float> x(n, 0.0f);
  double phase = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double f0 = midi_hz(midi_at(static_cast<double>(i) / sr));
    phase += f0 / sr;
    phase -= std::floor(phase);
    double acc = 0.0;
    for (int k = 1; k * f0 < 4000.0; ++k) acc += std::sin(kTwoPiD * k * phase) / k;
    x[i] = static_cast<float>(0.05 * acc);
  }
  return x;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------------------------

TEST_CASE("Transformer latency is max(shifter, vocoder group delay) plus the limiter look-ahead",
          "[voice_transform][latency]") {
  for (const double sr : {44100.0, 48000.0}) {
    for (const float f0_min : {70.0f, 200.0f}) {
      for (const int bands : {20, 32}) {
        CAPTURE(sr, f0_min, bands);
        ChannelVocoder oracle;
        oracle.prepare(sr, kBlock, bands);
        IspLimiter isp;
        isp.prepare(sr, kBlock);
        const int shifter = static_cast<int>(std::ceil(sr / static_cast<double>(f0_min)));
        const int expected =
            std::max(shifter, oracle.group_delay_samples()) + isp.latency_samples();

        RealtimeVoiceChangerConfig config = transparent();
        config.voice_engine = {PitchEngine::Psola, f0_min};
        config.vocoder.bands = bands;
        RealtimeVoiceChanger changer(config);
        changer.prepare(sr, kBlock, 1);
        CHECK(changer.latency_samples() == expected);

        // Turning the vocoder on, at construction or live, leaves the latency alone.
        RealtimeVoiceChangerConfig with_vocoder = config;
        with_vocoder.vocoder.enabled = true;
        RealtimeVoiceChanger vocoded(with_vocoder);
        vocoded.prepare(sr, kBlock, 1);
        CHECK(vocoded.latency_samples() == expected);
        changer.set_config(with_vocoder);
        CHECK(changer.latency_samples() == expected);

        // At ratio 1 the output is the input delayed by the reported latency.
        const std::vector<float> x = alternating(sr, 4, 0.15);
        const Rendered y = render(config, x, sr);
        const int measured =
            vl::estimate_latency_samples(x.data(), y.out.data(), x.size(), 1500, false);
        INFO("reported " << y.latency << " measured " << measured);
        CHECK(std::abs(measured - y.latency) <= 1);
      }
    }
  }
}

TEST_CASE("Transformer default latency is 717 samples at 48 kHz and 659 at 44.1 kHz",
          "[voice_transform][latency]") {
  const RealtimeVoiceChangerConfig config;
  REQUIRE(config.voice_engine.engine == PitchEngine::Psola);
  REQUIRE(config.voice_engine.f0_min_hz == 70.0f);
  REQUIRE(config.vocoder.bands == 20);
  REQUIRE(config.limiter.enable_isp_limiter);
  RealtimeVoiceChanger changer(config);
  changer.prepare(48000.0, kBlock, 1);
  CHECK(changer.latency_samples() == 717);
  CHECK(changer.latency_samples() <= 720);
  changer.prepare(44100.0, kBlock, 1);
  CHECK(changer.latency_samples() == 659);
}

TEST_CASE("Transformer envelope latency holds at +-7 semitones", "[voice_transform][latency]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = alternating(kSr, 4, 0.15);
  for (const float semitones : {-7.0f, 7.0f}) {
    CAPTURE(semitones);
    const Rendered y = render(transparent(semitones), x, kSr);
    const int envelope = vl::estimate_latency_samples(x.data(), y.out.data(), x.size(), 1500, true);
    INFO("reported " << y.latency << " envelope " << envelope);
    CHECK(std::abs(envelope - y.latency) <= 48);
  }
}

// ---------------------------------------------------------------------------------------------
// Pitch accuracy, formants and aliasing through the whole chain (tracker included)
// ---------------------------------------------------------------------------------------------

namespace {

void check_chain_pitch(const std::vector<float>& f0s, const std::vector<int>& shifts) {
  constexpr double kSr = 48000.0;
  std::vector<double> pool;
  for (const float f0 : f0s) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.4, f0)}, kSr);
    for (const int semitones : shifts) {
      const double expected = f0 * semitone_ratio(semitones);
      if (expected < 50.0 || expected > 1400.0) continue;
      const Rendered y = render(transparent(static_cast<float>(semitones)), x, kSr);
      const std::vector<double> e = pitch_errors(y.out, kSr, y.latency, expected);
      REQUIRE_FALSE(e.empty());
      INFO("f0 " << f0 << " shift " << semitones << " worst " << quantile(e, 1.0));
      pool.insert(pool.end(), e.begin(), e.end());
    }
  }
  check_pitch_pool(pool);
}

/// One formant measurement through the chain in absolute mode.
struct FormantCell {
  float f0;
  int semitones;
  float factor;
};

RealtimeVoiceChangerConfig absolute_config(int semitones, float factor) {
  RealtimeVoiceChangerConfig c = transparent(static_cast<float>(semitones));
  c.formant_mode = FormantMode::Absolute;
  c.formant.factor = factor;
  return c;
}

void check_formant_cell(const FormantCell& cell, double tolerance) {
  constexpr double kSr = 48000.0;
  for (int vowel = 0; vowel < kVowelCount; ++vowel) {
    const std::array<float, 3> known = vl::vowel_formants_hz(vowel);
    const std::vector<float> x = vl::synthesize_voice({steady(0.5, cell.f0, vowel)}, kSr);
    const Rendered y = render(absolute_config(cell.semitones, cell.factor), x, kSr);
    const Span span = steady_span(x.size(), kSr, y.latency);
    const std::array<float, 3> formants =
        vl::estimate_formants(y.out.data() + span.begin, span.size, kSr);
    for (std::size_t k = 0; k < 3; ++k) {
      const double expected = known[k] * cell.factor;
      INFO("f0 " << cell.f0 << " shift " << cell.semitones << " factor " << cell.factor << " vowel "
                 << vowel << " F" << (k + 1) << " measured " << formants[k] << " expected "
                 << expected);
      CHECK(std::abs(formants[k] / expected - 1.0) <= tolerance);
    }
  }
}

void check_formant_lsd(float factor) {
  // 220 Hz up an octave: log-spectral distance to the input envelope stretched by the factor.
  constexpr double kSr = 48000.0;
  for (int vowel = 0; vowel < kVowelCount; ++vowel) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.5, 220.0f, vowel)}, kSr);
    const Rendered y = render(absolute_config(12, factor), x, kSr);
    const Span span = steady_span(x.size(), kSr, y.latency);
    const auto ref_begin = static_cast<std::size_t>(0.1 * kSr);
    const float lsd = vl::log_spectral_distance_db(x.data() + ref_begin, y.out.data() + span.begin,
                                                   span.size, kSr, factor);
    INFO("vowel " << vowel << " factor " << factor << " LSD " << lsd << " dB");
    CHECK(lsd <= 3.0f);
  }
}

}  // namespace

TEST_CASE("Transformer output f0 follows the commanded shift", "[voice_transform][pitch]") {
  check_chain_pitch({110.0f, 440.0f}, {-12, 0, 7});
}

TEST_CASE("Transformer output f0 follows the commanded shift over the full grid",
          "[.][slow][voice_transform][pitch]") {
  check_chain_pitch({82.0f, 110.0f, 220.0f, 440.0f, 660.0f}, {-12, -7, -3, 0, 3, 7, 12});
}

TEST_CASE("Transformer absolute formants within 5%: 110 Hz down an octave",
          "[voice_transform][formant]") {
  check_formant_cell({110.0f, -12, 1.0f}, 0.05);
}

TEST_CASE("Transformer absolute formants above a 220 Hz output follow the stretched envelope",
          "[voice_transform][formant]") {
  check_formant_lsd(1.0f);
}

TEST_CASE("Transformer absolute formants over the remaining cells",
          "[.][slow][voice_transform][formant]") {
  check_formant_cell({110.0f, 12, 1.0f}, 0.05);
  check_formant_cell({110.0f, -12, 1.2f}, 0.05);
  // Output harmonics between the scaled input harmonics: the overlap-add limit.
  check_formant_cell({220.0f, -12, 1.0f}, 0.20);
  check_formant_cell({220.0f, -12, 1.2f}, 0.20);
  check_formant_cell({110.0f, 12, 1.2f}, 0.20);
  check_formant_lsd(1.2f);
}

TEST_CASE("Transformer +12 semitones keeps aliasing at most -50 dB", "[voice_transform][alias]") {
  for (const double sr : {48000.0, 44100.0}) {
    const std::vector<float> x = vl::synthesize_voice({steady(0.5, 220.0f)}, sr);
    for (const bool absolute : {false, true}) {
      const RealtimeVoiceChangerConfig config =
          absolute ? absolute_config(12, 1.2f) : transparent(12.0f);
      const Rendered y = render(config, x, sr);
      const Span span = steady_span(x.size(), sr, y.latency);
      const float* out = y.out.data() + span.begin;
      const float f0 = median_f0(out, span.size, sr);
      const float db = vl::inharmonic_energy_db(out, span.size, sr, f0);
      INFO("sr " << sr << " absolute " << absolute << " f0 " << f0 << " inharmonic " << db
                 << " dB");
      CHECK(std::abs(cents(f0, 440.0)) <= 3.0);
      CHECK(db <= -50.0f);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// The pairwise configuration table
// ---------------------------------------------------------------------------------------------

namespace {

enum class Carrier { None, Internal, External };

struct PairwiseRow {
  PitchEngine engine;
  PitchCorrectionMode mode;
  int harmony_voices;
  bool vocoder;
  Carrier carrier;
  CharacterType character;
  ReverbVariation variation;
  FormantMode formant;
};

constexpr PitchEngine kGrain = PitchEngine::Grain;
constexpr PitchEngine kPsola = PitchEngine::Psola;
constexpr PitchCorrectionMode kOff = PitchCorrectionMode::Off;
constexpr PitchCorrectionMode kAuto = PitchCorrectionMode::Auto;
constexpr PitchCorrectionMode kRobot = PitchCorrectionMode::Robot;
constexpr FormantMode kRel = FormantMode::Relative;
constexpr FormantMode kAbs = FormantMode::Absolute;
using CT = CharacterType;
using RV = ReverbVariation;

// Two-way coverage of engine x mode x harmony voices x vocoder x carrier x character x
// reverb variation x formant mode, under the constraints that grain takes no correction, harmony
// or vocoder and that a carrier exists exactly when the vocoder is on.
constexpr std::array<PairwiseRow, 27> kPairwise = {{
    {kPsola, kOff, 0, false, Carrier::None, CT::Radio, RV::TempoDelay, kAbs},
    {kGrain, kOff, 0, false, Carrier::None, CT::Megaphone, RV::Echo, kRel},
    {kPsola, kRobot, 3, true, Carrier::Internal, CT::Strobo, RV::Reverb, kRel},
    {kPsola, kAuto, 1, false, Carrier::None, CT::None, RV::Reverb, kAbs},
    {kPsola, kOff, 1, false, Carrier::None, CT::Strobo, RV::DubEcho, kAbs},
    {kPsola, kRobot, 1, true, Carrier::External, CT::BbdChorus, RV::Echo, kAbs},
    {kPsola, kOff, 3, true, Carrier::Internal, CT::Megaphone, RV::Reverb, kAbs},
    {kPsola, kAuto, 3, true, Carrier::External, CT::None, RV::TempoDelay, kRel},
    {kGrain, kOff, 0, false, Carrier::None, CT::BbdChorus, RV::Reverb, kAbs},
    {kPsola, kAuto, 0, true, Carrier::Internal, CT::Strobo, RV::DubEcho, kRel},
    {kPsola, kRobot, 3, false, Carrier::None, CT::Radio, RV::DubEcho, kAbs},
    {kPsola, kAuto, 0, true, Carrier::External, CT::Radio, RV::Reverb, kRel},
    {kPsola, kRobot, 0, true, Carrier::Internal, CT::None, RV::Echo, kRel},
    {kPsola, kAuto, 1, false, Carrier::None, CT::Megaphone, RV::Reverb, kAbs},
    {kPsola, kOff, 1, false, Carrier::None, CT::Strobo, RV::TempoDelay, kRel},
    {kPsola, kOff, 3, false, Carrier::None, CT::None, RV::DubEcho, kRel},
    {kPsola, kOff, 3, true, Carrier::External, CT::Megaphone, RV::DubEcho, kRel},
    {kPsola, kAuto, 3, true, Carrier::External, CT::BbdChorus, RV::Reverb, kRel},
    {kPsola, kOff, 1, true, Carrier::Internal, CT::Radio, RV::Echo, kRel},
    {kPsola, kAuto, 3, false, Carrier::None, CT::Strobo, RV::Echo, kAbs},
    {kPsola, kRobot, 3, true, Carrier::Internal, CT::BbdChorus, RV::DubEcho, kRel},
    {kPsola, kRobot, 3, false, Carrier::None, CT::Megaphone, RV::TempoDelay, kAbs},
    {kPsola, kRobot, 1, true, Carrier::External, CT::Strobo, RV::DubEcho, kRel},
    {kPsola, kAuto, 3, true, Carrier::Internal, CT::BbdChorus, RV::TempoDelay, kAbs},
    {kGrain, kOff, 0, false, Carrier::None, CT::Strobo, RV::DubEcho, kAbs},
    {kGrain, kOff, 0, false, Carrier::None, CT::Radio, RV::Reverb, kAbs},
    {kGrain, kOff, 0, false, Carrier::None, CT::None, RV::TempoDelay, kAbs},
}};

/// The default chain shifted up three semitones, configured as @p row says.
RealtimeVoiceChangerConfig row_config(const PairwiseRow& row) {
  RealtimeVoiceChangerConfig c;
  c.retune.semitones = 3.0f;
  c.voice_engine.engine = row.engine;
  c.pitch_correction.mode = row.mode;
  c.harmony.voices = row.harmony_voices;
  c.vocoder.enabled = row.vocoder;
  c.vocoder.carrier_source = row.carrier == Carrier::External ? VocoderCarrierSource::External
                                                              : VocoderCarrierSource::Internal;
  c.character.type = row.character;
  c.reverb_variation.variation = row.variation;
  c.formant_mode = row.formant;
  return c;
}

/// A band-limited 110 Hz sawtooth used as the external carrier.
std::vector<float> saw_carrier(double sr, std::size_t n) {
  std::vector<float> c(n, 0.0f);
  constexpr double kF0 = 110.0;
  for (int k = 1; k * kF0 < 0.45 * sr; ++k) {
    const double w = kTwoPiD * k * kF0 / sr;
    for (std::size_t i = 0; i < n; ++i) c[i] += static_cast<float>(0.1 * std::sin(w * i) / k);
  }
  return c;
}

void check_pairwise_stability(int segments) {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = alternating(kSr, segments, 0.2);
  const std::vector<float> carrier = saw_carrier(kSr, x.size());
  for (std::size_t r = 0; r < kPairwise.size(); ++r) {
    const PairwiseRow& row = kPairwise[r];
    CAPTURE(r);
    const RealtimeVoiceChangerConfig config = row_config(row);
    const Rendered y = render(config, x, kSr, row.carrier == Carrier::None ? nullptr : &carrier);
    CHECK(y.all_processed);
    bool finite = true;
    for (float v : y.out) finite = finite && std::isfinite(v);
    CHECK(finite);
    CHECK(y.discards == 0u);
    // The radio's sample-and-hold staircase sits at the click detector's threshold by design, so
    // a radio row is checked for clicks with its character off and everything else unchanged.
    if (row.character == CharacterType::Radio) {
      RealtimeVoiceChangerConfig plain = config;
      plain.character.type = CharacterType::None;
      const Rendered z = render(plain, x, kSr, row.carrier == Carrier::None ? nullptr : &carrier);
      CHECK(vl::count_clicks(z.out.data(), z.out.size(), kSr) == 0);
    } else {
      CHECK(vl::count_clicks(y.out.data(), y.out.size(), kSr) == 0);
    }
  }
}

}  // namespace

TEST_CASE("Every pairwise configuration stays finite and click-free",
          "[voice_transform][stability]") {
  check_pairwise_stability(5);
}

TEST_CASE("Every pairwise configuration stays finite and click-free over 10 s",
          "[.][slow][voice_transform][stability]") {
  check_pairwise_stability(50);
}

TEST_CASE("Every pairwise configuration processes and takes notes without allocating",
          "[voice_transform][rt]") {
  constexpr double kSr = 48000.0;
  for (std::size_t r = 0; r < kPairwise.size(); ++r) {
    const PairwiseRow& row = kPairwise[r];
    CAPTURE(r);
    RealtimeVoiceChangerConfig config = row_config(row);
    RealtimeVoiceChanger changer(config);
    changer.prepare(kSr, kBlock, 2);
    const std::vector<float> voice = alternating(kSr, 2, 0.05);
    const std::vector<float> carrier = saw_carrier(kSr, static_cast<std::size_t>(kBlock));
    const float* carrier_ptr = row.carrier == Carrier::None ? nullptr : carrier.data();
    std::array<float, kBlock> left{};
    std::array<float, kBlock> right{};
    std::array<float, kBlock> mono{};
    const auto fill = [&](int b) {
      for (int i = 0; i < kBlock; ++i) {
        const float v = voice[static_cast<std::size_t>(b * kBlock + i) % voice.size()];
        left[static_cast<std::size_t>(i)] = v;
        right[static_cast<std::size_t>(i)] = -v;
      }
    };
    float* planes[] = {left.data(), right.data()};
    fill(0);
    changer.process_block(planes, 2, carrier_ptr, kBlock);
    RealtimeVoiceChangerConfig edited = config;
    edited.retune.semitones = -2.0f;
    std::size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      for (int b = 1; b < 24; ++b) {
        fill(b);
        if (b == 4) changer.note_on(60, 0.8f);
        if (b == 6) changer.note_on(64, 1.0f);
        if (b == 8) changer.set_config(edited);
        if (b == 10) changer.note_off(60);
        if (b == 14) changer.all_notes_off();
        if (b % 2 == 0) {
          changer.process_block(planes, 2, carrier_ptr, kBlock);
        } else if (carrier_ptr != nullptr) {
          changer.process_block(left.data(), carrier_ptr, mono.data(), kBlock);
        } else {
          changer.process_block(left.data(), mono.data(), kBlock);
        }
      }
      allocations = guard.count();
    }
    CHECK(allocations == 0u);
  }
}

// ---------------------------------------------------------------------------------------------
// Pitch correction
// ---------------------------------------------------------------------------------------------

namespace {

RealtimeVoiceChangerConfig corrected(PitchCorrectionMode mode) {
  RealtimeVoiceChangerConfig c = transparent();
  c.pitch_correction.mode = mode;
  c.pitch_correction.key_root = 0;
  c.pitch_correction.scale = ScaleKind::Major;
  return c;
}

/// Changes of the nearest of C4 / D4 across the voiced frames of `y` from `from_s` on.
int note_switches(const std::vector<float>& y, double sr, double from_s) {
  constexpr double kHop = 0.005;
  const std::vector<float> track = vl::estimate_f0_track(y.data(), y.size(), sr, kHop);
  int switches = 0;
  int previous = -1;
  for (std::size_t i = 0; i < track.size(); ++i) {
    const double centre = static_cast<double>(i) * kHop * sr;
    if (centre < from_s * sr || centre > static_cast<double>(y.size()) - 0.06 * sr) continue;
    if (!(track[i] > 0.0f)) continue;
    const double note = kMidiA4 + kSemitonesPerOctave * std::log2(track[i] / kA4Hz);
    const int nearest = note < 61.0 ? 60 : 62;
    if (previous >= 0 && nearest != previous) ++switches;
    previous = nearest;
  }
  return switches;
}

/// Auto pitch over a glide around the C4 / D4 midpoint with +-10 cent, 5.5 Hz vibrato: the output
/// switches at most `max_switches` times while the input itself crosses at least `min_plain`
/// times, for both vibrato phases.
void check_auto_hysteresis(double seconds, double glide_offset_s, int max_switches, int min_plain) {
  constexpr double kSr = 48000.0;
  for (int phase = 0; phase < 2; ++phase) {
    CAPTURE(phase);
    const auto midi_at = [&](double t) {
      const double vib = kTwoPiD * 5.5 * t;
      return 61.0 + 0.6 * std::sin(kTwoPiD * 0.1 * (t - glide_offset_s)) +
             0.1 * (phase == 0 ? std::sin(vib) : std::cos(vib));
    };
    const std::vector<float> x = pitched_tone(kSr, seconds, midi_at);
    RealtimeVoiceChangerConfig config = corrected(PitchCorrectionMode::Auto);
    config.pitch_correction.speed_ms = 0.0f;
    config.pitch_correction.amount = 1.0f;
    const Rendered y = render(config, x, kSr);
    const int switches = note_switches(y.out, kSr, 0.1 + y.latency / kSr);
    const int plain = note_switches(x, kSr, 0.1);
    INFO("switches " << switches << " plain " << plain);
    CHECK(switches <= max_switches);
    CHECK(plain >= min_plain);
  }
}

}  // namespace

TEST_CASE("Auto pitch lands on the scale note within 20 ms of the first voiced output",
          "[voice_transform][auto]") {
  constexpr double kSr = 48000.0;
  const auto sharp = static_cast<float>(220.0 * std::exp2(40.0 / kCentsPerOctave));
  const std::vector<float> x = vl::synthesize_voice({steady(0.5, sharp)}, kSr);
  RealtimeVoiceChangerConfig config = corrected(PitchCorrectionMode::Auto);
  config.pitch_correction.speed_ms = 0.0f;
  config.pitch_correction.amount = 1.0f;
  const Rendered y = render(config, x, kSr);
  constexpr double kHop = 0.001;
  const std::vector<float> track = vl::estimate_f0_track(y.out.data(), y.out.size(), kSr, kHop);
  std::size_t first = track.size();
  for (std::size_t i = 0; i < track.size(); ++i) {
    if (track[i] > 0.0f && static_cast<double>(i) * kHop * kSr >= y.latency) {
      first = i;
      break;
    }
  }
  REQUIRE(first < track.size());
  const auto settled = first + static_cast<std::size_t>(0.020 / kHop);
  const auto last = track.size() - static_cast<std::size_t>(kGuardS / kHop);
  REQUIRE(settled < last);
  double worst = 0.0;
  for (std::size_t i = settled; i < last; ++i) {
    worst = std::max(worst, track[i] > 0.0f ? std::abs(cents(track[i], 220.0)) : 1e9);
  }
  INFO("first voiced output frame at " << first * kHop << " s, worst " << worst << " cent");
  CHECK(worst <= 5.0);
}

TEST_CASE("Auto pitch switches at most once across one midpoint crossing",
          "[voice_transform][auto]") {
  // Two seconds around one crossing of the 0.1 Hz glide: half of a round trip.
  check_auto_hysteresis(2.0, 1.0, 1, 4);
}

TEST_CASE("Auto pitch switches at most twice per round trip of the glide",
          "[.][slow][voice_transform][auto]") {
  check_auto_hysteresis(10.0, 0.0, 2, 8);
}

TEST_CASE("Robot holds the output f0 steady under vibrato", "[voice_transform][robot]") {
  constexpr double kSr = 48000.0;
  constexpr float kVibratoCents = 50.0f;
  const std::vector<float> x =
      vl::synthesize_voice({steady(2.0, 220.0f, kVowelA, kVibratoCents, 5.5f)}, kSr);
  const Rendered y = render(corrected(PitchCorrectionMode::Robot), x, kSr);
  const double output_spread =
      f0_spread_cents(y.out.data(), y.out.size(), kSr, 0.3 + y.latency / kSr);
  // Sensitivity control: the same evaluator reads at least 0.7 of the input's true spread.
  const double true_spread = kVibratoCents / std::sqrt(2.0);
  const double input_spread = f0_spread_cents(x.data(), x.size(), kSr, 0.3);
  INFO("output spread " << output_spread << " input spread " << input_spread << " true "
                        << true_spread);
  CHECK(input_spread >= 0.7 * true_spread);
  CHECK(output_spread <= 3.0);
}

// ---------------------------------------------------------------------------------------------
// Harmony and MIDI notes
// ---------------------------------------------------------------------------------------------

TEST_CASE("A single harmony voice sits on the scale note its step names",
          "[voice_transform][harmony]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(0.6, 220.0f)}, kSr);  // A3
  // C major from A3: two steps up is C4, four steps up is E4.
  const std::array<std::pair<int, double>, 2> cases = {{{2, 60.0}, {4, 64.0}}};
  for (const auto& [step, note] : cases) {
    CAPTURE(step);
    RealtimeVoiceChangerConfig config = corrected(PitchCorrectionMode::Off);
    config.harmony.voices = 1;
    config.harmony.steps[0] = step;
    config.harmony.lead_level = 0.0f;
    config.harmony.level = 1.0f;
    config.harmony.detune_cents = 0.0f;
    const Rendered y = render(config, x, kSr);
    const Span span = steady_span(x.size(), kSr, y.latency);
    const float f0 = median_f0(y.out.data() + span.begin, span.size, kSr);
    INFO("measured " << f0 << " expected " << midi_hz(note));
    CHECK(std::abs(cents(f0, midi_hz(note))) <= 5.0);
  }
}

TEST_CASE("Held MIDI notes become the voices, each on its note", "[voice_transform][midi]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(0.6, 220.0f)}, kSr);
  const std::vector<std::vector<int>> chords = {{60, 64, 67}, {60, 64, 67, 71}};
  for (const std::vector<int>& notes : chords) {
    CAPTURE(notes.size());
    RealtimeVoiceChangerConfig config = corrected(PitchCorrectionMode::Off);
    config.harmony.voices = 3;
    config.harmony.level = 1.0f;
    config.harmony.lead_level = 1.0f;
    config.harmony.detune_cents = 0.0f;
    const Rendered y =
        render(config, x, kSr, nullptr, [&](RealtimeVoiceChanger& c, std::size_t pos) {
          if (pos != 0) return;
          for (int note : notes) REQUIRE(c.note_on(note, 1.0f));
        });
    const Span span = steady_span(x.size(), kSr, y.latency);
    for (int note : notes) {
      const double line = line_near(y.out.data() + span.begin, span.size, kSr, midi_hz(note));
      INFO("note " << note << " line " << line << " expected " << midi_hz(note));
      CHECK(std::abs(cents(line, midi_hz(note))) <= 5.0);
    }
  }
}

TEST_CASE("Note API refuses bad events, a grain chain and a full queue",
          "[voice_transform][midi]") {
  CHECK(RealtimeVoiceChanger::is_valid_note(0));
  CHECK(RealtimeVoiceChanger::is_valid_note(127));
  CHECK_FALSE(RealtimeVoiceChanger::is_valid_note(-1));
  CHECK_FALSE(RealtimeVoiceChanger::is_valid_note(128));
  CHECK(RealtimeVoiceChanger::is_valid_velocity(1.0f));
  CHECK_FALSE(RealtimeVoiceChanger::is_valid_velocity(0.0f));
  CHECK_FALSE(RealtimeVoiceChanger::is_valid_velocity(1.5f));
  CHECK_FALSE(RealtimeVoiceChanger::is_valid_velocity(std::numeric_limits<float>::quiet_NaN()));

  RealtimeVoiceChanger unprepared;
  CHECK_FALSE(unprepared.note_on(60, 1.0f));

  RealtimeVoiceChanger psola;
  psola.prepare(48000.0, kBlock, 1);
  CHECK_FALSE(psola.note_on(128, 1.0f));
  CHECK_FALSE(psola.note_on(60, 0.0f));
  CHECK_FALSE(psola.note_off(-1));
  // Unheld note off and a repeated note on are accepted.
  CHECK(psola.note_off(61));
  std::array<float, kBlock> in{};
  std::array<float, kBlock> out{};
  CHECK(psola.process_block(in.data(), out.data(), kBlock));
  int accepted = 0;
  while (accepted <= kNoteQueueCapacity && psola.note_on(60, 1.0f)) ++accepted;
  CHECK(accepted == kNoteQueueCapacity);
  CHECK_FALSE(psola.note_on(60, 1.0f));
  psola.all_notes_off();
  CHECK(psola.process_block(in.data(), out.data(), kBlock));
  CHECK(psola.note_on(60, 1.0f));

  RealtimeVoiceChangerConfig grain_config;
  grain_config.voice_engine.engine = PitchEngine::Grain;
  RealtimeVoiceChanger grain(grain_config);
  grain.prepare(48000.0, kBlock, 1);
  CHECK_FALSE(grain.note_on(60, 1.0f));
  CHECK_FALSE(grain.note_off(60));
  grain.all_notes_off();
}

TEST_CASE("Releasing the notes follows midi.release and all_notes_off returns to the input",
          "[voice_transform][midi]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(1.2, 220.0f)}, kSr);
  const auto release_at = static_cast<std::size_t>(0.5 * kSr) / kBlock * kBlock;
  for (const MidiRelease release : {MidiRelease::Hold, MidiRelease::Input}) {
    for (const bool all_off : {false, true}) {
      CAPTURE(static_cast<int>(release), all_off);
      RealtimeVoiceChangerConfig config = corrected(PitchCorrectionMode::Off);
      config.midi.release = release;
      const Rendered y =
          render(config, x, kSr, nullptr, [&](RealtimeVoiceChanger& c, std::size_t pos) {
            if (pos == 0) REQUIRE(c.note_on(64, 1.0f));
            if (pos == release_at) {
              if (all_off) {
                c.all_notes_off();
              } else {
                REQUIRE(c.note_off(64));
              }
            }
          });
      const auto held_begin =
          static_cast<std::size_t>(y.latency) + static_cast<std::size_t>(0.1 * kSr);
      const auto held_size = release_at + static_cast<std::size_t>(y.latency) - held_begin;
      const float held = median_f0(y.out.data() + held_begin, held_size, kSr);
      CHECK(std::abs(cents(held, midi_hz(64))) <= 5.0);
      const std::size_t after_begin =
          release_at + static_cast<std::size_t>(y.latency) + static_cast<std::size_t>(0.1 * kSr);
      const float after =
          median_f0(y.out.data() + after_begin,
                    y.out.size() - after_begin - static_cast<std::size_t>(0.03 * kSr), kSr);
      // Hold keeps the last note after a note off; all_notes_off clears it whatever the policy.
      const double expected = release == MidiRelease::Hold && !all_off ? midi_hz(64) : 220.0;
      INFO("held " << held << " after " << after << " expected " << expected);
      CHECK(std::abs(cents(after, expected)) <= 5.0);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Vocoder
// ---------------------------------------------------------------------------------------------

TEST_CASE("Vocoder output takes the pitch of its internal carrier note",
          "[voice_transform][vocoder]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(0.6, 220.0f)}, kSr);
  RealtimeVoiceChangerConfig config = transparent();
  config.vocoder.enabled = true;
  const Rendered y = render(config, x, kSr, nullptr, [](RealtimeVoiceChanger& c, std::size_t pos) {
    if (pos == 0) REQUIRE(c.note_on(60, 1.0f));
  });
  const Span span = steady_span(x.size(), kSr, y.latency);
  const float f0 = median_f0(y.out.data() + span.begin, span.size, kSr);
  INFO("measured " << f0 << " expected " << midi_hz(60.0));
  CHECK(std::abs(cents(f0, midi_hz(60.0))) <= 2.0);
}

TEST_CASE("Vocoder envelope stays aligned with the reported latency",
          "[voice_transform][vocoder]") {
  constexpr double kSr = 48000.0;
  // 150 ms vowel bursts separated by 150 ms of silence, 5 ms edges.
  std::vector<float> x = vl::synthesize_voice({steady(1.2, 220.0f)}, kSr);
  const auto period = static_cast<std::size_t>(0.3 * kSr);
  constexpr auto edge = static_cast<std::size_t>(0.005 * kSr);
  const std::size_t half = period / 2;
  const auto ramp = [](std::size_t k) {
    return 0.5 - 0.5 * std::cos(kTwoPiD * 0.5 * static_cast<double>(k) / static_cast<double>(edge));
  };
  for (std::size_t i = 0; i < x.size(); ++i) {
    const std::size_t p = i % period;
    double g = 0.0;
    if (p < half) {
      g = 1.0;
      if (p < edge) g = ramp(p);
      if (half - p < edge) g = ramp(half - p);
    }
    x[i] *= static_cast<float>(g);
  }
  RealtimeVoiceChangerConfig config = transparent();
  config.vocoder.enabled = true;
  const Rendered y = render(config, x, kSr, nullptr, [](RealtimeVoiceChanger& c, std::size_t pos) {
    if (pos == 0) REQUIRE(c.note_on(57, 1.0f));
  });
  const int envelope = vl::estimate_latency_samples(x.data(), y.out.data(), x.size(), 3000, true);
  const double tolerance = (config.vocoder.attack_ms + 1.0) * 1e-3 * kSr;
  INFO("reported " << y.latency << " envelope " << envelope << " tolerance " << tolerance);
  CHECK(std::abs(envelope - y.latency) <= tolerance);
}

TEST_CASE("External carrier is required by the carrier-less process and ignored when internal",
          "[voice_transform][vocoder]") {
  constexpr double kSr = 48000.0;
  const std::vector<float> x = vl::synthesize_voice({steady(0.2, 220.0f)}, kSr);
  const std::vector<float> carrier = saw_carrier(kSr, x.size());

  RealtimeVoiceChangerConfig external = transparent();
  external.vocoder.enabled = true;
  external.vocoder.carrier_source = VocoderCarrierSource::External;
  RealtimeVoiceChanger changer(external);
  changer.prepare(kSr, kBlock, 1);
  constexpr float kSentinel = -0.25f;
  std::vector<float> out(static_cast<std::size_t>(kBlock), kSentinel);
  CHECK_FALSE(changer.process_block(x.data(), out.data(), kBlock));
  for (float v : out) REQUIRE(v == kSentinel);
  std::vector<float> left(x.begin(), x.begin() + kBlock);
  const std::vector<float> left_before = left;
  float* planes[] = {left.data()};
  CHECK_FALSE(changer.process_block(planes, 1, kBlock));
  CHECK(left == left_before);
  CHECK(changer.process_block(x.data(), carrier.data(), out.data(), kBlock));

  // An internal carrier never reads the carrier buffer.
  RealtimeVoiceChangerConfig internal = transparent();
  internal.vocoder.enabled = true;
  const Rendered with = render(internal, x, kSr, &carrier);
  const Rendered without = render(internal, x, kSr);
  CHECK(with.all_processed);
  CHECK(with.out == without.out);
}

TEST_CASE("A grain chain refuses pitch correction, harmony and the vocoder",
          "[voice_transform][config]") {
  const auto refused = [](const RealtimeVoiceChangerConfig& config) {
    std::string error;
    const bool ok = validate_realtime_voice_changer_config(config, nullptr, &error);
    INFO(error);
    return !ok && error.find("grain") != std::string::npos;
  };
  RealtimeVoiceChangerConfig grain;
  grain.voice_engine.engine = PitchEngine::Grain;
  CHECK(validate_realtime_voice_changer_config(grain, nullptr, nullptr));
  RealtimeVoiceChangerConfig c = grain;
  c.pitch_correction.mode = PitchCorrectionMode::Auto;
  CHECK(refused(c));
  c = grain;
  c.pitch_correction.mode = PitchCorrectionMode::Robot;
  CHECK(refused(c));
  c = grain;
  c.harmony.voices = 1;
  CHECK(refused(c));
  c = grain;
  c.vocoder.enabled = true;
  CHECK(refused(c));
  c = grain;
  c.vocoder.enabled = true;
  try {
    RealtimeVoiceChanger changer(c);
    FAIL("expected a refusal");
  } catch (const sonare::SonareException& e) {
    CHECK(e.code() == sonare::ErrorCode::InvalidParameter);
  }
}
