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
/// The weights sum to one at every bin, so adding every note back to the
/// residual returns the input to within float rounding. That identity is the
/// point of the representation: an edit is a change to one note's masked
/// spectrum, and everything not edited is carried through untouched rather than
/// re-synthesised.
///
/// That identity holds for any division whatsoever, since the residual is one
/// minus whatever the notes took. It says the representation loses nothing; it
/// says nothing about whether a note got the right share, and a reconstruction
/// that matches is not evidence that the separation is good.
///
/// A mask is stored sparsely because a note only reaches its own partials. Dense
/// per note would be a spectrogram each, which for a full arrangement is the
/// input many times over.

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
  /// width in Hz works out to @c 2 * claim_lobes * sample_rate / win_length, so
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
  /// Overlap is the normal case and not an edge. Counting partial positions at
  /// 20 partials a note, @c n_fft 4096 and 44.1 kHz: an octave puts ten of the
  /// lower note's twenty in the same bin as one of the upper note's, a fifth
  /// six, and a major third, a semitone and a tritone each still put six to nine
  /// within five bins.
  float claim_lobes = 1.0f;

  /// B in @c f_h = h * f0 * sqrt(1 + B * h^2), the same stretch the salience
  /// model uses. 0 is the ideal harmonic series.
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
///          against their own range too: a weight of zero or two is not a shape
///          error and would break the total and the residual without any call
///          failing, which is a worse outcome than a rejection.
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
  /// Share of that bin this note takes, in (0, 1].
  std::vector<float> weights;

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
};

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
/// @details Never below zero, and one at every claimed bin up to the rounding of
///          adding @c k copies of @c 1/k -- which for a bin many notes claim can
///          carry the sum a few ULP past one. It is not clamped: a clamp would
///          hide a total that is genuinely wrong as readily as one that is merely
///          rounded, and this exists so the sum can be checked rather than
///          assumed. It is the one property every later stage rests on.
/// @throws SonareException(InvalidParameter) on a @p masks with no shape.
std::vector<float> mask_total(const NoteMaskSet& masks);

}  // namespace sonare::editing::polyphony
