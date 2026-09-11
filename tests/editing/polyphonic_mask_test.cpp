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
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace sonare::editing::polyphony;

namespace {

constexpr int kSampleRate = 44100;
/// The framing every fixture and every expected claim width is reasoned in.
constexpr int kNfft = 4096;
constexpr int kHopLength = 512;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// @brief Tolerance of the reconstruction identity, relative to the value being
///        reconstructed: about sixteen float ulps.
/// @details A float rounding is 6e-8 relative, and the identity passes through
///          one product per note, the accumulation of the per-bin total and the
///          final sum -- a few dozen roundings at a handful of notes, which this
///          covers with room. What it guards against is a division error, and
///          every division error is a fraction of the value: a half where a
///          third belongs is 0.17 of it, a missed claimant 0.5, a dropped note
///          all of it. Those sit five orders of magnitude above this bound, so
///          1e-3 would still catch them and would stop catching a weight table
///          that is merely slightly wrong -- which is the error worth finding.
constexpr float kReconstructionTolerance = 1e-6f;

/// @brief The gap between 1.0f and the next float above it.
/// @details The unit the header states the total's overshoot in.
constexpr float kUlpAboveOne = 1.1920929e-7f;

/// @brief How far a total of @p claimants equal shares may sit from one.
/// @details @c k copies of the rounded @c 1/k accumulated in float: @c 1/k is
///          rounded once and each of the @c k-1 additions rounds by at most half
///          an ULP of a running sum at or under one. @c k ULP is that with room.
///          It is stated per claim count rather than as one number because the
///          supported range runs to a bin many notes claim, where a single
///          constant is either too tight there or too loose everywhere else --
///          and it is still four orders under the error a wrong share makes.
double total_allowance(int claimants) {
  return static_cast<double>(claimants) * static_cast<double>(kUlpAboveOne);
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

// --- Synthetic material ----------------------------------------------------

/// @brief Adds a steady harmonic tone, partials at 1/h amplitude.
/// @details The fixed per-harmonic phase keeps the partials from summing into an
///          impulse train; a steady tone's magnitude spectrum does not depend on
///          it.
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

/// @brief Half a second of one harmonic tone.
/// @details Long enough for a ridge to survive the duration floor, short enough
///          that a case building several spectrograms stays well under a second.
sonare::Audio tone_audio(float f0_hz, int n_partials = 10, float seconds = 0.5f) {
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  add_tone(samples, f0_hz, 0.3f, n_partials);
  return audio_of(std::move(samples));
}

sonare::Spectrogram spectrogram_of(const sonare::Audio& audio,
                                   const sonare::StftConfig& stft = polyphony_stft_defaults()) {
  return sonare::Spectrogram::compute(audio, stft);
}

// --- Hand-built tracks -----------------------------------------------------

/// @brief A ridge holding one pitch over [frame_start, frame_start + n_frames).
F0Ridge steady_ridge(float f0_hz, int frame_start, int n_frames, int hop_length) {
  F0Ridge ridge;
  ridge.frame_start = frame_start;
  ridge.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  ridge.salience.assign(static_cast<size_t>(n_frames), 1.0f);
  ridge.onset_sample = static_cast<int64_t>(frame_start) * hop_length;
  ridge.offset_sample = static_cast<int64_t>(frame_start + n_frames) * hop_length;
  ridge.median_hz = f0_hz;
  return ridge;
}

/// @brief A track carrying @p ridges over @p spec's framing.
MultiF0Track track_over(const sonare::Spectrogram& spec, std::vector<F0Ridge> ridges) {
  MultiF0Track track;
  track.ridges = std::move(ridges);
  track.n_frames = spec.n_frames();
  track.hop_length = spec.hop_length();
  track.sample_rate = spec.sample_rate();
  track.polyphony.assign(static_cast<size_t>(spec.n_frames()), 0);
  // Clamped, because the rejection cases hand this a ridge that deliberately
  // reaches outside the framing and the count is only a fixture detail.
  for (const F0Ridge& ridge : track.ridges) {
    for (int frame = std::max(0, ridge.frame_start);
         frame < std::min(spec.n_frames(), ridge.frame_end()); ++frame) {
      ++track.polyphony[static_cast<size_t>(frame)];
    }
  }
  return track;
}

/// @brief One ridge at @p f0_hz spanning every frame of @p spec.
MultiF0Track single_note_track(const sonare::Spectrogram& spec, float f0_hz) {
  return track_over(spec, {steady_ridge(f0_hz, 0, spec.n_frames(), spec.hop_length())});
}

// --- Reading a mask --------------------------------------------------------

/// @brief Every structural rule @ref NoteMask and @ref NoteMaskSet state.
void require_well_formed(const NoteMaskSet& masks) {
  REQUIRE(masks.n_bins > 0);
  REQUIRE(masks.n_frames > 0);
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    const NoteMask& mask = masks.notes[i];
    INFO("mask " << i << " over frames [" << mask.frame_start << ", " << mask.frame_end() << ")");
    REQUIRE(mask.ridge_index == static_cast<int>(i));
    REQUIRE(mask.frame_start >= 0);
    REQUIRE(mask.n_frames >= 0);
    REQUIRE(mask.frame_end() <= masks.n_frames);
    REQUIRE(mask.frame_offset.size() == static_cast<size_t>(mask.n_frames) + 1);
    REQUIRE(mask.frame_offset.front() == 0);
    REQUIRE(mask.bins.size() == mask.weights.size());
    REQUIRE(mask.frame_offset.back() == static_cast<int32_t>(mask.bins.size()));
    for (int f = 0; f < mask.n_frames; ++f) {
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      // Ascending rather than strictly so: a frame the note spans may be empty.
      REQUIRE(to >= from);
      for (int32_t k = from; k < to; ++k) {
        const size_t at = static_cast<size_t>(k);
        REQUIRE(mask.bins[at] >= 0);
        REQUIRE(mask.bins[at] < masks.n_bins);
        // Ascending within the frame, which is also what rules out a duplicate.
        if (k > from) REQUIRE(mask.bins[at] > mask.bins[at - 1]);
        REQUIRE(std::isfinite(mask.weights[at]));
        REQUIRE(mask.weights[at] > 0.0f);
        REQUIRE(mask.weights[at] <= 1.0f);
      }
    }
  }
}

/// @brief Notes claiming each (bin, frame), in the spectrogram's own layout.
std::vector<int> claim_counts(const NoteMaskSet& masks) {
  require_well_formed(masks);
  std::vector<int> counts(static_cast<size_t>(masks.n_bins) * static_cast<size_t>(masks.n_frames),
                          0);
  for (const NoteMask& mask : masks.notes) {
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const size_t bin = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]);
        ++counts[bin * static_cast<size_t>(masks.n_frames) + static_cast<size_t>(frame)];
      }
    }
  }
  return counts;
}

/// @brief Contiguous bin runs of one frame of a mask, as inclusive [first, last].
/// @details One run is one partial's claim: the partials of the fixtures below
///          are hundreds of hertz apart and the claims tens, so a run boundary
///          is a gap between partials rather than a gap inside one.
std::vector<std::pair<int, int>> claimed_runs(const NoteMask& mask, int frame_in_span) {
  std::vector<std::pair<int, int>> runs;
  const int32_t from = mask.frame_offset[static_cast<size_t>(frame_in_span)];
  const int32_t to = mask.frame_offset[static_cast<size_t>(frame_in_span) + 1];
  for (int32_t k = from; k < to; ++k) {
    const int bin = static_cast<int>(mask.bins[static_cast<size_t>(k)]);
    if (!runs.empty() && bin == runs.back().second + 1) {
      runs.back().second = bin;
    } else {
      runs.emplace_back(bin, bin);
    }
  }
  return runs;
}

/// @brief One framing's claim geometry, measured off the mask it produces.
struct ClaimProbe {
  int n_fft = 0;
  int win_length = 0;
  float bin_hz = 0.0f;
  std::vector<std::pair<int, int>> runs;

  /// @brief Width of the fundamental's claim, as a bin index span.
  int span_bins() const { return runs.front().second - runs.front().first; }
  float span_hz() const { return static_cast<float>(span_bins()) * bin_hz; }
};

/// @brief Builds one note's mask at @p n_fft / @p win_length and reads its runs.
ClaimProbe probe_claim(int n_fft, int win_length, float f0_hz, const NoteMaskConfig& config) {
  sonare::StftConfig stft = sonare::make_stft_config(n_fft, kHopLength);
  stft.win_length = win_length;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(f0_hz), stft);
  REQUIRE(spec.win_length() == win_length);
  REQUIRE(spec.n_frames() > 2);

  const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, f0_hz), config);
  require_well_formed(masks);
  REQUIRE(masks.notes.size() == 1);
  REQUIRE(masks.notes[0].n_frames > 2);

  ClaimProbe probe;
  probe.n_fft = n_fft;
  probe.win_length = win_length;
  probe.bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(n_fft);
  probe.runs = claimed_runs(masks.notes[0], masks.notes[0].n_frames / 2);
  REQUIRE(!probe.runs.empty());
  return probe;
}

// --- The identity the representation rests on ------------------------------

/// @brief Asserts that the notes plus the residual return @p spec.
/// @details Both readings the header states: directly on the reconstructed
///          complex values, and against @ref mask_total, which is what a later
///          stage checks instead of assuming.
///
///          The worst deviation is accumulated and asserted once rather than
///          asserted per entry -- the loop covers every bin of every frame, and
///          a Catch assertion each would cost more than the masking it checks.
void require_reconstructs(const sonare::Spectrogram& spec, const NoteMaskSet& masks) {
  // The floor covers a handful of notes at one bin; past that the shares
  // themselves are what rounds, so the allowance for the busiest bin takes over.
  const std::vector<int> counts = claim_counts(masks);
  const int busiest = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
  const double tolerance =
      std::max(static_cast<double>(kReconstructionTolerance), 4.0 * total_allowance(busiest));

  std::vector<sonare::Spectrogram> parts;
  parts.reserve(masks.notes.size());
  for (const NoteMask& mask : masks.notes) parts.push_back(apply_note_mask(spec, mask));
  const sonare::Spectrogram residual = residual_spectrum(spec, masks);
  const std::vector<float> total = mask_total(masks);

  REQUIRE(residual.n_bins() == spec.n_bins());
  REQUIRE(residual.n_frames() == spec.n_frames());
  REQUIRE(total.size() ==
          static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames()));

  double worst_sum = 0.0;
  double worst_share = 0.0;
  double worst_residual = 0.0;
  int worst_bin = 0;
  int worst_frame = 0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      const std::complex<float> want = spec.at(bin, frame);
      const float weight = total[static_cast<size_t>(bin) * static_cast<size_t>(spec.n_frames()) +
                                 static_cast<size_t>(frame)];
      std::complex<float> notes(0.0f, 0.0f);
      for (const sonare::Spectrogram& part : parts) notes += part.at(bin, frame);
      const std::complex<float> carried = residual.at(bin, frame);

      // An exactly zero bin has nothing to be relative to, and every term is a
      // product of it, so its reconstruction is exactly zero.
      const double scale = static_cast<double>(std::abs(want));
      const double denominator = scale > 0.0 ? scale : 1.0;

      const double error = static_cast<double>(std::abs(notes + carried - want)) / denominator;
      if (error > worst_sum) {
        worst_sum = error;
        worst_bin = bin;
        worst_frame = frame;
      }
      worst_share =
          std::max(worst_share, static_cast<double>(std::abs(notes - want * weight)) / denominator);
      worst_residual =
          std::max(worst_residual,
                   static_cast<double>(std::abs(carried - want * (1.0f - weight))) / denominator);
    }
  }

  INFO("worst at bin " << worst_bin << " frame " << worst_frame << ", busiest bin claimed by "
                       << busiest);
  REQUIRE(worst_sum <= tolerance);
  REQUIRE(worst_share <= tolerance);
  REQUIRE(worst_residual <= tolerance);
}

/// @brief Total |z|^2 over a spectrogram.
double energy_of(const sonare::Spectrogram& spec) {
  double total = 0.0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      total += static_cast<double>(std::norm(spec.at(bin, frame)));
    }
  }
  return total;
}

}  // namespace

// --- Shape and numbering ---------------------------------------------------

TEST_CASE("build_note_masks returns one mask per ridge, in the track's order", "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  REQUIRE(spec.n_frames() > 20);

  // Three ridges with deliberately different spans, so a mask that took the
  // track's framing rather than its own ridge's shows up as a wrong span.
  const int frames = spec.n_frames();
  const MultiF0Track track =
      track_over(spec, {steady_ridge(220.0f, 0, frames, kHopLength),
                        steady_ridge(330.0f, 4, frames - 8, kHopLength),
                        steady_ridge(523.25f, frames / 2, frames - frames / 2, kHopLength)});

  const NoteMaskSet masks = build_note_masks(spec, track);
  require_well_formed(masks);

  // The set describes the framing it was built over.
  REQUIRE(masks.n_bins == spec.n_bins());
  REQUIRE(masks.n_frames == spec.n_frames());
  REQUIRE(masks.hop_length == spec.hop_length());
  REQUIRE(masks.sample_rate == spec.sample_rate());

  REQUIRE(masks.notes.size() == track.ridges.size());
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    INFO("mask " << i);
    // The documented index, so a mask traces back to the pitch that produced it.
    REQUIRE(masks.notes[i].ridge_index == static_cast<int>(i));
    // And the span it describes is that ridge's span.
    REQUIRE(masks.notes[i].frame_start == track.ridges[i].frame_start);
    REQUIRE(masks.notes[i].n_frames == static_cast<int>(track.ridges[i].f0_hz.size()));
    REQUIRE(masks.notes[i].frame_end() == track.ridges[i].frame_end());
    // A note whose partials are inside the spectrum claims something.
    REQUIRE(!masks.notes[i].bins.empty());
  }
}

TEST_CASE("a track with no ridges leaves the whole spectrum in the residual", "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(440.0f));
  const NoteMaskSet masks = build_note_masks(spec, track_over(spec, {}));

  REQUIRE(masks.notes.empty());
  REQUIRE(masks.n_bins == spec.n_bins());
  REQUIRE(masks.n_frames == spec.n_frames());

  // Nothing claims anything, so the total is zero by construction rather than by
  // arithmetic and there is no rounding to allow for.
  const std::vector<float> total = mask_total(masks);
  REQUIRE(total.size() ==
          static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames()));
  for (const float weight : total) REQUIRE(weight == 0.0f);

  require_reconstructs(spec, masks);
}

TEST_CASE("a note whose every partial is over Nyquist spans its frames and claims nothing",
          "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(440.0f));
  // 30 kHz at 44.1 kHz: the fundamental is already past Nyquist, so every
  // partial falls outside the spectrum.
  const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 30000.0f));
  require_well_formed(masks);

  REQUIRE(masks.notes.size() == 1);
  const NoteMask& mask = masks.notes[0];
  // The span is still the ridge's; the emptiness is per frame, which is the case
  // the header names.
  REQUIRE(mask.n_frames == spec.n_frames());
  REQUIRE(mask.bins.empty());
  REQUIRE(mask.weights.empty());
  for (const int32_t offset : mask.frame_offset) REQUIRE(offset == 0);

  for (const float weight : mask_total(masks)) REQUIRE(weight == 0.0f);
  require_reconstructs(spec, masks);
}

// --- The weights -----------------------------------------------------------

TEST_CASE("a bin one note claims is whole and a bin several claim is split equally",
          "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));

  SECTION("one note takes every bin it claims whole") {
    const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 300.0f));
    require_well_formed(masks);
    REQUIRE(!masks.notes[0].weights.empty());
    for (const float weight : masks.notes[0].weights) REQUIRE(weight == 1.0f);
  }

  SECTION("two notes at one pitch halve every bin") {
    // The same F0 twice: every claimed bin has exactly two claimants, so the
    // split is read without depending on which partials happen to collide.
    const NoteMaskSet masks = build_note_masks(
        spec, track_over(spec, {steady_ridge(300.0f, 0, spec.n_frames(), kHopLength),
                                steady_ridge(300.0f, 0, spec.n_frames(), kHopLength)}));
    require_well_formed(masks);
    REQUIRE(masks.notes.size() == 2);
    REQUIRE(masks.notes[0].bins == masks.notes[1].bins);
    for (const NoteMask& mask : masks.notes) {
      REQUIRE(!mask.weights.empty());
      // A half is exact in binary, so this is an equality and not a tolerance.
      for (const float weight : mask.weights) REQUIRE(weight == 0.5f);
    }
  }

  SECTION("three notes at one pitch take a third each") {
    std::vector<F0Ridge> ridges;
    for (int i = 0; i < 3; ++i) {
      ridges.push_back(steady_ridge(300.0f, 0, spec.n_frames(), kHopLength));
    }
    const NoteMaskSet masks = build_note_masks(spec, track_over(spec, std::move(ridges)));
    require_well_formed(masks);
    REQUIRE(masks.notes.size() == 3);
    for (const NoteMask& mask : masks.notes) {
      REQUIRE(!mask.weights.empty());
      for (const float weight : mask.weights) {
        REQUIRE_THAT(weight, WithinRel(1.0f / 3.0f, kReconstructionTolerance));
      }
    }
  }

  SECTION("a fifth apart, where some partials collide and most do not") {
    // 300 and 450 Hz are 3:2, so partials coincide at 900, 1800, 2700 and so on
    // while the rest stand alone. Both populations have to be non-empty or the
    // case only exercises one of the two rules.
    const NoteMaskSet masks = build_note_masks(
        spec, track_over(spec, {steady_ridge(300.0f, 0, spec.n_frames(), kHopLength),
                                steady_ridge(450.0f, 0, spec.n_frames(), kHopLength)}));
    require_well_formed(masks);
    const std::vector<int> counts = claim_counts(masks);

    int alone = 0;
    int shared = 0;
    for (const NoteMask& mask : masks.notes) {
      for (int f = 0; f < mask.n_frames; ++f) {
        const int frame = mask.frame_start + f;
        const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
        const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
        for (int32_t k = from; k < to; ++k) {
          const size_t at = static_cast<size_t>(k);
          const int bin = static_cast<int>(mask.bins[at]);
          const int claimants =
              counts[static_cast<size_t>(bin) * static_cast<size_t>(masks.n_frames) +
                     static_cast<size_t>(frame)];
          REQUIRE(claimants >= 1);
          if (claimants == 1) {
            ++alone;
            REQUIRE(mask.weights[at] == 1.0f);
          } else {
            ++shared;
            REQUIRE_THAT(mask.weights[at],
                         WithinRel(1.0f / static_cast<float>(claimants), kReconstructionTolerance));
          }
        }
      }
    }
    INFO("bins claimed alone " << alone << ", shared " << shared);
    REQUIRE(alone > 0);
    REQUIRE(shared > 0);
  }
}

TEST_CASE("mask_total never falls below zero and is one at every claimed bin", "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));
  const NoteMaskSet masks = build_note_masks(
      spec, track_over(spec, {steady_ridge(300.0f, 0, spec.n_frames(), kHopLength),
                              steady_ridge(450.0f, 2, spec.n_frames() - 4, kHopLength),
                              steady_ridge(300.0f, 5, spec.n_frames() - 10, kHopLength)}));
  require_well_formed(masks);

  const std::vector<float> total = mask_total(masks);
  const std::vector<int> counts = claim_counts(masks);
  REQUIRE(total.size() == counts.size());
  REQUIRE(total.size() ==
          static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames()));

  // Walked without a Catch assertion per entry -- the surface is every bin of
  // every frame -- and reported as the two extremes plus the counts that
  // disagree with the claim map.
  float lowest = 2.0f;
  float highest = -1.0f;
  size_t unclaimed_nonzero = 0;
  size_t claimed_not_one = 0;
  size_t claimed = 0;
  for (size_t i = 0; i < total.size(); ++i) {
    const float weight = total[i];
    REQUIRE(std::isfinite(weight));
    lowest = std::min(lowest, weight);
    highest = std::max(highest, weight);
    if (counts[i] == 0) {
      if (weight != 0.0f) ++unclaimed_nonzero;
    } else {
      ++claimed;
      if (static_cast<double>(std::abs(weight - 1.0f)) > total_allowance(counts[i])) {
        ++claimed_not_one;
      }
    }
  }

  REQUIRE(lowest >= 0.0f);
  // Never below zero, and past one only by the rounding of adding k copies of
  // 1/k -- the total is deliberately not clamped, so the bound is that rounding
  // and not exactly one. Written against the busiest bin rather than a constant
  // so a wider fixture does not need the number loosened.
  const int busiest = *std::max_element(counts.begin(), counts.end());
  REQUIRE(busiest >= 2);
  REQUIRE(static_cast<double>(highest) <= 1.0 + total_allowance(busiest));
  // Equal shares of one bin sum to that bin, so a claimed bin's total is one and
  // an unclaimed one's is zero -- which is also what makes the residual carry
  // exactly what no note claimed.
  REQUIRE(claimed > 0);
  REQUIRE(unclaimed_nonzero == 0);
  REQUIRE(claimed_not_one == 0);
}

TEST_CASE("every note's masked spectrum plus the residual is the input", "[polyphony_mask]") {
  SECTION("a track a real extraction produced") {
    std::vector<float> samples(static_cast<size_t>(kSampleRate / 2), 0.0f);
    add_tone(samples, 329.6276f, 0.25f, 10);
    add_tone(samples, 493.8833f, 0.25f, 10);
    const sonare::Audio audio = audio_of(std::move(samples));

    const sonare::Spectrogram spec = spectrogram_of(audio);
    const MultiF0Track track = extract_multi_f0(audio);
    // The same framing on both sides, which is what the builder requires and
    // what makes a real track usable here at all.
    REQUIRE(track.n_frames == spec.n_frames());
    REQUIRE(!track.ridges.empty());

    const NoteMaskSet masks = build_note_masks(spec, track);
    require_well_formed(masks);
    require_reconstructs(spec, masks);
  }

  SECTION("a hand-built track whose notes overlap heavily") {
    // An octave and a fifth over one root, which is the overlap the header calls
    // the normal case: ten of the lower note's twenty partials sit on the
    // octave's and six on the fifth's.
    const sonare::Spectrogram spec = spectrogram_of(tone_audio(220.0f));
    const NoteMaskSet masks = build_note_masks(
        spec, track_over(spec, {steady_ridge(220.0f, 0, spec.n_frames(), kHopLength),
                                steady_ridge(440.0f, 0, spec.n_frames(), kHopLength),
                                steady_ridge(330.0f, 3, spec.n_frames() - 6, kHopLength)}));
    require_well_formed(masks);
    // The overlap is real, or the section checks the disjoint case a second time.
    const std::vector<int> counts = claim_counts(masks);
    REQUIRE(*std::max_element(counts.begin(), counts.end()) >= 3);
    require_reconstructs(spec, masks);
  }
}

TEST_CASE("apply_note_mask zeroes every bin the mask does not name", "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));
  const NoteMaskSet masks = build_note_masks(
      spec, track_over(spec, {steady_ridge(300.0f, 2, spec.n_frames() - 5, kHopLength),
                              steady_ridge(450.0f, 0, spec.n_frames(), kHopLength)}));
  require_well_formed(masks);

  const NoteMask& mask = masks.notes[0];
  const sonare::Spectrogram masked = apply_note_mask(spec, mask);

  // The same shape, so the result is that note's spectrogram rather than a
  // reshaped one -- including the window, which is what its iSTFT divides by.
  REQUIRE(masked.n_bins() == spec.n_bins());
  REQUIRE(masked.n_frames() == spec.n_frames());
  REQUIRE(masked.n_fft() == spec.n_fft());
  REQUIRE(masked.hop_length() == spec.hop_length());
  REQUIRE(masked.sample_rate() == spec.sample_rate());
  REQUIRE(masked.window() == spec.window());

  // Everything the mask names carries that share of the input, and nothing else
  // carries anything. Counted and asserted once rather than per bin: the second
  // sweep walks the whole surface.
  std::vector<bool> named(static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames()),
                          false);
  size_t wrong_share = 0;
  size_t checked = 0;
  for (int f = 0; f < mask.n_frames; ++f) {
    const int frame = mask.frame_start + f;
    const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
    const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
    for (int32_t k = from; k < to; ++k) {
      const size_t at = static_cast<size_t>(k);
      const int bin = static_cast<int>(mask.bins[at]);
      named[static_cast<size_t>(bin) * static_cast<size_t>(spec.n_frames()) +
            static_cast<size_t>(frame)] = true;
      ++checked;
      const std::complex<float> want = spec.at(bin, frame) * mask.weights[at];
      const float scale = std::abs(want);
      if (std::abs(masked.at(bin, frame) - want) >
          kReconstructionTolerance * (scale > 0.0f ? scale : 1.0f)) {
        ++wrong_share;
      }
    }
  }
  REQUIRE(checked > 0);
  REQUIRE(wrong_share == 0);

  size_t leaked = 0;
  size_t outside_span = 0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      if (named[static_cast<size_t>(bin) * static_cast<size_t>(spec.n_frames()) +
                static_cast<size_t>(frame)]) {
        continue;
      }
      if (masked.at(bin, frame) != std::complex<float>(0.0f, 0.0f)) ++leaked;
      if (frame < mask.frame_start || frame >= mask.frame_end()) ++outside_span;
    }
  }
  // Every frame outside the note's span is unnamed in full, so this count says
  // the sweep really covered frames the note does not span.
  REQUIRE(outside_span > 0);
  REQUIRE(leaked == 0);
}

// --- Claim geometry --------------------------------------------------------

TEST_CASE("a claim is a fixed frequency span that travels across zero padding",
          "[polyphony_mask]") {
  // Six partials of 300 Hz reach 1800 Hz, hundreds of hertz apart, so every
  // claim below is a run of its own however wide the lobe gets.
  NoteMaskConfig config;
  config.n_harmonics = 6;
  config.claim_lobes = 1.0f;

  // The Hann main lobe is 4 * n_fft / win_length bins wide and a claim of one
  // lobe reaches half of that either side of the partial, so the width in hertz
  // is 4 * sr / win_length: it depends on the window and not on the transform
  // size.
  const ClaimProbe plain = probe_claim(kNfft, kNfft, 300.0f, config);
  const ClaimProbe padded = probe_claim(2 * kNfft, kNfft, 300.0f, config);
  const ClaimProbe shorter = probe_claim(kNfft, kNfft / 2, 300.0f, config);

  for (const ClaimProbe* probe : {&plain, &padded, &shorter}) {
    INFO("n_fft " << probe->n_fft << " win_length " << probe->win_length);
    REQUIRE(probe->runs.size() == static_cast<size_t>(config.n_harmonics));
    // The stated width, read as a bin span. The grid can move an edge by a bin
    // either way; a width taken from n_fft alone, or fixed in bins, is off by a
    // factor of two, so the band separates the readings.
    const double expected = 4.0 * static_cast<double>(config.claim_lobes) *
                            static_cast<double>(probe->n_fft) /
                            static_cast<double>(probe->win_length);
    REQUIRE_THAT(static_cast<double>(probe->span_bins()), WithinAbs(expected, 1.5));
  }

  // Zero padding: twice the transform over the same window is twice the bins and
  // the same frequency span. This is the claim the header makes for the unit and
  // the one a width written in bins fails.
  REQUIRE(padded.span_bins() > plain.span_bins() + 1);
  REQUIRE_THAT(static_cast<double>(padded.span_hz()),
               WithinAbs(static_cast<double>(plain.span_hz()), static_cast<double>(plain.bin_hz)));

  // Halving the window at the same transform size is the other axis: the same
  // bin count as the padded probe and twice the frequency span, because a
  // shorter window really does have a wider lobe.
  REQUIRE(std::abs(shorter.span_bins() - padded.span_bins()) <= 1);
  REQUIRE(shorter.span_hz() > 1.5f * plain.span_hz());
  REQUIRE_THAT(static_cast<double>(shorter.span_hz()),
               WithinAbs(2.0 * static_cast<double>(plain.span_hz()),
                         static_cast<double>(2.0f * plain.bin_hz)));

  // And the value scales the width it is the unit of.
  NoteMaskConfig wider = config;
  wider.claim_lobes = 2.0f;
  const ClaimProbe doubled = probe_claim(kNfft, kNfft, 300.0f, wider);
  REQUIRE_THAT(static_cast<double>(doubled.span_bins()),
               WithinAbs(2.0 * static_cast<double>(plain.span_bins()), 1.5));
}

TEST_CASE("a partial over Nyquist claims nothing and the ones under it claim in order",
          "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(440.0f));
  NoteMaskConfig config;
  config.n_harmonics = 20;

  // At 5 kHz only four partials are under the 22.05 kHz Nyquist, so a count high
  // for the register costs the loop and nothing else.
  const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 5000.0f), config);
  require_well_formed(masks);
  const std::vector<std::pair<int, int>> runs =
      claimed_runs(masks.notes[0], masks.notes[0].n_frames / 2);
  REQUIRE(runs.size() == 4);

  const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kNfft);
  for (size_t h = 0; h < runs.size(); ++h) {
    INFO("partial " << h + 1);
    const float centre = 0.5f * static_cast<float>(runs[h].first + runs[h].second) * bin_hz;
    REQUIRE_THAT(static_cast<double>(centre),
                 WithinAbs(5000.0 * static_cast<double>(h + 1), static_cast<double>(bin_hz)));
  }

  // And the count is honoured: the same note with two harmonics claims two.
  NoteMaskConfig fewer = config;
  fewer.n_harmonics = 2;
  const NoteMaskSet narrow = build_note_masks(spec, single_note_track(spec, 5000.0f), fewer);
  require_well_formed(narrow);
  REQUIRE(claimed_runs(narrow.notes[0], narrow.notes[0].n_frames / 2).size() == 2);
}

TEST_CASE("inharmonicity stretches the upper claims sharp and leaves the fundamental",
          "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));
  NoteMaskConfig ideal;
  ideal.n_harmonics = 10;
  NoteMaskConfig stiff = ideal;
  stiff.inharmonicity = 5e-4f;

  const NoteMaskSet plain = build_note_masks(spec, single_note_track(spec, 300.0f), ideal);
  const NoteMaskSet stretched = build_note_masks(spec, single_note_track(spec, 300.0f), stiff);
  require_well_formed(plain);
  require_well_formed(stretched);

  const int frame = plain.notes[0].n_frames / 2;
  const std::vector<std::pair<int, int>> ideal_runs = claimed_runs(plain.notes[0], frame);
  const std::vector<std::pair<int, int>> stiff_runs = claimed_runs(stretched.notes[0], frame);
  REQUIRE(ideal_runs.size() == 10);
  REQUIRE(stiff_runs.size() == 10);

  const float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kNfft);
  const auto centre_hz = [&](const std::pair<int, int>& run) {
    return 0.5f * static_cast<float>(run.first + run.second) * bin_hz;
  };

  // f_h = h * f0 * sqrt(1 + B h^2), so at B = 5e-4 the fundamental moves by a
  // quarter of a hertz and partial ten by seventy-four.
  REQUIRE_THAT(
      static_cast<double>(centre_hz(stiff_runs[0])),
      WithinAbs(static_cast<double>(centre_hz(ideal_runs[0])), static_cast<double>(bin_hz)));
  for (size_t h = 0; h < 10; ++h) {
    INFO("partial " << h + 1);
    const double harmonic = static_cast<double>(h + 1);
    const double expected =
        300.0 * harmonic *
        std::sqrt(1.0 + static_cast<double>(stiff.inharmonicity) * harmonic * harmonic);
    REQUIRE_THAT(static_cast<double>(centre_hz(stiff_runs[h])),
                 WithinAbs(expected, static_cast<double>(bin_hz)));
    // Sharp, never flat.
    REQUIRE(centre_hz(stiff_runs[h]) >= centre_hz(ideal_runs[h]));
  }
  // The stretch grows with the harmonic number, so the top partial's claim moves
  // by more than the grid could account for.
  REQUIRE(centre_hz(stiff_runs[9]) - centre_hz(ideal_runs[9]) > 2.0f * bin_hz);
}

// --- The residual ----------------------------------------------------------

TEST_CASE("the residual holds what no note claims and noise puts more there", "[polyphony_mask]") {
  const size_t length = static_cast<size_t>(kSampleRate / 2);
  std::vector<float> clean(length, 0.0f);
  add_tone(clean, 440.0f, 0.3f, 10);

  std::vector<float> noisy = clean;
  const std::vector<float> noise = noise_samples(7u, length);
  for (size_t i = 0; i < length; ++i) noisy[i] += 0.05f * noise[i];

  /// @brief Share of the input's energy no note claimed.
  const auto residual_share = [](std::vector<float> samples) {
    const sonare::Audio audio = audio_of(std::move(samples));
    const sonare::Spectrogram spec = spectrogram_of(audio);
    const MultiF0Track track = extract_multi_f0(audio);
    REQUIRE(track.n_frames == spec.n_frames());
    REQUIRE(!track.ridges.empty());
    const NoteMaskSet masks = build_note_masks(spec, track);
    const double total = energy_of(spec);
    REQUIRE(total > 0.0);
    return energy_of(residual_spectrum(spec, masks)) / total;
  };

  const double tonal = residual_share(clean);
  const double with_noise = residual_share(noisy);
  INFO("residual share clean " << tonal << ", with noise " << with_noise);

  // The claims took something, and an empty residual is not the goal either, so
  // both ends are asserted as directions rather than as levels.
  REQUIRE(tonal > 0.0);
  REQUIRE(tonal < 1.0);
  // Noise sits at no partial position, so it lands in the residual and raises
  // its share of the whole.
  REQUIRE(with_noise > tonal);
}

// --- Rejections ------------------------------------------------------------

TEST_CASE("build_note_masks rejects an empty spectrogram and a track from another framing",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const MultiF0Track track = single_note_track(spec, 330.0f);
  REQUIRE_NOTHROW(build_note_masks(spec, track));

  // The empty spectrogram is the only malformed one reachable from outside:
  // Spectrogram::compute validates the framing, and from_complex rejects a
  // non-positive n_fft, hop_length or sample_rate itself, so the rest of that
  // clause guards a spectrogram no caller can hand it.
  const sonare::Spectrogram empty;
  REQUIRE(empty.empty());
  REQUIRE(code_of([&] { return build_note_masks(empty, track); }) == kInvalid);

  for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
    INFO("hop_length " << bad);
    MultiF0Track wrong = track;
    wrong.hop_length = bad;
    REQUIRE(code_of([&] { return build_note_masks(spec, wrong); }) == kInvalid);
  }
  for (const int bad : {kSampleRate + 1, 48000, 0, -1}) {
    INFO("sample_rate " << bad);
    MultiF0Track wrong = track;
    wrong.sample_rate = bad;
    REQUIRE(code_of([&] { return build_note_masks(spec, wrong); }) == kInvalid);
  }
  for (const int delta : {1, -1}) {
    INFO("n_frames offset by " << delta);
    MultiF0Track wrong = track;
    wrong.n_frames = spec.n_frames() + delta;
    REQUIRE(code_of([&] { return build_note_masks(spec, wrong); }) == kInvalid);
  }
  {
    MultiF0Track wrong = track;
    wrong.n_frames = 0;
    REQUIRE(code_of([&] { return build_note_masks(spec, wrong); }) == kInvalid);
  }
}

TEST_CASE("build_note_masks rejects a claim geometry outside its documented range",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const MultiF0Track track = single_note_track(spec, 330.0f);

  SECTION("n_harmonics is in [1, 128]") {
    for (const int bad : {0, -1, -20, 129, 1000}) {
      INFO("n_harmonics " << bad);
      NoteMaskConfig config;
      config.n_harmonics = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, track, config); }) == kInvalid);
    }
    // Both ends of the range are inside it.
    for (const int good : {1, 128}) {
      INFO("n_harmonics " << good);
      NoteMaskConfig config;
      config.n_harmonics = good;
      REQUIRE_NOTHROW(build_note_masks(spec, track, config));
    }
  }

  SECTION("claim_lobes is in (0, 64]") {
    for (const float bad : {0.0f, -1.0f, -64.0f, 64.5f, 1000.0f}) {
      INFO("claim_lobes " << bad);
      NoteMaskConfig config;
      config.claim_lobes = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, track, config); }) == kInvalid);
    }
    // Zero is out and the upper bound is in, so the two ends need different
    // comparisons and a guard using one of them twice fails here.
    for (const float good : {1e-3f, 0.5f, 64.0f}) {
      INFO("claim_lobes " << good);
      NoteMaskConfig config;
      config.claim_lobes = good;
      REQUIRE_NOTHROW(build_note_masks(spec, track, config));
    }
  }

  SECTION("inharmonicity is not negative") {
    for (const float bad : {-1e-6f, -5e-4f, -1.0f}) {
      INFO("inharmonicity " << bad);
      NoteMaskConfig config;
      config.inharmonicity = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, track, config); }) == kInvalid);
    }
    for (const float good : {0.0f, 5e-4f}) {
      INFO("inharmonicity " << good);
      NoteMaskConfig config;
      config.inharmonicity = good;
      REQUIRE_NOTHROW(build_note_masks(spec, track, config));
    }
  }

  SECTION("a non-finite geometry is outside every range") {
    // The header states ranges rather than finiteness, and this is the reading a
    // range check written as a pair of comparisons takes: a NaN is in no
    // interval and an infinity is outside both bounded ones. Its own section
    // because a guard phrased as a negated rejection admits a NaN, and that
    // difference is worth reading as its own failure.
    for (const float bad : {kNaN, kInf, -kInf}) {
      INFO("claim_lobes " << bad);
      NoteMaskConfig config;
      config.claim_lobes = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, track, config); }) == kInvalid);
    }
    for (const float bad : {kNaN, kInf, -kInf}) {
      INFO("inharmonicity " << bad);
      NoteMaskConfig config;
      config.inharmonicity = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, track, config); }) == kInvalid);
    }
  }
}

TEST_CASE("apply_note_mask rejects a mask that reaches outside the spectrogram",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 330.0f));
  require_well_formed(masks);
  const NoteMask& good = masks.notes[0];
  REQUIRE_NOTHROW(apply_note_mask(spec, good));
  REQUIRE(good.bins.size() > 2);

  SECTION("a bin past either end") {
    for (const int bad : {spec.n_bins(), spec.n_bins() + 1, -1, -1000}) {
      INFO("bin " << bad);
      NoteMask reaching = good;
      reaching.bins.back() = static_cast<int32_t>(bad);
      REQUIRE(code_of([&] { return apply_note_mask(spec, reaching); }) == kInvalid);
      // And in the middle of the list, which a check that only reads the ends of
      // an ascending array would miss.
      NoteMask hidden = good;
      hidden.bins[hidden.bins.size() / 2] = static_cast<int32_t>(bad);
      REQUIRE(code_of([&] { return apply_note_mask(spec, hidden); }) == kInvalid);
    }
  }

  SECTION("a span past either end") {
    NoteMask before = good;
    before.frame_start = -1;
    REQUIRE(code_of([&] { return apply_note_mask(spec, before); }) == kInvalid);

    NoteMask after = good;
    after.frame_start = spec.n_frames() - after.n_frames + 1;
    REQUIRE(after.frame_end() == spec.n_frames() + 1);
    REQUIRE(code_of([&] { return apply_note_mask(spec, after); }) == kInvalid);

    NoteMask far = good;
    far.frame_start = spec.n_frames();
    REQUIRE(code_of([&] { return apply_note_mask(spec, far); }) == kInvalid);

    // The spectrogram's last frame is inside it, so the bound is the frame count
    // and not one under it.
    NoteMask at_end = good;
    at_end.frame_start = spec.n_frames() - at_end.n_frames;
    REQUIRE(at_end.frame_end() == spec.n_frames());
    REQUIRE_NOTHROW(apply_note_mask(spec, at_end));
  }

  SECTION("a spectrogram narrower than the one the mask was built over") {
    // The same mask against a shorter transform: nothing about the mask changed,
    // so the rejection is reading the spectrogram rather than the mask alone.
    const sonare::Spectrogram narrow =
        spectrogram_of(tone_audio(330.0f), sonare::make_stft_config(kNfft / 4, kHopLength));
    REQUIRE(narrow.n_bins() < spec.n_bins());
    REQUIRE(code_of([&] { return apply_note_mask(narrow, good); }) == kInvalid);
  }
}

TEST_CASE("residual_spectrum and mask_total reject a set that describes nothing",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 330.0f));
  REQUIRE_NOTHROW(residual_spectrum(spec, masks));
  REQUIRE_NOTHROW(mask_total(masks));

  SECTION("residual_spectrum wants the shape it was built over") {
    for (const int delta : {1, -1}) {
      INFO("shape offset by " << delta);
      NoteMaskSet wrong_bins = masks;
      wrong_bins.n_bins = spec.n_bins() + delta;
      REQUIRE(code_of([&] { return residual_spectrum(spec, wrong_bins); }) == kInvalid);

      NoteMaskSet wrong_frames = masks;
      wrong_frames.n_frames = spec.n_frames() + delta;
      REQUIRE(code_of([&] { return residual_spectrum(spec, wrong_frames); }) == kInvalid);
    }
    // A set with no shape at all, against a spectrogram that has one.
    const NoteMaskSet unset;
    REQUIRE(code_of([&] { return residual_spectrum(spec, unset); }) == kInvalid);
  }

  SECTION("residual_spectrum wants the framing too, and mask_total cannot want it") {
    // A set carrying another framing's hop indexes the same array while meaning
    // different times, so the two fields that are not sizes are checked as well.
    for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
      INFO("hop_length " << bad);
      NoteMaskSet wrong = masks;
      wrong.hop_length = bad;
      REQUIRE(code_of([&] { return residual_spectrum(spec, wrong); }) == kInvalid);
    }
    for (const int bad : {kSampleRate + 1, 48000, 0, -1}) {
      INFO("sample_rate " << bad);
      NoteMaskSet wrong = masks;
      wrong.sample_rate = bad;
      REQUIRE(code_of([&] { return residual_spectrum(spec, wrong); }) == kInvalid);
    }

    // mask_total is handed no spectrogram to check a framing against, so the two
    // are deliberately not symmetric here and the same set passes it. Asserted
    // rather than left out, because a symmetry that does not exist is exactly
    // what a reader would otherwise assume from the pair above.
    NoteMaskSet other_framing = masks;
    other_framing.hop_length = kHopLength * 2;
    other_framing.sample_rate = 48000;
    REQUIRE_NOTHROW(mask_total(other_framing));
  }

  SECTION("mask_total wants a shape") {
    const NoteMaskSet unset;
    REQUIRE(unset.n_bins == 0);
    REQUIRE(unset.n_frames == 0);
    REQUIRE(code_of([&] { return mask_total(unset); }) == kInvalid);

    NoteMaskSet no_bins = masks;
    no_bins.n_bins = 0;
    REQUIRE(code_of([&] { return mask_total(no_bins); }) == kInvalid);

    NoteMaskSet no_frames = masks;
    no_frames.n_frames = 0;
    REQUIRE(code_of([&] { return mask_total(no_frames); }) == kInvalid);

    for (const int bad : {-1, -4096}) {
      INFO("n_bins " << bad);
      NoteMaskSet negative = masks;
      negative.n_bins = bad;
      REQUIRE(code_of([&] { return mask_total(negative); }) == kInvalid);
    }
  }
}

TEST_CASE("build_note_masks rejects a ridge outside the framing or carrying no pitch",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const int frames = spec.n_frames();
  REQUIRE(frames > 20);

  SECTION("a ridge reaching outside the spectrogram's frames") {
    // Past the end by one frame, which is the boundary rather than a wild value.
    const MultiF0Track over = track_over(spec, {steady_ridge(330.0f, 1, frames, kHopLength)});
    REQUIRE(over.ridges[0].frame_end() == frames + 1);
    REQUIRE(code_of([&] { return build_note_masks(spec, over); }) == kInvalid);

    MultiF0Track before = track_over(spec, {steady_ridge(330.0f, 0, frames / 2, kHopLength)});
    before.ridges[0].frame_start = -1;
    REQUIRE(code_of([&] { return build_note_masks(spec, before); }) == kInvalid);

    // And a second ridge out of range, which a check reading only the first
    // would miss.
    MultiF0Track second = track_over(spec, {steady_ridge(330.0f, 0, frames, kHopLength),
                                            steady_ridge(440.0f, 0, frames, kHopLength)});
    second.ridges[1].frame_start = 4;
    REQUIRE(code_of([&] { return build_note_masks(spec, second); }) == kInvalid);

    // The spectrogram's last frame is inside it, so the bound is the frame count
    // and not one under it.
    REQUIRE_NOTHROW(build_note_masks(
        spec, track_over(spec, {steady_ridge(330.0f, 1, frames - 1, kHopLength)})));
  }

  SECTION("an f0 that is not positive and finite") {
    for (const float bad : {0.0f, -330.0f, kNaN, kInf, -kInf}) {
      INFO("f0_hz " << bad);
      MultiF0Track first = track_over(spec, {steady_ridge(330.0f, 0, frames, kHopLength)});
      first.ridges[0].f0_hz[0] = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, first); }) == kInvalid);

      // In the middle of the span too, which a check reading only a ridge's
      // first frame would miss -- and it is not quietly absorbed as a frame that
      // claims nothing.
      MultiF0Track hidden = track_over(spec, {steady_ridge(330.0f, 0, frames, kHopLength)});
      hidden.ridges[0].f0_hz[hidden.ridges[0].f0_hz.size() / 2] = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, hidden); }) == kInvalid);

      MultiF0Track second = track_over(spec, {steady_ridge(330.0f, 0, frames, kHopLength),
                                              steady_ridge(440.0f, 0, frames, kHopLength)});
      second.ridges[1].f0_hz.back() = bad;
      REQUIRE(code_of([&] { return build_note_masks(spec, second); }) == kInvalid);
    }
  }
}

TEST_CASE("a claim at either edge of the spectrum stays inside it", "[polyphony_mask]") {
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(440.0f));
  NoteMaskConfig config;
  config.n_harmonics = 20;
  // Four lobes either side, so the claim is wide enough to run off an edge it is
  // not held at rather than happening to fit.
  config.claim_lobes = 4.0f;

  SECTION("a fundamental under the first bin") {
    // 1 Hz sits at bin 0.09 at this framing, so the claim of partial one reaches
    // below bin zero and the claim of partial two nearly does.
    const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 1.0f), config);
    // Every bin inside [0, n_bins) is the structural rule, and it is what stops
    // the low edge from being a negative index into the spectrogram.
    require_well_formed(masks);
    REQUIRE(!masks.notes[0].bins.empty());
    REQUIRE(masks.notes[0].bins.front() >= 0);
    require_reconstructs(spec, masks);
  }

  SECTION("a fundamental just under Nyquist") {
    // 22 kHz leaves only the fundamental under the 22.05 kHz Nyquist, at bin
    // 2043 of 2048, so a claim eight bins wide either side reaches past the last
    // bin and has to stop at it.
    const NoteMaskSet masks = build_note_masks(spec, single_note_track(spec, 22000.0f), config);
    require_well_formed(masks);
    const std::vector<std::pair<int, int>> runs =
        claimed_runs(masks.notes[0], masks.notes[0].n_frames / 2);
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].second == spec.n_bins() - 1);
    require_reconstructs(spec, masks);
  }
}

TEST_CASE("ten notes at one pitch land the total within the rounding of ten shares",
          "[polyphony_mask]") {
  // Ten copies of 1/10 accumulated in float land one ULP over one; ten copies
  // multiplied out, or added pairwise, land exactly on it. Which of the two a
  // total shows is an accumulation order the contract does not fix, so the
  // assertion is a band around one and not a direction -- and the band is what
  // says the total was not clamped on the way.
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));
  std::vector<F0Ridge> ridges;
  for (int i = 0; i < 10; ++i) {
    ridges.push_back(steady_ridge(300.0f, 0, spec.n_frames(), kHopLength));
  }
  const NoteMaskSet masks = build_note_masks(spec, track_over(spec, std::move(ridges)));
  require_well_formed(masks);
  REQUIRE(masks.notes.size() == 10);

  for (const NoteMask& mask : masks.notes) {
    REQUIRE(!mask.weights.empty());
    for (const float weight : mask.weights) REQUIRE(weight == 0.1f);
  }

  const std::vector<int> counts = claim_counts(masks);
  const std::vector<float> total = mask_total(masks);
  size_t outside = 0;
  size_t claimed = 0;
  for (size_t i = 0; i < total.size(); ++i) {
    if (counts[i] == 0) continue;
    ++claimed;
    REQUIRE(counts[i] == 10);
    if (static_cast<double>(std::abs(total[i] - 1.0f)) > total_allowance(10)) ++outside;
  }
  REQUIRE(claimed > 0);
  REQUIRE(outside == 0);

  require_reconstructs(spec, masks);
}

TEST_CASE("neither the total nor the residual is clamped when the shares exceed one",
          "[polyphony_mask]") {
  // Two hand-built masks taking a whole bin each. Both are well formed -- a
  // weight of one is inside (0, 1] -- so nothing here is a rejected input; what
  // the pair produces is a total of two, which is the only way to read whether
  // the total saturates and whether the residual goes negative rather than
  // stopping at zero. An over-claiming set is not something build_note_masks
  // produces; it is what a later stage's non-neutral split could.
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(300.0f));
  // The second partial of the fixture tone, so the bin carries real signal and
  // the equalities below are not reading a leakage floor.
  const int bin = 56;
  const int frame = 3;
  REQUIRE(bin < spec.n_bins());
  REQUIRE(frame < spec.n_frames());
  REQUIRE(std::abs(spec.at(bin, frame)) > 0.0f);

  NoteMask whole;
  whole.frame_start = frame;
  whole.n_frames = 1;
  whole.frame_offset = {0, 1};
  whole.bins = {static_cast<int32_t>(bin)};
  whole.weights = {1.0f};

  NoteMaskSet masks;
  masks.n_bins = spec.n_bins();
  masks.n_frames = spec.n_frames();
  masks.hop_length = spec.hop_length();
  masks.sample_rate = spec.sample_rate();
  masks.notes = {whole, whole};
  masks.notes[1].ridge_index = 1;

  const std::vector<float> total = mask_total(masks);
  const size_t at =
      static_cast<size_t>(bin) * static_cast<size_t>(spec.n_frames()) + static_cast<size_t>(frame);
  // Two whole shares are two, not one: a clamp would read as identical to a
  // correct total here, which is the case it exists to be distinguishable from.
  REQUIRE(total[at] == 2.0f);

  // And one minus that is minus one, so the residual carries the input negated
  // rather than saturating at silence.
  const sonare::Spectrogram residual = residual_spectrum(spec, masks);
  REQUIRE(residual.at(bin, frame) == -spec.at(bin, frame));
  // Everywhere else is untouched, so the sign is the over-claim and not a global
  // inversion.
  REQUIRE(residual.at(bin, frame + 1) == spec.at(bin, frame + 1));
  REQUIRE(residual.at(bin + 1, frame) == spec.at(bin + 1, frame));
}

TEST_CASE("every entry point rejects a hand-built mask whose sparse shape is broken",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const NoteMaskSet built = build_note_masks(spec, single_note_track(spec, 330.0f));
  require_well_formed(built);
  const NoteMask good = built.notes[0];
  REQUIRE(good.n_frames > 4);
  REQUIRE(good.bins.size() > 4);

  // One break per entry, each of which would otherwise be a read or a write
  // outside the mask's own arrays rather than a rejected input.
  std::vector<std::pair<const char*, NoteMask>> broken;
  {
    NoteMask mask = good;
    mask.frame_offset.pop_back();
    broken.emplace_back("frame_offset one short of n_frames + 1", mask);
  }
  {
    NoteMask mask = good;
    mask.frame_offset.push_back(mask.frame_offset.back());
    broken.emplace_back("frame_offset one long", mask);
  }
  {
    NoteMask mask = good;
    mask.frame_offset.front() = 1;
    broken.emplace_back("frame_offset does not start at zero", mask);
  }
  {
    NoteMask mask = good;
    mask.frame_offset[1] = mask.frame_offset[2] + 1;
    broken.emplace_back("frame_offset descends", mask);
  }
  {
    NoteMask mask = good;
    mask.frame_offset.back() = static_cast<int32_t>(mask.bins.size()) + 1;
    broken.emplace_back("the last offset reaches past the arrays", mask);
  }
  {
    NoteMask mask = good;
    mask.bins.pop_back();
    broken.emplace_back("bins shorter than weights", mask);
  }
  {
    NoteMask mask = good;
    mask.weights.pop_back();
    broken.emplace_back("weights shorter than bins", mask);
  }
  {
    NoteMask mask = good;
    mask.n_frames = -1;
    broken.emplace_back("a negative frame count", mask);
  }
  {
    NoteMask mask = good;
    mask.bins[mask.bins.size() / 2] = static_cast<int32_t>(spec.n_bins());
    broken.emplace_back("a bin past the spectrogram", mask);
  }
  broken.emplace_back("a default-constructed mask", NoteMask{});

  for (const auto& entry : broken) {
    INFO(entry.first);
    // Every function taking a mask checks the shape before allocating against
    // it, so the three entry points agree rather than one of them being the
    // only guarded route in.
    REQUIRE(code_of([&] { return apply_note_mask(spec, entry.second); }) == kInvalid);

    NoteMaskSet set = built;
    set.notes[0] = entry.second;
    REQUIRE(code_of([&] { return residual_spectrum(spec, set); }) == kInvalid);
    REQUIRE(code_of([&] { return mask_total(set); }) == kInvalid);

    // And in the second mask of a set, which a check reading only the first
    // would miss.
    NoteMaskSet second = built;
    second.notes.push_back(entry.second);
    second.notes[1].ridge_index = 1;
    REQUIRE(code_of([&] { return residual_spectrum(spec, second); }) == kInvalid);
    REQUIRE(code_of([&] { return mask_total(second); }) == kInvalid);
  }

  // A mask whose span leaves the set's frames is the same class of break, and
  // reaches the set-level entry points rather than only apply_note_mask.
  NoteMaskSet over = built;
  over.notes[0].frame_start = 1;
  REQUIRE(over.notes[0].frame_end() == spec.n_frames() + 1);
  REQUIRE(code_of([&] { return residual_spectrum(spec, over); }) == kInvalid);
  REQUIRE(code_of([&] { return mask_total(over); }) == kInvalid);

  NoteMaskSet under = built;
  under.notes[0].frame_start = -1;
  REQUIRE(code_of([&] { return residual_spectrum(spec, under); }) == kInvalid);
  REQUIRE(code_of([&] { return mask_total(under); }) == kInvalid);
}

TEST_CASE("a mask of no frames is accepted and a default-constructed one is not",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));

  // The two readings of an empty mask, pinned apart: no frames still carries the
  // one frame_offset entry the n_frames + 1 rule asks for, and a zeroed struct
  // has none. The default exists so the struct is an aggregate, not so a zeroed
  // one means an empty mask.
  NoteMask empty;
  empty.frame_start = 0;
  empty.n_frames = 0;
  empty.frame_offset = {0};

  const NoteMask zeroed;
  REQUIRE(zeroed.frame_offset.empty());

  REQUIRE_NOTHROW(apply_note_mask(spec, empty));
  REQUIRE(code_of([&] { return apply_note_mask(spec, zeroed); }) == kInvalid);

  // An empty span inside the framing rather than at its start, so the acceptance
  // is not resting on frame_start being zero.
  NoteMask mid = empty;
  mid.frame_start = 3;
  REQUIRE(mid.frame_end() == 3);
  REQUIRE_NOTHROW(apply_note_mask(spec, mid));

  // It names no bin, so its share of the spectrum is silence.
  const sonare::Spectrogram masked = apply_note_mask(spec, empty);
  REQUIRE(masked.n_bins() == spec.n_bins());
  REQUIRE(masked.n_frames() == spec.n_frames());
  size_t sounding = 0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      if (masked.at(bin, frame) != std::complex<float>(0.0f, 0.0f)) ++sounding;
    }
  }
  REQUIRE(sounding == 0);

  NoteMaskSet set;
  set.n_bins = spec.n_bins();
  set.n_frames = spec.n_frames();
  set.hop_length = spec.hop_length();
  set.sample_rate = spec.sample_rate();
  set.notes = {empty, mid};
  set.notes[1].ridge_index = 1;

  REQUIRE_NOTHROW(residual_spectrum(spec, set));
  REQUIRE_NOTHROW(mask_total(set));
  // Claiming nothing is not the same as being absent: the set is valid, its
  // total is zero throughout, and the residual is the input.
  for (const float weight : mask_total(set)) REQUIRE(weight == 0.0f);
  const sonare::Spectrogram residual = residual_spectrum(spec, set);
  size_t moved = 0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      if (residual.at(bin, frame) != spec.at(bin, frame)) ++moved;
    }
  }
  REQUIRE(moved == 0);

  NoteMaskSet with_zeroed = set;
  with_zeroed.notes[1] = zeroed;
  REQUIRE(code_of([&] { return residual_spectrum(spec, with_zeroed); }) == kInvalid);
  REQUIRE(code_of([&] { return mask_total(with_zeroed); }) == kInvalid);
}

TEST_CASE("every entry point rejects a weight outside (0, 1] and accepts the whole share",
          "[polyphony_mask]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of(tone_audio(330.0f));
  const NoteMaskSet built = build_note_masks(spec, single_note_track(spec, 330.0f));
  require_well_formed(built);
  const NoteMask good = built.notes[0];
  REQUIRE(good.weights.size() > 4);

  // A weight out of range is not a shape error and would break the total and the
  // residual with no call failing, which is why it is checked in the same pass
  // as the bins rather than trusted.
  for (const float bad : {0.0f, -0.5f, -1.0f, 2.0f, 1.5f, kNaN, kInf, -kInf}) {
    INFO("weight " << bad);
    for (const size_t at : {size_t{0}, good.weights.size() / 2, good.weights.size() - 1}) {
      NoteMask broken = good;
      broken.weights[at] = bad;
      REQUIRE(code_of([&] { return apply_note_mask(spec, broken); }) == kInvalid);

      NoteMaskSet first = built;
      first.notes[0] = broken;
      REQUIRE(code_of([&] { return residual_spectrum(spec, first); }) == kInvalid);
      REQUIRE(code_of([&] { return mask_total(first); }) == kInvalid);

      // And in the second note of a set, which a check reading only the first
      // would miss.
      NoteMaskSet second = built;
      second.notes.push_back(broken);
      second.notes[1].ridge_index = 1;
      REQUIRE(code_of([&] { return residual_spectrum(spec, second); }) == kInvalid);
      REQUIRE(code_of([&] { return mask_total(second); }) == kInvalid);
    }
  }

  // One is the inclusive end of the range, and it is load-bearing rather than a
  // formality: a whole share is what an unshared bin carries, and it is what the
  // over-claiming set that shows the residual is unclamped is built from. A
  // guard written with the wrong comparison at this end takes every unshared
  // bin with it.
  for (const float good_weight : {1.0f, 0.5f, 1e-6f}) {
    INFO("weight " << good_weight);
    NoteMask edge = good;
    edge.weights[edge.weights.size() / 2] = good_weight;
    REQUIRE_NOTHROW(apply_note_mask(spec, edge));

    NoteMaskSet set = built;
    set.notes[0] = edge;
    REQUIRE_NOTHROW(residual_spectrum(spec, set));
    REQUIRE_NOTHROW(mask_total(set));
  }
}
