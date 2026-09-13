/// @file polyphonic_inharmonicity_test.cpp
/// @brief Contract tests for the per-note partial stretch fit.
///
/// Written from the header's contract and the measured tolerances it cites, and
/// two choices about the quantity asserted follow from what those measurements
/// say rather than from convenience:
///
/// - Claim geometry is judged as a displacement in bins, never as a rendered
///   decibel. Half a bin of claim offset is where a flat claim over a four-bin
///   main lobe starts leaving 6 dB more behind, so the displacement is the same
///   criterion read where shared-bin apportionment cannot enter it. A partial
///   two or three bins from a neighbour's is unstable in dB -- two nearly
///   identical fixtures disagreed by 77 dB on one -- and stable in bins.
/// - Every bound over a note's partials is a worst case over all of them. A
///   median hides a single partial left behind, which is the failure this entry
///   point exists for: one partial of twenty outside its claim reads -55.88 dB
///   at the median while the true cost is +52.94 dB, all of it on that partial.
///
/// No threshold or tolerance band is located by sweeping a grid. A grid coarser
/// than the band it measures reports the band as empty, which has happened on
/// this material at a 6.5% step against a 1.008x band. The one continuous
/// threshold located here is bisected; the one integer threshold is stepped to
/// its exact boundary.
///
/// An integer threshold may be stepped to its boundary and a continuous one may
/// not. A float literal is not the real number it spells -- 0.05f is
/// 0.05000000074505806 -- so whether a value equal to a continuous threshold falls
/// inside or outside depends on the width the comparison is made in, and no
/// contract here states that. Every fixture therefore sits clearly either side,
/// and nothing asserts the outcome at equality. Where a quantity the
/// implementation computes at run time is the boundary, the fixture is placed an
/// order of magnitude away from it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/polyphony/inharmonicity.h"
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

/// @brief The sentinel a refusal returns. Exactly this value, not a small one.
constexpr float kRefused = -1.0f;

/// @brief Stretches fitted from a sampled piano, with the partial counts that
///        were usable at each pitch.
constexpr double kLowF0 = 130.8128;  ///< C3.
constexpr float kLowB = 1.130e-4f;
constexpr int kLowPartials = 20;
constexpr double kHighF0 = 587.3295;  ///< D5, two octaves and a tone above C3.
constexpr float kHighB = 1.040e-3f;
constexpr int kHighPartials = 8;

/// @brief The stretch whose bands over those two notes are 7.87x apart, so it is
///        inside neither.
constexpr float kGeometricMeanB = 3.428e-4f;

/// @brief Highest partial, in Hz, a ridge's f0 may be refined from at the default
///        framing and f0 tolerance. A ridge whose every partial is above it has no
///        refinable f0 at all.
constexpr double kRefineCeilingHz = 1470.0;

/// @brief Claim half-width in bins at @c claim_lobes 1 and @c win_length equal to
///        @c n_fft, so a fixture can be placed inside or outside its own claims on
///        purpose.
constexpr double kClaimHalfWidthBins = 2.0;

/// @brief The analysis framing every fixture is reasoned in.
/// @details Centre padding off, so every frame holds whole signal and a steady
///          partial's per-bin trajectory is one pole. A centred STFT puts partly
///          zero data in the end frames and a peak located there carries the
///          padding's bias rather than the partial's position.
sonare::StftConfig analysis_stft() {
  sonare::StftConfig stft = sonare::make_stft_config(kNfft, kHopLength);
  stft.center = false;
  return stft;
}

double bin_hz() { return static_cast<double>(kSampleRate) / static_cast<double>(kNfft); }

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

/// @brief A steady tone whose partials follow @c h*f0*sqrt(1 + B*h^2).
/// @details Steady rather than decaying, because the fit locates a partial's
///          frequency and a per-harmonic decay would leave the high partials tens
///          of decibels down -- the quantity under test would then be dominated by
///          how loud the fixture made each partial. @ref misfit_hz perturbs the
///          series away from the model without moving a partial out of its claim,
///          which is the one axis that separates a fit's residual from its count.
struct StretchedTone {
  double f0_hz = kLowF0;
  double inharmonicity = 0.0;
  int n_partials = kLowPartials;
  double amplitude = 0.18;
  double phase_step = 0.0;
  /// Added to partial @c h with alternating sign, so no straight line through
  /// <tt>(h^2, (f_h/h)^2)</tt> absorbs it.
  double misfit_hz = 0.0;
};

/// @brief Where partial @p h of @p tone really stands.
double partial_hz(const StretchedTone& tone, int h) {
  const double hd = static_cast<double>(h);
  const double model = hd * tone.f0_hz * std::sqrt(1.0 + tone.inharmonicity * hd * hd);
  return model + ((h % 2 == 0) ? -tone.misfit_hz : tone.misfit_hz);
}

/// @brief Where a claim placed at @p declared_b puts partial @p h.
double claim_centre_hz(double f0_hz, double declared_b, int h) {
  const double hd = static_cast<double>(h);
  return hd * f0_hz * std::sqrt(1.0 + declared_b * hd * hd);
}

/// @brief Worst distance, in bins, between @p tone's partials and the claims a
///        single declared stretch places on them.
/// @details The worst over every rendered partial, with the partial it came from
///          reported by the caller. Half a bin is the 6 dB edge, so this is the
///          masking criterion in the unit the measurement found it constant in.
double worst_claim_offset_bins(const StretchedTone& tone, double declared_b, int& at_harmonic) {
  double worst = 0.0;
  at_harmonic = 0;
  for (int h = 1; h <= tone.n_partials; ++h) {
    const double offset =
        std::abs(partial_hz(tone, h) - claim_centre_hz(tone.f0_hz, declared_b, h)) / bin_hz();
    if (offset > worst) {
      worst = offset;
      at_harmonic = h;
    }
  }
  return worst;
}

/// @brief Largest stretch error that keeps every partial inside half a bin of its
///        claim, which is the 6 dB band's own edge.
/// @details From @c d(f_h)/dB = h^3*f0 / (2*sqrt(1 + B*h^2)): half a bin of
///          displacement at the top partial is @c bin_hz*sqrt(1 + B*h^2)/(h^3*f0).
///          It reproduces the bisected bands -- 1.05e-5 against a measured
///          1.03e-5 at C3 over 20 partials -- so a fixture the table does not
///          cover is judged by the same criterion rather than by a looser one.
double stretch_tolerance(double f0_hz, double inharmonicity, int n_partials) {
  const double hd = static_cast<double>(n_partials);
  return bin_hz() * std::sqrt(1.0 + inharmonicity * hd * hd) / (hd * hd * hd * f0_hz);
}

void add_tone(std::vector<float>& into, const StretchedTone& tone) {
  const double nyquist = 0.5 * kSampleRate;
  for (int h = 1; h <= tone.n_partials; ++h) {
    const double hz = partial_hz(tone, h);
    if (hz >= nyquist) break;
    const double level = tone.amplitude / static_cast<double>(h);
    double phase = tone.phase_step * static_cast<double>(h);
    const double step = sonare::constants::kTwoPiD * hz / kSampleRate;
    for (size_t i = 0; i < into.size(); ++i) {
      into[i] += static_cast<float>(level * std::sin(phase));
      phase += step;
    }
  }
}

size_t samples_of(float seconds) {
  return static_cast<size_t>(seconds * static_cast<float>(kSampleRate));
}

sonare::Spectrogram spectrogram_of_tones(const std::vector<StretchedTone>& tones, float seconds) {
  std::vector<float> samples(samples_of(seconds), 0.0f);
  for (const StretchedTone& tone : tones) add_tone(samples, tone);
  return sonare::Spectrogram::compute(sonare::Audio::from_vector(std::move(samples), kSampleRate),
                                      analysis_stft());
}

// --- Hand-built tracks and masks -------------------------------------------

sonare::editing::polyphony::F0Ridge steady_ridge(float f0_hz, int n_frames) {
  F0Ridge ridge;
  ridge.frame_start = 0;
  ridge.f0_hz.assign(static_cast<size_t>(n_frames), f0_hz);
  ridge.salience.assign(static_cast<size_t>(n_frames), 1.0f);
  ridge.onset_sample = 0;
  ridge.offset_sample = static_cast<int64_t>(n_frames) * kHopLength;
  ridge.median_hz = f0_hz;
  return ridge;
}

/// @brief One ridge per pitch, each spanning every frame of @p spec.
MultiF0Track track_of_pitches(const sonare::Spectrogram& spec, const std::vector<double>& pitches) {
  MultiF0Track track;
  track.n_frames = spec.n_frames();
  track.hop_length = spec.hop_length();
  track.sample_rate = spec.sample_rate();
  track.polyphony.assign(static_cast<size_t>(spec.n_frames()), static_cast<int>(pitches.size()));
  track.ridges.reserve(pitches.size());
  for (const double hz : pitches) {
    track.ridges.push_back(steady_ridge(static_cast<float>(hz), spec.n_frames()));
  }
  return track;
}

MultiF0Track track_of_tones(const sonare::Spectrogram& spec,
                            const std::vector<StretchedTone>& tones) {
  std::vector<double> pitches;
  pitches.reserve(tones.size());
  for (const StretchedTone& tone : tones) pitches.push_back(tone.f0_hz);
  return track_of_pitches(spec, pitches);
}

/// @brief Masks at the declared stretch, which is what the fit is handed.
NoteMaskSet masks_at(const sonare::Spectrogram& spec, const MultiF0Track& track,
                     float declared_b = NoteMaskConfig{}.inharmonicity,
                     int n_harmonics = NoteMaskConfig{}.n_harmonics) {
  NoteMaskConfig config;
  config.n_harmonics = n_harmonics;
  config.inharmonicity = declared_b;
  return build_note_masks(spec, track, config);
}

/// @brief Partials of each note that no other note's claim reaches.
/// @details Counted over the partials the fixture rendered, from the geometry the
///          fit is handed -- the declared stretch, where a claim placed with the
///          wrong stretch under-reports the contest rather than over-reporting it.
///          A fixture asserts this so a refusal caused by the count falling under
///          @c min_partials fails as a premise about the material rather than as an
///          unexplained refusal, and so a default that rises past what the fixture
///          supplies is visible.
std::vector<int> uncontested_partials(const sonare::Spectrogram& spec,
                                      const std::vector<StretchedTone>& tones, float declared_b,
                                      int n_harmonics) {
  NoteMaskConfig config;
  config.n_harmonics = n_harmonics;
  config.inharmonicity = declared_b;
  std::vector<std::vector<PartialClaim>> placed;
  placed.reserve(tones.size());
  for (const StretchedTone& tone : tones) {
    placed.push_back(partial_claims(spec, static_cast<float>(tone.f0_hz), config));
  }

  std::vector<int> alone(tones.size(), 0);
  for (size_t i = 0; i < tones.size(); ++i) {
    for (const PartialClaim& mine : placed[i]) {
      if (mine.harmonic > tones[i].n_partials) continue;
      bool contested = false;
      for (size_t j = 0; j < tones.size() && !contested; ++j) {
        if (j == i) continue;
        for (const PartialClaim& theirs : placed[j]) {
          if (mine.first_bin <= theirs.last_bin && theirs.first_bin <= mine.last_bin) {
            contested = true;
            break;
          }
        }
      }
      if (!contested) ++alone[i];
    }
  }
  return alone;
}

// --- Reading a result ------------------------------------------------------

/// @brief A refusal, stated as both halves of the contract.
/// @details The inequality against zero is not implied by the equality for a
///          reader: zero is a fitted result meaning the harmonic series, and a
///          build returning it here would send a caller to the ideal series where
///          it meant to fall back on the declared value. Asserting both is what
///          keeps that distinction from going untested.
void require_refused(float value) {
  INFO("returned " << value);
  REQUIRE(value == kRefused);
  REQUIRE(value != 0.0f);
  REQUIRE(value < 0.0f);
}

/// @brief A fit, with the stretch it found inside the tolerance its own geometry
///        allows.
void require_fitted_near(float value, double truth, double tolerance, const char* what) {
  INFO(what << ": returned " << value << " against a true " << truth << ", tolerance "
            << tolerance);
  REQUIRE(value != kRefused);
  REQUIRE(value >= 0.0f);
  REQUIRE(std::isfinite(value));
  const double error = std::abs(static_cast<double>(value) - truth);
  // The headroom the feasibility argument asks for, read in the unit the fit
  // returns: the band the masking allows over the error the fit made. Under one
  // means the fit cannot place a claim well enough to matter.
  INFO("error " << error << ", headroom " << (error > 0.0 ? tolerance / error : kInf));
  REQUIRE(error <= tolerance);
}

// --- The per-ridge stretch a set has to carry ------------------------------

/// @brief Whether @c NoteMaskSet carries the per-ridge stretch its claims were
///        placed with.
/// @details The cases below that need it are compiled either way and fail with
///          the contract they are asserting when it is absent, rather than failing
///          to compile -- a translation unit that does not build takes every other
///          case in the binary with it, including the ones that would say what
///          else is wrong.
template <typename T, typename = void>
struct carries_per_ridge_stretch : std::false_type {};
template <typename T>
struct carries_per_ridge_stretch<
    T, std::void_t<decltype(std::declval<T&>().inharmonicity = std::vector<float>{})>>
    : std::true_type {};

constexpr const char* kNoPerRidgeStretch =
    "NoteMaskSet carries no per-ridge inharmonicity vector, so a set cannot state "
    "the geometry its claims were placed with when that differs per note, and the "
    "stage that replays the geometry reads the wrong partial";

/// @brief Notes claiming each (bin, frame), in the spectrogram's own layout.
/// @details A template only so it is not emitted while the set it reads cannot
///          carry a per-ridge stretch; see @ref carries_per_ridge_stretch.
template <typename Set>
std::vector<int> claim_counts(const Set& masks) {
  std::vector<int> counts(static_cast<size_t>(masks.n_bins) * static_cast<size_t>(masks.n_frames),
                          0);
  for (const NoteMask& mask : masks.notes) {
    for (int f = 0; f < mask.n_frames; ++f) {
      const int frame = mask.frame_start + f;
      const int32_t from = mask.frame_offset[static_cast<size_t>(f)];
      const int32_t to = mask.frame_offset[static_cast<size_t>(f) + 1];
      for (int32_t k = from; k < to; ++k) {
        const size_t cell = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]) *
                                static_cast<size_t>(masks.n_frames) +
                            static_cast<size_t>(frame);
        ++counts[cell];
      }
    }
  }
  return counts;
}

/// @brief The gap the geometry predicts at one bin, replayed independently.
/// @details Per note, the partials whose claim covers @p bin at that note's own
///          declared stretch; then the closest pair across the two notes. The
///          refined f0 is the one the solver uses, taken from the same entry point
///          rather than from the track, so the only thing left to disagree about
///          is which partial the geometry names.
/// @returns The gap in radians per frame, or -1 where either note puts no single
///          unambiguous partial on the bin.
template <typename Set>
double predicted_gap_at(const sonare::Spectrogram& spec, const Set& masks,
                        const std::vector<float>& refined, const std::vector<float>& per_note_b,
                        int bin) {
  std::vector<std::vector<double>> standing(refined.size());
  for (size_t i = 0; i < refined.size(); ++i) {
    if (refined[i] <= 0.0f) return -1.0;
    NoteMaskConfig config;
    config.n_harmonics = masks.config.n_harmonics;
    config.inharmonicity = per_note_b[i];
    for (const PartialClaim& claim : partial_claims(spec, refined[i], config)) {
      if (bin >= claim.first_bin && bin <= claim.last_bin) {
        standing[i].push_back(static_cast<double>(claim.centre_hz));
      }
    }
  }
  // Only a bin where each note names exactly one partial is unambiguous about
  // which pair "the two closest predicted partials" means.
  for (const std::vector<double>& partials : standing) {
    if (partials.size() != 1) return -1.0;
  }
  double closest = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < standing.size(); ++i) {
    for (size_t j = i + 1; j < standing.size(); ++j) {
      closest = std::min(closest, std::abs(standing[i][0] - standing[j][0]));
    }
  }
  // Radians per frame the closest pair turns at this framing, which is the unit
  // the gate reads.
  return sonare::constants::kTwoPiD * closest * static_cast<double>(kHopLength) /
         static_cast<double>(kSampleRate);
}

/// @brief A two-note set whose claims were placed one stretch per note.
/// @details Assembled rather than built, because no entry point takes a stretch
///          per note: @ref build_note_masks takes one config and the spec adds the
///          per-ridge vector to @c NoteMaskSet without saying how a caller fills
///          it. The weights are each note's own 1 rather than an equal split on
///          the bins both reach, which the mask contract allows -- finite and
///          non-zero is the whole requirement -- and no assertion here reads a
///          weight.
template <typename Set>
Set assemble_per_note_set(const sonare::Spectrogram& spec, const NoteMask& low,
                          const NoteMask& high, int n_harmonics, float low_b, float high_b) {
  Set masks;
  masks.notes = {low, high};
  masks.notes[0].ridge_index = 0;
  masks.notes[1].ridge_index = 1;
  masks.n_bins = spec.n_bins();
  masks.n_frames = spec.n_frames();
  masks.hop_length = spec.hop_length();
  masks.sample_rate = spec.sample_rate();
  masks.config.n_harmonics = n_harmonics;
  // The scalar stays at the default, so anything reading it instead of the vector
  // replays the ideal series and is caught rather than agreeing by accident.
  masks.config.inharmonicity = NoteMaskConfig{}.inharmonicity;
  if constexpr (carries_per_ridge_stretch<Set>::value) {
    masks.inharmonicity = {low_b, high_b};
  } else {
    (void)low_b;
    (void)high_b;
  }
  return masks;
}

}  // namespace

// --- What the fit returns on material whose stretch is known ----------------

TEST_CASE("a fitted stretch lands inside the band its own masking allows",
          "[polyphony_inharmonicity]") {
  struct Fixture {
    const char* name;
    StretchedTone tone;
    // Negative takes the tolerance from the half-bin criterion instead of the
    // bisected table.
    double band_below;
    double band_above;
  };

  const std::vector<Fixture> fixtures = {
      // The two bisected rows of the measured table, at the pitches and partial
      // counts they were measured at.
      {"C3 over 20 partials, the fitted real C3 stretch",
       StretchedTone{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0},
       static_cast<double>(kLowB) - 1.0237e-4, 1.2273e-4 - static_cast<double>(kLowB)},
      {"D5 over 8 partials, the fitted real C5 stretch",
       StretchedTone{kHighF0, kHighB, kHighPartials, 0.18, 0.37, 0.0},
       static_cast<double>(kHighB) - 9.654e-4, 1.0628e-3 - static_cast<double>(kHighB)},
      // Two the table does not cover, judged by the half-bin criterion the table's
      // own rows reproduce.
      {"a mid register note at 1e-4", StretchedTone{300.0, 1e-4, 20, 0.18, 0.0, 0.0}, -1.0, -1.0},
      {"a mid register note at 1e-3", StretchedTone{300.0, 1e-3, 12, 0.18, 0.21, 0.0}, -1.0, -1.0},
  };

  // Each fixture is checked rather than required, so one of them refusing does not
  // hide what the other three returned. They are independent material.
  for (const Fixture& fixture : fixtures) {
    INFO(fixture.name);
    const sonare::Spectrogram spec = spectrogram_of_tones({fixture.tone}, 0.4f);
    REQUIRE(spec.n_frames() > 16);
    const MultiF0Track track = track_of_tones(spec, {fixture.tone});
    // At the declared stretch, which is 0: the claims this fit is handed do not
    // contain the partials it is looking for, and walking the harmonics upward is
    // how it is meant to find them anyway.
    const NoteMaskSet masks = masks_at(spec, track);

    // One note, so every partial it has stands uncontested and the count the fit
    // may use is the claim count; and its f0 refines. Both are premises rather
    // than results, and reading them here is what separates a refusal about the
    // stretch from a refusal about the two inputs the fit needs first.
    const std::vector<float> refined = refine_track_f0(spec, track, masks);
    REQUIRE(refined.size() == 1);
    INFO("refined to " << refined[0] << ", claims available "
                       << partial_claims(spec, static_cast<float>(fixture.tone.f0_hz)).size());
    CHECK(refined[0] > 0.0f);

    const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
    REQUIRE(fitted.size() == 1);

    const double derived =
        stretch_tolerance(fixture.tone.f0_hz, fixture.tone.inharmonicity, fixture.tone.n_partials);
    const double below = fixture.band_below >= 0.0 ? fixture.band_below : derived;
    const double above = fixture.band_above >= 0.0 ? fixture.band_above : derived;
    const double error = static_cast<double>(fitted[0]) - fixture.tone.inharmonicity;
    INFO("half-bin criterion " << derived << ", band -" << below << " / +" << above);
    INFO("fitted " << fitted[0] << " against a true " << fixture.tone.inharmonicity << ", error "
                   << error);
    CHECK(fitted[0] != kRefused);
    CHECK(fitted[0] >= 0.0f);
    CHECK(error >= -below);
    CHECK(error <= above);
  }
}

TEST_CASE("ideal harmonic material fits the harmonic series and is not refused",
          "[polyphony_inharmonicity]") {
  // Zero is the most ordinary stretch there is, and a build using it as the
  // refusal sentinel would be indistinguishable here from one that fitted it.
  //
  // It also carries the near side of the sign question. A fit of a truly harmonic
  // series lands either side of zero by chance, so what has to come back is zero
  // itself: not the refusal, and not a small negative either, since a caller
  // reading the sign would spend the fallback on a series it measured correctly.
  for (const double f0 : {kLowF0, 300.0}) {
    const StretchedTone tone{f0, 0.0, kLowPartials, 0.18, 0.0, 0.0};
    INFO("f0 " << f0);
    const sonare::Spectrogram spec = spectrogram_of_tones({tone}, 0.4f);
    const MultiF0Track track = track_of_tones(spec, {tone});
    const std::vector<float> fitted =
        estimate_track_inharmonicity(spec, track, masks_at(spec, track));
    REQUIRE(fitted.size() == 1);
    require_fitted_near(fitted[0], 0.0, stretch_tolerance(f0, 0.0, tone.n_partials),
                        "the ideal series");
  }
}

// --- The four refusals, each with a control that moves one thing -------------

TEST_CASE("too few usable partials is refused, and only the count gate moved",
          "[polyphony_inharmonicity]") {
  const StretchedTone tone{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({tone}, 0.4f);
  const MultiF0Track track = track_of_tones(spec, {tone});
  const NoteMaskSet masks = masks_at(spec, track, 0.0f, kLowPartials);

  // One note, so no partial of it is contested and the usable count is the claim
  // count. Nothing about the material changes across the three calls below.
  const double tolerance = stretch_tolerance(kLowF0, kLowB, kLowPartials);

  InharmonicityConfig strict;
  strict.min_partials = kLowPartials + 5;
  std::vector<float> refused;
  REQUIRE_NOTHROW(refused = estimate_track_inharmonicity(spec, track, masks, strict));
  REQUIRE(refused.size() == 1);
  require_refused(refused[0]);

  // The control: the same spectrogram, the same track, the same masks, and the
  // gate is the only thing that moved.
  const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
  REQUIRE(fitted.size() == 1);
  require_fitted_near(fitted[0], kLowB, tolerance, "the same input at the default gate");

  // The boundary, stepped to the exact integer either side rather than swept: the
  // count a fit needs is a count it may have.
  InharmonicityConfig at_edge;
  at_edge.min_partials = kLowPartials;
  const std::vector<float> at = estimate_track_inharmonicity(spec, track, masks, at_edge);
  REQUIRE(at.size() == 1);
  require_fitted_near(at[0], kLowB, tolerance, "min_partials at the count available");

  InharmonicityConfig past_edge;
  past_edge.min_partials = kLowPartials + 1;
  const std::vector<float> past = estimate_track_inharmonicity(spec, track, masks, past_edge);
  REQUIRE(past.size() == 1);
  require_refused(past[0]);
}

TEST_CASE("a misfit past the residual gate is refused, and the same material fits without it",
          "[polyphony_inharmonicity]") {
  // A partial displaced 1.5 bins from the series, alternating in sign so no
  // straight line absorbs it, and smaller than the claim half-width so it does
  // not on its own move a partial out of the window the fit searches.
  //
  // The material is the high note's stretch rather than a near-zero one, and that
  // is what keeps this case about the residual alone: a perturbation either side
  // of a series whose stretch is already near zero can carry the fitted value
  // negative, which is refused on its own grounds, and the two conditions would
  // then be indistinguishable here.
  const StretchedTone smooth{kHighF0, kHighB, kHighPartials, 0.18, 0.37, 0.0};
  const InharmonicityConfig defaults;

  SECTION("the material is the only thing that moves") {
    // The same series without the misfit, at the default gate, so the refusals
    // below are placed on the misfit rather than on anything the fixture has in
    // common with the fitted cases.
    const sonare::Spectrogram clean = spectrogram_of_tones({smooth}, 0.4f);
    const MultiF0Track clean_track = track_of_tones(clean, {smooth});
    const std::vector<float> fitted =
        estimate_track_inharmonicity(clean, clean_track, masks_at(clean, clean_track));
    REQUIRE(fitted.size() == 1);
    // The bisected band of this note at this partial count, taken at its wider
    // edge, so the arm says the material fits rather than how closely.
    require_fitted_near(fitted[0], kHighB, static_cast<double>(kHighB) - 9.654e-4,
                        "the same series without the misfit");
  }

  SECTION("the gate is the only thing that moves") {
    // Two magnitudes, one just past the gate and one three times it. A refusal
    // that survives a raised gate at both says the gate is not what refused;
    // one that survives only at the larger says the magnitude is.
    for (const double misfit_bins : {0.8, 1.5}) {
      INFO("misfit " << misfit_bins << " bins against a gate of " << defaults.max_residual_bins);
      REQUIRE(misfit_bins > static_cast<double>(defaults.max_residual_bins));
      REQUIRE(misfit_bins < kClaimHalfWidthBins);
      StretchedTone rough = smooth;
      rough.misfit_hz = misfit_bins * bin_hz();
      const sonare::Spectrogram spec = spectrogram_of_tones({rough}, 0.4f);
      const MultiF0Track track = track_of_tones(spec, {rough});
      const NoteMaskSet masks = masks_at(spec, track);

      const std::vector<float> refused = estimate_track_inharmonicity(spec, track, masks);
      REQUIRE(refused.size() == 1);
      INFO("at the default gate it returned " << refused[0]);
      CHECK(refused[0] == kRefused);
      CHECK(refused[0] != 0.0f);

      InharmonicityConfig loose;
      loose.max_residual_bins = 3.0f;
      std::vector<float> tolerated;
      REQUIRE_NOTHROW(tolerated = estimate_track_inharmonicity(spec, track, masks, loose));
      REQUIRE(tolerated.size() == 1);
      INFO("at a gate of " << loose.max_residual_bins << " it returned " << tolerated[0]);
      CHECK(tolerated[0] != kRefused);
      CHECK(tolerated[0] >= 0.0f);
    }
  }
}

TEST_CASE("a compressed series is refused well outside the fit's own resolution",
          "[polyphony_inharmonicity]") {
  // A compressed series: partials at h*f0*sqrt(1 - |B|*h^2), which no physical
  // string produces, so a fit reaching it has found something other than the
  // stretch.
  //
  // How far negative is the whole question here, and the refusal is only claimed
  // where the sign cannot be the fit's own noise. A series whose true stretch is
  // zero lands either side of zero by chance, so negative on its own is not a
  // refusal -- that side is the ideal-series case's subject and is asserted there
  // as a zero rather than as a small negative. The magnitude below is ten times
  // the resolution the geometry allows at this pitch and partial count, which is a
  // long way from the boundary on either reading of it: where the two behaviours
  // part company is a quantity the fit computes at run time, so no fixture here
  // sits near it and nothing asserts which side of it is which.
  const double magnitude = static_cast<double>(kLowB);
  const StretchedTone compressed{kLowF0, -magnitude, kLowPartials, 0.18, 0.0, 0.0};
  const StretchedTone stretched{kLowF0, magnitude, kLowPartials, 0.18, 0.0, 0.0};
  const double tolerance = stretch_tolerance(kLowF0, magnitude, kLowPartials);
  INFO("magnitude " << magnitude << " against a resolution of " << tolerance);
  REQUIRE(magnitude > 10.0 * tolerance);

  // The displacement the fit has to walk to find these partials, reported rather
  // than bounded: the claims are placed at the declared 0 and these partials are
  // nowhere near them, which is the ordinary case the bootstrap exists for and is
  // mirrored exactly by the control below.
  int at_harmonic = 0;
  INFO("worst claim offset " << worst_claim_offset_bins(compressed, 0.0, at_harmonic)
                             << " bins at partial " << at_harmonic);

  SECTION("the compressed series is refused") {
    const sonare::Spectrogram spec = spectrogram_of_tones({compressed}, 0.4f);
    const MultiF0Track track = track_of_tones(spec, {compressed});
    const std::vector<float> refused =
        estimate_track_inharmonicity(spec, track, masks_at(spec, track, 0.0f, kLowPartials));
    REQUIRE(refused.size() == 1);
    require_refused(refused[0]);
  }

  SECTION("the same magnitude stretched instead is fitted") {
    // The sign is the only thing that differs: same pitch, same partial count,
    // same levels, same claim offsets to within it. This is also the fixture the
    // band case fits independently, so a refusal above cannot be the material
    // being unreachable.
    const sonare::Spectrogram spec = spectrogram_of_tones({stretched}, 0.4f);
    const MultiF0Track track = track_of_tones(spec, {stretched});
    const std::vector<float> fitted =
        estimate_track_inharmonicity(spec, track, masks_at(spec, track, 0.0f, kLowPartials));
    REQUIRE(fitted.size() == 1);
    require_fitted_near(fitted[0], magnitude, tolerance, "the stretched mirror");
  }
}

TEST_CASE("a fitted stretch over the cap is refused, and the cap is where it flips",
          "[polyphony_inharmonicity]") {
  const StretchedTone tone{kHighF0, kHighB, kHighPartials, 0.18, 0.37, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({tone}, 0.4f);
  const MultiF0Track track = track_of_tones(spec, {tone});
  const NoteMaskSet masks = masks_at(spec, track);
  const double tolerance = stretch_tolerance(kHighF0, kHighB, kHighPartials);

  // The cap is the only thing that moves. The material is one the default cap
  // fits, so a refusal under a lower cap cannot be the material's doing.
  const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
  REQUIRE(fitted.size() == 1);
  require_fitted_near(fitted[0], kHighB, tolerance, "the default cap");

  InharmonicityConfig capped;
  capped.max_inharmonicity = static_cast<float>(kHighB) * 0.5f;
  const std::vector<float> refused = estimate_track_inharmonicity(spec, track, masks, capped);
  REQUIRE(refused.size() == 1);
  require_refused(refused[0]);

  // Where it flips, bisected rather than swept: a grid step coarser than the
  // distance being measured reports the interval as empty, which has happened on
  // this material. Twenty halvings of [0, the default cap] resolve the crossing
  // far finer than the band the fit's own accuracy allows, and the crossing must
  // be the fitted value -- a cap compared against anything else lands elsewhere.
  //
  // This locates the crossing and says nothing about which side of equality is
  // refused. The two differ by one bit of a float and the contract does not state
  // which it is, so the tolerance below is the fit's own band -- five orders wider
  // than the bisection's granularity, and wider still than that bit.
  double low = 0.0;
  double high = static_cast<double>(InharmonicityConfig{}.max_inharmonicity);
  for (int step = 0; step < 20; ++step) {
    const double mid = 0.5 * (low + high);
    InharmonicityConfig probe;
    probe.max_inharmonicity = static_cast<float>(mid);
    const std::vector<float> at = estimate_track_inharmonicity(spec, track, masks, probe);
    REQUIRE(at.size() == 1);
    if (at[0] == kRefused) {
      low = mid;
    } else {
      high = mid;
    }
  }
  INFO("the cap flips between " << low << " and " << high << ", fitted " << fitted[0]);
  REQUIRE(std::abs(0.5 * (low + high) - static_cast<double>(fitted[0])) <= tolerance);
}

TEST_CASE("a ridge whose f0 cannot be refined is refused, with a refinable one as the control",
          "[polyphony_inharmonicity]") {
  // Unwrapping a partial's rate to a frequency bounds how high a usable partial
  // may be, and at this framing and the default f0 tolerance that ceiling is
  // 1470 Hz. A single note above it has every partial above it, so it has no
  // refinable f0 while every partial it has stands uncontested -- which is what
  // keeps this case off the count gate.
  const StretchedTone above{1600.0, 0.0, kLowPartials, 0.18, 0.0, 0.0};
  const StretchedTone below{1400.0, 0.0, kLowPartials, 0.18, 0.0, 0.0};
  REQUIRE(above.f0_hz > kRefineCeilingHz);
  REQUIRE(below.f0_hz < kRefineCeilingHz);

  const sonare::Spectrogram above_spec = spectrogram_of_tones({above}, 0.4f);
  const MultiF0Track above_track = track_of_tones(above_spec, {above});
  const NoteMaskSet above_masks = masks_at(above_spec, above_track);

  // The premise, asserted through the entry point that exposes it rather than
  // left to the ceiling's arithmetic: this ridge's f0 really is unrefinable.
  const std::vector<float> above_refined = refine_track_f0(above_spec, above_track, above_masks);
  REQUIRE(above_refined.size() == 1);
  REQUIRE(above_refined[0] == 0.0f);

  const std::vector<float> refused =
      estimate_track_inharmonicity(above_spec, above_track, above_masks);
  REQUIRE(refused.size() == 1);
  require_refused(refused[0]);

  // The control, through the same machinery two hundred hertz lower: the f0
  // refines and the fit returns a value. Without it a build refusing every ridge
  // would satisfy the assertion above.
  const sonare::Spectrogram below_spec = spectrogram_of_tones({below}, 0.4f);
  const MultiF0Track below_track = track_of_tones(below_spec, {below});
  const NoteMaskSet below_masks = masks_at(below_spec, below_track);
  const std::vector<float> below_refined = refine_track_f0(below_spec, below_track, below_masks);
  REQUIRE(below_refined.size() == 1);
  INFO("the control refined to " << below_refined[0]);
  REQUIRE(below_refined[0] > 0.0f);

  const std::vector<float> fitted =
      estimate_track_inharmonicity(below_spec, below_track, below_masks);
  REQUIRE(fitted.size() == 1);
  REQUIRE(fitted[0] != kRefused);
  REQUIRE(fitted[0] >= 0.0f);
}

// --- The sentinel itself ----------------------------------------------------

TEST_CASE("a refusal is negative and a fit of the harmonic series is zero",
          "[polyphony_inharmonicity]") {
  // The distinction the entry point exists to make, with both sides present in
  // one case so neither can drift from the other. A fitted harmonic series and a
  // refusal have to come back as different values, and which is which is fixed.
  const StretchedTone ideal{kLowF0, 0.0, kLowPartials, 0.18, 0.0, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({ideal}, 0.4f);
  const MultiF0Track track = track_of_tones(spec, {ideal});
  const NoteMaskSet masks = masks_at(spec, track);

  const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
  REQUIRE(fitted.size() == 1);
  REQUIRE(fitted[0] >= 0.0f);

  InharmonicityConfig strict;
  strict.min_partials = kLowPartials + 5;
  const std::vector<float> refused = estimate_track_inharmonicity(spec, track, masks, strict);
  REQUIRE(refused.size() == 1);
  REQUIRE(refused[0] == kRefused);
  REQUIRE(refused[0] != fitted[0]);

  // And the negative sentinel cannot be spent by accident downstream, because the
  // value range it would be handed to rejects it. That is the argument for using a
  // negative rather than zero, checked rather than cited.
  NoteMaskConfig negative;
  negative.inharmonicity = kRefused;
  REQUIRE(code_of([&] { partial_claims(spec, static_cast<float>(kLowF0), negative); }) ==
          sonare::ErrorCode::InvalidParameter);
  REQUIRE(code_of([&] { build_note_masks(spec, track, negative); }) ==
          sonare::ErrorCode::InvalidParameter);
}

TEST_CASE("one value per ridge in ridge order, whatever happened to each",
          "[polyphony_inharmonicity]") {
  // A pair whose lower note fits and whose upper note cannot be refined, so the
  // result has to carry both outcomes in the order the ridges came in. A build
  // returning only the values it managed, or dropping a refused ridge, has the
  // wrong length; one returning them in any other order puts the refusal on the
  // note that fitted.
  const StretchedTone lower{300.0, 1e-4, kLowPartials, 0.18, 0.0, 0.0};
  const StretchedTone upper{1600.0, 0.0, kLowPartials, 0.18, 0.41, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({lower, upper}, 0.4f);
  const MultiF0Track track = track_of_tones(spec, {lower, upper});
  const NoteMaskSet masks = masks_at(spec, track);

  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  INFO("refined to " << refined[0] << " and " << refined[1]);
  REQUIRE(refined[0] > 0.0f);
  REQUIRE(refined[1] == 0.0f);

  const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
  REQUIRE(fitted.size() == track.ridges.size());
  require_fitted_near(fitted[0], lower.inharmonicity,
                      stretch_tolerance(lower.f0_hz, lower.inharmonicity, lower.n_partials),
                      "the lower ridge");
  require_refused(fitted[1]);
}

// --- The declared mode stays the default ------------------------------------

TEST_CASE("the default places claims at the declared stretch and does not read the signal",
          "[polyphony_inharmonicity]") {
  // The control on the existing path. Estimation becoming the default would drop
  // the invariant that claim geometry is independent of the signal, and a host
  // reads that invariant from a contract it was never told had changed.
  const NoteMaskConfig defaults;
  REQUIRE(defaults.inharmonicity == 0.0f);
  REQUIRE(defaults.n_harmonics == 20);
  REQUIRE(defaults.claim_lobes == 1.0f);

  const StretchedTone stretched{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0};
  StretchedTone ideal = stretched;
  ideal.inharmonicity = 0.0;
  const sonare::Spectrogram stretched_spec = spectrogram_of_tones({stretched}, 0.4f);
  const sonare::Spectrogram ideal_spec = spectrogram_of_tones({ideal}, 0.4f);
  const MultiF0Track track = track_of_tones(stretched_spec, {stretched});

  // Over material that is genuinely stretched, the default claim sits at h*f0 --
  // the ideal series -- rather than where the partial is. The two differ by 5.6
  // bins at the top partial here, so this is not a rounding question.
  for (const PartialClaim& claim : partial_claims(stretched_spec, static_cast<float>(kLowF0))) {
    const double ideal_hz = static_cast<double>(claim.harmonic) * kLowF0;
    INFO("partial " << claim.harmonic << " claimed at " << claim.centre_hz);
    REQUIRE(std::abs(static_cast<double>(claim.centre_hz) - ideal_hz) <= 1e-4 * ideal_hz);
  }
  int at_harmonic = 0;
  const double offset = worst_claim_offset_bins(stretched, 0.0, at_harmonic);
  INFO("the stretch the default ignores is " << offset << " bins at partial " << at_harmonic);
  REQUIRE(offset > 1.0);

  // Two different signals at one framing, one track, one config: bit-identical
  // masks. A geometry read from the data cannot satisfy this.
  const NoteMaskSet from_stretched = build_note_masks(stretched_spec, track);
  const NoteMaskSet from_ideal = build_note_masks(ideal_spec, track);
  REQUIRE(from_ideal.notes.size() == from_stretched.notes.size());
  REQUIRE(from_ideal.config.inharmonicity == from_stretched.config.inharmonicity);
  for (size_t i = 0; i < from_stretched.notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(from_ideal.notes[i].frame_offset == from_stretched.notes[i].frame_offset);
    REQUIRE(from_ideal.notes[i].bins == from_stretched.notes[i].bins);
    REQUIRE(from_ideal.notes[i].weights == from_stretched.notes[i].weights);
  }

  // And running the fit changes nothing about the path that does not ask for it.
  const std::vector<float> fitted =
      estimate_track_inharmonicity(stretched_spec, track, from_stretched);
  REQUIRE(fitted.size() == 1);
  const NoteMaskSet after = build_note_masks(stretched_spec, track);
  REQUIRE(after.notes.size() == from_stretched.notes.size());
  REQUIRE(after.config.inharmonicity == from_stretched.config.inharmonicity);
  REQUIRE(after.config.n_harmonics == from_stretched.config.n_harmonics);
  REQUIRE(after.config.claim_lobes == from_stretched.config.claim_lobes);
  for (size_t i = 0; i < after.notes.size(); ++i) {
    INFO("note " << i);
    REQUIRE(after.notes[i].bins == from_stretched.notes[i].bins);
    REQUIRE(after.notes[i].weights == from_stretched.notes[i].weights);
  }
}

// --- Why the entry point exists ---------------------------------------------

TEST_CASE("a stretch per note reaches both notes of a two-octave pair where no scalar does",
          "[polyphony_inharmonicity]") {
  // The measurement's own fixture: a real C3's stretch over 20 partials against a
  // real C5's over 8, two octaves and a tone apart so every measured partial
  // stays resolvable. An exact octave cannot be measured at all here -- the two
  // notes' partials land within 0.2 Hz of each other.
  const StretchedTone low{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0};
  const StretchedTone high{kHighF0, kHighB, kHighPartials, 0.18, 0.37, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({low, high}, 0.5f);
  const MultiF0Track track = track_of_tones(spec, {low, high});
  const NoteMaskSet masks = masks_at(spec, track);

  // The premise, so a refusal here reads as a refusal about the stretch rather
  // than about the pair: both ridges keep a partial they can be refined from. The
  // high note's fundamental sits between the low note's fourth and fifth partials
  // with 5.9 bins of clearance against the 5.0 the test demands at the default f0
  // tolerance, which is the tightest thing about this fixture.
  const std::vector<float> refined = refine_track_f0(spec, track, masks);
  REQUIRE(refined.size() == 2);
  INFO("refined to " << refined[0] << " and " << refined[1]);
  REQUIRE(refined[0] > 0.0f);
  REQUIRE(refined[1] > 0.0f);

  // The other premise: each note still supplies the partials a fit needs after the
  // contest between them. The high note is the tight one -- it renders eight and
  // loses two to the low note's ninth and eighteenth -- so this reads the default
  // rather than a number, and a default that rises past six fails here instead of
  // surfacing as a refusal with no stated cause.
  const std::vector<int> alone =
      uncontested_partials(spec, {low, high}, masks.config.inharmonicity, masks.config.n_harmonics);
  REQUIRE(alone.size() == 2);
  INFO("uncontested partials " << alone[0] << " and " << alone[1] << " against a minimum of "
                               << InharmonicityConfig{}.min_partials);
  REQUIRE(alone[0] >= InharmonicityConfig{}.min_partials);
  REQUIRE(alone[1] >= InharmonicityConfig{}.min_partials);

  // The control first, because it is a property of the material and holds whatever
  // the fit returns: every single value, including each note's own truth and the
  // geometric mean between them, leaves one partial of one note far outside its
  // claim. The worst partial decides, not the median -- one partial left behind is
  // the whole defect and a median hides it.
  double best_scalar = std::numeric_limits<double>::infinity();
  for (const float scalar : {kLowB, kHighB, kGeometricMeanB, 0.0f}) {
    int low_at = 0;
    int high_at = 0;
    const double low_offset = worst_claim_offset_bins(low, scalar, low_at);
    const double high_offset = worst_claim_offset_bins(high, scalar, high_at);
    const double worst = std::max(low_offset, high_offset);
    INFO("the single value " << scalar << " leaves " << low_offset << " bins at the low note's "
                             << low_at << " and " << high_offset << " bins at the high note's "
                             << high_at);
    REQUIRE(worst > 2.0);
    best_scalar = std::min(best_scalar, worst);
  }
  INFO("the best single value still leaves " << best_scalar << " bins");

  const std::vector<float> fitted = estimate_track_inharmonicity(spec, track, masks);
  REQUIRE(fitted.size() == 2);
  INFO("fitted " << fitted[0] << " and " << fitted[1]);
  CHECK(fitted[0] != kRefused);
  CHECK(fitted[1] != kRefused);

  // Each note inside its own bisected band, which is the thing no one value can
  // do: the two bands are 7.87x apart.
  CHECK(static_cast<double>(fitted[0]) >= 1.0237e-4);
  CHECK(static_cast<double>(fitted[0]) <= 1.2273e-4);
  CHECK(static_cast<double>(fitted[1]) >= 9.654e-4);
  CHECK(static_cast<double>(fitted[1]) <= 1.0628e-3);

  // What that buys, in the unit the 6 dB criterion was found constant in: the worst
  // partial of either note sits well inside the claim it is placed in, and an order
  // of magnitude closer than the best single value manages. Read per partial
  // against that note's own geometry, so a neighbour's apportionment -- which a
  // per-note stretch does not fix and which moved one partial by 77 dB between two
  // near-identical fixtures -- cannot enter the number.
  //
  // Neither bound is the measured band edge. That edge is a sub-bin effect and it
  // came out 0.42 to 0.99 bins across every row measured, so a bound of one bin
  // would sit within a percent of a quantity the fit's own accuracy moves --
  // deciding the case on the last bit of a displacement rather than on whether a
  // per-note stretch reached both notes. The nominal claim half-width and a factor
  // of four are both far from anything measured.
  REQUIRE(fitted[0] >= 0.0f);
  REQUIRE(fitted[1] >= 0.0f);
  int at_low = 0;
  int at_high = 0;
  const double per_note = std::max(worst_claim_offset_bins(low, fitted[0], at_low),
                                   worst_claim_offset_bins(high, fitted[1], at_high));
  INFO("per-note worst claim offset " << per_note << " bins, at partials " << at_low << " and "
                                      << at_high);
  REQUIRE(per_note <= kClaimHalfWidthBins);
  REQUIRE(per_note * 4.0 < best_scalar);
}

// --- The set has to carry the geometry it was built with ---------------------

namespace {

template <typename Set>
void require_empty_stretch_reads_the_scalar(const sonare::Spectrogram& spec,
                                            const MultiF0Track& track, const Set& masks) {
  if constexpr (!carries_per_ridge_stretch<Set>::value) {
    FAIL(kNoPerRidgeStretch);
  } else {
    // Empty means every note used the scalar, so a set built before the vector
    // existed and a hand-built one carrying the default both still read
    // correctly. Stated as an identity against the vector spelled out, which is
    // the only way to tell "empty is the scalar" from "empty is zero".
    REQUIRE(masks.inharmonicity.empty());
    Set spelled_out = masks;
    spelled_out.inharmonicity.assign(masks.notes.size(), masks.config.inharmonicity);

    SharedBinReport from_empty;
    SharedBinReport from_spelled;
    const Set a = solve_shared_bins(spec, masks, track, SharedBinConfig{}, &from_empty);
    const Set b = solve_shared_bins(spec, spelled_out, track, SharedBinConfig{}, &from_spelled);
    REQUIRE(a.notes.size() == b.notes.size());
    for (size_t i = 0; i < a.notes.size(); ++i) {
      INFO("note " << i);
      REQUIRE(a.notes[i].bins == b.notes[i].bins);
      REQUIRE(a.notes[i].weights == b.notes[i].weights);
    }
    REQUIRE(from_empty.outcome == from_spelled.outcome);
    REQUIRE(from_empty.partial_separation == from_spelled.partial_separation);

    // A length that disagrees with the note count describes a different set, and
    // reading it would name the wrong partial on every bin.
    Set too_long = masks;
    too_long.inharmonicity.assign(masks.notes.size() + 1, masks.config.inharmonicity);
    REQUIRE(code_of([&] { solve_shared_bins(spec, too_long, track); }) ==
            sonare::ErrorCode::InvalidParameter);
    REQUIRE(code_of([&] { estimate_track_inharmonicity(spec, track, too_long); }) ==
            sonare::ErrorCode::InvalidParameter);

    Set too_short = masks;
    if (masks.notes.size() > 1) {
      too_short.inharmonicity.assign(masks.notes.size() - 1, masks.config.inharmonicity);
      REQUIRE(code_of([&] { solve_shared_bins(spec, too_short, track); }) ==
              sonare::ErrorCode::InvalidParameter);
    }
  }
}

}  // namespace

TEST_CASE("an empty per-note stretch reads as the declared value and a wrong length is rejected",
          "[polyphony_inharmonicity]") {
  const StretchedTone low{300.0, 0.0, kHighPartials, 0.18, 0.0, 0.0};
  const StretchedTone high{451.5, 0.0, kHighPartials, 0.15, 0.7, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({low, high}, 0.5f);
  const MultiF0Track track = track_of_tones(spec, {low, high});
  require_empty_stretch_reads_the_scalar(spec, track, masks_at(spec, track, 0.0f, kHighPartials));
}

namespace {

/// @brief Asserts the replay names the partial the per-note geometry put there.
template <typename Set>
void require_per_note_replay(const sonare::Spectrogram& spec, const MultiF0Track& track,
                             const NoteMask& low, const NoteMask& high, int n_harmonics,
                             float low_b, float high_b) {
  if constexpr (!carries_per_ridge_stretch<Set>::value) {
    (void)spec;
    (void)track;
    (void)low;
    (void)high;
    (void)n_harmonics;
    (void)low_b;
    (void)high_b;
    FAIL(kNoPerRidgeStretch);
  } else {
    const Set masks = assemble_per_note_set<Set>(spec, low, high, n_harmonics, low_b, high_b);
    // The fixture's track carries the exact f0, and the default 50 cents widens
    // the distance a rival partial must clear by enough to disqualify the high
    // note's fundamental -- its only partial under the refinement ceiling that
    // stands alone. Declaring the error the track has is what the field is for.
    SharedBinConfig config;
    config.f0_tolerance_cents = 10.0f;
    const std::vector<float> refined = refine_track_f0(spec, track, masks, config);
    REQUIRE(refined.size() == 2);
    INFO("refined to " << refined[0] << " and " << refined[1]);
    REQUIRE(refined[0] > 0.0f);
    REQUIRE(refined[1] > 0.0f);

    SharedBinReport report;
    REQUIRE_NOTHROW(solve_shared_bins(spec, masks, track, config, &report));

    const std::vector<int> counts = claim_counts(masks);
    const std::vector<float> per_note = {low_b, high_b};
    const std::vector<float> scalar_only = {masks.config.inharmonicity, masks.config.inharmonicity};

    size_t checked = 0;
    size_t discriminating = 0;
    double worst = 0.0;
    int worst_bin = 0;
    for (int bin = 0; bin < masks.n_bins; ++bin) {
      // One frame is enough: the separation is a span scalar, so every frame of a
      // span carries the same value.
      const size_t cell = static_cast<size_t>(bin) * static_cast<size_t>(masks.n_frames);
      if (counts[cell] != 2) continue;
      const double want = predicted_gap_at(spec, masks, refined, per_note, bin);
      if (want < 0.0) continue;
      const double as_scalar = predicted_gap_at(spec, masks, refined, scalar_only, bin);
      ++checked;
      const double error = std::abs(static_cast<double>(report.partial_separation[cell]) - want);
      if (error > worst) {
        worst = error;
        worst_bin = bin;
      }
      // A bin where the two geometries predict different gaps is the only kind
      // that can tell a replay of the per-note stretch from a replay of the
      // scalar; without one the case would pass on either.
      if (as_scalar >= 0.0 && std::abs(as_scalar - want) > 1e-2 * std::max(1.0, want)) {
        ++discriminating;
        INFO("bin " << bin << ": per-note geometry predicts " << want << " rad/frame, the scalar "
                    << as_scalar << ", reported " << report.partial_separation[cell]);
        REQUIRE(std::abs(static_cast<double>(report.partial_separation[cell]) - as_scalar) >
                std::abs(static_cast<double>(report.partial_separation[cell]) - want));
      }
    }
    INFO("bins checked " << checked << ", of which discriminating " << discriminating
                         << "; worst gap error " << worst << " rad/frame at bin " << worst_bin);
    REQUIRE(checked > 0);
    REQUIRE(discriminating > 0);
    REQUIRE(worst <= 1e-3);
  }
}

}  // namespace

TEST_CASE("shared bins replay the per-note geometry the set was built with",
          "[polyphony_inharmonicity]") {
  // The one place the stretch becoming per-note can break something that was
  // working: the stage that has to know which partial of which note stands on a
  // bin replays the set's geometry, and a replay reading one scalar where the
  // claims were placed with two names the wrong partial. At these two pitches the
  // lower note's ninth partial and the upper note's second stand 5.6 Hz apart
  // under the real stretches and 2.7 Hz apart under the ideal series, so the
  // predicted gap the gate reads differs by a factor of two between the two
  // readings.
  const StretchedTone low{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0};
  const StretchedTone high{kHighF0, kHighB, kLowPartials, 0.18, 0.37, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({low, high}, 0.5f);
  const MultiF0Track track = track_of_tones(spec, {low, high});

  // Each note's own claims, built one note at a time because that is the only way
  // to place two geometries with the entry point available.
  const MultiF0Track low_only = track_of_tones(spec, {low});
  const MultiF0Track high_only = track_of_tones(spec, {high});
  const NoteMaskSet low_masks = masks_at(spec, low_only, kLowB, kLowPartials);
  const NoteMaskSet high_masks = masks_at(spec, high_only, kHighB, kLowPartials);
  REQUIRE(low_masks.notes.size() == 1);
  REQUIRE(high_masks.notes.size() == 1);

  require_per_note_replay<NoteMaskSet>(spec, track, low_masks.notes[0], high_masks.notes[0],
                                       kLowPartials, kLowB, kHighB);
}

// --- What the entry point rejects -------------------------------------------

TEST_CASE("the fit rejects a config outside its range and inputs that disagree",
          "[polyphony_inharmonicity]") {
  const StretchedTone tone{kLowF0, kLowB, kLowPartials, 0.18, 0.0, 0.0};
  const sonare::Spectrogram spec = spectrogram_of_tones({tone}, 0.3f);
  const MultiF0Track track = track_of_tones(spec, {tone});
  const NoteMaskSet masks = masks_at(spec, track);

  const InharmonicityConfig defaults;
  REQUIRE(defaults.min_partials > 0);
  REQUIRE(defaults.max_residual_bins > 0.0f);
  REQUIRE(defaults.max_inharmonicity > 0.0f);

  std::vector<std::pair<std::string, InharmonicityConfig>> bad;
  for (const int value : {0, -1}) {
    InharmonicityConfig config;
    config.min_partials = value;
    bad.emplace_back("min_partials " + std::to_string(value), config);
  }
  // Each range below is one-sided, and one comparison against a one-sided range
  // admits both NaN and infinity, so both are listed rather than left to it.
  const auto with_residual = [&bad](float value, const char* label) {
    InharmonicityConfig config;
    config.max_residual_bins = value;
    bad.emplace_back(std::string("max_residual_bins ") + label, config);
  };
  with_residual(0.0f, "0");
  with_residual(-0.5f, "negative");
  with_residual(kNaN, "NaN");
  with_residual(kInf, "inf");

  const auto with_cap = [&bad](float value, const char* label) {
    InharmonicityConfig config;
    config.max_inharmonicity = value;
    bad.emplace_back(std::string("max_inharmonicity ") + label, config);
  };
  with_cap(-1e-3f, "negative");
  with_cap(kNaN, "NaN");
  with_cap(kInf, "inf");

  for (const auto& entry : bad) {
    INFO(entry.first);
    REQUIRE(code_of([&] { estimate_track_inharmonicity(spec, track, masks, entry.second); }) ==
            sonare::ErrorCode::InvalidParameter);
  }

  // The same disagreements between spec, track and masks the shared-bin solver
  // rejects, since the entry point copies its shape.
  const MultiF0Track pair = track_of_pitches(spec, {kLowF0, kHighF0});
  REQUIRE(code_of([&] { estimate_track_inharmonicity(spec, pair, masks); }) ==
          sonare::ErrorCode::InvalidParameter);

  NoteMaskSet wrong_framing = masks;
  wrong_framing.hop_length = masks.hop_length * 2;
  REQUIRE(code_of([&] { estimate_track_inharmonicity(spec, track, wrong_framing); }) ==
          sonare::ErrorCode::InvalidParameter);

  NoteMaskSet wrong_shape = masks;
  wrong_shape.n_bins = masks.n_bins + 1;
  REQUIRE(code_of([&] { estimate_track_inharmonicity(spec, track, wrong_shape); }) ==
          sonare::ErrorCode::InvalidParameter);
}
