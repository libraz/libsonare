#pragma once

/// @file
/// Fits each note's partial stretch from the spectrum, so a chord spanning more
/// than an octave can carry one stretch per note instead of one for the set.
///
/// @c NoteMaskConfig::inharmonicity is a single value and a real instrument's
/// stretch is not: on a sampled piano it runs 1.13e-4 at C3 to 1.04e-3 at C5,
/// while the declared range that holds a note within 6 dB of its own best is
/// 1.2x at C3 and 1.008x at D5. Those ranges do not overlap, so one value has no
/// compromise setting for a wider span rather than a worse one.
///
/// This never places a claim. @ref partial_claims stays independent of the
/// signal, because the stage that has to know which partial stands on a bin
/// replays that geometry and a geometry read from the data could replay
/// differently. The fitted stretch is handed back to the caller, which passes it
/// to @ref build_note_masks explicitly.
///
/// **What it reaches**, measured on real piano references at the default polyphony
/// framing -- @c n_fft 4096 at 48 kHz, so one window main lobe is 46.9 Hz. It is
/// opt-in and a refusal falls back on the declared value, so it improves where it
/// reaches and is neutral where it does not:
///
/// - An isolated note from about C2 to C4 fits, 20 partials at a misfit of 0.07 to
///   0.24 of a bin.
/// - A0 is refused for too few partials: under about 55 Hz the claim half-width
///   leaves no search window inside the harmonic spacing.
/// - C6 is refused on the misfit, 0.95 to 1.97 bins over 7 partials. C8 is refused
///   for an unrefinable f0 -- every partial of it is above @ref refine_track_f0's
///   alias ceiling, which is 1470 Hz at that function's default tolerance.
/// - **A real chord does not fit at this framing.** C3+E3+G3 refuses all three
///   notes where each of the same tones alone fits. Being clear of a rival is not
///   the same as entering the fit, and the gap is wide: claim geometry leaves each
///   note five to eleven partials no rival stands on, while the walk locates them
///   from the bottom upward and narrows each window on the last reading, so one
///   contaminated reading costs every partial above it.
/// - Resolution is the lever, and it is not a complete one. At @c n_fft 16384 the
///   same chord admits 15 and 17 partials for C3 and E3 and E3 fits at 1.63e-4,
///   while G3 still admits none: a fifth's partials coincide with every third one of
///   the note below exactly, which no framing separates.
/// - Eight notes under a held pedal are refused at every framing for an unrefinable
///   f0, which is @c SharedBinConfig::f0_tolerance_cents' own documented price.
///
/// So the reach at the default framing is isolated notes in the middle register.
/// Raising @c n_fft is the caller's decision and not this file's: the default is the
/// whole polyphony pipeline's, and moving it would move detection, masking and
/// division with it.

#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"

namespace sonare::editing::polyphony {

/// @brief When a fitted stretch may be believed and when it is refused.
struct InharmonicityConfig {
  /// Usable partials a fit needs before its result is believed. Three, because at
  /// two the line is exact and the misfit below reads zero whatever the partials
  /// did, so nothing judges the fit; from three up every count measured lands
  /// inside its own band, which widens as @c 1/h^3 faster than the fit degrades.
  int min_partials = 3;

  /// Largest per-partial misfit, in bins of @p spec's framing, that still counts as
  /// a fit; equal to it is accepted. **The claim's own 6 dB point decides the
  /// value** -- half a bin is where a flat claim over a four-bin main lobe starts
  /// leaving 6 dB behind -- and the measured misfit says that choice is admissible
  /// rather than setting it: a correct fit leaves 0.02 of a bin alone and 0.25 at
  /// worst beside another note, while a reading contaminated by a neighbour's
  /// partial left 3.51, so anything in (0.25, 3.51) separates the two and 0.5 is
  /// the tighter of the two bounds.
  float max_residual_bins = 0.5f;

  /// Refuse rather than return a stretch above this; equal to it is accepted. A
  /// wider ceiling opens the first search windows onto a neighbouring partial, so
  /// 0.1 loses C2 outright and 0.5 every pitch measured, while published values
  /// reach 1e-2 at the top of a piano -- this is three times that and a power of
  /// two, so a stretch exactly on it is decided by the rule and not by whether the
  /// float happened to round the ceiling up.
  float max_inharmonicity = 0.03125f;
};

/// @brief Fits one partial stretch per ridge of @p track.
/// @details The fit walks the harmonics upward. A partial's displacement grows as
///          the cube of its harmonic number, so the low ones sit inside a window
///          built on any plausible stretch and locate reliably, and each fitted
///          value narrows the window the next one is searched in. A harmonic whose
///          window has widened onto a position a neighbouring partial of the same
///          note may stand at ends the walk, since the windows only widen from
///          there. Both sides are checked: the one above looks binding, but on a
///          low note it is the partial below that the widest window reaches first.
///
///          A partial is read as the magnitude peak inside its window, over the
///          mean magnitude of the frames the ridge spans, interpolated
///          quadratically over log magnitude -- which is what takes the reading
///          below the half bin a flat claim needs, and what makes the walk
///          possible at all: the accuracy needed and the accuracy reached both
///          scale as @c h^3*f0, so their ratio is pitch-independent and measures
///          0.02 to 0.07 of a bin against the half bin available.
///
///          The stretch is then the slope over the intercept of a line through
///          @c (h^2, (f_h/h)^2), which is @c f0^2 + f0^2*B*h^2 -- so the f0 is
///          fitted with it rather than taken from the track.
///
///          Only partials no other note stands on enter the fit, and the test is on
///          the reading rather than on the window: refusing every window a rival
///          reaches into refuses the whole upper note of a two-octave dyad, whose
///          windows are all wide until its stretch is pinned, while refusing the
///          reading drops only the partials that really coincide -- including the
///          ones where a rival is the louder of the two, since the window then
///          yields that rival's position.
///
///          Where the other notes are comes from @p masks, at the declared stretch,
///          and then **from the stretch this run fitted for them**, which is why the
///          ridges are walked twice. The declared geometry alone is not enough and
///          the gap is not small: on a fifth of stretched tones the upper note's
///          eleventh partial sits 52 Hz above where a claim at the declared stretch
///          puts it, which is more than a main lobe, and lands on the lower note's
///          sixteenth -- so the lower note reads its neighbour's partial as its own
///          and the claim geometry says nothing is there. A ridge the first walk
///          refused keeps the declared geometry in the second, so the second is
///          never worse informed than the first.
/// @param spec Complex STFT the partials are located in.
/// @param track Ridges to fit, from the same framing as @p spec.
/// @param masks Claim geometry at the declared stretch, used to tell which
///        partials are uncontested.
/// @returns One value per ridge, in ridge order, so it is @c track.ridges.size()
///          long whatever happened to each. Non-negative where the stretch was
///          fitted; exactly -1 where it was refused. **0 is a fitted result and
///          means the harmonic series**, so 0 is not the refusal -- a caller that
///          cannot tell the two apart would spend the ideal series where it meant
///          to fall back on the declared value.
///
///          What a returned stretch is worth, and it is two statements:
///          - Up to the highest harmonic it read a position for, claims placed with
///            it land inside half a bin of the partial.
///          - Above that it is **not** inside half a bin, and it is still better
///            than the declared value at every harmonic. That is structural rather
///            than lucky: both displacements are @c (b - B)*h^3*f0/2 to first order,
///            so their ratio has no @c h in it and the comparison reduces to
///            @c |B_fitted - B_true| against @c |B_declared - B_true|. Measured over
///            a two-octave dyad, a 0 dB-SNR fit 37% low, and a three-partial fit 5%
///            high, the fitted claim was never once the worse of the two and ran
///            0.1x to 0.4x the declared displacement. **There is therefore no
///            per-harmonic accuracy gate to be had** -- a gate of that shape could
///            only ever fire as the scalar comparison, which needs the true stretch.
///
///          **What no refusal covers**: @c min_partials is a count, and the count is
///          not the quantity that decides whether a fit can place the claims it will
///          be spent on. A fit from few partials placing many claims extrapolates --
///          at 0 dB SNR three partials miss a twenty-claim band twenty times over,
///          while three partials placing three claims are exact to a tenth of it --
///          and no count separates those two. What would is the fit's own precision
///          carried to the highest claim, and that is deliberately not a refusal
///          here: the precision estimate runs about 15x pessimistic, so it refuses a
///          correct fit, and a refusal spends the declared value, which is wrong by
///          orders rather than by a fraction of a band.
///
///          A claim above the note's own highest partial is a separate limit that
///          this does not touch: @ref build_note_masks carries what a spare claim
///          costs and why measuring the partial count is not worth shipping.
///
///          A ridge is refused on exactly four grounds, and they are enumerated
///          because an unenumerated one is unverified:
///          - fewer uncontested partials located than @c min_partials
///          - a per-partial misfit over @c max_residual_bins
///          - a fitted stretch above @c max_inharmonicity, or below zero by more
///            than the fit's own precision. **A negative fit is refused rather
///            than rounded up**: no partial series is compressed, so the fit has
///            found something other than this note's partials. Inside that
///            precision it is the harmonic series and returns 0, which is the
///            ordinary result for a note that does not stretch and has to be
///            reachable -- a true stretch of zero reads negative half the time.
///          - an f0 @ref refine_track_f0 could not re-estimate, which is its own 0
///            and leaves the windows with nothing to be predicted from
/// @throws SonareException(InvalidParameter) on the same disagreements between
///         @p spec, @p track and @p masks that @ref solve_shared_bins rejects; a
///         @c min_partials outside [2, 128], since a line through fewer than two
///         points is not a fit; a @c max_residual_bins or @c max_inharmonicity
///         that is not positive. Both floats must additionally be finite, because
///         a one-sided range admits infinity and NaN alike.
std::vector<float> estimate_track_inharmonicity(const Spectrogram& spec, const MultiF0Track& track,
                                                const NoteMaskSet& masks,
                                                const InharmonicityConfig& config = {});

}  // namespace sonare::editing::polyphony
