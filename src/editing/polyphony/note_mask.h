#pragma once

/// @file note_mask.h
/// @brief Soft masks that divide one STFT among the notes an analysis found.
///
/// A note claims the bins around each of its partials, a claimed bin is divided
/// among the notes claiming it, and every unclaimed bin is the residual. How a
/// shared partial divides is what decides whether a separated note sounds like a
/// note; this file makes the neutral choice of an equal split and estimates
/// nothing about which note the energy came from.
///
/// Adding every note back to the residual returns the input to within float
/// rounding. That identity is the point of the representation: an edit is a
/// change to one note's masked spectrum, and everything not edited is carried
/// through untouched rather than re-synthesised.
///
/// That identity holds for any division whatsoever, since the residual is one
/// minus whatever the notes took. It says the representation loses nothing; it
/// says nothing about whether a note got the right share, and a reconstruction
/// that matches is not evidence that the separation is good.
///
/// A weight is complex because two partials sharing a bin interfere, and the
/// sum's phase is neither one's. The equal split built here is real, but a later
/// stage estimates the division and needs the phase to state it; a real weight
/// caps what that stage can reach about 45 dB short.
///
/// A mask is stored sparsely because a note only reaches its own partials. Dense
/// per note would be a spectrogram each, which for a full arrangement is the
/// input many times over.

#include <complex>
#include <cstdint>
#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"

namespace sonare::editing::polyphony {

struct NoteMaskConfig {
  /// Partials claimed per note. A partial over Nyquist claims nothing, so a
  /// count high for the register costs only the loop. At most 128.
  int n_harmonics = 20;

  /// Width of a partial's claim, in the window's main lobes. The Hann main lobe
  /// is @c 4 * n_fft / win_length bins wide, so one lobe reaches half of that
  /// either side of the partial -- counting in lobes rather than in bins is what
  /// makes the value travel across zero padding instead of describing one
  /// framing. Zero padding is the axis it is invariant on and the only one: the
  /// half-width in Hz works out to @c 2 * claim_lobes * sample_rate / win_length, so
  /// it does not depend on @c n_fft at all, and halving the window doubles it --
  /// which is the window's own main lobe widening, not a defect.
  ///
  /// Measured at a Hann window, @c win_length equal to @c n_fft, and a partial
  /// halfway between two bins, which is the worst offset: one lobe captures
  /// 0.9995 of that partial's energy and two capture 0.99993, so the second buys
  /// 4e-4 while widening every overlap. A claim tapered rather than flat keeps
  /// 0.88 of it at the same width, which is the wrong trade against a leak
  /// already under 1e-4.
  ///
  /// That argument is against widening only. Narrowing costs something it cannot
  /// see: a claim has to stay wider than the track's f0 error for the partial to
  /// fall inside it at all, and ten cents at 1800 Hz is already 0.97 bins, so a
  /// stage dividing shared bins loses its tolerance to a track's error well
  /// before the claim loses energy. The two bound the default from opposite
  /// sides.
  ///
  /// Overlap is the normal case and not an edge. Counting partial positions at
  /// 20 partials a note, @c n_fft 4096 and 44.1 kHz: an octave puts ten of the
  /// lower note's twenty in the same bin as one of the upper note's, a fifth
  /// six, and a major third, a semitone and a tritone each still put six to nine
  /// within five bins.
  float claim_lobes = 1.0f;

  /// B in @c f_h = h * f0 * sqrt(1 + B * h^2), the same stretch the salience
  /// model uses. 0 is the ideal harmonic series.
  ///
  /// At a piano's 1e-4 the twentieth partial of a B4 sits 18.2 bins from where a
  /// claim built on 0 puts it, against a half-width of 2 at the default
  /// @ref claim_lobes: the claim misses its own partial, which becomes residual,
  /// and the residual is carried unedited -- so it keeps sounding at the old pitch
  /// after its note is moved. Landing inside the half-width is not landing right,
  /// though: the claim is flat over a main lobe 4 bins wide, so half a bin of
  /// offset already leaves 6 dB more behind and the usable tolerance is about a
  /// quarter of the half-width. Measuring the margin by whether the partial is
  /// still inside its claim overstates it fourfold.
  ///
  /// **One value covers about an octave.** On a sampled piano B runs 1.13e-4 at C3
  /// to 1.04e-3 at C5, while the declared range that holds a note within 6 dB of
  /// its own best is 1.2x at C3 and 1.008x at D5 -- the tolerance goes as
  /// 1/(h^3 * f0). Those bands do not overlap, so a wider span has no compromise
  /// value rather than a slightly worse one: either choice leaves the other note's
  /// partials some 63 dB further behind, and the geometric mean is worse than
  /// both. Widening @ref claim_lobes is not the way out -- the neighbour's partial
  /// it then claims lands on an equal division's -6.02 dB instead of losing a
  /// little, and no width brings the moved note within 6 dB of its own best.
  float inharmonicity = 0.0f;
};

/// @brief One note's weights, sparse over the frames it spans.
/// @details Frame @c f of the span occupies
///          <tt>[frame_offset[f], frame_offset[f + 1])</tt> of @ref bins and
///          @ref weights, and @ref bins is ascending within a frame. A frame the
///          note spans may still be empty, which is a note whose every partial
///          fell outside the spectrum, and so may the whole mask -- but a mask of
///          no frames still carries the one @c frame_offset entry the rule asks
///          for. A default-constructed one has none and is rejected: the default
///          exists so the struct is an aggregate, not so a zeroed one means an
///          empty mask.
///
///          Every function taking one checks that shape before it allocates
///          against it, because a hand-built mask is otherwise a write outside
///          its own arrays rather than a rejected input. The weights are checked
///          against their own range too: a weight of zero or one that is not
///          finite is not a shape error and would break the total and the
///          residual without any call failing, which is a worse outcome than a
///          rejection.
struct NoteMask {
  /// Index into the track's ridges, so a mask can be traced back to the pitch
  /// that produced it.
  int ridge_index = 0;
  /// First frame of the span, in the spectrogram's frames. A built mask spans
  /// exactly the frames its ridge does -- neither clipped nor padded, so a ridge
  /// and its mask can be indexed by the same frame.
  int frame_start = 0;
  int n_frames = 0;

  /// @c n_frames + 1 entries, ascending, first 0.
  std::vector<int32_t> frame_offset;
  /// Linear STFT bin of each weight.
  std::vector<int32_t> bins;
  /// Share of that bin this note takes. Finite and non-zero. An equal split is
  /// real and in (0, 1]; an estimated one carries the partial's phase and may
  /// exceed one in modulus, which is correct where two partials partly cancel
  /// and the observed bin is smaller than either component.
  std::vector<std::complex<float>> weights;

  int frame_end() const noexcept { return frame_start + n_frames; }
};

/// @brief Every note's mask over one spectrogram, plus the framing they describe.
struct NoteMaskSet {
  /// One per ridge of the track, in the track's order, so
  /// <tt>notes[i].ridge_index == i</tt>.
  std::vector<NoteMask> notes;
  int n_bins = 0;
  int n_frames = 0;
  int hop_length = 0;
  int sample_rate = 0;
  /// The geometry the claims were placed with, carried so a later stage can tell
  /// which partial of which note stands on a bin. That is not recoverable from
  /// the bins: the claim centre is @c h * f0 * sqrt(1 + B * h^2), so recovering
  /// the harmonic number as @c round(bin_hz / f0) disagrees from about the
  /// fourteenth partial up at an ordinary piano stretch, and at the first for a
  /// note low enough that a claim spans more than half a step.
  ///
  /// It is metadata about how the bins were chosen and not part of the set's
  /// shape, so the functions that consume a set do not validate it: a hand-built
  /// set carrying the default is accepted everywhere a set is accepted. Only a
  /// stage that has to interpret the claims reads it, and such a stage validates
  /// it for itself.
  NoteMaskConfig config;
};

/// @brief One partial's claim: which harmonic, where it sits, which bins it takes.
/// @details Two different edge rules meet here and are not the same rule. A
///          partial whose @c centre_hz is **above** Nyquist claims nothing and is
///          dropped, because a real signal has nothing up there -- a claim
///          reaching back down from above Nyquist would take bins that cannot
///          hold that note's content. A partial at or below Nyquist whose claim
///          *width* runs off either end is **clamped** to the spectrum and kept,
///          so a partial near the top keeps the part of its width that fits.
///
///          A partial exactly at Nyquist is therefore kept. That bin is
///          real-valued but can carry content, and the bins its claim reaches
///          below it certainly can -- dropping the claim to avoid the one would
///          discard the others with it.
struct PartialClaim {
  int harmonic = 0;        ///< 1-based, so @c centre_hz is that multiple of the f0.
  float centre_hz = 0.0f;  ///< @c harmonic * f0 * sqrt(1 + B * harmonic^2).
  /// The claimed bins, as a closed interval: bin @c last_bin is claimed. Both are
  /// already clamped to <tt>[0, n_bins)</tt>, so a consumer never clamps again,
  /// and @c first_bin <= @c last_bin always -- a claim with nothing left after
  /// clamping is not returned at all.
  int first_bin = 0;
  int last_bin = 0;
};

/// @brief Where one note's partials fall and what each of them claims.
/// @details The single derivation of claim geometry. @ref build_note_masks places
///          its claims from this, and a stage that has to know which partial of
///          which note stands on a bin reads it rather than recovering a harmonic
///          number from the bin's frequency -- two routes to one quantity, with
///          nothing asserting they agree, is how the stretch below goes unnoticed
///          until it is outside every tested range.
///
///          Strictly ascending in @c harmonic, and the ranges are disjoint and
///          ascending with it -- but the harmonics may skip, so
///          <tt>claims[i].harmonic</tt> is not @c i+1. Two partials closer
///          together than a bin resolve to one claim, the upper one having
///          nothing left once the lower has taken the bins: at the default
///          framing that begins below about 10.8 Hz, and an f0 of 5 with 128
///          harmonics returns 60 claims with 59 gaps. Keeping
///          <tt>first_bin <= last_bin</tt> is worth more than an index identity,
///          because a consumer expands the range and a reversed one is a real
///          fault where a skipped harmonic is only a missing entry.
///
///          Partial frequency is monotone in the harmonic for any
///          @c inharmonicity the config allows, so the partials that fall off the
///          top fall off together: the result is shorter than
///          @c config.n_harmonics near the top of the register and empty for an
///          f0 above Nyquist.
/// @param spec Supplies the framing -- @c n_bins, @c win_length, @c sample_rate.
///        Its contents are never read, so the claim geometry does not depend on
///        the signal.
/// @param f0_hz Positive and finite.
/// @param config Claim geometry.
/// @throws SonareException(InvalidParameter) on an empty @p spec or one whose
///         @c n_fft, @c win_length, @c hop_length or @c sample_rate is not
///         positive; an @p f0_hz that is not positive and finite; an
///         @c n_harmonics outside [1, 128]; a @c claim_lobes outside (0, 64]; or
///         a negative @c inharmonicity. The same grounds @ref build_note_masks
///         rejects, enumerated rather than referenced because this is a public
///         entry point of its own and an unenumerated contract is an unverified
///         one.
std::vector<PartialClaim> partial_claims(const Spectrogram& spec, float f0_hz,
                                         const NoteMaskConfig& config = {});

/// @brief Builds one mask per ridge over @p spec.
/// @details The claim of a partial is flat across its width, and a claim running
///          off either end of the spectrum is clamped to it rather than dropped,
///          so a partial near Nyquist keeps the part of its width that fits.
///
///          Where two notes claim one bin they take equal shares. That is a
///          neutral division and not a good one: an octave's lower note loses
///          half of every partial its upper note stands on, so editing one of a
///          pair audibly thins the other. Deciding the share from the material is
///          what a later stage does, and this contract is what it will change.
///
///          A claim is predicted and never read from the spectrum, so a note
///          claims bins its own partials never reached and takes a share of
///          whatever stands there. **The share it takes does not move when its
///          note moves**, so the cost is not a level error: it is a partial left
///          sounding at the old pitch. Measured on a fifth whose upper note is
///          shifted, the partial a spare claim sits on drops 1.81 dB while the
///          same claim set cut to the real partial count drops it 77.39 dB, every
///          other partial agreeing to within 0.01.
///
///          **Dropping such a claim by measuring how many partials the note has
///          was tried anyway and is not worth shipping**, and that 75 dB is the
///          reason rather than a counter-argument: a claim dropped wrongly sends a
///          real partial to the residual, which is carried unedited, so the payoff
///          is the same size in both directions. The estimate does not survive real
///          material -- on a piano dyad the two notes' measured partial counts came
///          out 19 and 3 at the same level, so the rule would have dropped a
///          partial 29 dB above the floor. A large symmetric bet on an estimator
///          that is wrong on the material that matters is worse than the claim.
/// @param spec Complex STFT the masks index into.
/// @param track Ridges to build masks for, from the same framing as @p spec.
/// @param config Claim geometry.
/// @throws SonareException(InvalidParameter) on an empty @p spec or one whose
///         @c n_fft, @c win_length, @c hop_length or @c sample_rate is not
///         positive; a @p track whose @c hop_length, @c sample_rate or
///         @c n_frames disagrees with @p spec; a ridge reaching outside
///         @p spec's frames or carrying an @c f0_hz that is not positive and
///         finite, which @ref track_f0_ridges rejects for the same reason and
///         which is not quietly absorbed here either; an @c n_harmonics outside
///         [1, 128]; a @c claim_lobes outside (0, 64]; or a negative
///         @c inharmonicity.
NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config = {});

/// @brief One note's share of @p spec.
/// @details Every bin the mask does not name is zero, so the result is a
///          spectrogram of the same shape carrying only that note.
/// @throws SonareException(InvalidParameter) when @p mask reaches outside
///         @p spec.
Spectrogram apply_note_mask(const Spectrogram& spec, const NoteMask& mask);

/// @brief What no note claimed: @p spec weighted by one minus every mask.
/// @details It holds noise, reverb tails and anything at no partial position,
///          and it is played back unedited. An empty residual is not the goal --
///          energy forced into a note is energy that breaks when the note moves.
/// @throws SonareException(InvalidParameter) when @p masks does not describe
///         @p spec -- its @c n_bins, @c n_frames, @c hop_length and
///         @c sample_rate must all match, because a set carrying another
///         framing's hop indexes the same array while meaning different times.
Spectrogram residual_spectrum(const Spectrogram& spec, const NoteMaskSet& masks);

/// @brief Total weight the notes place on each bin, @c [n_bins x n_frames] in
///        the spectrogram's own layout.
/// @details From @ref build_note_masks this is one at every claimed bin, up to
///          the rounding of adding @c k copies of @c 1/k -- which for a bin many
///          notes claim can carry the sum a few ULP past one. From a stage that
///          estimates the division it is one only where the estimate accounted
///          for the whole bin, and the shortfall is what the residual then
///          carries; that is the intent, since energy pushed into a note that did
///          not produce it breaks when the note moves.
///
///          It is not clamped or normalised. A clamp would hide a total that is
///          genuinely wrong as readily as one that is merely rounded, and
///          normalising would erase exactly the shortfall the residual is for.
///          This exists so the sum can be checked rather than assumed.
/// @throws SonareException(InvalidParameter) on a @p masks with no shape.
std::vector<std::complex<float>> mask_total(const NoteMaskSet& masks);

}  // namespace sonare::editing::polyphony
