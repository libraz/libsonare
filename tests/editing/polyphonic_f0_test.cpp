#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/polyphony/f0_salience.h"
#include "editing/polyphony/multi_f0.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare::editing::polyphony;

namespace {

constexpr int kSampleRate = 44100;
/// The default framing every fixture position and frame rate is reasoned in.
constexpr int kNfft = 4096;
constexpr int kHopLength = 512;
/// The longer window the header names for bass material, at its own eighth hop.
constexpr int kBassNfft = 8192;
constexpr int kBassHop = 1024;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// Equal-tempered Cmaj7 above middle C: no two tones an octave apart, so a
/// resolved voice cannot be an octave ghost of another.
constexpr float kChordMid[] = {261.6256f, 329.6276f, 391.9954f, 493.8833f};
/// The same chord two octaves down, under the register the default framing
/// reaches.
constexpr float kChordLow[] = {65.4064f, 82.4069f, 97.9989f, 123.4708f};

/// Four F0s chosen so no pair is a low-order ratio: the harmonic-sum tests read
/// one voice's salience without another voice's partials landing on it.
constexpr float kSpreadVoices[] = {200.0f, 274.6f, 388.6f, 520.0f};

// --- Cent arithmetic, as an oracle ----------------------------------------

float cents_between(float from_hz, float to_hz) {
  return sonare::constants::kCentsPerOctave * std::log2(to_hz / from_hz);
}

float hz_up(float from_hz, float cents) {
  return from_hz * std::pow(2.0f, cents / sonare::constants::kCentsPerOctave);
}

/// @brief The error code a call throws, or Ok when it does not throw.
template <typename Fn>
sonare::ErrorCode code_of(Fn&& fn) {
  try {
    fn();
  } catch (const sonare::SonareException& error) {
    return error.code();
  }
  return sonare::ErrorCode::Ok;
}

int argmax(const float* values, int n) {
  int best = 0;
  for (int i = 1; i < n; ++i) {
    if (values[i] > values[best]) best = i;
  }
  return best;
}

float median_of(std::vector<float> values) {
  REQUIRE(!values.empty());
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

int median_of(std::vector<int> values) {
  REQUIRE(!values.empty());
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// --- Cent-spectrum columns built by hand -----------------------------------

/// @brief A cent axis at the struct's own defaults, reaching @p max_hz.
/// @details The documented derivation: the span in bins rounded up, plus one, so
///          the last bin sits at or above @p max_hz rather than under it.
CentAxis spectrum_axis_for(float max_hz) {
  CentAxis axis;
  axis.n_bins = static_cast<int>(std::ceil(axis.bin_at(max_hz))) + 1;
  return axis;
}

/// @brief Writes one harmonic series at 1/h amplitude into a cent column.
/// @details Partial positions come from the documented
///          f_h = h * f0 * sqrt(1 + B * h^2) and from CentAxis::bin_at, never
///          from SalienceKernel, so a kernel that places its partials wrongly
///          cannot make this column agree with it. Each partial is split between
///          its two neighbouring bins the way compute_cent_spectrum splits one.
void add_series(std::vector<float>& column, const CentAxis& axis, float f0_hz, int n_partials,
                float amplitude, float inharmonicity = 0.0f) {
  for (int h = 1; h <= n_partials; ++h) {
    const float harmonic = static_cast<float>(h);
    const float hz = harmonic * f0_hz * std::sqrt(1.0f + inharmonicity * harmonic * harmonic);
    const float bin = axis.bin_at(hz);
    const int lo = static_cast<int>(std::floor(bin));
    const float frac = bin - static_cast<float>(lo);
    const float level = amplitude / harmonic;
    if (lo >= 0 && lo < axis.n_bins) column[static_cast<size_t>(lo)] += level * (1.0f - frac);
    if (lo + 1 >= 0 && lo + 1 < axis.n_bins) column[static_cast<size_t>(lo + 1)] += level * frac;
  }
}

/// @brief A column holding one series per entry of @p f0s, all at one level.
std::vector<float> voiced_column(const CentAxis& axis, const std::vector<float>& f0s,
                                 int n_partials = 12) {
  std::vector<float> column(static_cast<size_t>(axis.n_bins), 0.0f);
  for (const float f0 : f0s) add_series(column, axis, f0, n_partials, 1.0f);
  return column;
}

float column_sum(const std::vector<float>& column, int n_bins) {
  double total = 0.0;
  for (int i = 0; i < n_bins; ++i) total += static_cast<double>(column[static_cast<size_t>(i)]);
  return static_cast<float>(total);
}

// --- Synthetic audio -------------------------------------------------------

/// @brief Adds a steady harmonic tone, partials at 1/h amplitude.
/// @details The fixed per-harmonic phase keeps the partials from summing into an
///          impulse train; a steady tone's magnitude spectrum does not depend on
///          it, and its instantaneous frequency does not either.
void add_tone(std::vector<float>& into, float f0_hz, float amplitude, int n_partials) {
  const double nyquist = 0.5 * kSampleRate;
  for (int h = 1; h <= n_partials; ++h) {
    const double hz = static_cast<double>(h) * static_cast<double>(f0_hz);
    if (hz >= nyquist) break;
    const double phase = 0.37 * static_cast<double>(h) * static_cast<double>(h);
    const float level = amplitude / static_cast<float>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += level * static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz *
                                                         static_cast<double>(i) / kSampleRate +
                                                     phase));
    }
  }
}

/// @brief Adds a harmonic tone whose F0 is modulated sinusoidally in cents.
/// @details The fundamental's phase is integrated sample by sample and partial h
///          takes h times it, which is what a harmonic voice with a moving F0
///          actually does -- multiplying a fixed frequency by a moving factor
///          would put a spurious phase step under every partial.
void add_vibrato_tone(std::vector<float>& into, float centre_hz, float depth_cents, float rate_hz,
                      float amplitude, int n_partials) {
  std::vector<double> phase(into.size(), 0.0);
  double accumulated = 0.0;
  for (size_t i = 0; i < into.size(); ++i) {
    phase[i] = accumulated;
    const double t = static_cast<double>(i) / kSampleRate;
    const double f0 =
        static_cast<double>(centre_hz) *
        std::pow(2.0, static_cast<double>(depth_cents) *
                          std::sin(sonare::constants::kTwoPiD * static_cast<double>(rate_hz) * t) /
                          sonare::constants::kCentsPerOctave);
    accumulated += sonare::constants::kTwoPiD * f0 / kSampleRate;
  }
  const double nyquist = 0.5 * kSampleRate;
  for (int h = 1; h <= n_partials; ++h) {
    // The vibrato moves the partial by at most a few tens of cents, so a
    // fundamental-based ceiling with a little headroom keeps every partial real.
    if (static_cast<double>(h) * static_cast<double>(centre_hz) * 1.1 >= nyquist) break;
    const float level = amplitude / static_cast<float>(h);
    const double offset = 0.37 * static_cast<double>(h) * static_cast<double>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += level * static_cast<float>(std::sin(static_cast<double>(h) * phase[i] + offset));
    }
  }
}

/// @brief Deterministic uniform noise in [-1, 1).
std::vector<float> noise_samples(uint32_t seed, size_t samples) {
  std::vector<float> output(samples, 0.0f);
  uint32_t state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < samples; ++i) {
    state = state * 1664525u + 1013904223u;
    output[i] = static_cast<float>(state >> 9) / 4194304.0f - 1.0f;
  }
  return output;
}

sonare::Audio audio_of(std::vector<float> samples) {
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief A sustained chord, every tone at the same level.
sonare::Audio chord_audio(const float* tones, size_t count, float seconds, int n_partials) {
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  for (size_t i = 0; i < count; ++i) add_tone(samples, tones[i], 0.12f, n_partials);
  return audio_of(std::move(samples));
}

float peak_of(const std::vector<float>& samples) {
  float worst = 0.0f;
  for (const float sample : samples) worst = std::max(worst, std::abs(sample));
  return worst;
}

// --- Analysis shorthands ---------------------------------------------------

CentSpectrum cent_spectrum_of(const sonare::Audio& audio, const CentSpectrumConfig& config = {},
                              const sonare::StftConfig& stft = polyphony_stft_defaults()) {
  return compute_cent_spectrum(sonare::Spectrogram::compute(audio, stft), config);
}

/// @brief Chord tones a ridge holds over at least @p min_share of the frames.
int resolved_tones(const MultiF0Track& track, const float* tones, size_t count,
                   float tolerance_cents, float min_share) {
  int found = 0;
  for (size_t i = 0; i < count; ++i) {
    for (const F0Ridge& ridge : track.ridges) {
      const float share =
          static_cast<float>(ridge.f0_hz.size()) / static_cast<float>(std::max(track.n_frames, 1));
      if (share < min_share) continue;
      if (std::abs(cents_between(tones[i], ridge.median_hz)) <= tolerance_cents) {
        ++found;
        break;
      }
    }
  }
  return found;
}

/// @brief Ridges holding over at least @p min_share of the analysis.
std::vector<F0Ridge> long_ridges(const MultiF0Track& track, float min_share) {
  std::vector<F0Ridge> kept;
  for (const F0Ridge& ridge : track.ridges) {
    const float share =
        static_cast<float>(ridge.f0_hz.size()) / static_cast<float>(std::max(track.n_frames, 1));
    if (share >= min_share) kept.push_back(ridge);
  }
  return kept;
}

int ridges_alive_at(const MultiF0Track& track, int frame) {
  int alive = 0;
  for (const F0Ridge& ridge : track.ridges) {
    if (ridge.frame_start <= frame && frame < ridge.frame_end()) ++alive;
  }
  return alive;
}

// --- Hand-built candidate frames -------------------------------------------

F0Candidate candidate(float hz, float salience) {
  F0Candidate value;
  value.f0_hz = hz;
  value.salience = salience;
  value.harmonic_share = 0.5f;
  return value;
}

/// @brief @p count frames each holding one candidate at @p hz.
std::vector<std::vector<F0Candidate>> steady_frames(float hz, int count, float salience = 1.0f) {
  return std::vector<std::vector<F0Candidate>>(static_cast<size_t>(count),
                                               std::vector<F0Candidate>{candidate(hz, salience)});
}

void append_frames(std::vector<std::vector<F0Candidate>>& into,
                   const std::vector<std::vector<F0Candidate>>& more) {
  into.insert(into.end(), more.begin(), more.end());
}

/// @brief Per-frame cent move of a @p depth_cents vibrato at @p rate_hz.
/// @details The header's own arithmetic: the modulation's peak slope divided by
///          the frame rate. Kept here so the framing claims are checked against
///          a formula rather than against a remembered number.
float vibrato_cents_per_frame(float depth_cents, float rate_hz, int hop_length) {
  return depth_cents * sonare::constants::kTwoPi * rate_hz * static_cast<float>(hop_length) /
         static_cast<float>(kSampleRate);
}

/// @brief One candidate per frame following a sinusoidal vibrato.
std::vector<std::vector<F0Candidate>> vibrato_frames(float centre_hz, float depth_cents,
                                                     float rate_hz, int hop_length, int count) {
  std::vector<std::vector<F0Candidate>> frames;
  frames.reserve(static_cast<size_t>(count));
  for (int f = 0; f < count; ++f) {
    const float t =
        static_cast<float>(f) * static_cast<float>(hop_length) / static_cast<float>(kSampleRate);
    const float cents = depth_cents * std::sin(sonare::constants::kTwoPi * rate_hz * t);
    frames.push_back({candidate(hz_up(centre_hz, cents), 1.0f)});
  }
  return frames;
}

}  // namespace

// --- CentAxis --------------------------------------------------------------

TEST_CASE("CentAxis maps bins to frequencies and back", "[polyphonic_f0]") {
  CentAxis axis;
  axis.n_bins = 260;

  // Bin 0 is the reference, and a bin is a third of a semitone.
  REQUIRE_THAT(axis.hz_at(0.0f), WithinRel(axis.ref_hz, 1e-6f));
  REQUIRE_THAT(axis.cents_per_bin, WithinRel(100.0f / 3.0f, 1e-6f));
  REQUIRE_THAT(cents_between(axis.hz_at(0.0f), axis.hz_at(1.0f)),
               WithinAbs(static_cast<double>(axis.cents_per_bin), 1e-3));

  // An octave is thirty-six bins at that resolution.
  REQUIRE_THAT(axis.hz_at(36.0f), WithinRel(2.0f * axis.ref_hz, 1e-5f));
  REQUIRE_THAT(axis.bin_at(2.0f * axis.ref_hz), WithinAbs(36.0, 1e-3));

  // Round trip, over the whole span the cent spectrum uses and at fractional
  // bins, which the header says hz_at accepts.
  for (const float bin : {0.0f, 0.5f, 12.25f, 108.0f, 108.5f, 200.75f, 258.0f}) {
    INFO("bin " << bin);
    REQUIRE_THAT(axis.bin_at(axis.hz_at(bin)), WithinAbs(static_cast<double>(bin), 2e-3));
  }
  for (const float hz : {55.0f, 110.0f, 220.0f, 440.0f, 1760.0f, 7902.13f}) {
    INFO("hz " << hz);
    REQUIRE_THAT(axis.hz_at(axis.bin_at(hz)), WithinRel(hz, 1e-4f));
  }

  // A4 is three octaves over the 55 Hz reference, so it lands on a whole bin.
  REQUIRE_THAT(axis.bin_at(440.0f), WithinAbs(108.0, 2e-3));
}

TEST_CASE("CentAxis::bin_at returns out-of-range bins rather than rejecting", "[polyphonic_f0]") {
  CentAxis axis;
  axis.n_bins = 260;

  // Below the reference is a negative bin, not an error: the header says callers
  // range-check.
  REQUIRE_NOTHROW(axis.bin_at(27.5f));
  REQUIRE(axis.bin_at(27.5f) < 0.0f);
  REQUIRE_THAT(axis.bin_at(27.5f), WithinAbs(-36.0, 1e-3));

  // Above the top bin is a bin past the end, which is how a partial over the
  // ceiling reads as absent.
  REQUIRE_NOTHROW(axis.bin_at(20000.0f));
  REQUIRE(axis.bin_at(20000.0f) > static_cast<float>(axis.n_bins - 1));

  // n_bins takes no part in either conversion.
  CentAxis unsized;
  REQUIRE(unsized.n_bins == 0);
  REQUIRE_THAT(unsized.bin_at(440.0f), WithinAbs(108.0, 2e-3));
  REQUIRE_THAT(unsized.hz_at(108.0f), WithinRel(440.0f, 1e-4f));
}

// --- tonality_weights ------------------------------------------------------

TEST_CASE(
    "tonality_weights reads a flat instantaneous frequency as tonal and a bin-tracking one "
    "as noise",
    "[polyphonic_f0]") {
  constexpr int kBins = 16;
  const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kNfft);

  // One steady partial: the instantaneous frequency is the same across every bin
  // it reaches, so the advance is zero and the weight is one.
  const std::vector<float> steady(static_cast<size_t>(kBins), 440.0f);
  const std::vector<float> tonal = tonality_weights(steady.data(), kBins, bin_hz);
  REQUIRE(tonal.size() == static_cast<size_t>(kBins));
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(tonal[static_cast<size_t>(b)], WithinAbs(1.0, 1e-5));
  }

  // Noise: the instantaneous frequency tracks the bin centres, advancing by one
  // bin per bin, so the weight is zero -- at the two edge bins too, which take a
  // one-sided difference of the same slope.
  std::vector<float> tracking(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    tracking[static_cast<size_t>(b)] = static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> noisy = tonality_weights(tracking.data(), kBins, bin_hz);
  REQUIRE(noisy.size() == static_cast<size_t>(kBins));
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(noisy[static_cast<size_t>(b)], WithinAbs(0.0, 1e-5));
  }

  // A ramp offset from the bin centres is still a ramp: the rule reads the
  // slope, not the absolute frequency.
  std::vector<float> offset(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    offset[static_cast<size_t>(b)] = 1000.0f + static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> offset_weights = tonality_weights(offset.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(offset_weights[static_cast<size_t>(b)], WithinAbs(0.0, 1e-5));
  }

  // Two bins is the documented minimum, and both of them are edges.
  const std::vector<float> pair = {440.0f, 440.0f};
  const std::vector<float> both_edges = tonality_weights(pair.data(), 2, bin_hz);
  REQUIRE(both_edges.size() == 2);
  REQUIRE_THAT(both_edges[0], WithinAbs(1.0, 1e-5));
  REQUIRE_THAT(both_edges[1], WithinAbs(1.0, 1e-5));
}

TEST_CASE("tonality_weights reads a signed advance and clamps to [0, 1]", "[polyphonic_f0]") {
  constexpr int kBins = 12;
  const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kNfft);

  // Half a bin per bin: a partly steady bin, between the two endpoints and
  // nowhere near either clamp.
  std::vector<float> half(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    half[static_cast<size_t>(b)] = 500.0f + 0.5f * static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> partial = tonality_weights(half.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(partial[static_cast<size_t>(b)], WithinAbs(0.5, 1e-4));
  }

  // Three bins per bin drives one minus the advance to -2, which clamps at zero.
  std::vector<float> steep(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    steep[static_cast<size_t>(b)] = 3.0f * static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> clamped = tonality_weights(steep.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(clamped[static_cast<size_t>(b)], WithinAbs(0.0, 1e-5));
  }

  // The same slope downwards drives it to 4, which clamps at one, so both ends
  // of the clamp are exercised rather than only the lower one.
  std::vector<float> steep_down(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    steep_down[static_cast<size_t>(b)] = 9000.0f - 3.0f * static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> clamped_up = tonality_weights(steep_down.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(clamped_up[static_cast<size_t>(b)], WithinAbs(1.0, 1e-5));
  }

  // The advance is signed, so a falling instantaneous frequency reads as
  // perfectly steady. That looks like a defect and is load-bearing: between two
  // partials too close to resolve the frequency falls across them, and holding
  // those bins at full weight is what keeps a low chord's unresolved partials in
  // the harmonic sum.
  std::vector<float> falling(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    falling[static_cast<size_t>(b)] = 4000.0f - static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> descending = tonality_weights(falling.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    REQUIRE_THAT(descending[static_cast<size_t>(b)], WithinAbs(1.0, 1e-5));
  }

  // Signed swallows every falling slope in the clamp, so the falling side cannot
  // show a sign error as a value -- it shows it as the contrast against the
  // rising side. The rising side is where the value is read: half a bin per bin
  // up is 0.5 and a whole bin up is 0, under either convention.
  std::vector<float> falling_half(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    falling_half[static_cast<size_t>(b)] = 4000.0f - 0.5f * static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> gentle = tonality_weights(falling_half.data(), kBins, bin_hz);
  std::vector<float> rising(static_cast<size_t>(kBins), 0.0f);
  for (int b = 0; b < kBins; ++b) {
    rising[static_cast<size_t>(b)] = 500.0f + static_cast<float>(b) * bin_hz;
  }
  const std::vector<float> noisy = tonality_weights(rising.data(), kBins, bin_hz);
  for (int b = 0; b < kBins; ++b) {
    INFO("bin " << b);
    // Rising, where the weight tracks the slope.
    REQUIRE_THAT(partial[static_cast<size_t>(b)], WithinAbs(0.5, 1e-4));
    REQUIRE_THAT(noisy[static_cast<size_t>(b)], WithinAbs(0.0, 1e-5));
    // Falling by the same amounts, where it does not. Both read as perfectly
    // steady, and that is deliberate: an absolute advance would make the first
    // of these 0.5 and the second 0, and costs a voice of a C3 chord.
    REQUIRE_THAT(gentle[static_cast<size_t>(b)], WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(descending[static_cast<size_t>(b)], WithinAbs(1.0, 1e-5));
    REQUIRE(gentle[static_cast<size_t>(b)] - partial[static_cast<size_t>(b)] > 0.4f);
  }
}

TEST_CASE("tonality_weights rejects fewer than two bins and a non-positive spacing",
          "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const std::vector<float> frequencies(8, 440.0f);
  const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kNfft);

  for (const int bad : {1, 0, -1}) {
    INFO("n_bins " << bad);
    REQUIRE(code_of([&] { tonality_weights(frequencies.data(), bad, bin_hz); }) == kInvalid);
  }
  for (const float bad : {0.0f, -1.0f, -bin_hz}) {
    INFO("bin_hz " << bad);
    REQUIRE(code_of([&] { tonality_weights(frequencies.data(), 8, bad); }) == kInvalid);
  }

  // Two bins is the inclusive floor of the documented range.
  REQUIRE_NOTHROW(tonality_weights(frequencies.data(), 2, bin_hz));
}

// --- compute_cent_spectrum -------------------------------------------------

TEST_CASE("compute_cent_spectrum puts a sine where the axis says and covers the ceiling",
          "[polyphonic_f0]") {
  std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
  add_tone(samples, 440.0f, 0.5f, 1);
  const CentSpectrum spectrum = cent_spectrum_of(audio_of(std::move(samples)));

  REQUIRE(spectrum.n_frames > 4);
  REQUIRE(spectrum.hop_length == kHopLength);
  REQUIRE(spectrum.sample_rate == kSampleRate);

  // The axis is the configured one, and its top bin sits within one bin of the
  // configured ceiling.
  const CentSpectrumConfig defaults;
  REQUIRE_THAT(spectrum.axis.ref_hz, WithinRel(defaults.ref_hz, 1e-6f));
  REQUIRE_THAT(spectrum.axis.cents_per_bin, WithinRel(defaults.cents_per_bin, 1e-6f));
  REQUIRE(spectrum.axis.n_bins > 1);
  REQUIRE(spectrum.axis.n_bins ==
          static_cast<int>(std::ceil(spectrum.axis.bin_at(defaults.max_hz))) + 1);
  // The stated consequence of rounding up: the last bin is at or above the
  // ceiling rather than under it.
  REQUIRE(spectrum.axis.hz_at(static_cast<float>(spectrum.axis.n_bins - 1)) >= defaults.max_hz);

  // A single sine lands on the bin the axis names for its frequency. 440 Hz is
  // three octaves over the 55 Hz reference, so that bin is a whole number.
  const int middle = spectrum.n_frames / 2;
  const int peak = argmax(spectrum.column(middle), spectrum.axis.n_bins);
  REQUIRE_THAT(static_cast<double>(peak),
               WithinAbs(static_cast<double>(spectrum.axis.bin_at(440.0f)), 1.0));

  // Nothing in the surface is negative or non-finite.
  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    const float* column = spectrum.column(frame);
    for (int bin = 0; bin < spectrum.axis.n_bins; ++bin) {
      INFO("frame " << frame << " bin " << bin);
      REQUIRE(std::isfinite(column[bin]));
      REQUIRE(column[bin] >= 0.0f);
    }
  }
}

TEST_CASE("CentSpectrum is frame-major and frame 0 carries frame 1's frequencies",
          "[polyphonic_f0]") {
  std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
  add_tone(samples, 330.0f, 0.4f, 6);
  CentSpectrum spectrum = cent_spectrum_of(audio_of(std::move(samples)));

  // The layout is the transpose of Spectrogram's: one column is contiguous, and
  // column(f) is the f-th block rather than the f-th stride.
  REQUIRE(spectrum.values.size() ==
          static_cast<size_t>(spectrum.n_frames) * static_cast<size_t>(spectrum.axis.n_bins));
  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    INFO("frame " << frame);
    const size_t offset = static_cast<size_t>(frame) * static_cast<size_t>(spectrum.axis.n_bins);
    REQUIRE(spectrum.column(frame) == spectrum.values.data() + offset);
  }
  // The non-const accessor addresses the same storage, which is what iterated
  // subtraction writes through.
  const CentSpectrum& readable = spectrum;
  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    REQUIRE(spectrum.column(frame) == readable.column(frame));
  }

  // Frame 0 has no phase difference of its own and takes frame 1's, so it is a
  // real column rather than a zero or a garbage one, and it names the same
  // fundamental every later frame does.
  const int peak_first = argmax(spectrum.column(0), spectrum.axis.n_bins);
  const int peak_middle = argmax(spectrum.column(spectrum.n_frames / 2), spectrum.axis.n_bins);
  REQUIRE(column_sum(spectrum.values, spectrum.axis.n_bins) > 0.0f);
  REQUIRE(std::abs(peak_first - peak_middle) <= 1);
  REQUIRE_THAT(static_cast<double>(peak_first),
               WithinAbs(static_cast<double>(spectrum.axis.bin_at(330.0f)), 1.0));
}

TEST_CASE("compute_cent_spectrum rejects a one-frame spectrogram and a malformed axis",
          "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  std::vector<float> samples(static_cast<size_t>(kSampleRate / 2), 0.0f);
  add_tone(samples, 220.0f, 0.4f, 4);
  const sonare::Spectrogram spectrogram =
      sonare::Spectrogram::compute(audio_of(std::move(samples)), polyphony_stft_defaults());
  REQUIRE(spectrogram.n_frames() > 2);

  // An empty spectrogram has no frames at all.
  const sonare::Spectrogram empty;
  REQUIRE(code_of([&] { compute_cent_spectrum(empty); }) == kInvalid);

  // One frame carries no phase difference to read a frequency from.
  const std::vector<std::complex<float>> one_column(static_cast<size_t>(kNfft / 2 + 1),
                                                    std::complex<float>(1.0f, 0.0f));
  const sonare::Spectrogram single =
      sonare::Spectrogram::from_complex(one_column.data(), kNfft / 2 + 1, 1, kNfft, kHopLength,
                                        kSampleRate, sonare::WindowType::Hann);
  REQUIRE(single.n_frames() == 1);
  REQUIRE(code_of([&] { compute_cent_spectrum(single); }) == kInvalid);

  for (const float bad : {0.0f, -55.0f, -1.0f}) {
    INFO("ref_hz " << bad);
    CentSpectrumConfig config;
    config.ref_hz = bad;
    REQUIRE(code_of([&] { compute_cent_spectrum(spectrogram, config); }) == kInvalid);
  }
  // Under one cent is rejected. 0.999 is just outside and 1.0 just inside, so a
  // guard placed a factor away from the documented bound fails here rather than
  // passing on a wildly out-of-range value.
  for (const float bad : {0.999f, 0.5f, 0.0f, -100.0f / 3.0f, -1.0f, kNaN}) {
    INFO("cents_per_bin " << bad);
    CentSpectrumConfig config;
    config.cents_per_bin = bad;
    REQUIRE(code_of([&] { compute_cent_spectrum(spectrogram, config); }) == kInvalid);
  }
  {
    CentSpectrumConfig at_floor;
    at_floor.cents_per_bin = 1.0f;
    REQUIRE_NOTHROW(compute_cent_spectrum(spectrogram, at_floor));
  }
  // max_hz at or under ref_hz leaves no axis to build.
  for (const float bad : {55.0f, 27.5f, 0.0f, -100.0f}) {
    INFO("max_hz " << bad);
    CentSpectrumConfig config;
    config.max_hz = bad;
    REQUIRE(code_of([&] { compute_cent_spectrum(spectrogram, config); }) == kInvalid);
  }

  // The axis-size bound is reachable by a combination in which every field
  // passes its own check: the reference is the default, the resolution is
  // exactly the legal floor, and only their product with the ceiling is out of
  // range. A per-field check misses this and it is the case worth having.
  // With n_bins = ceil(span) + 1 and 32768 bins as the ceiling, a span of
  // 32766.5 bins is the last accepted axis and 32767.5 the first rejected one --
  // one bin apart, with half a bin of clearance so the hz_at/bin_at round trip
  // cannot move either across. The pair also lands the same way whether the
  // guard is written against the span or against the bin count.
  CentAxis probe;
  probe.cents_per_bin = 1.0f;
  const float inside_hz = probe.hz_at(32766.5f);
  const float outside_hz = probe.hz_at(32767.5f);
  REQUIRE(std::isfinite(inside_hz));
  REQUIRE(std::isfinite(outside_hz));
  {
    CentSpectrumConfig oversized;
    oversized.cents_per_bin = 1.0f;
    oversized.max_hz = outside_hz;
    REQUIRE(oversized.ref_hz > 0.0f);
    REQUIRE(oversized.max_hz > oversized.ref_hz);
    // InvalidParameter, not a failed allocation: the guard has to run before the
    // axis is sized, or it is not doing the job.
    REQUIRE(code_of([&] { compute_cent_spectrum(spectrogram, oversized); }) == kInvalid);

    CentSpectrumConfig just_inside;
    just_inside.cents_per_bin = 1.0f;
    just_inside.max_hz = inside_hz;
    REQUIRE_NOTHROW(compute_cent_spectrum(spectrogram, just_inside));
  }

  // Both settings of the tonality weight are accepted.
  for (const bool use_tonality : {true, false}) {
    CentSpectrumConfig config;
    config.use_tonality = use_tonality;
    REQUIRE_NOTHROW(compute_cent_spectrum(spectrogram, config));
  }

  // Two bins is the inclusive floor of the spectrogram rule. Fewer is not
  // constructible: Spectrogram::from_complex requires n_bins == n_fft / 2 + 1
  // and an n_fft of at least two, so the rejection below two bins cannot be
  // reached from outside and only the accepted floor is testable.
  std::vector<float> narrow(static_cast<size_t>(kSampleRate / 4), 0.0f);
  add_tone(narrow, 220.0f, 0.4f, 4);
  const sonare::Spectrogram two_bins =
      sonare::Spectrogram::compute(audio_of(std::move(narrow)), sonare::make_stft_config(2, 512));
  REQUIRE(two_bins.n_bins() == 2);
  REQUIRE(two_bins.n_frames() >= 2);
  REQUIRE_NOTHROW(compute_cent_spectrum(two_bins));
}

// --- SalienceKernel --------------------------------------------------------

TEST_CASE("SalienceKernel places partials at h f0 sqrt(1 + B h^2)", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);

  SECTION("an ideal harmonic series") {
    SalienceConfig config;
    REQUIRE(config.inharmonicity == 0.0f);
    const SalienceKernel kernel(axis, config);

    // The F0 axis runs from f0_min_hz at the cent spectrum's own resolution.
    REQUIRE_THAT(kernel.f0_axis().ref_hz, WithinRel(config.f0_min_hz, 1e-6f));
    REQUIRE_THAT(kernel.f0_axis().cents_per_bin, WithinRel(axis.cents_per_bin, 1e-6f));
    REQUIRE(kernel.n_harmonics() == config.n_harmonics);
    REQUIRE(kernel.spectrum_axis().n_bins == axis.n_bins);
    // Within a bin rather than exact: 55 Hz to 1760 Hz is exactly five octaves,
    // so the span is exactly 180 bins in real arithmetic, and 100/3 is not
    // exactly representable -- which side of the integer the computed span falls
    // decides whether rounding up adds a bin. The ceiling is asserted exactly
    // where the span is not near an integer, on the spectrum axis.
    REQUIRE_THAT(static_cast<double>(kernel.f0_axis().n_bins - 1),
                 WithinAbs(static_cast<double>(kernel.f0_axis().bin_at(config.f0_max_hz)), 2.0));
    REQUIRE(kernel.f0_axis().hz_at(static_cast<float>(kernel.f0_axis().n_bins - 1)) >=
            config.f0_max_hz);

    const float octave_bins = sonare::constants::kCentsPerOctave / axis.cents_per_bin;
    for (const int f0_bin : {0, 12, 72, 108}) {
      const float f0 = kernel.f0_axis().hz_at(static_cast<float>(f0_bin));
      for (int h = 1; h <= 8; ++h) {
        INFO("f0_bin " << f0_bin << " harmonic " << h);
        const float expected = axis.bin_at(static_cast<float>(h) * f0);
        REQUIRE_THAT(kernel.partial_bin(f0_bin, h), WithinAbs(static_cast<double>(expected), 0.05));
      }
      // Harmonic 1 is the fundamental, not the first overtone.
      REQUIRE_THAT(kernel.partial_bin(f0_bin, 1),
                   WithinAbs(static_cast<double>(axis.bin_at(f0)), 0.05));
      // Doubling the harmonic number is an octave, which is a constant shift on
      // a cent axis: that is the whole reason the axis is cents.
      REQUIRE_THAT(kernel.partial_bin(f0_bin, 2) - kernel.partial_bin(f0_bin, 1),
                   WithinAbs(static_cast<double>(octave_bins), 0.05));
      REQUIRE_THAT(kernel.partial_bin(f0_bin, 8) - kernel.partial_bin(f0_bin, 4),
                   WithinAbs(static_cast<double>(octave_bins), 0.05));
    }
  }

  SECTION("a stiff string stretches its upper partials sharp") {
    SalienceConfig config;
    config.inharmonicity = 5e-4f;
    const SalienceKernel kernel(axis, config);

    const int f0_bin = 72;
    const float f0 = kernel.f0_axis().hz_at(static_cast<float>(f0_bin));
    for (int h = 1; h <= 12; ++h) {
      INFO("harmonic " << h);
      const float harmonic = static_cast<float>(h);
      const float hz = harmonic * f0 * std::sqrt(1.0f + config.inharmonicity * harmonic * harmonic);
      REQUIRE_THAT(kernel.partial_bin(f0_bin, h),
                   WithinAbs(static_cast<double>(axis.bin_at(hz)), 0.05));
    }

    // The stretch is sharp and grows with the harmonic number: at B = 5e-4 the
    // tenth partial sits about forty-two cents over the ideal one.
    const SalienceKernel ideal(axis, SalienceConfig{});
    const float shift_cents =
        (kernel.partial_bin(f0_bin, 10) - ideal.partial_bin(f0_bin, 10)) * axis.cents_per_bin;
    REQUIRE(shift_cents > 0.0f);
    REQUIRE_THAT(shift_cents, WithinAbs(42.0, 2.0));
    REQUIRE(kernel.partial_bin(f0_bin, 12) - ideal.partial_bin(f0_bin, 12) >
            kernel.partial_bin(f0_bin, 4) - ideal.partial_bin(f0_bin, 4));
    // B = 0 is the ideal series, which is the documented meaning of the default.
    REQUIRE_THAT(ideal.partial_bin(f0_bin, 10),
                 WithinAbs(static_cast<double>(axis.bin_at(10.0f * f0)), 0.05));
  }
}

TEST_CASE("SalienceKernel weights a partial by (f0 + alpha) / (h f0 + beta)", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  SalienceConfig config;
  const SalienceKernel kernel(axis, config);

  for (const int f0_bin : {0, 36, 72, 108, 144}) {
    const float f0 = kernel.f0_axis().hz_at(static_cast<float>(f0_bin));
    for (int h = 1; h <= 10; ++h) {
      INFO("f0_bin " << f0_bin << " harmonic " << h);
      const float expected = (f0 + config.alpha_hz) / (static_cast<float>(h) * f0 + config.beta_hz);
      REQUIRE_THAT(kernel.partial_weight(f0_bin, h), WithinRel(expected, 1e-4f));
    }

    // The shape, stated without the absolute scale, so a kernel that normalized
    // its weights would still have to get the fall-off right.
    for (int h = 2; h <= 10; ++h) {
      INFO("f0_bin " << f0_bin << " ratio at harmonic " << h);
      const float ratio_expected =
          (f0 + config.beta_hz) / (static_cast<float>(h) * f0 + config.beta_hz);
      REQUIRE_THAT(kernel.partial_weight(f0_bin, h) / kernel.partial_weight(f0_bin, 1),
                   WithinRel(ratio_expected, 1e-4f));
      // Weights fall monotonically with the harmonic number.
      REQUIRE(kernel.partial_weight(f0_bin, h) < kernel.partial_weight(f0_bin, h - 1));
    }
  }

  // beta moves the weight the way the formula says.
  SalienceConfig heavier;
  heavier.beta_hz = 640.0f;
  const SalienceKernel wide(axis, heavier);
  REQUIRE(wide.partial_weight(72, 1) < kernel.partial_weight(72, 1));
}

TEST_CASE("a partial over the spectrum ceiling reads as absent rather than as an error",
          "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  SalienceConfig config;
  const SalienceKernel kernel(axis, config);

  // 8000 / 20 is 400 Hz, so every F0 over that loses partials off the top of the
  // axis. It is a bin past the end, not a throw.
  const int high_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(1000.0f)));
  REQUIRE(high_bin < kernel.f0_axis().n_bins);
  REQUIRE_NOTHROW(kernel.partial_bin(high_bin, config.n_harmonics));
  REQUIRE(kernel.partial_bin(high_bin, config.n_harmonics) > static_cast<float>(axis.n_bins - 1));
  REQUIRE(std::isfinite(kernel.partial_bin(high_bin, config.n_harmonics)));

  // The lowest F0 keeps all twenty of them: 55 * 20 is well under the ceiling.
  for (int h = 1; h <= config.n_harmonics; ++h) {
    INFO("harmonic " << h);
    REQUIRE(kernel.partial_bin(0, h) < static_cast<float>(axis.n_bins - 1));
  }

  // Subtracting an F0 whose upper partials are off the axis removes only what is
  // on it, and writes nothing past the end of the column.
  std::vector<float> column(static_cast<size_t>(axis.n_bins) + 8, -7.5f);
  std::fill(column.begin(), column.begin() + axis.n_bins, 0.0f);
  add_series(column, axis, kernel.f0_axis().hz_at(static_cast<float>(high_bin)), 6, 1.0f);
  const float before = column_sum(column, axis.n_bins);
  REQUIRE(before > 0.0f);
  const float removed = kernel.subtract(column.data(), high_bin, 1.0f);
  REQUIRE(removed >= 0.0f);
  REQUIRE(removed <= before + 1e-3f);
  for (size_t i = static_cast<size_t>(axis.n_bins); i < column.size(); ++i) {
    INFO("guard slot " << i);
    REQUIRE(column[i] == -7.5f);
  }
}

TEST_CASE("the ceiling costs a high F0 partials, and the estimator still finds it",
          "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  SalienceConfig config;
  const SalienceKernel kernel(axis, config);

  /// @brief Partials of @p f0_hz that land on the spectrum axis.
  const auto partials_inside = [&](float f0_hz) {
    const int f0_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(f0_hz)));
    int inside = 0;
    for (int h = 1; h <= config.n_harmonics; ++h) {
      if (kernel.partial_bin(f0_bin, h) < static_cast<float>(axis.n_bins - 1)) ++inside;
    }
    return inside;
  };

  // max_hz over n_harmonics is 400 Hz, so that is about where an F0 starts
  // losing partials off the top. 380 and 420 sit either side of it rather than
  // on it, where partial 20 lands at the ceiling itself.
  REQUIRE(partials_inside(380.0f) == config.n_harmonics);
  REQUIRE(partials_inside(420.0f) < config.n_harmonics);

  // The count falls as the F0 rises, so the bias covers most of the F0 range
  // rather than an edge of it.
  int previous = config.n_harmonics + 1;
  for (const float f0 : {110.0f, 220.0f, 380.0f, 550.0f, 880.0f, 1760.0f}) {
    INFO("f0 " << f0 << " keeps " << partials_inside(f0) << " partials");
    REQUIRE(partials_inside(f0) <= previous);
    previous = partials_inside(f0);
  }
  REQUIRE(partials_inside(1760.0f) < partials_inside(110.0f));
  REQUIRE(partials_inside(1760.0f) >= 1);

  // Summing fewer partials is not the same as being lost: a voice well over the
  // crossover is still estimated, and still found alongside one under it.
  // Whether a higher ceiling would recover anything is measured elsewhere and is
  // deliberately not asserted here.
  const MultiF0Estimator estimator(axis, MultiF0Config{});
  const std::vector<F0Candidate> alone = estimator.estimate(voiced_column(axis, {880.0f}).data());
  REQUIRE(alone.size() == 1);
  REQUIRE(std::abs(cents_between(880.0f, alone[0].f0_hz)) < 16.0f);

  const std::vector<float> pair = voiced_column(axis, {274.6f, 880.0f});
  const std::vector<F0Candidate> both = estimator.estimate(pair.data());
  REQUIRE(both.size() == 2);
  for (const float f0 : {274.6f, 880.0f}) {
    INFO("looking for " << f0);
    bool matched = false;
    for (const F0Candidate& value : both) {
      if (std::abs(cents_between(f0, value.f0_hz)) <= 16.0f) matched = true;
    }
    REQUIRE(matched);
  }
}

TEST_CASE("evaluate sums a harmonic series onto its fundamental and holds off the octave below",
          "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const SalienceKernel kernel(axis, SalienceConfig{});

  // 220 Hz is two octaves over the 55 Hz axis floor, so the fundamental and both
  // octave neighbours are whole bins and the three are compared without any
  // interpolation difference between them.
  const int octave_bins =
      static_cast<int>(std::lround(sonare::constants::kCentsPerOctave / axis.cents_per_bin));
  const int f0_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(220.0f)));
  REQUIRE(f0_bin - octave_bins >= 0);
  REQUIRE(f0_bin + octave_bins < kernel.f0_axis().n_bins);

  const std::vector<float> column =
      voiced_column(axis, {kernel.f0_axis().hz_at(static_cast<float>(f0_bin))});

  // A guard past the end of the output: evaluate writes f0_axis().n_bins values.
  std::vector<float> out(static_cast<size_t>(kernel.f0_axis().n_bins) + 8, -7.5f);
  kernel.evaluate(column.data(), out.data());
  for (size_t i = static_cast<size_t>(kernel.f0_axis().n_bins); i < out.size(); ++i) {
    INFO("guard slot " << i);
    REQUIRE(out[i] == -7.5f);
  }
  for (int b = 0; b < kernel.f0_axis().n_bins; ++b) {
    INFO("f0 bin " << b);
    REQUIRE(std::isfinite(out[static_cast<size_t>(b)]));
    REQUIRE(out[static_cast<size_t>(b)] >= 0.0f);
  }

  // The true fundamental wins the surface outright.
  REQUIRE(argmax(out.data(), kernel.f0_axis().n_bins) == f0_bin);

  // The two octave ghosts stand -- a candidate an octave under collects every
  // second partial and one an octave over collects the even ones -- and the
  // partial weighting keeps both under the true fundamental.
  const float here = out[static_cast<size_t>(f0_bin)];
  const float below = out[static_cast<size_t>(f0_bin - octave_bins)];
  const float above = out[static_cast<size_t>(f0_bin + octave_bins)];
  REQUIRE(below > 0.0f);
  REQUIRE(above > 0.0f);
  REQUIRE(here > below);
  REQUIRE(here > above);

  // A bin that is a harmonic of nothing collects less than any of the three.
  REQUIRE(out[static_cast<size_t>(f0_bin + 5)] < here);
}

TEST_CASE("subtract removes exactly what the next evaluation stops seeing", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const SalienceKernel kernel(axis, SalienceConfig{});
  const int f0_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(220.0f)));
  const float f0 = kernel.f0_axis().hz_at(static_cast<float>(f0_bin));

  std::vector<float> column = voiced_column(axis, {f0});
  std::vector<float> before(static_cast<size_t>(kernel.f0_axis().n_bins), 0.0f);
  kernel.evaluate(column.data(), before.data());
  REQUIRE(before[static_cast<size_t>(f0_bin)] > 0.0f);

  const float total_before = column_sum(column, axis.n_bins);
  const float removed = kernel.subtract(column.data(), f0_bin, 1.0f);
  const float total_after = column_sum(column, axis.n_bins);

  // The return value is the total taken, in the column's own units.
  REQUIRE(removed > 0.0f);
  REQUIRE_THAT(removed, WithinRel(total_before - total_after, 2e-3f));

  // Clamped at zero: nothing in the column goes negative.
  for (int b = 0; b < axis.n_bins; ++b) {
    INFO("bin " << b);
    REQUIRE(column[static_cast<size_t>(b)] >= 0.0f);
  }

  // What the call removed is what the next evaluation stops seeing. Taking the
  // whole share of the only voice in the column empties its salience.
  std::vector<float> after(static_cast<size_t>(kernel.f0_axis().n_bins), 0.0f);
  kernel.evaluate(column.data(), after.data());
  REQUIRE(after[static_cast<size_t>(f0_bin)] < 0.02f * before[static_cast<size_t>(f0_bin)]);

  // A second subtraction of the same F0 finds nothing left to take.
  REQUIRE(kernel.subtract(column.data(), f0_bin, 1.0f) < 0.02f * removed);
}

TEST_CASE("subtract takes the share it is given and leaves other voices standing",
          "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const SalienceKernel kernel(axis, SalienceConfig{});
  const int f0_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(220.0f)));
  const float f0 = kernel.f0_axis().hz_at(static_cast<float>(f0_bin));

  std::vector<float> whole = voiced_column(axis, {f0});
  const float removed_whole = kernel.subtract(whole.data(), f0_bin, 1.0f);

  // Half the share takes about half as much. The band is wide enough that the
  // interpolation's own rounding cannot reach either edge.
  std::vector<float> half = voiced_column(axis, {f0});
  const float removed_half = kernel.subtract(half.data(), f0_bin, 0.5f);
  REQUIRE(removed_half > 0.4f * removed_whole);
  REQUIRE(removed_half < 0.6f * removed_whole);

  // An empty column has nothing to take and is left alone.
  std::vector<float> silent(static_cast<size_t>(axis.n_bins), 0.0f);
  REQUIRE_THAT(kernel.subtract(silent.data(), f0_bin, 1.0f), WithinAbs(0.0, 1e-6));
  for (int b = 0; b < axis.n_bins; ++b) {
    REQUIRE(silent[static_cast<size_t>(b)] == 0.0f);
  }

  // Removing one voice of two leaves the other's salience essentially untouched:
  // the two share no low-order partial, so little of the second is taken.
  const int other_bin = static_cast<int>(std::lround(kernel.f0_axis().bin_at(388.6f)));
  const float other = kernel.f0_axis().hz_at(static_cast<float>(other_bin));
  std::vector<float> pair = voiced_column(axis, {f0, other});
  std::vector<float> before(static_cast<size_t>(kernel.f0_axis().n_bins), 0.0f);
  kernel.evaluate(pair.data(), before.data());
  kernel.subtract(pair.data(), f0_bin, 1.0f);
  std::vector<float> after(static_cast<size_t>(kernel.f0_axis().n_bins), 0.0f);
  kernel.evaluate(pair.data(), after.data());
  REQUIRE(after[static_cast<size_t>(f0_bin)] < 0.35f * before[static_cast<size_t>(f0_bin)]);
  REQUIRE(after[static_cast<size_t>(other_bin)] > 0.7f * before[static_cast<size_t>(other_bin)]);
}

TEST_CASE("SalienceKernel rejects a malformed axis and configuration", "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const CentAxis axis = spectrum_axis_for(8000.0f);

  for (const int bad : {0, -1}) {
    INFO("spectrum axis n_bins " << bad);
    CentAxis empty = axis;
    empty.n_bins = bad;
    REQUIRE(code_of([&] { return SalienceKernel(empty, SalienceConfig{}); }) == kInvalid);
  }
  // The axis is bounded above as well: 32769 is just outside and 32768 just
  // inside, so a ceiling off by a factor does not pass here.
  {
    CentAxis oversized = axis;
    oversized.n_bins = 32769;
    REQUIRE(code_of([&] { return SalienceKernel(oversized, SalienceConfig{}); }) == kInvalid);
    CentAxis at_ceiling = axis;
    at_ceiling.n_bins = 32768;
    REQUIRE_NOTHROW(SalienceKernel(at_ceiling, SalienceConfig{}));
  }
  // And below, by resolution rather than by count.
  for (const float bad : {0.999f, 0.5f, 0.0f, -1.0f, kNaN}) {
    INFO("spectrum axis cents_per_bin " << bad);
    CentAxis fine = axis;
    fine.cents_per_bin = bad;
    REQUIRE(code_of([&] { return SalienceKernel(fine, SalienceConfig{}); }) == kInvalid);
  }
  {
    CentAxis at_floor = axis;
    at_floor.cents_per_bin = 1.0f;
    REQUIRE_NOTHROW(SalienceKernel(at_floor, SalienceConfig{}));
  }
  // The harmonic count sizes the tables, so it is capped: 129 just outside, 128
  // just inside.
  for (const int bad : {0, -1, -20, 129, 1000, std::numeric_limits<int>::max()}) {
    INFO("n_harmonics " << bad);
    SalienceConfig config;
    config.n_harmonics = bad;
    REQUIRE(code_of([&] { return SalienceKernel(axis, config); }) == kInvalid);
  }
  {
    SalienceConfig at_cap;
    at_cap.n_harmonics = 128;
    REQUIRE_NOTHROW(SalienceKernel(axis, at_cap));
  }
  // f0_max_hz at or under f0_min_hz leaves no F0 axis.
  for (const float bad : {55.0f, 27.5f, 0.0f}) {
    INFO("f0_max_hz " << bad);
    SalienceConfig config;
    config.f0_max_hz = bad;
    REQUIRE(code_of([&] { return SalienceKernel(axis, config); }) == kInvalid);
  }
  for (const float bad : {0.0f, -55.0f, kNaN}) {
    INFO("f0_min_hz " << bad);
    SalienceConfig config;
    config.f0_min_hz = bad;
    REQUIRE(code_of([&] { return SalienceKernel(axis, config); }) == kInvalid);
  }
  for (const float bad : {0.0f, -320.0f, kNaN}) {
    INFO("beta_hz " << bad);
    SalienceConfig config;
    config.beta_hz = bad;
    REQUIRE(code_of([&] { return SalienceKernel(axis, config); }) == kInvalid);
  }
  for (const float bad : {-1e-6f, -1.0f, kNaN}) {
    INFO("inharmonicity " << bad);
    SalienceConfig config;
    config.inharmonicity = bad;
    REQUIRE(code_of([&] { return SalienceKernel(axis, config); }) == kInvalid);
  }

  // Zero inharmonicity is the documented ideal series and is accepted, as is a
  // one-harmonic kernel and a zero alpha.
  SalienceConfig ideal;
  ideal.inharmonicity = 0.0f;
  REQUIRE_NOTHROW(SalienceKernel(axis, ideal));
  SalienceConfig single;
  single.n_harmonics = 1;
  REQUIRE_NOTHROW(SalienceKernel(axis, single));
  SalienceConfig no_alpha;
  no_alpha.alpha_hz = 0.0f;
  REQUIRE_NOTHROW(SalienceKernel(axis, no_alpha));
}

// --- compute_salience ------------------------------------------------------

TEST_CASE("compute_salience is frame-major and agrees with the kernel column by column",
          "[polyphonic_f0]") {
  std::vector<float> samples(static_cast<size_t>(kSampleRate / 2), 0.0f);
  add_tone(samples, 220.0f, 0.4f, 10);
  const CentSpectrum spectrum = cent_spectrum_of(audio_of(std::move(samples)));
  const SalienceConfig config;
  const SalienceSurface surface = compute_salience(spectrum, config);

  REQUIRE(surface.n_frames == spectrum.n_frames);
  REQUIRE(surface.hop_length == spectrum.hop_length);
  REQUIRE(surface.sample_rate == spectrum.sample_rate);
  REQUIRE_THAT(surface.axis.ref_hz, WithinRel(config.f0_min_hz, 1e-6f));
  REQUIRE_THAT(surface.axis.cents_per_bin, WithinRel(spectrum.axis.cents_per_bin, 1e-6f));
  REQUIRE(surface.values.size() ==
          static_cast<size_t>(surface.n_frames) * static_cast<size_t>(surface.axis.n_bins));
  for (int frame = 0; frame < surface.n_frames; ++frame) {
    INFO("frame " << frame);
    const size_t offset = static_cast<size_t>(frame) * static_cast<size_t>(surface.axis.n_bins);
    REQUIRE(surface.column(frame) == surface.values.data() + offset);
  }

  // Every column is the kernel's own evaluation of the matching cent column.
  const SalienceKernel kernel(spectrum.axis, config);
  REQUIRE(kernel.f0_axis().n_bins == surface.axis.n_bins);
  std::vector<float> expected(static_cast<size_t>(kernel.f0_axis().n_bins), 0.0f);
  for (int frame = 0; frame < surface.n_frames; ++frame) {
    kernel.evaluate(spectrum.column(frame), expected.data());
    const float* actual = surface.column(frame);
    // Scaled by the column's own peak: a relative tolerance on a bin that is
    // near zero asks for agreement no float summation order can give.
    const float scale = *std::max_element(expected.begin(), expected.end());
    REQUIRE(scale > 0.0f);
    for (int b = 0; b < surface.axis.n_bins; ++b) {
      INFO("frame " << frame << " f0 bin " << b);
      REQUIRE_THAT(actual[b], WithinAbs(static_cast<double>(expected[static_cast<size_t>(b)]),
                                        1e-4 * static_cast<double>(scale)));
    }
  }

  // The surface names the sounding pitch.
  const int peak = argmax(surface.column(surface.n_frames / 2), surface.axis.n_bins);
  REQUIRE_THAT(static_cast<double>(peak),
               WithinAbs(static_cast<double>(surface.axis.bin_at(220.0f)), 1.0));
}

TEST_CASE("compute_salience rejects an empty spectrum and a malformed configuration",
          "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  std::vector<float> samples(static_cast<size_t>(kSampleRate / 2), 0.0f);
  add_tone(samples, 220.0f, 0.4f, 6);
  const CentSpectrum spectrum = cent_spectrum_of(audio_of(std::move(samples)));

  const CentSpectrum empty;
  REQUIRE(empty.n_frames == 0);
  REQUIRE(code_of([&] { compute_salience(empty); }) == kInvalid);

  CentSpectrum no_bins = spectrum;
  no_bins.axis.n_bins = 0;
  no_bins.values.clear();
  REQUIRE(code_of([&] { compute_salience(no_bins); }) == kInvalid);

  // Every reason the kernel throws reaches here too.
  SalienceConfig inverted;
  inverted.f0_max_hz = inverted.f0_min_hz;
  REQUIRE(code_of([&] { compute_salience(spectrum, inverted); }) == kInvalid);
  SalienceConfig no_harmonics;
  no_harmonics.n_harmonics = 0;
  REQUIRE(code_of([&] { compute_salience(spectrum, no_harmonics); }) == kInvalid);
  SalienceConfig negative_b;
  negative_b.inharmonicity = -1.0f;
  REQUIRE(code_of([&] { compute_salience(spectrum, negative_b); }) == kInvalid);
}

// --- MultiF0Estimator ------------------------------------------------------

TEST_CASE("estimate finds one, two, three and four voices in a column", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  MultiF0Config config;
  const MultiF0Estimator estimator(axis, config);
  REQUIRE(estimator.config().max_polyphony == 4);

  for (size_t voices = 1; voices <= 4; ++voices) {
    INFO("voices " << voices);
    const std::vector<float> f0s(kSpreadVoices, kSpreadVoices + voices);
    const std::vector<float> column = voiced_column(axis, f0s);
    const std::vector<F0Candidate> found = estimator.estimate(column.data());

    REQUIRE(found.size() == voices);
    // Every voice put in comes back, within a third of the separation rule.
    for (const float f0 : f0s) {
      INFO("looking for " << f0);
      bool matched = false;
      for (const F0Candidate& value : found) {
        if (std::abs(cents_between(f0, value.f0_hz)) <= 16.0f) matched = true;
      }
      REQUIRE(matched);
    }
    for (const F0Candidate& value : found) {
      REQUIRE(std::isfinite(value.f0_hz));
      REQUIRE(value.f0_hz > 0.0f);
      REQUIRE(value.salience > 0.0f);
      REQUIRE(value.harmonic_share >= 0.0f);
      REQUIRE(value.harmonic_share <= 1.0f);
    }
  }

  // The cap is hard: a fifth voice in the column does not produce a fifth
  // candidate.
  const std::vector<float> crowded = voiced_column(axis, {200.0f, 274.6f, 388.6f, 520.0f, 703.0f});
  REQUIRE(estimator.estimate(crowded.data()).size() <= 4);
}

TEST_CASE("estimate returns nothing for a silent column", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const MultiF0Estimator estimator(axis, MultiF0Config{});

  const std::vector<float> silence(static_cast<size_t>(axis.n_bins), 0.0f);
  REQUIRE(estimator.estimate(silence.data()).empty());

  // Estimating does not modify the column it reads.
  std::vector<float> column = voiced_column(axis, {220.0f});
  const std::vector<float> untouched = column;
  REQUIRE(!estimator.estimate(column.data()).empty());
  REQUIRE(column == untouched);
}

TEST_CASE("estimate returns candidates by descending salience", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const MultiF0Estimator estimator(axis, MultiF0Config{});

  // Levels deliberately unequal so the ordering carries information.
  std::vector<float> column(static_cast<size_t>(axis.n_bins), 0.0f);
  add_series(column, axis, kSpreadVoices[0], 12, 0.4f);
  add_series(column, axis, kSpreadVoices[1], 12, 1.0f);
  add_series(column, axis, kSpreadVoices[2], 12, 0.7f);
  const std::vector<F0Candidate> found = estimator.estimate(column.data());
  REQUIRE(found.size() == 3);

  for (size_t i = 1; i < found.size(); ++i) {
    INFO("candidate " << i);
    REQUIRE(found[i].salience <= found[i - 1].salience + 1e-4f * found[0].salience);
  }
  // Iterated subtraction only ever removes energy, so no later candidate can
  // read louder than the first.
  REQUIRE(found.back().salience <= found.front().salience);

  // The harmonic shares are shares of one column and cannot total more than it.
  float total = 0.0f;
  for (const F0Candidate& value : found) total += value.harmonic_share;
  REQUIRE(total <= 1.0f + 1e-3f);
}

TEST_CASE("the separation rule keeps one F0 from being taken twice", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  MultiF0Config config;
  REQUIRE_THAT(config.min_separation_cents, WithinAbs(50.0, 1e-6));
  const MultiF0Estimator estimator(axis, config);

  // One clean voice under a polyphony cap of four is one candidate, not four
  // copies of itself standing on the interpolation's neighbours.
  const std::vector<float> single = voiced_column(axis, {220.0f});
  REQUIRE(estimator.estimate(single.data()).size() == 1);

  // Two voices twenty cents apart, comfortably inside the rule, are one voice.
  const std::vector<float> close = voiced_column(axis, {220.0f, hz_up(220.0f, 20.0f)});
  REQUIRE(estimator.estimate(close.data()).size() == 1);

  // Eighty cents apart, comfortably outside it, are two.
  const std::vector<float> apart = voiced_column(axis, {220.0f, hz_up(220.0f, 80.0f)});
  REQUIRE(estimator.estimate(apart.data()).size() == 2);

  // Whatever the column, no two returned candidates sit inside the rule.
  for (const std::vector<float>* column : {&single, &close, &apart}) {
    const std::vector<F0Candidate> found = estimator.estimate(column->data());
    for (size_t i = 0; i < found.size(); ++i) {
      for (size_t j = i + 1; j < found.size(); ++j) {
        INFO("candidates " << i << " and " << j);
        REQUIRE(std::abs(cents_between(found[i].f0_hz, found[j].f0_hz)) >=
                config.min_separation_cents - 0.5f);
      }
    }
  }
}

TEST_CASE("parabolic refinement puts an F0 off the axis grid", "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const MultiF0Estimator estimator(axis, MultiF0Config{});
  const CentAxis& f0_axis = estimator.kernel().f0_axis();

  // A voice placed exactly halfway between two F0-axis bins: the grid cannot
  // name it, so a returned value that lands on the grid has not been refined.
  const float off_grid = f0_axis.hz_at(60.5f);
  const std::vector<float> column = voiced_column(axis, {off_grid});
  const std::vector<F0Candidate> found = estimator.estimate(column.data());
  // Not necessarily one: a column built from exact partial positions carries
  // none of the leakage a rendered tone has, so a harmonic of this single voice
  // stands at 0.23 of the first peak and clears the frame floor. The first
  // candidate is still the voice, which is what refinement is asserted on.
  REQUIRE(!found.empty());

  const float returned_bin = f0_axis.bin_at(found[0].f0_hz);
  const float grid_distance = std::abs(returned_bin - std::round(returned_bin));
  INFO("returned " << found[0].f0_hz << " Hz at bin " << returned_bin);
  // Well clear of either neighbouring bin: a fifth of a bin is nearly seven
  // cents, far outside anything a float round trip could produce.
  REQUIRE(grid_distance > 0.2f);
  // And close to where the voice actually is.
  REQUIRE(std::abs(cents_between(off_grid, found[0].f0_hz)) < 12.0f);

  // A voice sitting on a bin still comes back on it, so the refinement is not a
  // constant offset.
  const float on_grid = f0_axis.hz_at(60.0f);
  const std::vector<float> aligned = voiced_column(axis, {on_grid});
  const std::vector<F0Candidate> exact = estimator.estimate(aligned.data());
  REQUIRE(exact.size() == 1);
  REQUIRE(std::abs(cents_between(on_grid, exact[0].f0_hz)) < 6.0f);
}

TEST_CASE("harmonic_share ranks within a frame and does not separate a chord from noise",
          "[polyphonic_f0]") {
  const CentAxis axis = spectrum_axis_for(8000.0f);
  const MultiF0Estimator estimator(axis, MultiF0Config{});

  const std::vector<float> chord(kSpreadVoices, kSpreadVoices + 4);
  const std::vector<float> column = voiced_column(axis, chord);
  const std::vector<F0Candidate> voices = estimator.estimate(column.data());
  REQUIRE(voices.size() == 4);
  for (const F0Candidate& voice : voices) {
    REQUIRE(voice.harmonic_share >= 0.0f);
    REQUIRE(voice.harmonic_share <= 1.0f);
  }
  // It ranks inside one frame: the voice taken first accounts for more of the
  // column than the one taken last.
  REQUIRE(voices.front().harmonic_share >= voices.back().harmonic_share);

  // A noise frame's first candidate still accounts for a real share of the
  // column, which is the header's point: no threshold on this number tells a
  // chord from noise, and min_duration_ms is what drops the noise.
  const CentSpectrum noise_spectrum = cent_spectrum_of(audio_of(noise_samples(7u, 22050)));
  const MultiF0Estimator noise_estimator(noise_spectrum.axis, MultiF0Config{});
  const std::vector<F0Candidate> from_noise =
      noise_estimator.estimate(noise_spectrum.column(noise_spectrum.n_frames / 2));
  REQUIRE(!from_noise.empty());
  REQUIRE(from_noise[0].harmonic_share > 0.05f);
  REQUIRE(from_noise[0].harmonic_share <= 1.0f);
}

TEST_CASE("MultiF0Estimator rejects a malformed configuration", "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const CentAxis axis = spectrum_axis_for(8000.0f);

  // Bounded above as well as below, because the iteration bound is derived from
  // it: 65 is just outside and 64 just inside.
  for (const int bad : {0, -1, -4, 65, 1000, std::numeric_limits<int>::max()}) {
    INFO("max_polyphony " << bad);
    MultiF0Config config;
    config.max_polyphony = bad;
    REQUIRE(code_of([&] { return MultiF0Estimator(axis, config); }) == kInvalid);
  }
  {
    MultiF0Config at_cap;
    at_cap.max_polyphony = 64;
    REQUIRE_NOTHROW(MultiF0Estimator(axis, at_cap));
  }
  for (const float bad : {-0.01f, -1.0f, 1.01f, 2.0f, kNaN, kInf}) {
    INFO("min_frame_peak_ratio " << bad);
    MultiF0Config config;
    config.min_frame_peak_ratio = bad;
    REQUIRE(code_of([&] { return MultiF0Estimator(axis, config); }) == kInvalid);
  }
  for (const float bad : {-0.01f, -50.0f, kNaN}) {
    INFO("min_separation_cents " << bad);
    MultiF0Config config;
    config.min_separation_cents = bad;
    REQUIRE(code_of([&] { return MultiF0Estimator(axis, config); }) == kInvalid);
  }
  for (const float bad : {0.0f, -0.5f, 1.01f, 2.0f, kNaN, kInf}) {
    INFO("subtraction_factor " << bad);
    MultiF0Config config;
    config.subtraction_factor = bad;
    REQUIRE(code_of([&] { return MultiF0Estimator(axis, config); }) == kInvalid);
  }
  // The salience config's own rules reach here.
  MultiF0Config inverted;
  inverted.salience.f0_max_hz = inverted.salience.f0_min_hz;
  REQUIRE(code_of([&] { return MultiF0Estimator(axis, inverted); }) == kInvalid);

  // Both ends of every documented range are inside it.
  for (const float ratio : {0.0f, 1.0f}) {
    INFO("min_frame_peak_ratio " << ratio);
    MultiF0Config config;
    config.min_frame_peak_ratio = ratio;
    REQUIRE_NOTHROW(MultiF0Estimator(axis, config));
  }
  MultiF0Config no_separation;
  no_separation.min_separation_cents = 0.0f;
  REQUIRE_NOTHROW(MultiF0Estimator(axis, no_separation));
  MultiF0Config whole_partials;
  whole_partials.subtraction_factor = 1.0f;
  REQUIRE_NOTHROW(MultiF0Estimator(axis, whole_partials));
  MultiF0Config one_voice;
  one_voice.max_polyphony = 1;
  REQUIRE_NOTHROW(MultiF0Estimator(axis, one_voice));
}

// --- track_f0_ridges, driven by hand-built candidates ----------------------

TEST_CASE("a steady voice is one ridge with the spans its frames cover", "[polyphonic_f0]") {
  constexpr int kFrames = 40;
  const std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, kFrames);
  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);

  REQUIRE(ridges.size() == 1);
  const F0Ridge& ridge = ridges[0];
  REQUIRE(ridge.frame_start == 0);
  REQUIRE(ridge.f0_hz.size() == static_cast<size_t>(kFrames));
  REQUIRE(ridge.salience.size() == ridge.f0_hz.size());
  REQUIRE(ridge.frame_end() == kFrames);
  REQUIRE_THAT(ridge.median_hz, WithinRel(220.0f, 1e-5f));
  for (const float value : ridge.f0_hz) {
    REQUIRE_THAT(value, WithinRel(220.0f, 1e-5f));
  }

  // The span is the frame count times the hop, wherever frame 0 is anchored.
  REQUIRE(ridge.length_samples() ==
          static_cast<int64_t>(ridge.f0_hz.size()) * static_cast<int64_t>(kHopLength));
  // This entry point is handed no length and so does not clamp: frame f is at
  // sample f * hop and nothing pulls the end back.
  REQUIRE(ridge.onset_sample == static_cast<int64_t>(ridge.frame_start) * kHopLength);
  REQUIRE(ridge.offset_sample == static_cast<int64_t>(ridge.frame_end()) * kHopLength);

  // No frames is no ridges.
  REQUIRE(track_f0_ridges({}, kHopLength, kSampleRate).empty());
}

TEST_CASE("median_hz is the median of the ridge's own values", "[polyphonic_f0]") {
  // Eleven frames at one pitch and ten at another, a step of thirty-one cents
  // that the jump bound holds: the median is the more common of the two.
  std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 11);
  append_frames(frames, steady_frames(224.0f, 10));
  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);

  REQUIRE(ridges.size() == 1);
  REQUIRE(ridges[0].f0_hz.size() == 21);
  REQUIRE_THAT(ridges[0].median_hz, WithinRel(220.0f, 1e-5f));
  REQUIRE_THAT(ridges[0].median_hz, WithinRel(median_of(ridges[0].f0_hz), 1e-5f));
}

TEST_CASE("a jump under max_jump_cents holds a ridge and one over it breaks", "[polyphonic_f0]") {
  REQUIRE_THAT(RidgeConfig{}.max_jump_cents, WithinAbs(50.0, 1e-6));

  SECTION("forty-five cents, inside the bound") {
    std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 30);
    append_frames(frames, steady_frames(hz_up(220.0f, 45.0f), 30));
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 1);
    REQUIRE(ridges[0].f0_hz.size() == 60);
  }

  SECTION("sixty cents, outside it") {
    std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 30);
    append_frames(frames, steady_frames(hz_up(220.0f, 60.0f), 30));
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 2);
    REQUIRE(ridges[0].frame_start == 0);
    REQUIRE(ridges[0].f0_hz.size() == 30);
    REQUIRE(ridges[1].frame_start == 30);
    REQUIRE(ridges[1].f0_hz.size() == 30);
    REQUIRE_THAT(ridges[0].median_hz, WithinRel(220.0f, 1e-5f));
    REQUIRE_THAT(ridges[1].median_hz, WithinRel(hz_up(220.0f, 60.0f), 1e-5f));
  }

  SECTION("the bound is a per-frame move, not an excursion") {
    // Six frames each stepping forty cents walk two hundred and forty cents in
    // total and stay one ridge, because no single frame moves past the bound.
    std::vector<std::vector<F0Candidate>> frames;
    for (int f = 0; f < 40; ++f) {
      const float cents = 40.0f * static_cast<float>(std::min(f, 6));
      frames.push_back({candidate(hz_up(220.0f, cents), 1.0f)});
    }
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 1);
    REQUIRE(ridges[0].f0_hz.size() == 40);
    REQUIRE(std::abs(cents_between(ridges[0].f0_hz.front(), ridges[0].f0_hz.back())) > 200.0f);
  }
}

TEST_CASE("a salience dip under min_ridge_peak_ratio breaks a ridge", "[polyphonic_f0]") {
  REQUIRE_THAT(RidgeConfig{}.min_ridge_peak_ratio, WithinAbs(0.10, 1e-6));

  SECTION("a dip to thirty percent of the running peak holds") {
    std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 40);
    frames[20] = {candidate(220.0f, 0.30f)};
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 1);
    REQUIRE(ridges[0].f0_hz.size() == 40);
    REQUIRE_THAT(ridges[0].salience[20], WithinAbs(0.30, 1e-5));
  }

  SECTION("a dip to two percent breaks") {
    std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 40);
    frames[20] = {candidate(220.0f, 0.02f)};
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 2);
    REQUIRE(ridges[0].frame_start == 0);
    REQUIRE(ridges[0].f0_hz.size() == 20);
    // The rejected candidate is claimed by no ridge, so it starts one.
    REQUIRE(ridges[1].frame_start == 20);
    REQUIRE(ridges[1].f0_hz.size() == 20);
  }

  SECTION("the share is of the ridge's own running peak, not of the frame") {
    // A ridge that never rises above a quiet level is not broken by being quiet.
    const std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 40, 0.001f);
    const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
    REQUIRE(ridges.size() == 1);
    REQUIRE(ridges[0].f0_hz.size() == 40);
  }
}

TEST_CASE("an empty frame ends every ridge", "[polyphonic_f0]") {
  const float second = hz_up(220.0f, 700.0f);
  std::vector<std::vector<F0Candidate>> frames;
  for (int f = 0; f < 20; ++f) {
    frames.push_back({candidate(220.0f, 1.0f), candidate(second, 0.8f)});
  }
  frames.push_back({});
  for (int f = 0; f < 20; ++f) {
    frames.push_back({candidate(220.0f, 1.0f), candidate(second, 0.8f)});
  }

  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
  REQUIRE(ridges.size() == 4);
  for (const F0Ridge& ridge : ridges) {
    REQUIRE(ridge.f0_hz.size() == 20);
    // Nothing spans the empty frame.
    REQUIRE_FALSE((ridge.frame_start <= 20 && 20 < ridge.frame_end()));
  }
  REQUIRE(ridges[0].frame_start == 0);
  REQUIRE(ridges[1].frame_start == 0);
  REQUIRE(ridges[2].frame_start == 21);
  REQUIRE(ridges[3].frame_start == 21);
}

TEST_CASE("a ridge shorter than min_duration_ms is dropped", "[polyphonic_f0]") {
  const RidgeConfig defaults;
  REQUIRE_THAT(defaults.min_duration_ms, WithinAbs(140.0, 1e-6));

  const float ms_per_frame =
      static_cast<float>(kHopLength) * 1000.0f / static_cast<float>(kSampleRate);
  // Every ridge noise produces is under ten frames, so the longest one is nine,
  // and ten frames is the 116 ms the band's lower endpoint is quoted against.
  REQUIRE_THAT(10.0f * ms_per_frame, WithinAbs(116.0, 0.5));
  // The default clears the noise ceiling's own duration and stays under the
  // point a fragmented low-register voice starts being dropped.
  REQUIRE(9.0f * ms_per_frame < defaults.min_duration_ms);
  REQUIRE(defaults.min_duration_ms < 175.0f);

  // A ridge at the noise ceiling, an empty frame, then a held voice at about
  // three hundred and fifty milliseconds.
  std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 9);
  frames.push_back({});
  append_frames(frames, steady_frames(330.0f, 30));
  REQUIRE(30.0f * ms_per_frame > 2.0f * defaults.min_duration_ms);

  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
  REQUIRE(ridges.size() == 1);
  REQUIRE(ridges[0].frame_start == 10);
  REQUIRE(ridges[0].f0_hz.size() == 30);
  REQUIRE_THAT(ridges[0].median_hz, WithinRel(330.0f, 1e-5f));

  // Both endpoints of the documented band drop the noise-length ridge and keep
  // the held one, which is what makes the band a band rather than a guess.
  for (const float endpoint : {120.0f, 175.0f}) {
    INFO("min_duration_ms " << endpoint);
    RidgeConfig config;
    config.min_duration_ms = endpoint;
    const std::vector<F0Ridge> banded = track_f0_ridges(frames, kHopLength, kSampleRate, config);
    REQUIRE(banded.size() == 1);
    REQUIRE(banded[0].f0_hz.size() == 30);
  }

  // "under that noise survives": ninety milliseconds is under the nine-frame
  // ridge's own hundred and four, so it is kept.
  RidgeConfig too_low;
  too_low.min_duration_ms = 90.0f;
  const std::vector<F0Ridge> noisy = track_f0_ridges(frames, kHopLength, kSampleRate, too_low);
  REQUIRE(noisy.size() == 2);
  REQUIRE(noisy[0].f0_hz.size() == 9);

  // Zero is the inclusive floor of the setting, and keeps everything.
  RidgeConfig keep_all;
  keep_all.min_duration_ms = 0.0f;
  const std::vector<F0Ridge> everything =
      track_f0_ridges(frames, kHopLength, kSampleRate, keep_all);
  REQUIRE(everything.size() == 2);
  REQUIRE(everything[0].f0_hz.size() == 9);
  REQUIRE(everything[1].f0_hz.size() == 30);
}

TEST_CASE("a live ridge takes the nearest candidate, not the loudest", "[polyphonic_f0]") {
  // Two voices forty cents apart: close enough that either is a legal
  // continuation of either ridge, so only "nearest" keeps them straight.
  const float lower = 220.0f;
  const float upper = hz_up(lower, 40.0f);
  REQUIRE(std::abs(cents_between(lower, upper)) < RidgeConfig{}.max_jump_cents);

  std::vector<std::vector<F0Candidate>> frames;
  for (int f = 0; f < 30; ++f) {
    // The loudness order flips halfway through; the pitches do not move.
    if (f < 15) {
      frames.push_back({candidate(lower, 1.0f), candidate(upper, 0.6f)});
    } else {
      frames.push_back({candidate(upper, 1.0f), candidate(lower, 0.6f)});
    }
  }

  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
  REQUIRE(ridges.size() == 2);
  for (const F0Ridge& ridge : ridges) {
    INFO("ridge at " << ridge.median_hz);
    REQUIRE(ridge.f0_hz.size() == 30);
    // Each ridge holds one pitch for its whole life: a ridge that followed the
    // loudest candidate would swap at frame fifteen.
    for (const float value : ridge.f0_hz) {
      REQUIRE_THAT(value, WithinRel(ridge.f0_hz.front(), 1e-5f));
    }
  }
  REQUIRE_THAT(ridges[0].median_hz, WithinRel(lower, 1e-5f));
  REQUIRE_THAT(ridges[1].median_hz, WithinRel(upper, 1e-5f));
}

TEST_CASE("a candidate no ridge takes starts one, and ridges sort by frame_start then median_hz",
          "[polyphonic_f0]") {
  const float low = 200.0f;
  const float high = 500.0f;
  const float late = 300.0f;

  std::vector<std::vector<F0Candidate>> frames;
  for (int f = 0; f < 40; ++f) {
    std::vector<F0Candidate> column;
    // The higher voice is offered first every frame, so a result that kept the
    // input order would put it first.
    column.push_back(candidate(high, 1.0f));
    column.push_back(candidate(low, 0.9f));
    if (f >= 10) column.push_back(candidate(late, 0.8f));
    frames.push_back(std::move(column));
  }

  const std::vector<F0Ridge> ridges = track_f0_ridges(frames, kHopLength, kSampleRate);
  REQUIRE(ridges.size() == 3);

  // Ascending frame_start, ties by ascending median_hz.
  REQUIRE(ridges[0].frame_start == 0);
  REQUIRE(ridges[1].frame_start == 0);
  REQUIRE(ridges[2].frame_start == 10);
  REQUIRE_THAT(ridges[0].median_hz, WithinRel(low, 1e-5f));
  REQUIRE_THAT(ridges[1].median_hz, WithinRel(high, 1e-5f));
  REQUIRE_THAT(ridges[2].median_hz, WithinRel(late, 1e-5f));
  for (size_t i = 1; i < ridges.size(); ++i) {
    INFO("ridge " << i);
    REQUIRE(ridges[i - 1].frame_start <= ridges[i].frame_start);
    if (ridges[i - 1].frame_start == ridges[i].frame_start) {
      REQUIRE(ridges[i - 1].median_hz <= ridges[i].median_hz);
    }
  }

  // The third voice arrives mid-signal and lives from there to the end.
  REQUIRE(ridges[2].f0_hz.size() == 30);
  REQUIRE(ridges[2].onset_sample == static_cast<int64_t>(10) * kHopLength);
}

TEST_CASE("tracking is deterministic", "[polyphonic_f0]") {
  // A layout with every branch in it: two near neighbours, a late arrival, a
  // gap, a salience dip and a jump.
  std::vector<std::vector<F0Candidate>> frames;
  for (int f = 0; f < 60; ++f) {
    if (f == 25) {
      frames.push_back({});
      continue;
    }
    std::vector<F0Candidate> column;
    column.push_back(candidate(hz_up(240.0f, f < 40 ? 0.0f : 45.0f), f == 33 ? 0.05f : 1.0f));
    column.push_back(candidate(hz_up(240.0f, 40.0f), 0.9f));
    if (f >= 12) column.push_back(candidate(430.0f, 0.7f));
    frames.push_back(std::move(column));
  }

  const std::vector<F0Ridge> first = track_f0_ridges(frames, kHopLength, kSampleRate);
  const std::vector<F0Ridge> second = track_f0_ridges(frames, kHopLength, kSampleRate);
  REQUIRE(!first.empty());
  REQUIRE(first.size() == second.size());
  for (size_t i = 0; i < first.size(); ++i) {
    INFO("ridge " << i);
    REQUIRE(first[i].frame_start == second[i].frame_start);
    REQUIRE(first[i].onset_sample == second[i].onset_sample);
    REQUIRE(first[i].offset_sample == second[i].offset_sample);
    REQUIRE(first[i].median_hz == second[i].median_hz);
    REQUIRE(first[i].f0_hz == second[i].f0_hz);
    REQUIRE(first[i].salience == second[i].salience);
  }
}

TEST_CASE("the jump bound and the framing have to be read together", "[polyphonic_f0]") {
  const float bound = RidgeConfig{}.max_jump_cents;

  // The header's own arithmetic for the documented case: +-45 cents at 5.5 Hz
  // moves at most eighteen cents per frame at the default framing.
  REQUIRE_THAT(vibrato_cents_per_frame(45.0f, 5.5f, kHopLength), WithinAbs(18.0, 0.2));
  REQUIRE(vibrato_cents_per_frame(45.0f, 5.5f, kHopLength) < bound);
  // The rest of the same table: the shallow vibrato survives the longer hop with
  // a much narrower margin, and the deep one does not survive it at all.
  REQUIRE_THAT(vibrato_cents_per_frame(45.0f, 5.5f, kBassHop), WithinAbs(36.0, 0.4));
  REQUIRE_THAT(vibrato_cents_per_frame(100.0f, 5.5f, kHopLength), WithinAbs(40.0, 0.4));
  REQUIRE_THAT(vibrato_cents_per_frame(100.0f, 5.5f, kBassHop), WithinAbs(80.0, 0.8));
  REQUIRE(vibrato_cents_per_frame(45.0f, 5.5f, kBassHop) < bound);

  const std::vector<F0Ridge> held = track_f0_ridges(
      vibrato_frames(440.0f, 45.0f, 5.5f, kHopLength, 172), kHopLength, kSampleRate);
  REQUIRE(held.size() == 1);
  REQUIRE(held[0].f0_hz.size() == 172);

  // A deeper vibrato at the same rate still holds at the default hop: a hundred
  // cents moves about forty per frame.
  REQUIRE(vibrato_cents_per_frame(100.0f, 5.5f, kHopLength) < bound);
  const std::vector<F0Ridge> deep = track_f0_ridges(
      vibrato_frames(440.0f, 100.0f, 5.5f, kHopLength, 172), kHopLength, kSampleRate);
  REQUIRE(deep.size() == 1);
  REQUIRE(deep[0].f0_hz.size() == 172);

  // The same gesture at the longer hop the header names for bass material moves
  // eighty cents per frame, past the same bound, and no longer survives as one
  // held voice.
  REQUIRE(vibrato_cents_per_frame(100.0f, 5.5f, kBassHop) > bound);
  const std::vector<F0Ridge> fragments =
      track_f0_ridges(vibrato_frames(440.0f, 100.0f, 5.5f, kBassHop, 86), kBassHop, kSampleRate);
  for (const F0Ridge& ridge : fragments) {
    INFO("fragment of " << ridge.f0_hz.size() << " frames");
    REQUIRE(ridge.f0_hz.size() < 52);
  }
}

TEST_CASE("track_f0_ridges rejects malformed framing, configuration and candidates",
          "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const std::vector<std::vector<F0Candidate>> frames = steady_frames(220.0f, 40);

  for (const int bad : {0, -1, -512}) {
    INFO("hop_length " << bad);
    REQUIRE(code_of([&] { track_f0_ridges(frames, bad, kSampleRate); }) == kInvalid);
  }
  for (const int bad : {0, -1, -44100}) {
    INFO("sample_rate " << bad);
    REQUIRE(code_of([&] { track_f0_ridges(frames, kHopLength, bad); }) == kInvalid);
  }
  for (const float bad : {0.0f, -1.0f, -50.0f}) {
    INFO("max_jump_cents " << bad);
    RidgeConfig config;
    config.max_jump_cents = bad;
    REQUIRE(code_of([&] { track_f0_ridges(frames, kHopLength, kSampleRate, config); }) == kInvalid);
  }
  for (const float bad : {-0.01f, -1.0f, 1.01f, 2.0f, kNaN, kInf}) {
    INFO("min_ridge_peak_ratio " << bad);
    RidgeConfig config;
    config.min_ridge_peak_ratio = bad;
    REQUIRE(code_of([&] { track_f0_ridges(frames, kHopLength, kSampleRate, config); }) == kInvalid);
  }
  for (const float bad : {-0.01f, -1.0f, -140.0f}) {
    INFO("min_duration_ms " << bad);
    RidgeConfig config;
    config.min_duration_ms = bad;
    REQUIRE(code_of([&] { track_f0_ridges(frames, kHopLength, kSampleRate, config); }) == kInvalid);
  }

  // A candidate F0 has to be positive and finite, wherever it sits.
  for (const float bad : {0.0f, -220.0f, kNaN, kInf, -kInf}) {
    INFO("f0_hz " << bad);
    std::vector<std::vector<F0Candidate>> broken = frames;
    broken[17] = {candidate(bad, 1.0f)};
    REQUIRE(code_of([&] { track_f0_ridges(broken, kHopLength, kSampleRate); }) == kInvalid);
    // Including alongside a good one, which a check that only reads a frame's
    // first candidate would miss.
    std::vector<std::vector<F0Candidate>> hidden = frames;
    hidden[17] = {candidate(220.0f, 1.0f), candidate(bad, 0.5f)};
    REQUIRE(code_of([&] { track_f0_ridges(hidden, kHopLength, kSampleRate); }) == kInvalid);
  }

  // The salience too: the candidate ordering is by salience, so a NaN there
  // makes the comparison intransitive rather than merely odd.
  for (const float bad : {kNaN, kInf, -kInf}) {
    INFO("salience " << bad);
    std::vector<std::vector<F0Candidate>> broken = frames;
    broken[17] = {candidate(220.0f, bad)};
    REQUIRE(code_of([&] { track_f0_ridges(broken, kHopLength, kSampleRate); }) == kInvalid);
    std::vector<std::vector<F0Candidate>> hidden = frames;
    hidden[17] = {candidate(220.0f, 1.0f), candidate(226.0f, bad)};
    REQUIRE(code_of([&] { track_f0_ridges(hidden, kHopLength, kSampleRate); }) == kInvalid);
  }
  // A finite negative salience is not rejected: only non-finite is.
  {
    std::vector<std::vector<F0Candidate>> negative = frames;
    negative[17] = {candidate(220.0f, -1.0f)};
    REQUIRE_NOTHROW(track_f0_ridges(negative, kHopLength, kSampleRate));
  }

  // Both ends of every documented range are inside it.
  for (const float ratio : {0.0f, 1.0f}) {
    INFO("min_ridge_peak_ratio " << ratio);
    RidgeConfig config;
    config.min_ridge_peak_ratio = ratio;
    REQUIRE_NOTHROW(track_f0_ridges(frames, kHopLength, kSampleRate, config));
  }
  RidgeConfig no_minimum;
  no_minimum.min_duration_ms = 0.0f;
  REQUIRE_NOTHROW(track_f0_ridges(frames, kHopLength, kSampleRate, no_minimum));
}

// --- extract_multi_f0 ------------------------------------------------------

TEST_CASE("a four-note chord above middle C resolves into four ridges", "[polyphonic_f0]") {
  const sonare::Audio audio = chord_audio(kChordMid, 4, 1.0f, 12);
  const MultiF0Track track = extract_multi_f0(audio);

  REQUIRE(track.sample_rate == kSampleRate);
  REQUIRE(track.hop_length == kHopLength);
  REQUIRE(track.n_frames > 60);
  REQUIRE_THAT(track.frame_rate_hz(),
               WithinRel(static_cast<float>(kSampleRate) / static_cast<float>(kHopLength), 1e-5f));

  // A held chord's voices run the length of the signal, so exactly four ridges
  // cover most of it and each names one chord tone.
  const std::vector<F0Ridge> held = long_ridges(track, 0.7f);
  REQUIRE(held.size() == 4);
  REQUIRE(resolved_tones(track, kChordMid, 4, 40.0f, 0.7f) == 4);

  for (const F0Ridge& ridge : held) {
    INFO("ridge at " << ridge.median_hz);
    REQUIRE(!ridge.f0_hz.empty());
    REQUIRE(ridge.f0_hz.size() == ridge.salience.size());
    // extract_multi_f0 holds the span inside the audio it read, so a ridge
    // reaching the last frame is shorter than its frames alone would make it.
    REQUIRE(ridge.onset_sample >= 0);
    REQUIRE(ridge.onset_sample < ridge.offset_sample);
    REQUIRE(ridge.offset_sample <= static_cast<int64_t>(audio.size()));
    REQUIRE(ridge.length_samples() <=
            static_cast<int64_t>(ridge.f0_hz.size()) * static_cast<int64_t>(kHopLength));
    REQUIRE_THAT(ridge.median_hz, WithinRel(median_of(ridge.f0_hz), 1e-4f));
    // A held tone does not wander: every frame stays inside a quarter tone of
    // the ridge's own median.
    for (const float value : ridge.f0_hz) {
      REQUIRE(std::abs(cents_between(ridge.median_hz, value)) < 50.0f);
    }
  }
}

TEST_CASE("polyphony is one entry per frame and bounds the ridges alive there", "[polyphonic_f0]") {
  const sonare::Audio audio = chord_audio(kChordMid, 4, 1.0f, 12);
  const MultiF0ExtractorConfig config;
  const MultiF0Track track = extract_multi_f0(audio, config);

  REQUIRE(track.polyphony.size() == static_cast<size_t>(track.n_frames));
  for (int frame = 0; frame < track.n_frames; ++frame) {
    INFO("frame " << frame);
    REQUIRE(track.polyphony[static_cast<size_t>(frame)] >= 0);
    REQUIRE(track.polyphony[static_cast<size_t>(frame)] <= config.estimation.max_polyphony);
    // It counts voices before tracking dropped anything, so it can only be at or
    // over the ridges alive at that frame.
    REQUIRE(track.polyphony[static_cast<size_t>(frame)] >= ridges_alive_at(track, frame));
  }

  // A four-voice chord that resolves is estimated as four voices in the typical
  // frame.
  REQUIRE(median_of(track.polyphony) == 4);
}

TEST_CASE("the default framing does not reach a chord two octaves down and a longer window does",
          "[polyphonic_f0]") {
  const sonare::Audio audio = chord_audio(kChordLow, 4, 1.5f, 20);

  const MultiF0Track at_default = extract_multi_f0(audio);
  const int reached_default = resolved_tones(at_default, kChordLow, 4, 40.0f, 0.6f);

  MultiF0ExtractorConfig longer;
  longer.stft = sonare::make_stft_config(kBassNfft, kBassHop);
  const MultiF0Track at_long = extract_multi_f0(audio, longer);
  const int reached_long = resolved_tones(at_long, kChordLow, 4, 40.0f, 0.6f);

  INFO("default framing resolved " << reached_default << ", long window resolved " << reached_long);
  // The header: n_fft 4096 resolves a four-note chord from about C3 up, so a
  // chord rooted at C2 is under it.
  REQUIRE(reached_default < 4);
  // 8192 reaches three of the four, and the one it drops is the chord's top
  // note: the lower voices take the salience peaks and the highest is left
  // under the frame floor.
  REQUIRE(reached_long > reached_default);
  REQUIRE(reached_long == 3);
  REQUIRE(resolved_tones(at_long, kChordLow, 3, 40.0f, 0.6f) == 3);

  // Doubling again resolves all four, so the missing voice is the framing
  // rather than a ceiling on what the method separates in the bass.
  MultiF0ExtractorConfig longest;
  longest.stft = sonare::make_stft_config(2 * kBassNfft, 2 * kBassHop);
  REQUIRE(resolved_tones(extract_multi_f0(audio, longest), kChordLow, 4, 40.0f, 0.6f) == 4);

  // Under the register the framing reaches, the estimator reports composite
  // pitches rather than nothing.
  REQUIRE(!at_default.ridges.empty());
}

TEST_CASE("vibrato stays one ridge and a legato step breaks", "[polyphonic_f0]") {
  SECTION("a vibrato of +-45 cents at 5.5 Hz") {
    std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
    add_vibrato_tone(samples, 440.0f, 45.0f, 5.5f, 0.3f, 10);
    const MultiF0Track track = extract_multi_f0(audio_of(std::move(samples)));

    const std::vector<F0Ridge> held = long_ridges(track, 0.7f);
    REQUIRE(held.size() == 1);
    REQUIRE_THAT(held[0].median_hz, WithinAbs(440.0, 12.0));
    // The ridge carries the excursion rather than flattening it: the modulation
    // is +-45 cents, so the spread across the ridge is close to ninety.
    const float lowest = *std::min_element(held[0].f0_hz.begin(), held[0].f0_hz.end());
    const float highest = *std::max_element(held[0].f0_hz.begin(), held[0].f0_hz.end());
    REQUIRE(cents_between(lowest, highest) > 55.0f);
  }

  SECTION("a legato semitone step") {
    // The two notes cross-fade over twenty milliseconds, so there is no
    // amplitude hole at the junction and the pitch move is the only thing that
    // can break the ridge.
    const size_t total = static_cast<size_t>(kSampleRate);
    const size_t junction = total / 2;
    const size_t fade = static_cast<size_t>(kSampleRate / 100);
    std::vector<float> first(total, 0.0f);
    std::vector<float> second(total, 0.0f);
    add_tone(first, 440.0f, 0.3f, 10);
    add_tone(second, hz_up(440.0f, 100.0f), 0.3f, 10);
    std::vector<float> samples(total, 0.0f);
    for (size_t i = 0; i < total; ++i) {
      float mix = i < junction ? 0.0f : 1.0f;
      if (i >= junction - fade && i <= junction + fade) {
        const float position =
            static_cast<float>(i - (junction - fade)) / static_cast<float>(2 * fade);
        mix = 0.5f - 0.5f * std::cos(sonare::constants::kPi * position);
      }
      samples[i] = (1.0f - mix) * first[i] + mix * second[i];
    }
    const MultiF0Track track = extract_multi_f0(audio_of(std::move(samples)));

    // A semitone is a hundred cents, twice the jump bound, so the two notes are
    // two ridges rather than one.
    const std::vector<F0Ridge> held = long_ridges(track, 0.3f);
    REQUIRE(held.size() == 2);
    REQUIRE_THAT(held[0].median_hz, WithinAbs(440.0, 8.0));
    REQUIRE_THAT(held[1].median_hz, WithinAbs(static_cast<double>(hz_up(440.0f, 100.0f)), 8.0));
    REQUIRE(held[0].frame_start < held[1].frame_start);
    REQUIRE(std::abs(cents_between(held[0].median_hz, held[1].median_hz)) > 80.0f);
  }
}

TEST_CASE(
    "two voices in unison are one voice, and relaxing the separation rule does not "
    "recover the second",
    "[polyphonic_f0]") {
  // Ten cents apart: two distinct sources, well inside one cent-axis bin.
  const float lower = 300.0f;
  std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
  add_tone(samples, lower, 0.25f, 10);
  add_tone(samples, hz_up(lower, 10.0f), 0.25f, 10);
  const sonare::Audio audio = audio_of(std::move(samples));

  // One pitch, but not one ridge: ten cents apart at 300 Hz beat at 1.7 Hz, and
  // the null carries the salience under the ridge floor about twice a second.
  // The pitch claim is what the separation rule governs, so that is what is
  // asserted; the continuity belongs to the beat.
  const std::vector<F0Ridge> held = long_ridges(extract_multi_f0(audio), 0.2f);
  REQUIRE(!held.empty());
  for (const F0Ridge& ridge : held) {
    INFO("ridge at " << ridge.median_hz << " Hz");
    REQUIRE(std::abs(cents_between(lower, ridge.median_hz)) <= 25.0f);
  }

  // "whatever this is set to": lowering the separation rule to nothing must not
  // turn one unison pair into two voices at the same pitch. Only the count of
  // ridges standing at the unison is asserted, because the same paragraph says a
  // low setting lets the interpolation's neighbours be taken as extra
  // candidates, which would put a ridge beside the pair rather than on it.
  // Counted per frame rather than over the whole analysis, because the beat
  // splits the one voice into several ridges in sequence and a total would read
  // that as several voices.
  MultiF0ExtractorConfig no_rule;
  no_rule.estimation.min_separation_cents = 0.0f;
  const MultiF0Track relaxed = extract_multi_f0(audio, no_rule);
  int most_at_once = 0;
  for (int frame = 0; frame < relaxed.n_frames; ++frame) {
    int here = 0;
    for (const F0Ridge& ridge : relaxed.ridges) {
      if (ridge.frame_start > frame || frame >= ridge.frame_end()) continue;
      if (std::abs(cents_between(lower, ridge.median_hz)) <= 25.0f) ++here;
    }
    most_at_once = std::max(most_at_once, here);
  }
  INFO("most ridges standing at the unison in one frame: " << most_at_once);
  REQUIRE(most_at_once <= 1);
}

TEST_CASE("the tonality weight changes nothing on clean material", "[polyphonic_f0]") {
  const sonare::Audio audio = chord_audio(kChordMid, 4, 1.0f, 12);

  MultiF0ExtractorConfig weighted;
  weighted.spectrum.use_tonality = true;
  MultiF0ExtractorConfig plain;
  plain.spectrum.use_tonality = false;

  const MultiF0Track with = extract_multi_f0(audio, weighted);
  const MultiF0Track without = extract_multi_f0(audio, plain);

  REQUIRE(with.n_frames == without.n_frames);
  std::vector<F0Ridge> held_with = long_ridges(with, 0.7f);
  std::vector<F0Ridge> held_without = long_ridges(without, 0.7f);
  // Compared by pitch, not by index. Ridges arrive in start order and a chord's
  // voices all start together, so the tie is broken by salience order -- which
  // tonality reorders without moving any pitch, making an index comparison read
  // a reordering as a pitch change of a whole chord interval.
  const auto by_pitch = [](const F0Ridge& a, const F0Ridge& b) {
    return a.median_hz < b.median_hz;
  };
  std::sort(held_with.begin(), held_with.end(), by_pitch);
  std::sort(held_without.begin(), held_without.end(), by_pitch);
  REQUIRE(held_with.size() == 4);
  REQUIRE(held_with.size() == held_without.size());
  for (size_t i = 0; i < held_with.size(); ++i) {
    INFO("ridge " << i);
    REQUIRE(std::abs(cents_between(held_without[i].median_hz, held_with[i].median_hz)) < 5.0f);
  }
  REQUIRE(resolved_tones(with, kChordMid, 4, 40.0f, 0.7f) ==
          resolved_tones(without, kChordMid, 4, 40.0f, 0.7f));
}

TEST_CASE("the tonality weight does not lose true F0s under a transient", "[polyphonic_f0]") {
  // A chord with a broadband click twelve decibels over it. The header's
  // measured result is one true F0 recovered and one false one dropped, which is
  // a property of its own fixture; what is asserted here is only the direction,
  // that the weighting never costs a true F0.
  std::vector<float> samples(static_cast<size_t>(kSampleRate), 0.0f);
  for (const float tone : kChordMid) add_tone(samples, tone, 0.12f, 12);
  const float chord_peak = peak_of(samples);
  REQUIRE(chord_peak > 0.0f);

  const size_t click_at = static_cast<size_t>(kSampleRate / 2);
  const std::vector<float> burst = noise_samples(21u, 132);  // 3 ms
  const float click_peak = chord_peak * std::pow(10.0f, 12.0f / 20.0f);
  for (size_t i = 0; i < burst.size(); ++i) {
    samples[click_at + i] += click_peak * std::exp(-static_cast<float>(i) / 40.0f) * burst[i];
  }
  const sonare::Audio audio = audio_of(std::move(samples));

  const int frame = static_cast<int>(click_at) / kHopLength;
  const auto true_f0s_at = [&](bool use_tonality) {
    CentSpectrumConfig spectrum;
    spectrum.use_tonality = use_tonality;
    const CentSpectrum cents = cent_spectrum_of(audio, spectrum);
    REQUIRE(frame < cents.n_frames);
    const MultiF0Estimator estimator(cents.axis, MultiF0Config{});
    const std::vector<F0Candidate> found = estimator.estimate(cents.column(frame));
    int matched = 0;
    for (const float tone : kChordMid) {
      for (const F0Candidate& value : found) {
        if (std::abs(cents_between(tone, value.f0_hz)) <= 40.0f) {
          ++matched;
          break;
        }
      }
    }
    return matched;
  };

  const int with = true_f0s_at(true);
  const int without = true_f0s_at(false);
  INFO("true F0s with tonality " << with << ", without " << without);
  REQUIRE(with >= without);
}

TEST_CASE("extract_multi_f0 holds its spans inside the audio and track_f0_ridges does not",
          "[polyphonic_f0]") {
  // A length that ends inside a hop rather than on one, so an unclamped span
  // genuinely overshoots instead of happening to fit.
  const size_t length = static_cast<size_t>(kSampleRate * 3 / 4) + 137;
  REQUIRE(length % static_cast<size_t>(kHopLength) != 0);
  std::vector<float> buffer(length, 0.0f);
  for (const float tone : kChordMid) add_tone(buffer, tone, 0.12f, 12);
  const sonare::Audio audio = audio_of(std::move(buffer));
  const MultiF0Track track = extract_multi_f0(audio);
  REQUIRE(!track.ridges.empty());

  // Every span a set it returns carries is sliceable over the audio it read.
  for (const F0Ridge& ridge : track.ridges) {
    INFO("ridge at " << ridge.median_hz << " over frames [" << ridge.frame_start << ", "
                     << ridge.frame_end() << ")");
    REQUIRE(ridge.onset_sample >= 0);
    REQUIRE(ridge.onset_sample <= ridge.offset_sample);
    REQUIRE(ridge.offset_sample <= static_cast<int64_t>(audio.size()));
  }

  // And the clamp bit rather than the arithmetic happening to fit: at least one
  // ridge runs to the last frame, whose unclamped end is past the audio. Centre
  // padding makes that the normal case for a held chord, not an edge one.
  int reaching_last = 0;
  for (const F0Ridge& ridge : track.ridges) {
    if (ridge.frame_end() != track.n_frames) continue;
    ++reaching_last;
    const int64_t unclamped = static_cast<int64_t>(ridge.frame_end()) * kHopLength;
    REQUIRE(unclamped > static_cast<int64_t>(audio.size()));
    REQUIRE(ridge.offset_sample < unclamped);
  }
  REQUIRE(reaching_last > 0);

  // The other entry point is handed no length and deliberately does not clamp,
  // so the same frame count taken through it ends past where the audio stopped.
  const std::vector<F0Ridge> unclamped_ridges =
      track_f0_ridges(steady_frames(220.0f, track.n_frames), kHopLength, kSampleRate);
  REQUIRE(unclamped_ridges.size() == 1);
  REQUIRE(unclamped_ridges[0].frame_end() == track.n_frames);
  REQUIRE(unclamped_ridges[0].offset_sample == static_cast<int64_t>(track.n_frames) * kHopLength);
  REQUIRE(unclamped_ridges[0].offset_sample > static_cast<int64_t>(audio.size()));
}

TEST_CASE(
    "extract_multi_f0 rejects empty audio, audio too short to frame, and a malformed "
    "configuration",
    "[polyphonic_f0]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Audio audio = chord_audio(kChordMid, 2, 0.5f, 8);

  const sonare::Audio empty;
  REQUIRE(empty.empty());
  REQUIRE(code_of([&] { extract_multi_f0(empty); }) == kInvalid);

  // Under one hop of audio a centred framing yields a single frame, which has no
  // phase difference to read an instantaneous frequency from.
  const sonare::Audio too_short = audio_of(std::vector<float>(100, 0.01f));
  REQUIRE(code_of([&] { extract_multi_f0(too_short); }) == kInvalid);

  // Every stage's rules reach the entry point.
  MultiF0ExtractorConfig bad_axis;
  bad_axis.spectrum.max_hz = bad_axis.spectrum.ref_hz;
  REQUIRE(code_of([&] { extract_multi_f0(audio, bad_axis); }) == kInvalid);

  MultiF0ExtractorConfig bad_estimation;
  bad_estimation.estimation.max_polyphony = 0;
  REQUIRE(code_of([&] { extract_multi_f0(audio, bad_estimation); }) == kInvalid);

  MultiF0ExtractorConfig bad_subtraction;
  bad_subtraction.estimation.subtraction_factor = 0.0f;
  REQUIRE(code_of([&] { extract_multi_f0(audio, bad_subtraction); }) == kInvalid);

  MultiF0ExtractorConfig bad_ridges;
  bad_ridges.ridges.max_jump_cents = 0.0f;
  REQUIRE(code_of([&] { extract_multi_f0(audio, bad_ridges); }) == kInvalid);

  MultiF0ExtractorConfig bad_duration;
  bad_duration.ridges.min_duration_ms = -1.0f;
  REQUIRE(code_of([&] { extract_multi_f0(audio, bad_duration); }) == kInvalid);

  REQUIRE_NOTHROW(extract_multi_f0(audio));
}
