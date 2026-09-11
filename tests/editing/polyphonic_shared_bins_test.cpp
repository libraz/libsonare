/// @file polyphonic_shared_bins_test.cpp
/// @brief Contract tests for the shared-bin solver.
///
/// A claim is predicted from an f0, so a claimed bin is not necessarily a bin
/// holding the partial it names. Three populations follow, and which cases cover
/// which is a decision rather than an accident:
///
/// - A claim standing on a partial the material never rendered. Real input has
///   these, because the mask claims twenty partials by default and few notes have
///   twenty. One case covers them at the builder's own geometry, and asserts that
///   what happens there cannot matter rather than what happens there.
/// - An edge bin of a claim that does stand on a real partial: two bins out, at
///   the Hann main lobe's null, where leakage outweighs the partial the claim
///   names. A verdict fixed before any decomposition -- the span, the claim
///   count, the refined f0, the predicted gap -- reads the same there as at the
///   centre, so the cases asserting one run at full width. A verdict the
///   decomposition produces does not, and no case asserts one over an edge bin.
///   That gap is deliberate: the outcome there is not contract, and asserting one
///   would be writing the test from the implementation.
/// - Bins inside the lobe. Only the cases asserting a decomposition's verdict
///   over every shared bin narrow to these; see @ref kDominatedLobes.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "editing/polyphony/shared_bins.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using namespace sonare::editing::polyphony;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kNfft = 4096;
constexpr int kHopLength = 512;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

using Complex = std::complex<float>;

/// @brief Partials every fixture tone renders, and every fixture mask claims.
/// @details @ref build_note_masks claims twenty by default, so the default leaves
///          a claim at every predicted partial position a fixture with fewer never
///          put a partial at. Those bins hold leakage from elsewhere in the
///          spectrum and nothing else, and the solver judges content rather than
///          claim geometry, so its verdict there is a verdict about noise. A case
///          quantifying over "every shared bin" would then be quantifying over
///          bins its own material never occupied.
constexpr int kFixturePartials = 8;

/// @brief Claim width for a case that asserts over every shared bin.
/// @details At one lobe a claim reaches the Hann main lobe's nulls, two bins out,
///          where the claimed partial contributes nothing and the bin is carried
///          by leakage from partials the claim does not name -- a bin about
///          something other than the two partials the case is about. Half a lobe
///          stops one bin out, inside the lobe, where the claimed partial
///          dominates. It is a property of the window, not of any outcome.
constexpr float kDominatedLobes = 0.5f;

/// @brief Tolerance of the reconstruction identity, relative to the bin and to
///        the share taken from it.
/// @details A float rounding is 6e-8 relative and the identity passes through one
///          product per note, the per-bin total and the final sum. A solved weight
///          reaches @c max_weight_modulus, so the terms can be eight times the
///          value they reconstruct; the modulus factor at each call site is what
///          allows for the cancellation between them.
constexpr double kReconstructionTolerance = 1e-6;

/// @brief The analysis framing every fixture is reasoned in.
/// @details Centre padding is off, deliberately. A centred STFT puts partly-zero
///          data in the frames at either end, and a trajectory through those is
///          not a sum of steady poles however steady the note is, so a centred
///          fixture would report a misfit belonging to the padding. Without it
///          every frame holds whole signal and a decaying sinusoid's per-bin
///          trajectory is exactly one pole, which is what every expectation below
///          rests on.
sonare::StftConfig analysis_stft() {
  sonare::StftConfig stft = sonare::make_stft_config(kNfft, kHopLength);
  stft.center = false;
  return stft;
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

const char* name_of(SharedBinOutcome outcome) {
  switch (outcome) {
    case SharedBinOutcome::Unclaimed:
      return "Unclaimed";
    case SharedBinOutcome::Solved:
      return "Solved";
    case SharedBinOutcome::Unshared:
      return "Unshared";
    case SharedBinOutcome::TooFewFrames:
      return "TooFewFrames";
    case SharedBinOutcome::TooManyClaimants:
      return "TooManyClaimants";
    case SharedBinOutcome::F0NotRefined:
      return "F0NotRefined";
    case SharedBinOutcome::PartialsTooClose:
      return "PartialsTooClose";
    case SharedBinOutcome::PolesNotFound:
      return "PolesNotFound";
    case SharedBinOutcome::FitDiverged:
      return "FitDiverged";
    case SharedBinOutcome::DegenerateWeight:
      return "DegenerateWeight";
  }
  return "?";
}

double cents_between(double hz, double reference_hz) {
  return 1200.0 * std::log2(hz / reference_hz);
}

double shift_cents(double hz, double cents) { return hz * std::pow(2.0, cents / 1200.0); }

// --- Synthetic material ----------------------------------------------------

/// @brief A harmonic tone whose partials each decay at their own rate.
/// @details One partial is exactly one pole of the per-bin trajectory its STFT
///          produces, so material built this way is the sum of poles the solver
///          fits and any misfit it reports belongs to the fit rather than to the
///          fixture. Partial @c h has amplitude @c amplitude/h, damping
///          @c decay_per_harmonic*h and starting phase @c phase_step*h.
struct ToneSpec {
  double f0_hz = 300.0;
  double amplitude = 0.3;
  int n_partials = kFixturePartials;
  /// Nepers per second per harmonic number.
  double decay_per_harmonic = 1.0;
  double phase_step = 0.0;
  /// Peak excursion in cents; 0 holds the pitch steady.
  double vibrato_cents = 0.0;
  double vibrato_hz = 5.5;
};

void add_tone(std::vector<float>& into, const ToneSpec& tone) {
  const double nyquist = 0.5 * kSampleRate;
  const bool bends = tone.vibrato_cents != 0.0;
  for (int h = 1; h <= tone.n_partials; ++h) {
    const double hz = tone.f0_hz * static_cast<double>(h);
    if (hz >= nyquist) break;
    const double level = tone.amplitude / static_cast<double>(h);
    const double damping = tone.decay_per_harmonic * static_cast<double>(h);
    // Integrated rather than evaluated, so a bent pitch and a steady one come off
    // one path and a bend stays continuous in phase.
    double phase = tone.phase_step * static_cast<double>(h);
    for (size_t i = 0; i < into.size(); ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      into[i] += static_cast<float>(level * std::exp(-damping * t) * std::sin(phase));
      const double bend =
          bends ? std::pow(2.0, tone.vibrato_cents / 1200.0 *
                                    std::sin(sonare::constants::kTwoPiD * tone.vibrato_hz * t))
                : 1.0;
      phase += sonare::constants::kTwoPiD * hz * bend / kSampleRate;
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

size_t samples_of(float seconds) {
  return static_cast<size_t>(seconds * static_cast<float>(kSampleRate));
}

sonare::Audio audio_of(std::vector<float> samples) {
  return sonare::Audio::from_vector(std::move(samples), kSampleRate);
}

sonare::Spectrogram spectrogram_of(const sonare::Audio& audio) {
  return sonare::Spectrogram::compute(audio, analysis_stft());
}

sonare::Spectrogram spectrogram_of_tones(const std::vector<ToneSpec>& tones, float seconds) {
  std::vector<float> samples(samples_of(seconds), 0.0f);
  for (const ToneSpec& tone : tones) add_tone(samples, tone);
  return spectrogram_of(audio_of(std::move(samples)));
}

// --- The oracle ------------------------------------------------------------

/// @brief Two notes rendered apart and together.
/// @details The STFT is linear, so the mix's spectrum is the sum of the two and
///          the true complex share of one note at a bin is exactly its own value
///          over the mix's -- known to float precision, with nothing annotated and
///          nothing to be wrong about. @ref require_linear checks that premise
///          instead of assuming it.
struct Duet {
  sonare::Spectrogram parts[2];
  sonare::Spectrogram mix;
  /// Largest modulus anywhere in @ref mix.
  float peak = 0.0f;
};

Duet render_duet(const ToneSpec& first, const ToneSpec& second, float seconds) {
  Duet duet;
  duet.parts[0] = spectrogram_of_tones({first}, seconds);
  duet.parts[1] = spectrogram_of_tones({second}, seconds);
  duet.mix = spectrogram_of_tones({first, second}, seconds);
  for (int bin = 0; bin < duet.mix.n_bins(); ++bin) {
    for (int frame = 0; frame < duet.mix.n_frames(); ++frame) {
      duet.peak = std::max(duet.peak, std::abs(duet.mix.at(bin, frame)));
    }
  }
  REQUIRE(duet.peak > 0.0f);
  return duet;
}

/// @brief Asserts the oracle's own premise: the parts add up to the mix.
void require_linear(const Duet& duet) {
  double worst = 0.0;
  for (int bin = 0; bin < duet.mix.n_bins(); ++bin) {
    for (int frame = 0; frame < duet.mix.n_frames(); ++frame) {
      const Complex sum = duet.parts[0].at(bin, frame) + duet.parts[1].at(bin, frame);
      worst = std::max(worst, static_cast<double>(std::abs(sum - duet.mix.at(bin, frame))));
    }
  }
  INFO("worst part-sum deviation " << worst << " against a peak of " << duet.peak);
  REQUIRE(worst <= kReconstructionTolerance * static_cast<double>(duet.peak));
}

// --- Hand-built tracks -----------------------------------------------------

/// @brief A ridge holding one pitch over [frame_start, frame_start + n_frames).
F0Ridge steady_ridge(float f0_hz, int frame_start, int n_frames) {
  F0Ridge ridge;
  ridge.frame_start = frame_start;
  ridge.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  ridge.salience.assign(static_cast<size_t>(n_frames), 1.0f);
  ridge.onset_sample = static_cast<int64_t>(frame_start) * kHopLength;
  ridge.offset_sample = static_cast<int64_t>(frame_start + n_frames) * kHopLength;
  ridge.median_hz = f0_hz;
  return ridge;
}

MultiF0Track track_over(const sonare::Spectrogram& spec, std::vector<F0Ridge> ridges) {
  MultiF0Track track;
  track.ridges = std::move(ridges);
  track.n_frames = spec.n_frames();
  track.hop_length = spec.hop_length();
  track.sample_rate = spec.sample_rate();
  track.polyphony.assign(static_cast<size_t>(spec.n_frames()), 0);
  for (const F0Ridge& ridge : track.ridges) {
    for (int frame = std::max(0, ridge.frame_start);
         frame < std::min(spec.n_frames(), ridge.frame_end()); ++frame) {
      ++track.polyphony[static_cast<size_t>(frame)];
    }
  }
  return track;
}

/// @brief One ridge per pitch, each spanning every frame of @p spec.
MultiF0Track track_of_pitches(const sonare::Spectrogram& spec, const std::vector<float>& pitches) {
  std::vector<F0Ridge> ridges;
  ridges.reserve(pitches.size());
  for (const float hz : pitches) ridges.push_back(steady_ridge(hz, 0, spec.n_frames()));
  return track_over(spec, std::move(ridges));
}

/// @brief Masks whose claims reach exactly as far as the fixture rendered.
/// @details @p claim_lobes stays at the builder's own default except where a case
///          asserts over every shared bin; see @ref kDominatedLobes.
NoteMaskSet matched_masks(const sonare::Spectrogram& spec, const MultiF0Track& track,
                          float claim_lobes = NoteMaskConfig{}.claim_lobes) {
  NoteMaskConfig config;
  config.n_harmonics = kFixturePartials;
  config.claim_lobes = claim_lobes;
  return build_note_masks(spec, track, config);
}

// --- Reading a mask and a report -------------------------------------------

/// @brief Where one (bin, frame) sits in a dense surface over the framing.
/// @details The layout @ref mask_total and @ref SharedBinReport both use, so an
///          outcome, a total and a claim count are all read at one subscript.
size_t at_index(const NoteMaskSet& masks, int bin, int frame) {
  return static_cast<size_t>(bin) * static_cast<size_t>(masks.n_frames) +
         static_cast<size_t>(frame);
}

/// @brief Notes claiming each (bin, frame), in the spectrogram's own layout.
std::vector<int> claim_counts(const NoteMaskSet& masks) {
  std::vector<int> counts(static_cast<size_t>(masks.n_bins) * static_cast<size_t>(masks.n_frames),
                          0);
  for (const NoteMask& mask : masks.notes) {
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        ++counts[at_index(masks, static_cast<int>(mask.bins[static_cast<size_t>(k)]), frame)];
      }
    }
  }
  return counts;
}

/// @brief Cells each note claims alone, below the highest frequency a partial may
///        be refined from.
/// @details The property that decides whether a ridge has anything to re-estimate
///          an f0 from, read off the claim map rather than off a refinement's
///          answer. A fixture that reaches the sentinel needs to say which of the
///          two ways it got there -- no qualifying bins at all, or qualifying bins
///          that turn out to be useless -- and the two are indistinguishable from
///          the returned value.
std::vector<size_t> unshared_below_refine_ceiling(const NoteMaskSet& masks,
                                                  const SharedBinConfig& config = {}) {
  const std::vector<int> counts = claim_counts(masks);
  const double bin_hz =
      static_cast<double>(masks.sample_rate) / (2.0 * static_cast<double>(masks.n_bins - 1));
  // The header's own expression, so this tracks the tolerance rather than a number
  // read off one framing.
  const double ceiling_hz =
      (static_cast<double>(masks.sample_rate) / static_cast<double>(masks.hop_length)) / 2.0 /
      (std::pow(2.0, static_cast<double>(config.f0_tolerance_cents) / 1200.0) - 1.0);
  std::vector<size_t> alone(masks.notes.size(), 0);
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    const NoteMask& mask = masks.notes[i];
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const int bin = static_cast<int>(mask.bins[static_cast<size_t>(k)]);
        if (counts[at_index(masks, bin, frame)] != 1) continue;
        if (static_cast<double>(bin) * bin_hz > ceiling_hz) continue;
        ++alone[i];
      }
    }
  }
  return alone;
}

/// @brief Everything solving must leave alone.
/// @details The bins, the frames and the note order are exactly the input's and
///          only the weights change, so this is an equality on every other field
///          rather than a tolerance on any of them.
void require_same_structure(const NoteMaskSet& before, const NoteMaskSet& after) {
  REQUIRE(after.n_bins == before.n_bins);
  REQUIRE(after.n_frames == before.n_frames);
  REQUIRE(after.hop_length == before.hop_length);
  REQUIRE(after.sample_rate == before.sample_rate);
  // The geometry the claims were placed with. Not shape, but the promise that
  // feeding a result back in is a no-op rests on it: a second call reading a
  // default geometry would identify different partials on the same bins, gate
  // them differently, and not reproduce the first.
  REQUIRE(after.config.n_harmonics == before.config.n_harmonics);
  REQUIRE(after.config.claim_lobes == before.config.claim_lobes);
  REQUIRE(after.config.inharmonicity == before.config.inharmonicity);
  REQUIRE(after.notes.size() == before.notes.size());
  for (size_t i = 0; i < after.notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(after.notes[i].ridge_index == before.notes[i].ridge_index);
    REQUIRE(after.notes[i].frame_start == before.notes[i].frame_start);
    REQUIRE(after.notes[i].n_frames == before.notes[i].n_frames);
    REQUIRE(after.notes[i].frame_offset == before.notes[i].frame_offset);
    REQUIRE(after.notes[i].bins == before.notes[i].bins);
    REQUIRE(after.notes[i].weights.size() == before.notes[i].weights.size());
  }
}

/// @brief Asserts that the notes plus the residual return @p spec.
/// @details Load-bearing, and worth stating plainly what it is not evidence of.
///          @ref residual_spectrum defines the residual as @c spec*(1 - total),
///          so this identity holds for any weights whatsoever -- a build that
///          returned the equal split untouched, or noise, passes it just as
///          exactly. It says the representation loses nothing under the new
///          weights. Whether a weight is any good is the oracle case's question,
///          not this one's.
void require_reconstructs(const sonare::Spectrogram& spec, const NoteMaskSet& masks) {
  const sonare::Spectrogram residual = residual_spectrum(spec, masks);
  const std::vector<Complex> total = mask_total(masks);
  REQUIRE(residual.n_bins() == spec.n_bins());
  REQUIRE(residual.n_frames() == spec.n_frames());
  REQUIRE(total.size() ==
          static_cast<size_t>(spec.n_bins()) * static_cast<size_t>(spec.n_frames()));

  std::vector<sonare::Spectrogram> parts;
  parts.reserve(masks.notes.size());
  for (const NoteMask& mask : masks.notes) parts.push_back(apply_note_mask(spec, mask));

  // How far each bin's terms reach past the value they reconstruct, so the
  // allowance follows the cancellation a weight over one puts into the sum.
  std::vector<double> reach(total.size(), 1.0);
  for (const NoteMask& mask : masks.notes) {
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const size_t at = static_cast<size_t>(k);
        reach[at_index(masks, static_cast<int>(mask.bins[at]), frame)] +=
            static_cast<double>(std::abs(mask.weights[at]));
      }
    }
  }

  double worst = 0.0;
  int worst_bin = 0;
  int worst_frame = 0;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    for (int frame = 0; frame < spec.n_frames(); ++frame) {
      const Complex want = spec.at(bin, frame);
      Complex notes(0.0f, 0.0f);
      for (const sonare::Spectrogram& part : parts) notes += part.at(bin, frame);
      // An exactly zero bin has nothing to be relative to, and every term is a
      // product of it, so its reconstruction is exactly zero.
      const double magnitude = static_cast<double>(std::abs(want));
      const double scale = (magnitude > 0.0 ? magnitude : 1.0) * reach[at_index(masks, bin, frame)];
      const double error =
          static_cast<double>(std::abs(notes + residual.at(bin, frame) - want)) / scale;
      if (error > worst) {
        worst = error;
        worst_bin = bin;
        worst_frame = frame;
      }
    }
  }
  INFO("worst reconstruction at bin " << worst_bin << " frame " << worst_frame);
  REQUIRE(worst <= kReconstructionTolerance);
}

/// @brief Everything the contract promises of any solve, over any material.
/// @details Every case runs it, so a fixture only asserts what is specific to it.
///          The three that each catch a different broken build: a refused bin
///          keeps the exact weight it arrived with, an outcome agrees with the
///          signal the report gives for it, and the result is still a mask set
///          every consumer accepts.
void require_solve_contract(const sonare::Spectrogram& spec, const NoteMaskSet& before,
                            const NoteMaskSet& after, const SharedBinReport& report,
                            const SharedBinConfig& config) {
  require_same_structure(before, after);

  const size_t surface = static_cast<size_t>(after.n_bins) * static_cast<size_t>(after.n_frames);
  REQUIRE(report.outcome.size() == surface);
  REQUIRE(report.partial_separation.size() == surface);
  REQUIRE(report.fit_residual.size() == surface);

  const std::vector<int> counts = claim_counts(before);
  const double ceiling =
      static_cast<double>(config.max_weight_modulus) * (1.0 + kReconstructionTolerance);

  size_t kept = 0;
  size_t solved_entries = 0;
  for (size_t i = 0; i < after.notes.size(); ++i) {
    const NoteMask& out = after.notes[i];
    const NoteMask& in = before.notes[i];
    for (int f = 0; f < out.n_frames; ++f) {
      const int frame = out.frame_start + f;
      const int32_t from = out.frame_offset[static_cast<size_t>(f)];
      const int32_t to = out.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const size_t at = static_cast<size_t>(k);
        const size_t cell = at_index(after, static_cast<int>(out.bins[at]), frame);
        const SharedBinOutcome outcome = report.outcome[cell];
        INFO("note " << i << " bin " << out.bins[at] << " frame " << frame << " outcome "
                     << name_of(outcome) << " claimants " << counts[cell]);

        // A weight that is not finite, or is zero, is not a valid mask entry --
        // the consumers below reject a whole set for one.
        REQUIRE(std::isfinite(out.weights[at].real()));
        REQUIRE(std::isfinite(out.weights[at].imag()));
        REQUIRE(std::abs(out.weights[at]) > 0.0f);
        REQUIRE(static_cast<double>(std::abs(out.weights[at])) <= ceiling);

        // One claimant has no assignment problem, so the bin is that note's whole
        // share -- exactly one, not a fit that landed near it.
        if (counts[cell] == 1) {
          REQUIRE(outcome == SharedBinOutcome::Unshared);
          REQUIRE(out.weights[at] == Complex(1.0f, 0.0f));
        } else {
          REQUIRE(outcome != SharedBinOutcome::Unshared);
        }

        // Never worse than the input by construction: a refused bin keeps the
        // weight it arrived with, and that is an equality rather than a bound.
        if (outcome == SharedBinOutcome::Solved) {
          ++solved_entries;
        } else {
          ++kept;
          REQUIRE(out.weights[at] == in.weights[at]);
        }
      }
    }
  }
  INFO("solved entries " << solved_entries << ", kept " << kept);

  // Every cell of the surface, not only the claimed ones, against the signals the
  // header states for each outcome. A cell where no fit ran carries zero rather
  // than a stale value from a neighbouring window, which is what lets the report
  // be read on its own instead of only beside a claim map.
  for (size_t cell = 0; cell < surface; ++cell) {
    INFO("cell " << cell << " outcome " << name_of(report.outcome[cell]) << " claimants "
                 << counts[cell]);
    REQUIRE(std::isfinite(report.partial_separation[cell]));
    REQUIRE(std::isfinite(report.fit_residual[cell]));
    // Unclaimed is exactly the cells no note reached -- neither more nor fewer,
    // so the report and the masks cannot disagree about what was even looked at.
    REQUIRE((report.outcome[cell] == SharedBinOutcome::Unclaimed) == (counts[cell] == 0));
    // Two arms below are unexercised by this file and it is worth saying so
    // where they are rather than leaving them to read as coverage. PolesNotFound
    // is reachable only through a numerically degenerate decomposition and is
    // deliberately not forced. DegenerateWeight was expected from the ceiling
    // fixture's deep null and does not arrive there; until something reaches it,
    // its arm asserts nothing.
    switch (report.outcome[cell]) {
      case SharedBinOutcome::Unclaimed:
        // No note reached it, so there is no pair of partials to have a gap.
        REQUIRE(report.partial_separation[cell] == 0.0f);
        REQUIRE(report.fit_residual[cell] == 0.0f);
        break;
      case SharedBinOutcome::Unshared:
      case SharedBinOutcome::TooFewFrames:
      case SharedBinOutcome::TooManyClaimants:
      case SharedBinOutcome::F0NotRefined:
        // No fit ran. The gap is read from the refined f0 rather than from a
        // decomposition, so whether it had been computed by the time one of these
        // fired is not stated; this asserts only what is.
        REQUIRE(report.fit_residual[cell] == 0.0f);
        REQUIRE(report.partial_separation[cell] >= 0.0f);
        break;
      case SharedBinOutcome::PartialsTooClose:
        REQUIRE(report.partial_separation[cell] < config.min_partial_separation);
        // Determined before any decomposition, so no misfit was measured -- and
        // the one that would have been is the trap the gate exists to avoid: it
        // would have read small while dividing the bin arbitrarily.
        REQUIRE(report.fit_residual[cell] == 0.0f);
        break;
      // Everything below is declared after PartialsTooClose, so by the enum's own
      // precedence rule it got past that gate and its gap held. That is the
      // invariant the declaration order buys, and it is checkable here rather
      // than only readable in the header.
      case SharedBinOutcome::PolesNotFound:
        // Past the gate; the decomposition then produced nothing to measure a
        // misfit on, so the residual is absent rather than small.
        REQUIRE(report.partial_separation[cell] >= config.min_partial_separation);
        REQUIRE(report.fit_residual[cell] == 0.0f);
        break;
      case SharedBinOutcome::FitDiverged:
        REQUIRE(report.partial_separation[cell] >= config.min_partial_separation);
        REQUIRE(report.fit_residual[cell] > config.max_fit_residual);
        break;
      case SharedBinOutcome::DegenerateWeight:
      case SharedBinOutcome::Solved:
        // A degenerate weight is a refusal whose signals read healthy -- the fit
        // ran and both thresholds held; what failed was the weight it produced.
        // So it stands with Solved here and with the refusals above, where the
        // equal split is kept.
        //
        // fit_residual is per window, so a solved frame carries the residual of
        // the window that solved it rather than the first covering window's --
        // the "first window" rule governs refusals only. partial_separation is a
        // span scalar and has no window to have come from.
        REQUIRE(report.partial_separation[cell] >= config.min_partial_separation);
        REQUIRE(report.fit_residual[cell] <= config.max_fit_residual);
        break;
    }
  }

  // A solved set is still a mask set, which is the only place a weight's new
  // range is checked from outside.
  for (const NoteMask& mask : after.notes) REQUIRE_NOTHROW(apply_note_mask(spec, mask));
  REQUIRE_NOTHROW(residual_spectrum(spec, after));
  REQUIRE_NOTHROW(mask_total(after));

  // Neither clamped nor normalised: the total is the sum of what the notes took,
  // whatever that comes to, and the shortfall is what the residual then carries.
  // Asserting the total is one would be asserting the opposite of the contract.
  const std::vector<Complex> total = mask_total(after);
  std::vector<Complex> summed(total.size(), Complex(0.0f, 0.0f));
  for (const NoteMask& mask : after.notes) {
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const size_t at = static_cast<size_t>(k);
        summed[at_index(after, static_cast<int>(mask.bins[at]), frame)] += mask.weights[at];
      }
    }
  }
  double worst_total = 0.0;
  for (size_t cell = 0; cell < total.size(); ++cell) {
    const double scale = std::max(1.0, static_cast<double>(std::abs(summed[cell])));
    worst_total =
        std::max(worst_total, static_cast<double>(std::abs(total[cell] - summed[cell])) / scale);
  }
  REQUIRE(worst_total <= kReconstructionTolerance);

  require_reconstructs(spec, after);
}

/// @brief Runs a solve and every invariant over it, returning the result.
NoteMaskSet solve_and_check(const sonare::Spectrogram& spec, const NoteMaskSet& masks,
                            const MultiF0Track& track, const SharedBinConfig& config,
                            SharedBinReport& report) {
  const NoteMaskSet solved = solve_shared_bins(spec, masks, track, config, &report);
  require_solve_contract(spec, masks, solved, report, config);
  return solved;
}

/// @brief One weight of one note, with the report cell it falls in.
struct Entry {
  size_t note = 0;
  size_t at = 0;
  size_t cell = 0;
  int bin = 0;
  int frame = 0;
};

std::vector<Entry> entries_of(const NoteMaskSet& masks) {
  std::vector<Entry> entries;
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    const NoteMask& mask = masks.notes[i];
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        Entry entry;
        entry.note = i;
        entry.at = static_cast<size_t>(k);
        entry.bin = static_cast<int>(mask.bins[entry.at]);
        entry.frame = frame;
        entry.cell = at_index(masks, entry.bin, frame);
        entries.push_back(entry);
      }
    }
  }
  return entries;
}

/// @brief How much closer the solved weights sit to the true share than the
///        equal split does, in dB, over the bins that were solved.
/// @details Energy-weighted across both notes. A bin sixty decibels under the
///          loudest one is window leakage rather than a partial, and what a
///          weight does there is not what the contract is about.
double gain_over_equal_split(const Duet& duet, const NoteMaskSet& before, const NoteMaskSet& after,
                             const SharedBinReport& report, size_t& measured, int first_bin = 0,
                             int last_bin = std::numeric_limits<int>::max()) {
  const std::vector<int> counts = claim_counts(before);
  const float floor = 1e-3f * duet.peak;
  double err_solved = 0.0;
  double err_equal = 0.0;
  measured = 0;
  for (const Entry& entry : entries_of(after)) {
    if (entry.bin < first_bin || entry.bin > last_bin) continue;
    if (counts[entry.cell] < 2) continue;
    if (report.outcome[entry.cell] != SharedBinOutcome::Solved) continue;
    const Complex mixed = duet.mix.at(entry.bin, entry.frame);
    if (std::abs(mixed) < floor) continue;
    // The share this note really has of this bin: the note rendered alone over
    // the pair rendered together, to float precision.
    const Complex truth = duet.parts[entry.note].at(entry.bin, entry.frame);
    err_solved +=
        static_cast<double>(std::norm(after.notes[entry.note].weights[entry.at] * mixed - truth));
    err_equal +=
        static_cast<double>(std::norm(before.notes[entry.note].weights[entry.at] * mixed - truth));
    ++measured;
  }
  REQUIRE(err_equal > 0.0);
  REQUIRE(err_solved > 0.0);
  return 10.0 * std::log10(err_equal / err_solved);
}

// --- Configs inside and outside the documented ranges -----------------------

std::vector<std::pair<std::string, SharedBinConfig>> out_of_range_configs() {
  std::vector<std::pair<std::string, SharedBinConfig>> bad;
  for (const int value : {3, 65, 0, -1, 1000}) {
    SharedBinConfig config;
    config.window_frames = value;
    bad.emplace_back("window_frames " + std::to_string(value), config);
  }

  const auto with_separation = [&bad](float value, const char* label) {
    SharedBinConfig config;
    config.min_partial_separation = value;
    bad.emplace_back(std::string("min_partial_separation ") + label, config);
  };
  with_separation(0.0f, "0");
  with_separation(-0.01f, "negative");
  with_separation(sonare::constants::kPi * 1.01f, "over pi");
  with_separation(kNaN, "NaN");
  with_separation(kInf, "inf");

  const auto with_residual = [&bad](float value, const char* label) {
    SharedBinConfig config;
    config.max_fit_residual = value;
    bad.emplace_back(std::string("max_fit_residual ") + label, config);
  };
  with_residual(0.0f, "0");
  with_residual(-0.5f, "negative");
  with_residual(1.001f, "over one");
  with_residual(kNaN, "NaN");
  with_residual(kInf, "inf");

  const auto with_modulus = [&bad](float value, const char* label) {
    SharedBinConfig config;
    config.max_weight_modulus = value;
    bad.emplace_back(std::string("max_weight_modulus ") + label, config);
  };
  with_modulus(0.999f, "just under one");
  with_modulus(0.0f, "0");
  with_modulus(-8.0f, "negative");
  // Neither of these is outside the range as the range is written -- a NaN is not
  // below one and an infinity is above it -- so finiteness is checked separately
  // and is what these two read.
  with_modulus(kNaN, "NaN");
  with_modulus(kInf, "inf");

  const auto with_refine = [&bad](float value, const char* label) {
    SharedBinConfig config;
    config.max_refine_hz = value;
    bad.emplace_back(std::string("max_refine_hz ") + label, config);
  };
  with_refine(-1.0f, "negative");
  with_refine(kNaN, "NaN");
  with_refine(kInf, "inf");

  const auto with_tolerance = [&bad](float value, const char* label) {
    SharedBinConfig config;
    config.f0_tolerance_cents = value;
    bad.emplace_back(std::string("f0_tolerance_cents ") + label, config);
  };
  with_tolerance(0.0f, "0");
  with_tolerance(-50.0f, "negative");
  with_tolerance(1200.1f, "over 1200");
  with_tolerance(kNaN, "NaN");
  with_tolerance(kInf, "inf");
  return bad;
}

/// @brief Every documented range's inclusive end, which a guard using one
///        comparison at both ends of an interval gets wrong.
std::vector<std::pair<std::string, SharedBinConfig>> edge_configs() {
  std::vector<std::pair<std::string, SharedBinConfig>> good;
  for (const int value : {4, 8, 64}) {
    SharedBinConfig config;
    config.window_frames = value;
    good.emplace_back("window_frames " + std::to_string(value), config);
  }
  SharedBinConfig at_pi;
  at_pi.min_partial_separation = sonare::constants::kPi;
  good.emplace_back("min_partial_separation pi", at_pi);
  SharedBinConfig tiny_separation;
  tiny_separation.min_partial_separation = 1e-6f;
  good.emplace_back("min_partial_separation 1e-6", tiny_separation);
  SharedBinConfig whole_residual;
  whole_residual.max_fit_residual = 1.0f;
  good.emplace_back("max_fit_residual 1", whole_residual);
  SharedBinConfig unit_modulus;
  unit_modulus.max_weight_modulus = 1.0f;
  good.emplace_back("max_weight_modulus 1", unit_modulus);
  SharedBinConfig derived_refine;
  derived_refine.max_refine_hz = 0.0f;
  good.emplace_back("max_refine_hz 0, derived", derived_refine);
  SharedBinConfig octave_tolerance;
  octave_tolerance.f0_tolerance_cents = 1200.0f;
  good.emplace_back("f0_tolerance_cents 1200", octave_tolerance);
  return good;
}

}  // namespace

// --- What solving leaves alone ---------------------------------------------

TEST_CASE("solving changes the weights and nothing else about the set", "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.5f);
  REQUIRE(spec.n_frames() > 16);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);

  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);

  // It did something: weights moved off the equal split, and they carry a phase,
  // which is what a real-valued division cannot express.
  size_t moved = 0;
  size_t with_phase = 0;
  for (const Entry& entry : entries_of(solved)) {
    const Complex weight = solved.notes[entry.note].weights[entry.at];
    if (weight != masks.notes[entry.note].weights[entry.at]) ++moved;
    if (std::abs(weight.imag()) > 1e-4f * std::abs(weight)) ++with_phase;
  }
  INFO("weights moved " << moved << ", of which carry a phase " << with_phase);
  REQUIRE(moved > 0);
  REQUIRE(with_phase > 0);
}

TEST_CASE("solving is deterministic and a no-op on its own result", "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.4f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  // Deliberately two axes away from the builder's defaults, because that is what
  // gives the round trip below its teeth: a second call that read a default
  // geometry instead of the one it was handed would claim different bins and
  // gate them differently, and only a non-default set can tell the two apart.
  const NoteMaskSet masks = matched_masks(spec, track, kDominatedLobes);
  const NoteMaskConfig defaults;
  REQUIRE(masks.config.n_harmonics != defaults.n_harmonics);
  REQUIRE(masks.config.claim_lobes != defaults.claim_lobes);

  const NoteMaskSet first = solve_shared_bins(spec, masks, track);
  const NoteMaskSet again = solve_shared_bins(spec, masks, track);
  // Every weight is computed from spec and never from the one it found in the
  // masks, and a refusal returns what it received, so feeding the result back is
  // the sharper reading of the same property: it must change nothing at all.
  const NoteMaskSet fed_back = solve_shared_bins(spec, first, track);
  REQUIRE(again.notes.size() == first.notes.size());
  REQUIRE(fed_back.notes.size() == first.notes.size());
  for (size_t i = 0; i < first.notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(again.notes[i].weights == first.notes[i].weights);
    REQUIRE(fed_back.notes[i].weights == first.notes[i].weights);
  }
}

TEST_CASE("a bin one note claims is unshared and that note keeps the whole of it",
          "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones({ToneSpec{}}, 0.4f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f});
  const NoteMaskSet masks = matched_masks(spec, track);

  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);

  // Nothing is shared, so the whole set comes back as it went in, and the weight
  // is one exactly rather than a fit that landed near it.
  REQUIRE(solved.notes.size() == 1);
  REQUIRE(!solved.notes[0].weights.empty());
  REQUIRE(solved.notes[0].weights == masks.notes[0].weights);
  for (const Complex weight : solved.notes[0].weights) REQUIRE(weight == Complex(1.0f, 0.0f));
  for (const Entry& entry : entries_of(solved)) {
    REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::Unshared);
  }
}

// --- What solving is worth, against an oracle -------------------------------

TEST_CASE("a solved bin lands far closer to the true share than the equal split",
          "[polyphony_shared_bins]") {
  // A fifth detuned by six cents, so the partials that would coincide at 900 and
  // 1800 Hz instead sit 3 and 6 Hz apart -- close enough to share every bin of
  // those two claims, far enough that the poles stand 0.2 and 0.4 radians per
  // frame apart and the fit is well conditioned.
  const ToneSpec lower{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0};
  const Duet duet = render_duet(lower, upper, 0.5f);
  require_linear(duet);

  const sonare::Spectrogram& spec = duet.mix;
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);

  size_t measured = 0;
  const double gain_db = gain_over_equal_split(duet, masks, solved, report, measured);
  INFO("solved entries measured " << measured << ", gain over the equal split " << gain_db
                                  << " dB");
  REQUIRE(measured > 50);
  // The header puts an interval it accepts at 23 dB and up. The bound sits an
  // order of magnitude under that on purpose: it asserts that the division was
  // estimated, rather than reproducing what one fixture happened to reach.
  REQUIRE(gain_db > 6.0);
}

TEST_CASE("a track whose f0 is wrong still solves, because the solve refines it first",
          "[polyphony_shared_bins]") {
  // Ten cents sharp on both notes. That is far too little to move the claims off
  // the partials -- the fundamental's claim is forty hertz wide and moves by five
  // -- and far too much for the assignment: at 900 Hz ten cents predicts 905.2
  // where the two poles stand at 900 and 903, so matching each note to its
  // nearest pole hands both notes the wrong one. A solve that takes the track's
  // f0 as given therefore swaps the two partials and loses to the equal split;
  // one that refines first does not. Nothing else in this file can tell the two
  // apart, because every other fixture hands an exact f0.
  const ToneSpec lower{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0};
  const Duet duet = render_duet(lower, upper, 0.5f);
  require_linear(duet);

  const sonare::Spectrogram& spec = duet.mix;
  const MultiF0Track track =
      track_of_pitches(spec, {static_cast<float>(shift_cents(lower.f0_hz, 10.0)),
                              static_cast<float>(shift_cents(upper.f0_hz, 10.0))});
  // Not @ref kDominatedLobes, and it cannot be: the claim here is centred on a
  // wrong prediction, and ten cents at 1800 Hz is 0.97 bins, so a claim narrowed
  // to one bin either side would exclude both true partials and the case would
  // measure leakage. The constraint belongs to this fixture rather than to the
  // width.
  const NoteMaskSet masks = matched_masks(spec, track);
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);

  size_t measured = 0;
  const double gain_db = gain_over_equal_split(duet, masks, solved, report, measured);
  INFO("solved entries measured " << measured << ", gain over the equal split " << gain_db
                                  << " dB");
  REQUIRE(measured > 50);
  // The same bound the exact-f0 case asserts: refined, the result returns to its
  // zero-error value, so a wrong track costs nothing rather than merely costing
  // less than it would have.
  REQUIRE(gain_db > 6.0);

  // And the refinement the solve did internally is visible through the entry
  // point that exposes it, so a caller can see what it got.
  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  REQUIRE(std::abs(cents_between(refined[0], lower.f0_hz)) < 1.0);
  REQUIRE(std::abs(cents_between(refined[1], upper.f0_hz)) < 1.0);
}

// --- Refusals ---------------------------------------------------------------

TEST_CASE("an equal-tempered octave's upper note cannot be refined, and its shared bins say so",
          "[polyphony_shared_bins]") {
  // Two to one exactly. Every partial the upper note has stands on one of the
  // lower note's, so it has no unshared partial to refine from and comes back as
  // the sentinel; every bin it shares therefore fails on the gate's missing input
  // before the gate itself is reached. The claim width is left at the builder's
  // default here, unlike the cases that assert over every shared bin: this verdict
  // is a property of the ridge rather than of what a decomposition saw in a bin,
  // so an edge bin and a centre bin must answer alike.
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{600.0, 0.25, 8, 1.5, 0.4, 0.0, 0.0}},
      0.5f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 600.0f});
  const NoteMaskSet masks = matched_masks(spec, track);

  // The sentinel, and the reason it is a sentinel rather than the input: the
  // lower note keeps 300 and 900 Hz to itself and refines, the upper note keeps
  // nothing under the alias bound and does not. An implementation that handed
  // back what it was given would pass an equality against the input and fails
  // this, because zero is not a frequency.
  // What makes this the other witness, and the half that was prose: the upper
  // note holds nothing alone under the refinement's ceiling, while the lower note
  // does. It reaches the sentinel by having nothing to qualify -- the opposite of
  // the near-unison, which has bins and cannot use them -- and stating it here is
  // what stops this case from staying green if that ever stops being true.
  const std::vector<size_t> alone = unshared_below_refine_ceiling(masks);
  REQUIRE(alone.size() == 2);
  INFO("cells held alone under the refine ceiling: " << alone[0] << " and " << alone[1]);
  REQUIRE(alone[0] > 0);
  REQUIRE(alone[1] == 0);

  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  REQUIRE(refined[0] > 0.0f);
  REQUIRE(std::abs(cents_between(refined[0], 300.0)) < 1.0);
  REQUIRE(refined[1] == 0.0f);

  const SharedBinConfig config;
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const std::vector<int> counts = claim_counts(masks);
  size_t shared = 0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    ++shared;
    INFO("bin " << entry.bin << " frame " << entry.frame << " outcome "
                << name_of(report.outcome[entry.cell]));
    REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::F0NotRefined);
    // Bit-identical to what it arrived with, which is what never being worse than
    // the input means on a bin the solver would lose on -- measured at 4.7 dB
    // here even with the poles assigned by an oracle.
    REQUIRE(solved.notes[entry.note].weights[entry.at] ==
            masks.notes[entry.note].weights[entry.at]);
  }
  INFO("shared entries " << shared);
  REQUIRE(shared > 0);
}

TEST_CASE("a near-unison has unshared bins and still cannot refine an f0",
          "[polyphony_shared_bins]") {
  // Not a case about a near-unison being refused. It is a case about *why*: the
  // pair has bins only one note claims, low enough to refine from, and they are
  // still useless -- which is the difference between a qualifier that judges what
  // is in a bin and one that counts claimants.
  const ToneSpec lower{300.0, 0.30, kFixturePartials, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{301.5, 0.30, kFixturePartials, 1.0, 0.785398, 0.0, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({lower, upper}, 0.5f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 301.5f});
  const NoteMaskSet masks = matched_masks(spec, track);

  // The property that makes this fixture the witness, asserted rather than left to
  // prose: both notes hold bins alone, under the refinement's own ceiling. Without
  // it the case would reach the sentinel the way an octave's upper note does --
  // by having nothing to qualify -- and deleting the qualifier under test would
  // leave it green.
  const std::vector<size_t> alone = unshared_below_refine_ceiling(masks);
  REQUIRE(alone.size() == 2);
  INFO("cells held alone under the refine ceiling: " << alone[0] << " and " << alone[1]);
  REQUIRE(alone[0] > 0);
  REQUIRE(alone[1] > 0);

  // And they are useless: a hertz and a half apart, neither note's own partial is
  // what those bins hold.
  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  REQUIRE(refined[0] == 0.0f);
  REQUIRE(refined[1] == 0.0f);

  // The control, through the same machinery: at a real interval it returns
  // frequencies. A build that returned the sentinel for everything satisfies every
  // assertion above and fails here, which is the failure mode a sentinel invites.
  {
    const sonare::Spectrogram fifth =
        spectrogram_of_tones({ToneSpec{300.0, 0.30, kFixturePartials, 1.0, 0.0, 0.0, 0.0},
                              ToneSpec{451.5, 0.25, kFixturePartials, 1.4, 0.7, 0.0, 0.0}},
                             0.5f);
    const MultiF0Track apart = track_of_pitches(fifth, {300.0f, 451.5f});
    const std::vector<float> control = refine_track_f0(fifth, apart, matched_masks(fifth, apart));
    REQUIRE(control.size() == 2);
    INFO("control refined to " << control[0] << " and " << control[1]);
    REQUIRE(control[0] > 0.0f);
    REQUIRE(control[1] > 0.0f);
  }

  // With no f0 to predict a gap from, every shared bin fails on that missing input
  // and keeps exactly what it arrived with.
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);
  const std::vector<int> counts = claim_counts(masks);
  size_t shared = 0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    ++shared;
    INFO("bin " << entry.bin << " frame " << entry.frame << " outcome "
                << name_of(report.outcome[entry.cell]));
    REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::F0NotRefined);
    REQUIRE(solved.notes[entry.note].weights[entry.at] ==
            masks.notes[entry.note].weights[entry.at]);
  }
  INFO("shared entries " << shared);
  REQUIRE(shared > 0);
}

TEST_CASE("the gate reads the predicted gap, and one fixture straddles it",
          "[polyphony_shared_bins]") {
  // A fifth detuned by a twentieth of a hertz. Both notes keep a low unshared
  // partial -- 300 and 450.05 Hz, a hundred and fifty hertz clear of anything the
  // other claims -- so both f0s refine and the gate has its input, which is what
  // separates this fixture from the octave above.
  //
  // The gap the gate reads is the partial's, not the note's, and a partial gap
  // scales with the harmonic number: at the third against the second it is
  // 0.10 Hz and at the sixth against the fourth it is 0.20, so one render puts a
  // population either side of the 0.01 rad threshold. That is the sharpest form
  // available -- the two groups differ in nothing but the gap.
  const ToneSpec lower{300.0, 0.3, kFixturePartials, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{450.05, 0.25, kFixturePartials, 1.4, 0.7, 0.0, 0.0};
  const Duet duet = render_duet(lower, upper, 0.5f);
  require_linear(duet);

  const sonare::Spectrogram& spec = duet.mix;
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 450.05f});
  // Full claim width: every verdict this case asserts is fixed from the refined
  // f0 before any decomposition runs, so an edge bin must answer as the centre
  // does and narrowing would only shrink the population.
  const NoteMaskSet masks = matched_masks(spec, track);

  // Both refine, or every shared bin fails on the gate's missing input and the
  // gate itself is never reached.
  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  REQUIRE(refined[0] > 0.0f);
  REQUIRE(refined[1] > 0.0f);

  // Bin 126 is 1357 Hz, between the two coincidence groups at 900 and 1800 and
  // far from either, so the split is read off the fixture rather than off a
  // verdict.
  const int split_bin = 126;
  const SharedBinConfig config;
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const std::vector<int> counts = claim_counts(masks);
  size_t under = 0;
  size_t over = 0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    INFO("bin " << entry.bin << " frame " << entry.frame << " outcome "
                << name_of(report.outcome[entry.cell]) << " separation "
                << report.partial_separation[entry.cell]);
    if (entry.bin < split_bin) {
      ++under;
      REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::PartialsTooClose);
      REQUIRE(report.partial_separation[entry.cell] < config.min_partial_separation);
      REQUIRE(solved.notes[entry.note].weights[entry.at] ==
              masks.notes[entry.note].weights[entry.at]);
    } else {
      ++over;
      REQUIRE(report.outcome[entry.cell] != SharedBinOutcome::PartialsTooClose);
      REQUIRE(report.partial_separation[entry.cell] >= config.min_partial_separation);
    }
  }
  INFO("shared entries under the threshold " << under << ", over it " << over);
  REQUIRE(under > 0);
  REQUIRE(over > 0);

  // What the gate is worth, measured rather than argued: the same two groups with
  // the gate opened far enough to admit both. Reported and not asserted -- which
  // side of the crossing a given gap falls on is the header's measurement to own,
  // and the two groups here sit at exactly the two detunings it was swept at.
  SharedBinConfig forced = config;
  forced.min_partial_separation = 1e-6f;
  SharedBinReport open_report;
  const NoteMaskSet forced_solved = solve_and_check(spec, masks, track, forced, open_report);
  const std::vector<int> forced_counts = claim_counts(masks);
  size_t solved_under = 0;
  size_t solved_over = 0;
  for (const Entry& entry : entries_of(forced_solved)) {
    if (forced_counts[entry.cell] < 2) continue;
    if (open_report.outcome[entry.cell] != SharedBinOutcome::Solved) continue;
    if (entry.bin < split_bin) {
      ++solved_under;
    } else {
      ++solved_over;
    }
  }
  INFO("with the gate open, solved entries under " << solved_under << ", over " << solved_over);
  if (solved_under > 0) {
    size_t measured = 0;
    const double gain =
        gain_over_equal_split(duet, masks, forced_solved, open_report, measured, 0, split_bin - 1);
    INFO("gap 0.10 Hz: the pole fit's error energy is "
         << gain << " dB below the equal split's, over " << measured
         << " entries -- positive means the fit wins");
    REQUIRE(std::isfinite(gain));
  }
  if (solved_over > 0) {
    size_t measured = 0;
    const double gain = gain_over_equal_split(duet, masks, forced_solved, open_report, measured,
                                              split_bin, std::numeric_limits<int>::max());
    INFO("gap 0.20 Hz: the pole fit's error energy is "
         << gain << " dB below the equal split's, over " << measured
         << " entries -- positive means the fit wins");
    REQUIRE(std::isfinite(gain));
  }
}

TEST_CASE("vibrato deep enough to move a partial is refused as a diverged fit",
          "[polyphony_shared_bins]") {
  // The same detuned fifth twice, once steady and once with the upper note bent
  // by fifteen cents at five and a half hertz -- the depth the residual threshold
  // was set against. Contrastive on purpose: a build that refused everything, or
  // nothing, passes one half of this and fails the other.
  const auto measure = [](double vibrato_cents) {
    const sonare::Spectrogram spec =
        spectrogram_of_tones({ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0},
                              ToneSpec{451.5, 0.25, 8, 1.4, 0.7, vibrato_cents, 5.5}},
                             0.5f);
    const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
    const NoteMaskSet masks = matched_masks(spec, track, kDominatedLobes);
    const SharedBinConfig config;
    SharedBinReport report;
    const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

    const std::vector<int> counts = claim_counts(masks);
    struct Tally {
      size_t shared = 0;
      size_t diverged = 0;
      size_t solved = 0;
      double worst_residual = 0.0;
    } tally;
    for (const Entry& entry : entries_of(solved)) {
      if (counts[entry.cell] < 2) continue;
      ++tally.shared;
      tally.worst_residual =
          std::max(tally.worst_residual, static_cast<double>(report.fit_residual[entry.cell]));
      if (report.outcome[entry.cell] == SharedBinOutcome::FitDiverged) ++tally.diverged;
      if (report.outcome[entry.cell] == SharedBinOutcome::Solved) ++tally.solved;
      // Whatever the verdict, a refused bin keeps its equal split untouched.
      if (report.outcome[entry.cell] != SharedBinOutcome::Solved) {
        REQUIRE(solved.notes[entry.note].weights[entry.at] ==
                masks.notes[entry.note].weights[entry.at]);
      }
    }
    return tally;
  };

  const SharedBinConfig defaults;
  const auto steady = measure(0.0);
  const auto bent = measure(15.0);
  INFO("steady: shared " << steady.shared << " solved " << steady.solved << " diverged "
                         << steady.diverged << " worst residual " << steady.worst_residual);
  INFO("bent: shared " << bent.shared << " solved " << bent.solved << " diverged " << bent.diverged
                       << " worst residual " << bent.worst_residual);

  // A sum of steady poles is exactly what the model is, so nothing about the
  // steady pair reads as a misfit.
  REQUIRE(steady.shared > 0);
  REQUIRE(steady.solved > 0);
  REQUIRE(steady.diverged == 0);
  REQUIRE(steady.worst_residual <= static_cast<double>(defaults.max_fit_residual));

  // The bend is not, and what separates them is the refusal rather than a worse
  // fit that was accepted anyway.
  REQUIRE(bent.shared > 0);
  REQUIRE(bent.diverged > 0);
  REQUIRE(bent.worst_residual > static_cast<double>(defaults.max_fit_residual));
  REQUIRE(bent.worst_residual > 10.0 * steady.worst_residual);
}

TEST_CASE("a note spanning fewer frames than the window is not solved", "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.5f);
  REQUIRE(spec.n_frames() > 16);
  REQUIRE(spec.n_frames() < 64);

  SECTION("a window longer than the whole analysis leaves the set untouched") {
    // 64 is the top of the documented range and the spectrogram is shorter than
    // that, so no note can be solved and the result is the input, entry for entry.
    const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
    const NoteMaskSet masks = matched_masks(spec, track);
    SharedBinConfig config;
    config.window_frames = 64;

    SharedBinReport report;
    const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

    const std::vector<int> counts = claim_counts(masks);
    size_t shared = 0;
    for (const Entry& entry : entries_of(solved)) {
      REQUIRE(solved.notes[entry.note].weights[entry.at] ==
              masks.notes[entry.note].weights[entry.at]);
      if (counts[entry.cell] < 2) continue;
      ++shared;
      REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::TooFewFrames);
    }
    REQUIRE(shared > 0);
  }

  SECTION("one short note among long ones refuses only the bins it stands on") {
    // Five frames against a window of eight, inside a span the other note covers
    // whole, so the same bins are solvable a few frames later and the refusal is
    // the span rather than the material.
    const int short_start = spec.n_frames() / 2;
    const MultiF0Track track = track_over(
        spec, {steady_ridge(300.0f, 0, spec.n_frames()), steady_ridge(451.5f, short_start, 5)});
    const NoteMaskSet masks = matched_masks(spec, track);
    SharedBinConfig config;
    config.window_frames = 8;

    SharedBinReport report;
    const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

    const std::vector<int> counts = claim_counts(masks);
    size_t shared = 0;
    for (const Entry& entry : entries_of(solved)) {
      if (counts[entry.cell] < 2) continue;
      ++shared;
      INFO("bin " << entry.bin << " frame " << entry.frame);
      // Sharing only happens where the short note stands, which is what says the
      // refusal below is about its span rather than about the whole set.
      REQUIRE(entry.frame >= short_start);
      REQUIRE(entry.frame < short_start + 5);
      REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::TooFewFrames);
      REQUIRE(solved.notes[entry.note].weights[entry.at] ==
              masks.notes[entry.note].weights[entry.at]);
    }
    REQUIRE(shared > 0);
  }
}

TEST_CASE("a bin whose claimants change and change back is fitted as two spans",
          "[polyphony_shared_bins]") {
  // A span is a maximal run of frames claimed by the same set of notes, so a note
  // that leaves and returns leaves two of them on the bins it shares, each fitted
  // on its own. The two runs are deliberately different lengths: taking only the
  // longest would leave the twelve-frame run holding the equal split with nothing
  // recorded against it, which is the same silence the tail window closes.
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.5f);
  const int frames = spec.n_frames();
  REQUIRE(frames > 30);
  const int first_end = 12;
  const int second_start = 22;
  REQUIRE(frames - second_start > first_end);

  // One held upper note arriving as two ridges, which is what a tracker does to a
  // note it loses and reacquires, so the material under both runs is a genuine
  // pair of poles rather than one note against silence.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(300.0f, 0, frames), steady_ridge(451.5f, 0, first_end),
                        steady_ridge(451.5f, second_start, frames - second_start)});
  const NoteMaskSet masks = matched_masks(spec, track, kDominatedLobes);

  SharedBinConfig config;
  config.window_frames = 8;
  REQUIRE(first_end >= config.window_frames);
  REQUIRE(frames - second_start >= config.window_frames);

  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const std::vector<int> counts = claim_counts(masks);
  size_t in_first = 0;
  size_t in_second = 0;
  size_t between = 0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    INFO("bin " << entry.bin << " frame " << entry.frame << " outcome "
                << name_of(report.outcome[entry.cell]));
    // Sharing happens only where the upper note stands, so the gap really is a
    // break in the claimant set rather than in the spectrum.
    REQUIRE((entry.frame < first_end || entry.frame >= second_start));
    REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::Solved);
    REQUIRE(solved.notes[entry.note].weights[entry.at] !=
            masks.notes[entry.note].weights[entry.at]);
    if (entry.frame < first_end) {
      ++in_first;
    } else {
      ++in_second;
    }
  }
  for (int frame = first_end; frame < second_start; ++frame) {
    for (int bin = 0; bin < masks.n_bins; ++bin) {
      const size_t cell = at_index(masks, bin, frame);
      if (counts[cell] == 0) continue;
      ++between;
      REQUIRE(report.outcome[cell] == SharedBinOutcome::Unshared);
    }
  }

  // partial_separation is a span scalar and both spans here carry the same
  // claimant set and the same refined f0s, so every shared frame of a bin must
  // hold one value. A per-window computation leaking in would still sit above the
  // threshold on this material and would pass every other assertion in the file;
  // it shows only as a wobble from frame to frame.
  size_t constant = 0;
  for (int bin = 0; bin < masks.n_bins; ++bin) {
    const size_t first_cell = at_index(masks, bin, 0);
    if (counts[first_cell] < 2) continue;
    const float want = report.partial_separation[first_cell];
    for (int frame = 0; frame < frames; ++frame) {
      const size_t cell = at_index(masks, bin, frame);
      if (counts[cell] < 2) continue;
      INFO("bin " << bin << " frame " << frame << " separation " << report.partial_separation[cell]
                  << " against " << want);
      REQUIRE(report.partial_separation[cell] == want);
      ++constant;
    }
  }
  REQUIRE(constant > 0);

  // Both ridges stop before the last frame, so it is claimed by nobody. Asserted
  // because the invariant helper's Unclaimed arm judges a population that no
  // other fixture in this file is built to contain, and an arm nothing reaches
  // asserts nothing while reading as covered.
  size_t unclaimed = 0;
  for (int bin = 0; bin < masks.n_bins; ++bin) {
    const size_t cell = at_index(masks, bin, frames - 1);
    if (counts[cell] != 0) continue;
    REQUIRE(report.outcome[cell] == SharedBinOutcome::Unclaimed);
    ++unclaimed;
  }
  REQUIRE(unclaimed > 0);

  INFO("shared entries: first run " << in_first << ", second " << in_second << ", unshared between "
                                    << between << ", frames holding one span scalar " << constant
                                    << ", unclaimed cells in the last frame " << unclaimed);
  // Both runs, not just whichever is longer.
  REQUIRE(in_first > 0);
  REQUIRE(in_second > 0);
  REQUIRE(between > 0);
}

TEST_CASE("the frames at the tail of a span are solved rather than quietly left alone",
          "[polyphony_shared_bins]") {
  // Windows step by half, so at a window of eight the regular starts land on
  // multiples of four and the last of them covers up to frame thirty-one of a
  // thirty-five frame span. Without the extra window placed to end exactly at the
  // span's end, the three frames after it keep the equal split with no refusal
  // recorded against them -- a bin that reads solved and was never solved.
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.5f);
  const int span = 35;
  REQUIRE(spec.n_frames() > span);

  SharedBinConfig config;
  config.window_frames = 8;
  const int step = config.window_frames / 2;
  // The span has to end off the step grid, or the extra window is not needed and
  // the case checks nothing.
  REQUIRE(span % step != 0);

  // Both notes over the same frames, so the claimant set is constant and the span
  // is the whole of it rather than several runs.
  const MultiF0Track track =
      track_over(spec, {steady_ridge(300.0f, 0, span), steady_ridge(451.5f, 0, span)});
  const NoteMaskSet masks = matched_masks(spec, track, kDominatedLobes);

  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const std::vector<int> counts = claim_counts(masks);
  const int tail_from = span - (step - 1);
  size_t interior = 0;
  size_t tail = 0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    if (report.outcome[entry.cell] != SharedBinOutcome::Solved) continue;
    if (entry.frame < tail_from) {
      ++interior;
      continue;
    }
    ++tail;
    // Solved and moved off the equal split, so the verdict came from a window that
    // really ran over these frames rather than being inherited from one that
    // stopped short of them.
    REQUIRE(solved.notes[entry.note].weights[entry.at] !=
            masks.notes[entry.note].weights[entry.at]);
  }
  INFO("solved entries: interior " << interior << ", in the last " << step - 1 << " frames "
                                   << tail);
  REQUIRE(interior > 0);
  REQUIRE(tail > 0);
}

TEST_CASE("more claimants than the window can fit poles for is refused",
          "[polyphony_shared_bins]") {
  // Eight notes inside eight hertz, so every bin near the fundamental carries all
  // eight claims. A window of eight frames cannot carry an order of eight under
  // any Hankel shape -- rows and columns sum to nine, so the order is at most four
  // -- which makes this a refusal the arithmetic forces rather than a threshold
  // the fixture had to be tuned against.
  std::vector<ToneSpec> tones;
  std::vector<float> pitches;
  for (int i = 0; i < 8; ++i) {
    ToneSpec tone;
    tone.f0_hz = 300.0 + static_cast<double>(i);
    tone.amplitude = 0.1;
    tone.n_partials = kFixturePartials;
    tone.phase_step = 0.11 * static_cast<double>(i);
    tones.push_back(tone);
    pitches.push_back(static_cast<float>(tone.f0_hz));
  }
  const sonare::Spectrogram spec = spectrogram_of_tones(tones, 0.4f);
  const MultiF0Track track = track_of_pitches(spec, pitches);
  const NoteMaskSet masks = matched_masks(spec, track);

  /// @brief Outcomes of the bins all eight notes claim, at one window length.
  const auto verdicts = [&](int window_frames) {
    SharedBinConfig config;
    config.window_frames = window_frames;
    SharedBinReport report;
    const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

    const std::vector<int> counts = claim_counts(masks);
    std::vector<SharedBinOutcome> outcomes;
    for (const Entry& entry : entries_of(solved)) {
      if (counts[entry.cell] != 8) continue;
      outcomes.push_back(report.outcome[entry.cell]);
      if (report.outcome[entry.cell] != SharedBinOutcome::Solved) {
        REQUIRE(solved.notes[entry.note].weights[entry.at] ==
                masks.notes[entry.note].weights[entry.at]);
      }
    }
    REQUIRE(!outcomes.empty());
    return outcomes;
  };

  SECTION("the boundary is half the window, on both sides of it") {
    // Eight claimants against a ceiling of window_frames / 2: refused at a window
    // of fourteen, where the ceiling is seven, and not refused at sixteen, where
    // it is eight. Whether a window of sixteen then solves or refuses for some
    // other reason is not the claim -- only that the claim count stopped being
    // the reason.
    for (const SharedBinOutcome outcome : verdicts(14)) {
      REQUIRE(outcome == SharedBinOutcome::TooManyClaimants);
    }
    for (const SharedBinOutcome outcome : verdicts(16)) {
      INFO("outcome " << name_of(outcome));
      REQUIRE(outcome != SharedBinOutcome::TooManyClaimants);
    }
    // And far under the boundary, where it is four.
    for (const SharedBinOutcome outcome : verdicts(8)) {
      REQUIRE(outcome == SharedBinOutcome::TooManyClaimants);
    }
  }

  SECTION("a span both too short and too crowded reports the earlier determination") {
    // Both apply: five frames against a window of eight, and eight claimants
    // against a ceiling of four. TooFewFrames is declared first, so it is the one
    // reported -- and reporting whichever was tested first is exactly what the
    // declaration order exists to stop.
    std::vector<F0Ridge> ridges;
    for (const float hz : pitches) ridges.push_back(steady_ridge(hz, 4, 5));
    const MultiF0Track short_track = track_over(spec, std::move(ridges));
    const NoteMaskSet short_masks = matched_masks(spec, short_track);

    SharedBinConfig config;
    config.window_frames = 8;
    SharedBinReport report;
    const NoteMaskSet solved = solve_and_check(spec, short_masks, short_track, config, report);

    const std::vector<int> counts = claim_counts(short_masks);
    size_t both = 0;
    for (const Entry& entry : entries_of(solved)) {
      if (counts[entry.cell] != 8) continue;
      ++both;
      INFO("bin " << entry.bin << " frame " << entry.frame);
      REQUIRE(report.outcome[entry.cell] == SharedBinOutcome::TooFewFrames);
    }
    INFO("entries that are both too short and too crowded " << both);
    REQUIRE(both > 0);
  }
}

TEST_CASE("a claim standing on a partial the material never rendered holds only leakage",
          "[polyphony_shared_bins]") {
  // The default mask claims twenty partials and few real notes have twenty, so a
  // claim standing on nothing is the normal case rather than an edge one. Here the
  // 3:2 pair's overlap groups at 4500 and 5400 Hz sit above both tones' eighth
  // partial: those bins carry leakage from elsewhere in the spectrum and nothing
  // the claim names, so whatever the solver decides there is a decision about
  // noise. That is allowed. What is not is letting it matter -- a weight is
  // allowed to reach eight in modulus, and an unbounded one would turn a sidelobe
  // into an audible artefact inside a note.
  const ToneSpec lower{300.0, 0.3, kFixturePartials, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{451.5, 0.25, kFixturePartials, 1.4, 0.7, 0.0, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({lower, upper}, 0.5f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  // Deliberately the builder's own defaults rather than this file's matched ones:
  // twenty harmonics and a full lobe is the geometry a caller actually gets.
  const NoteMaskSet masks = build_note_masks(spec, track);

  const double bin_hz = static_cast<double>(kSampleRate) / static_cast<double>(kNfft);
  std::vector<double> rendered;
  for (const ToneSpec& tone : {lower, upper}) {
    for (int h = 1; h <= tone.n_partials; ++h) {
      rendered.push_back(tone.f0_hz * static_cast<double>(h));
    }
  }
  /// @brief Whether a partial either tone rendered reaches this bin.
  /// @details Read off the fixture's own frequencies, so the population is fixed
  ///          before the solver runs and cannot be narrowed by what it decided.
  ///          The Hann main lobe is four bins wide, so a partial reaches two bins
  ///          either side and no further.
  const auto holds_signal = [&](int bin) {
    const double hz = static_cast<double>(bin) * bin_hz;
    for (const double partial : rendered) {
      if (std::abs(hz - partial) <= 2.0 * bin_hz) return true;
    }
    return false;
  };

  SharedBinConfig config;
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const std::vector<int> counts = claim_counts(masks);
  size_t empty_shared = 0;
  size_t empty_solved = 0;
  double from_leakage = 0.0;
  double from_partials = 0.0;
  for (const Entry& entry : entries_of(solved)) {
    const Complex taken =
        solved.notes[entry.note].weights[entry.at] * spec.at(entry.bin, entry.frame);
    if (holds_signal(entry.bin)) {
      from_partials += static_cast<double>(std::norm(taken));
      continue;
    }
    from_leakage += static_cast<double>(std::norm(taken));
    if (counts[entry.cell] < 2) continue;
    ++empty_shared;
    if (report.outcome[entry.cell] == SharedBinOutcome::Solved) ++empty_solved;
  }

  // The population is real, and both halves of it are, or the ratio below is
  // comparing something against nothing.
  INFO("shared entries on claims holding no partial " << empty_shared << ", of which solved "
                                                      << empty_solved);
  REQUIRE(empty_shared > 0);
  REQUIRE(from_partials > 0.0);

  // Which refusal a leakage bin draws is not contract and is not asserted; that it
  // cannot matter is. The leakage there sits some eighty decibels under the
  // partials and the ceiling bounds any amplification of it at eight, so a
  // thousandth of the energy leaves decades of margin and would still catch a
  // weight that escaped the clamp.
  const double share = from_leakage / (from_leakage + from_partials);
  INFO("share of a note's energy drawn from claims holding no partial: " << share);
  REQUIRE(share < 1e-3);
}

TEST_CASE("a claim on a partial one note does not have while the other does",
          "[polyphony_shared_bins]") {
  // The other trigger the refusal names: a component of zero rather than an
  // observation near silence. A claim is predicted from an f0, so the upper note
  // claims a fourth partial it never rendered -- and that claim lands on the lower
  // note's sixth, which is loud. The bin therefore holds one real partial and one
  // that does not exist, the observation stays strong, and no silence floor is in
  // play.
  //
  // One render gives both populations. At the lower note's third against the
  // upper's second, 900 against 903 Hz, both partials are real and the bin is the
  // ordinary shared case. At the sixth against the fourth, 1800 against 1806, only
  // one of them is. The two groups differ in nothing else.
  const ToneSpec lower{300.0, 0.3, kFixturePartials, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec upper{451.5, 0.25, 3, 1.4, 0.7, 0.0, 0.0};
  const Duet duet = render_duet(lower, upper, 0.5f);
  require_linear(duet);

  const sonare::Spectrogram& spec = duet.mix;
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track, kDominatedLobes);

  // Both notes keep a low unshared partial -- 300 and 451.5 Hz -- so the gate has
  // its input and neither group fails on a missing f0.
  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  REQUIRE(refined[0] > 0.0f);
  REQUIRE(refined[1] > 0.0f);

  const SharedBinConfig config;
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, config, report);

  const int split_bin = 126;
  const std::vector<int> counts = claim_counts(masks);
  size_t control = 0;
  size_t control_solved = 0;
  size_t target = 0;
  size_t target_degenerate = 0;
  size_t target_solved = 0;
  double target_absent_share = 0.0;
  double target_absent_weight = 0.0;
  for (const Entry& entry : entries_of(solved)) {
    if (counts[entry.cell] < 2) continue;
    if (entry.bin < split_bin) {
      ++control;
      if (report.outcome[entry.cell] == SharedBinOutcome::Solved) ++control_solved;
      continue;
    }
    ++target;
    if (report.outcome[entry.cell] == SharedBinOutcome::DegenerateWeight) ++target_degenerate;
    if (report.outcome[entry.cell] == SharedBinOutcome::Solved) ++target_solved;
    // The upper note really has nothing here, read from its own render, and the
    // weight it was given for it.
    if (entry.note == 1) {
      const Complex mixed = spec.at(entry.bin, entry.frame);
      if (std::abs(mixed) > 0.0f) {
        target_absent_share =
            std::max(target_absent_share,
                     static_cast<double>(std::abs(duet.parts[1].at(entry.bin, entry.frame))) /
                         static_cast<double>(std::abs(mixed)));
      }
      target_absent_weight = std::max(
          target_absent_weight, static_cast<double>(std::abs(solved.notes[1].weights[entry.at])));
    }
    UNSCOPED_INFO("  target cell: bin " << entry.bin << " frame " << entry.frame << " note "
                                        << entry.note << " outcome "
                                        << name_of(report.outcome[entry.cell]) << " separation "
                                        << report.partial_separation[entry.cell] << " residual "
                                        << report.fit_residual[entry.cell] << " |mix| "
                                        << std::abs(spec.at(entry.bin, entry.frame)));
  }

  INFO("control entries " << control << " (Solved " << control_solved << "), target entries "
                          << target << " (DegenerateWeight " << target_degenerate << ", Solved "
                          << target_solved << "), largest true share of the absent note there "
                          << target_absent_share << ", largest weight it was given "
                          << target_absent_weight);

  // The control group is the ordinary shared case and has to work, or the target
  // group's verdict is about the fixture rather than about the absent partial.
  REQUIRE(control > 0);
  REQUIRE(control_solved > 0);
  REQUIRE(target > 0);
  // The absent note holds essentially none of the bin -- measured at 2e-05, which
  // is the premise the assertion below rests on rather than a result.
  REQUIRE(target_absent_share < 0.05);

  // The property this case exists for: a note is not handed a partial it does not
  // have. The bin is solved, not refused, and the note that contributed nothing to
  // it is given a weight of the order of what it contributed. The bound is loose
  // on purpose -- the claim is orders of magnitude, and a tight one around a
  // measured 2e-05 would be a number with no owner. Even at 1e-3 it sits three
  // orders under the equal split this replaced.
  REQUIRE(target_solved == target);
  REQUIRE(target_absent_weight > 0.0);
  REQUIRE(target_absent_weight < 1e-3);
}

// --- The ceiling on a weight ------------------------------------------------

TEST_CASE("a weight over the ceiling is scaled back in modulus with its angle kept",
          "[polyphony_shared_bins]") {
  // The file's detuned fifth, so refinement works and the gate passes -- a
  // near-unison is refused outright and never reaches a weight at all. The
  // cancellation is between two *partials* rather than two notes: the lower
  // note's third and the upper note's second both stand near 900 Hz, three hertz
  // apart, so they beat through antiphase twice over half a second.
  //
  // ToneSpec gives partial h an amplitude of amp/h, so those two are amp_a/3 and
  // amp_b/2, and their dampings differ (3.0 against 2.8 per second), which carries
  // the ratio *through* equality rather than parking it near one. The amplitudes
  // put that crossing near the middle of the signal and the phase offset puts an
  // antiphase instant there too, so the null is as deep as the pair can make it.
  // The amplitudes are tuned and the population is four entries, which is as
  // thick as this can be made: a weight over the ceiling *is* a near-cancellation
  // by definition, so there is no construction that does not depend on one. The
  // depth is steep and not monotonic in the upper note's level -- 36.8, 119.8,
  // 49.5, 28.9 across four settings 0.0025 apart -- and this one sits in the
  // flattest neighbourhood rather than at the deepest null. Raising it toward
  // 0.1675 looks like more headroom and is the sharpest point on the curve.
  const ToneSpec first{300.0, 0.30, 8, 1.0, 0.0, 0.0, 0.0};
  const ToneSpec second{451.5, 0.1725, 8, 1.4, 0.7854, 0.0, 0.0};
  // Each note alone as well as the pair, so the share a note truly has of a bin
  // is read off the renders rather than off the fit that is under test.
  const Duet duet = render_duet(first, second, 0.5f);
  require_linear(duet);
  const sonare::Spectrogram& spec = duet.mix;
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);
  const std::vector<int> counts = claim_counts(masks);
  const std::vector<float> refined = refine_track_f0(spec, track, masks);

  SharedBinConfig tight;
  tight.max_weight_modulus = 8.0f;
  SharedBinConfig loose;
  loose.max_weight_modulus = 64.0f;

  SharedBinReport tight_report;
  SharedBinReport loose_report;
  const NoteMaskSet clamped = solve_and_check(spec, masks, track, tight, tight_report);
  const NoteMaskSet wide = solve_and_check(spec, masks, track, loose, loose_report);

  size_t bit = 0;
  double worst_angle = 0.0;
  double largest = 0.0;
  for (const Entry& entry : entries_of(clamped)) {
    if (tight_report.outcome[entry.cell] != SharedBinOutcome::Solved) continue;
    if (loose_report.outcome[entry.cell] != SharedBinOutcome::Solved) continue;
    const Complex loose_weight = wide.notes[entry.note].weights[entry.at];
    largest = std::max(largest, static_cast<double>(std::abs(loose_weight)));
    if (std::abs(loose_weight) <= tight.max_weight_modulus) continue;
    ++bit;
    const Complex tight_weight = clamped.notes[entry.note].weights[entry.at];
    INFO("bin " << entry.bin << " frame " << entry.frame << " loose |w| " << std::abs(loose_weight)
                << " tight |w| " << std::abs(tight_weight));
    REQUIRE(static_cast<double>(std::abs(tight_weight)) <=
            static_cast<double>(tight.max_weight_modulus) * (1.0 + kReconstructionTolerance));
    // The angle survived the clamp. A clamp that dropped the phase returns a
    // positive real weight, whose angle is zero where the unclamped one's is of
    // order a radian, so any bound well under that separates the two readings.
    //
    // Asserted here rather than accumulated and asserted after the loop. An
    // aggregate initialised to a passing value asserts nothing when the loop
    // never runs, and it shares its population with the count below -- so the
    // count fails loudly, the aggregate passes quietly, and the case reports one
    // failure where two assertions stopped measuring.
    const double turn =
        std::abs(static_cast<double>(std::arg(tight_weight) - std::arg(loose_weight)));
    const double angle = std::min(turn, 2.0 * sonare::constants::kPiD - turn);
    REQUIRE(angle < 0.05);
    worst_angle = std::max(worst_angle, angle);
  }

  // What the population above the ceiling actually is, and which verdict each of
  // its cells drew. Read from the oracle so it describes the material and the
  // claim geometry rather than what the solver decided about them.
  double oracle_max = 0.0;
  double solved_oracle_max = 0.0;
  double energy_all = 0.0;
  double energy_over = 0.0;
  size_t over_ceiling = 0;
  size_t over_solved = 0;
  size_t over_degenerate = 0;
  size_t over_other = 0;
  for (const Entry& entry : entries_of(clamped)) {
    if (counts[entry.cell] < 2) continue;
    const Complex mixed = spec.at(entry.bin, entry.frame);
    if (std::abs(mixed) == 0.0f) continue;
    const Complex alone = duet.parts[entry.note].at(entry.bin, entry.frame);
    const double share =
        static_cast<double>(std::abs(alone)) / static_cast<double>(std::abs(mixed));
    const double energy = static_cast<double>(std::norm(alone));
    oracle_max = std::max(oracle_max, share);
    energy_all += energy;
    if (tight_report.outcome[entry.cell] == SharedBinOutcome::Solved) {
      solved_oracle_max = std::max(solved_oracle_max, share);
    }
    if (share <= static_cast<double>(tight.max_weight_modulus)) continue;
    ++over_ceiling;
    energy_over += energy;
    if (tight_report.outcome[entry.cell] == SharedBinOutcome::Solved) {
      ++over_solved;
    } else if (tight_report.outcome[entry.cell] == SharedBinOutcome::DegenerateWeight) {
      ++over_degenerate;
    } else {
      ++over_other;
    }
    UNSCOPED_INFO("  over-ceiling cell: bin "
                  << entry.bin << " frame " << entry.frame << " oracle " << share << " outcome "
                  << name_of(tight_report.outcome[entry.cell]) << " separation "
                  << tight_report.partial_separation[entry.cell] << " residual "
                  << tight_report.fit_residual[entry.cell] << " |mix| " << std::abs(mixed));
  }

  INFO("refined f0 " << refined[0] << " and " << refined[1] << " (handed 300 and 451.5), gap "
                     << (refined[1] - refined[0]) << " Hz, which is "
                     << (sonare::constants::kTwoPiD * static_cast<double>(refined[1] - refined[0]) *
                         kHopLength / kSampleRate)
                     << " rad/frame per f0");
  INFO("oracle share: max " << oracle_max << ", max among cells reported Solved "
                            << solved_oracle_max);
  INFO("cells whose oracle share exceeds the ceiling: "
       << over_ceiling << " (Solved " << over_solved << ", DegenerateWeight " << over_degenerate
       << ", other " << over_other << ")");
  INFO("share of a note's energy in those cells: " << (energy_all > 0.0 ? energy_over / energy_all
                                                                        : 0.0));
  INFO("weights the ceiling bit on " << bit << ", largest unclamped modulus " << largest
                                     << ", worst angle change " << worst_angle);
  REQUIRE(bit > 0);
}

// --- The identity the representation rests on -------------------------------

TEST_CASE("the notes plus the residual still return the input after solving",
          "[polyphony_shared_bins]") {
  // Load-bearing, and worth stating plainly what it is not: residual_spectrum
  // defines the residual as spec * (1 - total), so this identity holds for any
  // weights whatsoever. A build that returned the equal split untouched, or
  // noise, passes it just as exactly. It says the representation loses nothing
  // under the new weights; the oracle case above is what says a weight is good.
  //
  // Noise is in the mix because it is the case where the model genuinely cannot
  // account for the whole bin, so the total falls short of one and the residual
  // has to carry the difference rather than it being normalised away.
  std::vector<float> samples(samples_of(0.5f), 0.0f);
  add_tone(samples, ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0});
  add_tone(samples, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0});
  const std::vector<float> noise = noise_samples(11u, samples.size());
  for (size_t i = 0; i < samples.size(); ++i) samples[i] += 0.002f * noise[i];

  const sonare::Spectrogram spec = spectrogram_of(audio_of(std::move(samples)));
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);

  // solve_and_check runs the identity itself, against mask_total and against the
  // masked spectra, and checks that the total is the sum of the weights rather
  // than a normalised or clamped version of it.
  SharedBinReport report;
  const NoteMaskSet solved = solve_and_check(spec, masks, track, SharedBinConfig{}, report);

  const std::vector<Complex> total = mask_total(solved);
  const std::vector<int> counts = claim_counts(solved);
  double worst_shortfall = 0.0;
  for (size_t cell = 0; cell < total.size(); ++cell) {
    if (counts[cell] < 2) continue;
    worst_shortfall =
        std::max(worst_shortfall, static_cast<double>(std::abs(total[cell] - Complex(1.0f, 0.0f))));
  }
  // Reported rather than asserted: how far a solved bin's total sits from one is
  // how much of that bin the pole model failed to explain, which is a property of
  // the material. Asserting it is one asserts the opposite of the contract.
  INFO("worst distance from a total of one, over shared bins: " << worst_shortfall);
  REQUIRE(std::isfinite(worst_shortfall));
}

// --- Refining an f0 ---------------------------------------------------------

TEST_CASE("refine_track_f0 recovers an f0 the track got fifty cents wrong",
          "[polyphony_shared_bins]") {
  const double truth = 300.0;
  const sonare::Spectrogram spec =
      spectrogram_of_tones({ToneSpec{truth, 0.3, 8, 1.0, 0.0, 0.0, 0.0}}, 0.5f);

  SECTION("one note, so every partial is unshared") {
    for (const double error_cents : {0.0, 10.0, -25.0, 50.0, -50.0}) {
      INFO("track off by " << error_cents << " cents");
      const float given = static_cast<float>(shift_cents(truth, error_cents));
      const MultiF0Track track = track_of_pitches(spec, {given});
      const NoteMaskSet masks = matched_masks(spec, track);

      const std::vector<float> refined = refine_track_f0(spec, track, masks);
      REQUIRE(refined.size() == 1);
      REQUIRE(std::isfinite(refined[0]));
      REQUIRE(refined[0] > 0.0f);
      const double after = std::abs(cents_between(refined[0], truth));
      INFO("refined to " << refined[0] << " Hz, " << after << " cents out");
      // The header puts the recovered error at 0.001 to 0.1 cents. One cent is an
      // order of magnitude over the worst of those, so this asserts that the rate
      // was recovered rather than reproducing a fixture's luck -- and it is fifty
      // times under the error the call was handed.
      REQUIRE(after < 1.0);
    }
  }

  SECTION("two notes, refined off the partials the other one does not cover") {
    const sonare::Spectrogram duet = spectrogram_of_tones(
        {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
        0.5f);
    const std::vector<double> truths = {300.0, 451.5};
    const MultiF0Track track =
        track_of_pitches(duet, {static_cast<float>(shift_cents(truths[0], 50.0)),
                                static_cast<float>(shift_cents(truths[1], -50.0))});
    const NoteMaskSet masks = matched_masks(duet, track);

    const std::vector<float> refined = refine_track_f0(duet, track, masks);
    REQUIRE(refined.size() == 2);
    for (size_t i = 0; i < refined.size(); ++i) {
      const double after = std::abs(cents_between(refined[i], truths[i]));
      INFO("ridge " << i << " refined to " << refined[i] << " Hz, " << after << " cents out");
      REQUIRE(after < 1.0);
    }
  }

  SECTION("a track with no ridges refines to nothing") {
    const MultiF0Track track = track_over(spec, {});
    const NoteMaskSet masks = matched_masks(spec, track);
    REQUIRE(refine_track_f0(spec, track, masks).empty());
  }
}

TEST_CASE("a ridge with no usable unshared partial comes back as the sentinel",
          "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones({ToneSpec{}}, 0.4f);
  const float given = static_cast<float>(shift_cents(300.0, 50.0));

  SECTION("another note covers every partial") {
    // The same pitch twice: every claimed bin has two claimants, so no partial is
    // unshared and there is nothing to fit an order-one model to.
    const MultiF0Track track = track_of_pitches(spec, {given, given});
    const NoteMaskSet masks = matched_masks(spec, track);

    const std::vector<float> refined = refine_track_f0(spec, track, masks);
    REQUIRE(refined.size() == 2);
    // Exactly zero, not the input: a returned input is bit-identical to a
    // refinement that agreed with it, so an implementation that never refined
    // anything would satisfy an equality against the input and cannot satisfy
    // this. Zero is not a frequency, so the sentinel cannot be spent by accident.
    for (const float hz : refined) REQUIRE(hz == 0.0f);
  }

  SECTION("every partial is over Nyquist, so the note claims nothing at all") {
    const MultiF0Track track = track_of_pitches(spec, {30000.0f});
    const NoteMaskSet masks = matched_masks(spec, track);
    REQUIRE(masks.notes[0].bins.empty());

    const std::vector<float> refined = refine_track_f0(spec, track, masks);
    REQUIRE(refined.size() == 1);
    REQUIRE(refined[0] == 0.0f);
  }

  SECTION("the widest tolerance leaves the derived ceiling under the fundamental") {
    // The ceiling is (sample_rate/hop_length) / 2 / (2^(tolerance/1200) - 1), so
    // at an octave of tolerance and this framing it is 43 Hz -- under every
    // partial the note has. Widening the tolerance really does leave fewer
    // partials usable, and at the top of the documented range it leaves none.
    const MultiF0Track track = track_of_pitches(spec, {given});
    const NoteMaskSet masks = matched_masks(spec, track);

    SharedBinConfig config;
    config.f0_tolerance_cents = 1200.0f;
    const std::vector<float> refined = refine_track_f0(spec, track, masks, config);
    REQUIRE(refined.size() == 1);
    REQUIRE(refined[0] == 0.0f);
  }

  SECTION("the ceiling on a usable partial sits under the fundamental") {
    const MultiF0Track track = track_of_pitches(spec, {given});
    const NoteMaskSet masks = matched_masks(spec, track);

    SharedBinConfig config;
    config.max_refine_hz = 50.0f;
    const std::vector<float> refined = refine_track_f0(spec, track, masks, config);
    REQUIRE(refined.size() == 1);
    REQUIRE(refined[0] == 0.0f);
  }
}

TEST_CASE("one usable unshared partial refines as well as several", "[polyphony_shared_bins]") {
  // The header's claim that taking five partials changes nothing, because the
  // recovered rate is exact rather than noisy. A ceiling just over the fundamental
  // leaves exactly one partial usable; the derived ceiling leaves four.
  const double truth = 300.0;
  const sonare::Spectrogram spec =
      spectrogram_of_tones({ToneSpec{truth, 0.3, 8, 1.0, 0.0, 0.0, 0.0}}, 0.5f);
  const float given = static_cast<float>(shift_cents(truth, 50.0));
  const MultiF0Track track = track_of_pitches(spec, {given});
  const NoteMaskSet masks = matched_masks(spec, track);

  SharedBinConfig just_one;
  just_one.max_refine_hz = 350.0f;
  const std::vector<float> from_one = refine_track_f0(spec, track, masks, just_one);
  const std::vector<float> from_many = refine_track_f0(spec, track, masks);
  REQUIRE(from_one.size() == 1);
  REQUIRE(from_many.size() == 1);

  const double one_out = std::abs(cents_between(from_one[0], truth));
  const double many_out = std::abs(cents_between(from_many[0], truth));
  const double between = std::abs(cents_between(from_one[0], from_many[0]));
  INFO("one partial " << one_out << " cents out, several " << many_out << " cents out, " << between
                      << " cents apart");
  REQUIRE(one_out < 1.0);
  REQUIRE(many_out < 1.0);
  REQUIRE(between < 0.5);
}

// --- Rejections -------------------------------------------------------------

TEST_CASE("both entry points reject a set that does not describe the spectrogram",
          "[polyphony_shared_bins]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.4f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);
  REQUIRE_NOTHROW(solve_shared_bins(spec, masks, track));
  REQUIRE_NOTHROW(refine_track_f0(spec, track, masks));

  // Both entry points state the same disagreements, so each break goes to both
  // rather than to whichever one happens to be the guarded route in.
  const auto rejects = [&](const NoteMaskSet& set, const MultiF0Track& given) {
    REQUIRE(code_of([&] { return solve_shared_bins(spec, set, given); }) == kInvalid);
    REQUIRE(code_of([&] { return refine_track_f0(spec, given, set); }) == kInvalid);
  };

  SECTION("a shape the spectrogram does not have") {
    for (const int delta : {1, -1}) {
      INFO("shape offset by " << delta);
      NoteMaskSet wrong_bins = masks;
      wrong_bins.n_bins = spec.n_bins() + delta;
      rejects(wrong_bins, track);

      NoteMaskSet wrong_frames = masks;
      wrong_frames.n_frames = spec.n_frames() + delta;
      rejects(wrong_frames, track);
    }
    rejects(NoteMaskSet{}, track);
  }

  SECTION("another framing's hop or rate") {
    for (const int bad : {kHopLength + 1, kHopLength / 2, 0, -1}) {
      INFO("hop_length " << bad);
      NoteMaskSet wrong = masks;
      wrong.hop_length = bad;
      rejects(wrong, track);

      MultiF0Track wrong_track = track;
      wrong_track.hop_length = bad;
      rejects(masks, wrong_track);
    }
    for (const int bad : {kSampleRate + 1, 48000, 0, -1}) {
      INFO("sample_rate " << bad);
      NoteMaskSet wrong = masks;
      wrong.sample_rate = bad;
      rejects(wrong, track);

      MultiF0Track wrong_track = track;
      wrong_track.sample_rate = bad;
      rejects(masks, wrong_track);
    }
    for (const int delta : {1, -1}) {
      INFO("track n_frames offset by " << delta);
      MultiF0Track wrong_track = track;
      wrong_track.n_frames = spec.n_frames() + delta;
      rejects(masks, wrong_track);
    }
  }

  SECTION("a track and a set that disagree on how many ridges there are") {
    MultiF0Track fewer = track;
    fewer.ridges.pop_back();
    rejects(masks, fewer);

    MultiF0Track more = track;
    more.ridges.push_back(steady_ridge(600.0f, 0, spec.n_frames()));
    rejects(masks, more);

    NoteMaskSet short_set = masks;
    short_set.notes.pop_back();
    rejects(short_set, track);
  }

  SECTION("a mask whose sparse shape is broken") {
    // Otherwise a read outside the mask's own arrays rather than a rejected input,
    // and the weights are written back into those same arrays here.
    NoteMaskSet truncated = masks;
    truncated.notes[0].frame_offset.pop_back();
    rejects(truncated, track);

    NoteMaskSet mismatched = masks;
    mismatched.notes[0].weights.pop_back();
    rejects(mismatched, track);

    NoteMaskSet reaching = masks;
    reaching.notes[0].bins[reaching.notes[0].bins.size() / 2] = static_cast<int32_t>(spec.n_bins());
    rejects(reaching, track);

    NoteMaskSet past_the_end = masks;
    past_the_end.notes[0].frame_start = 1;
    REQUIRE(past_the_end.notes[0].frame_end() == spec.n_frames() + 1);
    rejects(past_the_end, track);
  }

  SECTION("a set carrying the default geometry is accepted") {
    // The header says a hand-built set carrying the default config is accepted
    // everywhere a set is accepted, because the geometry is metadata about how the
    // bins were chosen rather than part of the set's shape. An explicit acceptance
    // nothing exercises is one refactor from becoming an explicit rejection.
    NoteMaskSet defaulted = masks;
    defaulted.config = NoteMaskConfig{};
    REQUIRE(defaulted.config.n_harmonics != masks.config.n_harmonics);
    REQUIRE_NOTHROW(solve_shared_bins(spec, defaulted, track));
    REQUIRE_NOTHROW(refine_track_f0(spec, track, defaulted));
  }

  SECTION("an empty spectrogram") {
    const sonare::Spectrogram empty;
    REQUIRE(empty.empty());
    REQUIRE(code_of([&] { return solve_shared_bins(empty, masks, track); }) == kInvalid);
    REQUIRE(code_of([&] { return refine_track_f0(empty, track, masks); }) == kInvalid);
  }
}

TEST_CASE("both entry points reject a config outside its documented ranges",
          "[polyphony_shared_bins]") {
  const sonare::ErrorCode kInvalid = sonare::ErrorCode::InvalidParameter;
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.3f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);

  for (const auto& entry : out_of_range_configs()) {
    INFO(entry.first);
    REQUIRE(code_of([&] { return solve_shared_bins(spec, masks, track, entry.second); }) ==
            kInvalid);
    REQUIRE(code_of([&] { return refine_track_f0(spec, track, masks, entry.second); }) == kInvalid);
  }

  // Both ends of every range are inside it, so a guard written with one
  // comparison used twice fails here rather than passing quietly.
  for (const auto& entry : edge_configs()) {
    INFO(entry.first);
    REQUIRE_NOTHROW(solve_shared_bins(spec, masks, track, entry.second));
    REQUIRE_NOTHROW(refine_track_f0(spec, track, masks, entry.second));
  }
}

TEST_CASE("the report is optional and is replaced rather than added to",
          "[polyphony_shared_bins]") {
  const sonare::Spectrogram spec = spectrogram_of_tones(
      {ToneSpec{300.0, 0.3, 8, 1.0, 0.0, 0.0, 0.0}, ToneSpec{451.5, 0.25, 8, 1.4, 0.7, 0.0, 0.0}},
      0.3f);
  const MultiF0Track track = track_of_pitches(spec, {300.0f, 451.5f});
  const NoteMaskSet masks = matched_masks(spec, track);

  SharedBinReport fresh;
  const NoteMaskSet with_report = solve_shared_bins(spec, masks, track, SharedBinConfig{}, &fresh);
  REQUIRE(!fresh.outcome.empty());

  // No report asked for is the default, and it produces the same weights.
  const NoteMaskSet without = solve_shared_bins(spec, masks, track, SharedBinConfig{}, nullptr);
  REQUIRE(without.notes.size() == with_report.notes.size());
  for (size_t i = 0; i < without.notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(without.notes[i].weights == with_report.notes[i].weights);
  }

  // A report handed in with entries already in it comes back describing this call
  // only, which a writer that appended would fail.
  SharedBinReport used;
  used.outcome.assign(7, SharedBinOutcome::Solved);
  used.partial_separation.assign(7, 1.0f);
  used.fit_residual.assign(3, 1.0f);
  solve_shared_bins(spec, masks, track, SharedBinConfig{}, &used);
  REQUIRE(used.outcome.size() == fresh.outcome.size());
  REQUIRE(used.partial_separation.size() == fresh.partial_separation.size());
  REQUIRE(used.fit_residual.size() == fresh.fit_residual.size());
  REQUIRE(used.outcome == fresh.outcome);
}
