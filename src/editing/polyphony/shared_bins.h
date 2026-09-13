#pragma once

/// @file shared_bins.h
/// @brief Divides a bin two notes stand on by fitting each partial's own
///        rotation, instead of splitting it equally.
///
/// One STFT bin across frames is a sum of rotating phasors, one per partial
/// standing on it, each turning at @c 2*pi*f*hop/sample_rate and decaying at its
/// own rate. Those are the poles of the trajectory and ESPRIT recovers them from
/// the data, so a partial can be told from its neighbour by how it turns rather
/// than by how loud it is. A weight is then that partial's fitted contribution
/// over the observed value, which makes it complex: a real weight cannot undo
/// the interference between two partials, and the optimal real one is capped
/// about 45 dB above what a complex one reaches.
///
/// What the notes do not account for stays out of them. The weights are the
/// fitted components over the observation and are deliberately not normalised to
/// sum to one, so the part the model failed to explain flows to the residual
/// rather than being pushed into a note that did not produce it.
///
/// Which recovered pole belongs to which note is decided over the claimants as a
/// set, by the assignment of lowest total **squared** distance between a pole's
/// angle and a note's predicted rate -- never by taking each note's nearest pole
/// in turn. Deciding in turn lets whichever note is asked first take a pole
/// another note fits better, and claimant order is ridge order: measured on a
/// fifth, swapping the two ridges moved the share in 16 of 44 frames of a bin
/// holding two real partials, and in 36 of 44 of a bin holding one, while the
/// recovered poles themselves were identical.
///
/// Squared rather than absolute, and the difference is not a preference. Summed
/// absolute distance cannot decide this problem at all: for two claimants whose
/// two poles both lie to one side of both predictions, the two assignments differ
/// by `(p2 - p1) - (p2 - p1)` and tie **identically**, for every such
/// configuration rather than by coincidence. Measured on a degenerate fixture
/// before the objective was squared, 10 of 36 windows tied to the last bit.
/// Squaring makes the difference `2*(p2 - p1)*(rA - rB)`, which is signed by
/// whether the order of the poles matches the order of the predictions, so the
/// objective prefers the monotone match and ties only where the predictions
/// coincide. That derivation is on a line, and a rate is an angle: where the pair
/// straddles the wrap the differences are not those of the unwrapped quantities
/// and the monotone match is not implied. What holds without that condition is
/// narrower and is what the contract rests on -- every permutation is enumerated,
/// so the minimum found is the exact minimum of a quantity computed from the
/// claimants as a set, which is order-independent whatever the geometry. Measured
/// over one suite, the squared objective left 0 ties in 1509 assignments against
/// 27 for the absolute one.
///
/// A tie is then refused rather than settled, because an order-independent result
/// cannot come from an order-dependent tiebreak. Equality is exact, with no
/// epsilon: an epsilon would be a width over which the refusal fires, and nothing
/// measures how wide that should be.
///
/// What the assignment cannot do is keep a note off another note's partial. A bin
/// may hold fewer real poles than it has claimants -- a claim is predicted from an
/// f0 and a harmonic number and is never read from the spectrum, so a note claims
/// bins its own partials never reached. The spare pole is then leakage at an
/// arbitrary angle, and which assignment minimises the total is decided by where
/// that angle fell: measured on a fifth, the note with no partial there took the
/// larger share in 38 of 44 frames, identically in both ridge orders. So the
/// misattribution is consistent rather than random, and it is not order.
///
/// **Restricting the assignment is the wrong place to fix it, and that is
/// measured.** Admitting a pole to a note only where no other claimant is closer
/// needs no threshold and looks free; it refused 95% of the cells of bins holding
/// **two** real partials (408 of 431 between 0.24 and 0.72 rad/frame), because
/// two partials close enough to need separating put both recovered poles nearest
/// the same prediction. Worst error on a real shared bin went from 1.67 dB back to
/// 6.88 and the bin the rule was aimed at came out 5.8 dB worse still. **The cases
/// such a rule refuses are the ones this file exists for.** Nor does the claim
/// geometry fix it -- @ref build_note_masks carries what pruning a claim by the
/// partial count measured from the spectrum is worth, and it is not enough to
/// ship. **The spare claim is a limit of the representation rather than a defect
/// with a known fix.** What it costs is not bounded by the division error: the
/// bin's content reaches the render either way, and the note that did not produce
/// it takes a share it cannot move, so the partial stays where it was. Measured at
/// 75 dB on one partial of a shifted note; @ref build_note_masks carries the
/// figure and why pruning is the larger risk.
///
/// Most of the value is in refusing. Solved everywhere, over a spread of
/// intervals, this returns 0.6 dB over an equal split, because the bins it
/// cannot do cost more than the bins it can do gain. Gated, the same material
/// returns 3.4 dB, and the intervals it accepts individually return 23 to 98 dB.
/// @ref SharedBinConfig is therefore mostly refusal thresholds. Two read the fit.
/// One reads the notes before any fit runs, because whether two partials can be
/// told apart at all is a question about where they are: a decomposition handed
/// one partial where two were claimed spends its spare pole on leakage and
/// returns an angle that means nothing, so the answer cannot come from the fit.

#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"

namespace sonare::editing::polyphony {

/// @brief Thresholds for when a bin may be solved and when it falls back.
struct SharedBinConfig {
  /// Frames per fit. The trajectory has to turn far enough for two rates to be
  /// distinguishable, and it has to stay a sum of steady poles while it does.
  /// Below 8 the fit is under-determined at order 4; well above it a note's own
  /// decay and drift stop looking like fixed poles. Windows step by half, are
  /// averaged where they overlap, and one extra window is placed to end exactly
  /// at the span's end -- a half step alone leaves up to @c window_frames/2 - 1
  /// frames covered by nothing, which would silently keep the equal split with
  /// no refusal recorded against it. A span shorter than this is not solved.
  int window_frames = 8;

  /// Smallest gap between two claiming notes' predicted partials, in radians per
  /// frame, that is worth fitting. Computed from @ref refine_track_f0's output
  /// and @c NoteMaskSet::config before any decomposition runs -- the geometry
  /// says which partial of which note stands on the bin, and the refined f0 says
  /// where it is -- and not from the recovered poles: a bin where the two
  /// claimed partials coincide holds one pole, the order-2 fit spends its second
  /// on leakage, and that pole's angle reaches pi -- so no threshold on a
  /// recovered angle separates an equal-tempered octave from a fifth, while the
  /// predicted gap separates them by five orders.
  ///
  /// Sweeping the detuning of an octave's upper note, the pole fit overtakes the
  /// equal split between 0.0073 and 0.0146; below the crossing it is not merely
  /// useless but worse than the split it replaces. The crossing is set by the
  /// signal-to-noise ratio rather than by the window, so it moves up on real
  /// material -- this is a floor, not a tuned point, and 0.01 is where noiseless
  /// synthetic material crosses at @c window_frames 8.
  float min_partial_separation = 0.01f;

  /// Largest relative misfit, @c ||x - model|| / ||x||, that still counts as
  /// solved. A trajectory that is not a sum of steady poles reads high here:
  /// vibrato at 15 cents gives 0.06 against 0.0000 for a steady note. Refusing
  /// above 0.02 is what keeps vibrato from costing more than it gains.
  float max_fit_residual = 0.02f;

  /// Ceiling on @c |weight|. A weight above one is correct where two partials
  /// partially cancel, since the observation is then smaller than the component,
  /// but it is unbounded and a near-cancellation makes it explode. Above the
  /// ceiling the modulus is scaled back and the angle kept -- the angle is the
  /// part worth having.
  ///
  /// The population is narrow and deep, and it is narrow **because the partials
  /// beat**. Two partials a few hertz apart sweep through antiphase, so the null
  /// is an instant and a window averages it away; only a frame or two per bin
  /// ever crosses. Measured on a detuned fifth, two cells carry 0.11% of the
  /// note's energy and peak at a share of 23.8, and clamping them beats the equal
  /// split by 3.6 dB with the angle taken from an oracle, which bounds what the
  /// fit can reach.
  ///
  /// Two things that table cannot be read to say. **Whether an interval crosses
  /// the ceiling at all is set by the level ratio, not by the interval** -- sweep
  /// one note's level and every interval tested crosses, so none is safe by
  /// virtue of being itself. And **a pair whose partials coincide exactly is the
  /// exception to "narrow"**: the cancellation is stationary rather than beating,
  /// the null persists across frames, and an equal-tempered octave puts over a
  /// hundred cells above the ceiling. @ref SharedBinOutcome::PartialsTooClose
  /// refuses all of them, so the clamp never sees the one population that is
  /// broad. What reaches the clamp is thin by construction, and exercising it
  /// therefore depends on a near-cancellation with no way around that.
  ///
  /// Applied once, to the weight formed from the averaged component, not to each
  /// window before averaging: clamping first moves the angle of the average,
  /// which is the one quantity the clamp exists to preserve.
  ///
  /// The ordering is out of reach, for a reason as arithmetic as the effect
  /// itself. Once both windows clamp they carry the same modulus, so clamping
  /// first bisects their angles while averaging first takes the modulus-weighted
  /// mean; the two part company only as that ratio leaves one, reaching 87
  /// degrees at 55:1. It does not leave one. Both windows must reproduce the bin
  /// across their whole overlap, and two separated modes are linearly independent
  /// over two frames or more, so their fits differ by no more than their
  /// residuals do. The same ratio of conditioning to noise governs the
  /// minimum-norm collapse, so a bin degenerate enough for the fits to part has
  /// already lost its weight to about one and cannot reach the ceiling. Measured,
  /// the ratio stays under 1.26 wherever both windows pass the residual gate and
  /// under 1.10 at the degenerate end, where the bound is measurement rather than
  /// algebra; at 1.2 even antiphase windows move the mean by about 0.01 rad.
  ///
  /// It also bounds how loud the residual can get, and that axis is not what set
  /// the default. @ref mask_total runs to roughly this value where a bin is
  /// clamped, so @ref residual_spectrum returns several times the input there --
  /// inaudible while nothing is edited, since the notes and the residual still
  /// sum to the input, but the moment one note moves, what is left no longer
  /// cancels. Lowering this trades separation for a quieter residual. 8 was
  /// chosen on separation alone.
  float max_weight_modulus = 8.0f;

  /// Highest partial, in Hz, that may be used to refine an f0. See
  /// @ref refine_track_f0. 0 derives it from @p spec's framing and
  /// @ref f0_tolerance_cents; any other value is used as given and is NOT capped
  /// by the derived one, because raising it is how a caller says its f0 is
  /// better than the tolerance claims. Raising it past the derived value on a
  /// track that does not justify it makes refinement worse rather than failing:
  /// partials above the alias bound unwrap to the wrong period and vote for the
  /// wrong f0.
  float max_refine_hz = 0.0f;

  /// Worst f0 error, in cents, the refinement must be able to undo. Read for two
  /// separate purposes, and it is always read even when @ref max_refine_hz is set
  /// explicitly.
  ///
  /// It sets how far a partial may sit from its prediction and still be unwrapped
  /// to the right alias, which is what derives @ref max_refine_hz when that is 0.
  ///
  /// It also widens the distance a rival partial must clear before this one
  /// counts as standing alone. Without that widening the test is not fail-safe:
  /// at 50 cents the f0 error is twice the claim half-width even at the alias
  /// ceiling, so over a sweep of ordinary intervals 6.7% of partials flip from
  /// disqualified to qualified, and the worst case is a minor third whose sixth
  /// and fifth partials coincide exactly being judged to stand alone. Widening by
  /// the declared error takes that to zero by construction.
  ///
  /// So this is a statement about the caller's tracker and it has a price paid
  /// in both directions. Declaring more error than the track has costs
  /// intervals: a semitone keeps 12 usable partials at 10 cents and 7 at 25, and
  /// loses all of them at 50, where it is refused outright. Declaring less is the
  /// one way to reach a wrong answer here rather than a refusal -- a track
  /// carrying 50 cents while declaring 10 puts 6.7% of partials back on the
  /// wrong side of the test, including pairs that coincide exactly.
  float f0_tolerance_cents = 50.0f;
};

/// @brief Why a bin was not solved, or that it was.
/// @details **The declaration order is the order the determinations are made,
///          and so is the precedence when more than one applies.** A bin can
///          easily be both too short and too crowded; reporting whichever the
///          implementation happened to test first makes a report unreadable, and
///          an order stated separately from the list drifts from it. @c Solved
///          is last because it is what remains when nothing else applied.
enum class SharedBinOutcome {
  Unclaimed,  ///< No note reached it; it is residual and nothing was tried.
  /// One note claimed it; that note takes the whole bin. Decided before the span
  /// length is looked at -- an unshared bin needs no fit, so a short span does
  /// not make it @c TooFewFrames.
  Unshared,
  TooFewFrames,  ///< The span is shorter than @c window_frames.
  /// More notes claim the bin than can be resolved, which is the lesser of two
  /// ceilings. The fit's is <tt>window_frames / 2</tt>: the Hankel of a
  /// @c window_frames trajectory has that many rows to spare, so the order cannot
  /// exceed it however the matrix is shaped. The assignment's is 8, because it
  /// enumerates the orders and a factorial turns one more claimant into a hang
  /// rather than a slower answer -- at the validated ceiling of @c window_frames
  /// the fit would otherwise admit 32 claimants and ask for 32! assignments.
  /// 8 is unreachable at the default @c window_frames, which admits 4.
  TooManyClaimants,
  /// A claiming note's f0 could not be re-estimated, so the gap below cannot be
  /// computed for it. Determined before @c PartialsTooClose because it is that
  /// test's missing input rather than a value it could judge: an f0 carrying the
  /// track's full error would be compared against a gap that needs a hundredth of
  /// a cent, and the comparison would pass on exactly the material it exists to
  /// refuse -- an octave, whose upper note is the one that cannot be refined.
  F0NotRefined,
  /// Below @c min_partial_separation: the two claimed partials are too close for
  /// a fit to tell apart, and fitting them anyway is worse than not. Determined
  /// from the refined f0 before any decomposition runs, so @c fit_residual stays
  /// zero here -- no fit was tried, and the one that would have been tried would
  /// have reported a small misfit while dividing the bin arbitrarily.
  PartialsTooClose,
  /// The decomposition itself failed and produced no poles to judge.
  /// @c fit_residual is zero because no misfit was ever measured -- which is why
  /// this is not @c FitDiverged: that would claim a misfit was computed and found
  /// large, when in fact nothing got far enough to compute one. The separation is
  /// present and above its threshold, since reaching here means passing the gate.
  PolesNotFound,
  /// Above @c max_fit_residual; not a sum of steady poles.
  ///
  /// A claim above the note's own highest partial does **not** reliably land
  /// here, and the residual does not separate that case from a genuine shared
  /// bin: measured on a fifth of ten-partial tones, two bins carrying one real
  /// partial against one empty claim read 0.0135 and 0.0144, while the bin
  /// carrying two real partials read 0.0174 -- the higher of the three. Tightening
  /// this threshold reaches the real shared bin first.
  FitDiverged,
  /// Two claimants fit the recovered poles equally well, so which partial belongs
  /// to which note is not decided by the data. Refused rather than broken by
  /// claimant order, which is ridge order and carries no physical meaning.
  AssignmentAmbiguous,
  /// A guard on @ref NoteMask's own invariant rather than a modelled failure:
  /// the fit passed and the weight it implies cannot be stored, because a mask
  /// weight must be finite and non-zero and a division can in principle produce
  /// neither. Alone among the refusals this one is not named for a signal --
  /// both read healthy, inside their thresholds, exactly as @c Solved does.
  ///
  /// Nothing measured reaches it, so it is retained as a guard and not as a case,
  /// and a caller should not write code expecting to see it.
  ///
  /// It does not protect a note from being handed a partial it does not have. On a
  /// fifth of ten-partial tones, the note whose own partial was never rendered
  /// took a weight at or above one in every frame of both such bins, and the note
  /// that owned the partial was left with as little as 8.5e-08 of it. What decides
  /// that is @ref AssignmentAmbiguous's subject, not this guard.
  DegenerateWeight,
  Solved,  ///< A pole fit produced the weights.
};

/// @brief What happened at every bin, @c [n_bins x n_frames] in the
///        spectrogram's own layout -- the same indexing as @ref mask_total, so
///        an outcome and a total can be read at one subscript.
/// @details Dense rather than one entry per mask entry, because a sparse report
///          would need an index map back to the bins that the interface does not
///          carry. @ref SharedBinOutcome::Unclaimed fills the cells no note
///          reached. Each signal is zero until the stage that measures it runs:
///          @c partial_separation from the gate, so it is present on every
///          outcome the gate let through and on the refusal it issues, and zero
///          on the structural refusals above it; @c fit_residual only where a fit
///          ran, so it is zero on every refusal decided before the decomposition.
///
///          The two are read at different granularities and that is not an
///          inconsistency. @c partial_separation is a span scalar -- one refined
///          f0 per ridge and a fixed claimant set over the span -- so every frame
///          of a span carries the same value, and the two verdicts decided from
///          it, @c F0NotRefined and @c PartialsTooClose, are span-wide: a gate
///          reading a span constant cannot refuse part of a span. @c fit_residual is per window: a
///          frame several windows cover reads @c Solved if any of them solved it
///          and otherwise carries the first window's refusal, and it carries the
///          residual of whichever window its outcome came from, or it would
///          report @c Solved beside a misfit that would have refused it.
struct SharedBinReport {
  std::vector<SharedBinOutcome> outcome;
  /// Gap between the two closest predicted partials, radians per frame. Read
  /// from the refined f0, so it is available on a bin no fit was run on.
  std::vector<float> partial_separation;
  /// Relative misfit of the pole model on that window.
  std::vector<float> fit_residual;
};

/// @brief Each ridge's f0, re-estimated from the partials it holds alone.
/// @details A partial holds a bin alone when no other ridge's partial falls
///          inside its claim, and only then does an order-1 fit on that bin's
///          trajectory give its rate exactly. **A bin no other note claims is not
///          enough.** Two notes a hertz and a half apart have claims that overlap
///          almost entirely, so the bins left geometrically unshared are slivers
///          at the claim edges where neither partial dominates and the content is
///          leakage -- an order-1 fit there returns the centroid of both partials,
///          which is a confident wrong answer rather than a refusal. Measured on
///          that pair: the lower note comes back exact and the upper one 7.9 cents
///          low, and the same value then feeds pole assignment.
///
///          The qualifier costs nothing where the method works. On a fifth, a
///          major third, a semitone, a tritone and an octave it keeps 16 to 28
///          partials against 16 to 30 and returns the same exact f0; even two
///          notes three hertz apart keep three, because the eighth partials are
///          twenty-four hertz apart where the fundamentals are three. It reaches
///          zero only when no harmonic of either note separates from its
///          neighbour, which is the case no unshared bin exists for.
///
///          Unwrapping the rate to a frequency needs the prediction to be within
///          half of @c sample_rate/hop_length, which bounds how high a partial
///          may be and still be usable:
///          <tt>(sample_rate/hop_length) / 2 / (2^(tolerance/1200) - 1)</tt>,
///          which at 50 cents and a 44.1 kHz, 512-hop framing is 1470 Hz. One
///          usable partial is enough --
///          taking five changes nothing, because the recovered rate is exact
///          rather than noisy.
///
///          This exists because the assignment, not the fit, is what f0 error
///          breaks. The fit is invariant: with the poles assigned correctly the
///          result is identical at 0 and at 50 cents. Matching a pole to a note
///          by nearest predicted rate is not, and it fails well before the alias
///          does -- a fifth's shared partials sit 0.89 Hz apart at 785 Hz, so
///          that match needs the f0 to under one cent where the track promises
///          fifty. Refined, 50 cents in comes back as 0.001 to 0.1 cents out and
///          the separation result returns to its zero-error value exactly.
///
///          A ridge with no usable unshared partial returns 0, not the f0 it
///          came in with. A returned input cannot be told apart from a
///          refinement that agreed with it, and that difference decides whether
///          the bins the ridge shares may be solved at all: an equal-tempered
///          octave's upper note is exactly the case -- every partial it has is
///          shared -- and it is also the note whose claimed bins the separation
///          test has to refuse. Handing back its unrefined f0 makes that test
///          read a gap of seventeen hertz where the truth is zero. Zero is not a
///          frequency, so the sentinel cannot be spent by accident.
///
///          One value per ridge, where the ridge carries an f0 per frame. A note
///          whose pitch moves across its span is therefore not represented, and
///          that is deliberate rather than unfinished: the refined value exists
///          to predict a partial's rate well enough to assign a pole, and a note
///          whose pitch moves enough for one value to be wrong is a note whose
///          fit @c FitDiverged refuses anyway.
/// @returns One value per ridge, in ridge order, so it is @c track.ridges.size()
///          long whatever happened to each. Positive where the f0 was re-estimated
///          from the data; exactly 0 where it could not be.
/// @throws SonareException(InvalidParameter) on the same disagreements between
///         @p spec, @p track and @p masks that @ref solve_shared_bins rejects.
std::vector<float> refine_track_f0(const Spectrogram& spec, const MultiF0Track& track,
                                   const NoteMaskSet& masks, const SharedBinConfig& config = {});

/// @brief Replaces the equal split on every bin two or more notes claim.
/// @details The bins, the frames, the note order and @c masks.config are exactly
///          @p masks's -- only the weights change, and only on shared bins. The
///          geometry passes through rather than being re-derived or defaulted,
///          which is what makes feeding a result back in a no-op: a second call
///          that read a default geometry would interpret the same bins as
///          different partials and would not reproduce the first. A bin that is
///          refused keeps the equal split it arrived with, so the result is
///          never worse than @p masks by construction and the caller does not
///          have to handle a partial answer. So does a bin whose fit produced a
///          weight the mask cannot hold -- a zero component, or a silent
///          observation to divide by -- which reports as
///          @ref SharedBinOutcome::DegenerateWeight rather than borrowing a
///          refusal named for a signal that in this case read healthy.
///
///          A fit runs over a span: a maximal run of frames on one bin claimed
///          by the same set of notes. Maximal, not longest -- a bin whose
///          claimants change and change back has two spans, and each is fitted
///          on its own. A span is where the model order is fixed,
///          which is what makes the order and @c TooManyClaimants consistent
///          with each other. Matching the recovered poles to the claiming notes
///          is decided over the claimants as a set, and the @c @file block above
///          is where that is stated; @ref track_f0_ridges resolving its own
///          ambiguity one ridge at a time is not a precedent for it.
///
///          The model order is the number of notes claiming the bin, not
///          anything read off the fit. The order fixes how many weights come out
///          and there has to be one per claiming note whatever the data turns out
///          to hold: a bin whose two claimed partials coincide still owes two
///          weights, so one pole in the data is not a reason to fit order one.
///          Such a bin is refused by @c PartialsTooClose before it is decomposed
///          at all, which is why the order never has to be decided from the data.
///          A rank read off the Hankel answers a different question than the one
///          being asked.
///
///          Which partial of which note stands on a bin comes from replaying
///          @c masks.config's geometry, never from guessing a harmonic number
///          back out of the bin's frequency. The claim centre carries the stretch
///          @c NoteMaskConfig::inharmonicity applies, so a guess disagrees with
///          the claim it is meant to describe on ordinary material, and a gate
///          reading the wrong partial refuses and accepts the wrong bins.
///
///          It calls @ref refine_track_f0 itself, and both gates and assigns from
///          the result, never from @p track's own f0. That is not an optimisation:
///          a fifth's shared partials sit 0.89 Hz apart, so assigning them needs
///          the f0 to under one cent where the track promises fifty, and without
///          the refinement this loses to the equal split on real input. The gate
///          needs it more sharply still -- it is a comparison between two
///          predicted partials, so the track's error lands in it undiluted. The
///          function is exposed separately so a caller can see what it got, not
///          so a caller can choose to skip it.
///
///          What it does not fix, measured rather than assumed: an equal-tempered
///          octave, whose partials coincide, is refused, and solving it loses
///          4.7 dB against the equal split even when the poles are assigned by an
///          oracle -- so that is a bound on the refusal's cost, not an estimate
///          of it. Vibrato deep enough to swing a partial across its neighbour is
///          refused too, and there the limit is real rather than an assignment
///          failure, since a perfect assignment still only reaches 0.1 dB.
/// @param spec The STFT @p masks indexes, in the framing @p masks declares.
/// @param masks Equal-split masks from @ref build_note_masks. Passing a result
///        back in is allowed and is a no-op: every weight this writes is
///        computed from @p spec and never from the weight it found there, and a
///        refusal returns what it received, so the second call reproduces the
///        first bit for bit.
/// @param track The ridges @p masks was built from.
/// @param config Window and refusal thresholds.
/// @param report Optional; per-bin outcomes and the two refusal signals.
/// @throws SonareException(InvalidParameter) when @p masks does not describe
///         @p spec, when @p track's framing or frame count does not match
///         @p spec's, when a ridge is empty, reaches outside @p spec's frames or
///         carries an f0 that is not finite and positive, when
///         @p track and @p masks disagree on the ridge count, on
///         a @c window_frames outside [4, 64], a @c min_partial_separation outside
///         (0, pi], a @c max_fit_residual outside (0, 1], a
///         @c max_weight_modulus below 1, a negative @c max_refine_hz, or a
///         @c f0_tolerance_cents outside (0, 1200].
///
///         Every float field must additionally be finite. Each range above is
///         one-sided, and a single comparison against a one-sided range admits
///         both NaN and infinity -- so the finiteness is checked rather than
///         left to the range, and infinity is rejected even where the range has
///         no upper bound.
NoteMaskSet solve_shared_bins(const Spectrogram& spec, const NoteMaskSet& masks,
                              const MultiF0Track& track, const SharedBinConfig& config = {},
                              SharedBinReport* report = nullptr);

}  // namespace sonare::editing::polyphony
