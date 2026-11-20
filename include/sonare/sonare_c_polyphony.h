#pragma once

/// @file sonare_c_polyphony.h
/// @brief Editing one note of a chord: an analysis held as a handle, the notes it
///        found, and a render back to audio.
///
/// The monophonic door (@ref sonare_extract_notes, @ref sonare_render_notes) passes
/// everything by value, because everything a note was measured against is either the
/// caller's audio or the caller's F0 track. That does not carry over. A polyphonic
/// analysis holds a complex spectrogram and, per note, the complex weight of every
/// bin it claimed -- the input over again plus the claims. Those are the measurement;
/// a host editing a chord never wants them, and no public struct here has ever
/// carried a complex number.
///
/// So the analysis is a handle. What crosses is what a host acts on: the notes, each
/// note's pending edit, and the figures that say what was found -- the per-frame voice
/// count, and per note a pitch, a level and a salience curve. Re-rendering an edit
/// therefore costs no second analysis, which is the only reason to hold the
/// measurement at all.
///
/// @ref SonareNoteObject and @ref SonareNoteEdit are the monophonic door's, reused
/// unchanged: an edit means the same thing whichever chain applies it. One field is
/// inert here and saying so is the contract. @c envelope_offset indexes an array the
/// caller owns in the by-value door, because the field has to mean one thing in both
/// directions; a handle has no such problem, so @ref sonare_polyphonic_set_note_edit
/// takes the points and the note keeps its own copy.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_effects.h"
#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque analysis: one STFT, the tracked ridges, a mask per note, and the
///        notes themselves.
typedef struct SonarePolyphonicAnalysis SonarePolyphonicAnalysis;

/// @brief Configuration for @ref sonare_polyphonic_analyze.
/// @details Prepare one with @ref sonare_polyphonic_config_init, which sets
///          @c struct_size and every field's library default; every field is then
///          read literally (a 0 is a 0) and checked against the range given beside
///          it, an out-of-range value being refused with
///          SONARE_ERROR_INVALID_PARAMETER. Three fields keep a named meaning at 0
///          and say so: @c win_length, @c max_refine_hz and @c reattack_ratio.
///
///          The window function and the centred framing are not settable. Every
///          other entry point in this library states an STFT as a size and a hop,
///          and every span, claim and mask offset in this chain is derived against
///          one framing, so a second way to state it would be a second thing to keep
///          in agreement.
typedef struct {
  /// Size of this struct in bytes, set by @ref sonare_polyphonic_config_init. A smaller value is
  /// refused with SONARE_ERROR_INVALID_PARAMETER.
  uint32_t struct_size;

  // Framing. One STFT serves the extraction, the claims, the apportionment and the render, so
  // it is stated once. n_fft, hop_length and win_length are samples at 44100 Hz, converted to
  // the input's sample rate.

  /// STFT size in samples at 44100 Hz. Default 4096, the size this chain is tuned at; positive.
  int32_t n_fft;
  /// STFT hop in samples at 44100 Hz. Default 512; positive.
  int32_t hop_length;
  /// Analysis window length in samples at 44100 Hz. Default 0, which means n_fft; otherwise in
  /// [1, n_fft].
  int32_t win_length;

  // The cent axis the salience is folded onto.

  /// Frequency of the bottom of the cent axis in Hz. Default 55; positive.
  float cent_ref_hz;
  /// Axis resolution in cents per bin. Default 100/3; at least 1.
  float cents_per_bin;
  /// Frequency of the top of the cent axis in Hz. Default 8000; above cent_ref_hz.
  float cent_max_hz;
  /// Non-zero weights bins by tonality. Default 1.
  int32_t tonality;

  // Salience: what an f0 candidate is worth.

  /// Partials summed per f0 candidate. Default 20, in [1, 128].
  int32_t salience_harmonics;
  /// Lowest f0 candidate in Hz. Default 55; positive.
  float f0_min_hz;
  /// Highest f0 candidate in Hz. Default 1760; above f0_min_hz.
  float f0_max_hz;
  /// Offset of the harmonic weighting in Hz. Default 27; finite.
  float salience_alpha_hz;
  /// Scale of the harmonic weighting in Hz. Default 320; positive.
  float salience_beta_hz;
  /// Partial-series stretch assumed while scoring candidates. Default 0; non-negative.
  float salience_inharmonicity;

  // Estimation: how many voices a frame is allowed and when to stop.

  /// Most voices estimated in one frame. Default 4, in [1, 64].
  int32_t max_polyphony;
  /// Iteration stops once the next voice falls below this share of the frame's peak. Default
  /// 0.20, in [0, 1].
  float min_frame_peak_ratio;
  /// Closest two candidates of one frame may sit, in cents. Default 50; non-negative.
  float min_separation_cents;
  /// Share of a found voice removed before the next is sought. Default 1, in (0, 1].
  float subtraction_factor;

  // Tracking: which frames become one ridge.

  /// A pitch jump larger than this many cents breaks the ridge. Default 50; positive.
  float max_jump_cents;
  /// A fade below this share of the ridge's own peak breaks it. Default 0.10, in [0, 1].
  float min_ridge_peak_ratio;
  /// Ridges shorter than this many milliseconds are dropped. Default 140; non-negative.
  float min_ridge_duration_ms;
  /// Splits a ridge where its salience climbs past this multiple of the level just before, the
  /// same pitch struck again while it sounds. Default 0, which means no split; otherwise above 1.
  float reattack_ratio;

  // Claims: which bins a note asks for. A claim is predicted from the note's f0 and harmonic
  // number and never read from the spectrum, so a note claims bins its own partials never
  // reached.

  /// Partials claimed per note. Default 20, in [1, 128].
  int32_t mask_harmonics;
  /// Claim half-width in Hann main lobes. Default 1, in (0, 64].
  float claim_lobes;
  /// Stretch of the partial series, B in f_h = h*f0*sqrt(1 + B*h^2). Default 0; non-negative.
  /// Leaving it at 0 for material that is stretched costs more than a widened claim would: at a
  /// piano's 1e-4 the highest partial of a twenty-harmonic claim sits outside the claim
  /// entirely, and a partial outside every claim is residual, which is carried unedited and so
  /// keeps sounding at the old pitch after its note is moved.
  float inharmonicity;
  /// Non-zero fits a stretch per note from the spectrum instead of using the field above for
  /// every note. Default 0. Off by default because of what it reaches rather than what it
  /// costs: at this framing the fit takes an isolated note in the middle register and refuses a
  /// chord. A refused note keeps the declared stretch, so the fit only ever replaces a guess
  /// with a measurement; @ref sonare_polyphonic_note_inharmonicity reports which notes it
  /// reached. The three fields after it are read only while it is on.
  int32_t estimate_inharmonicity;
  /// Usable partials a stretch fit needs. Default 3, in [2, 128].
  int32_t inharmonicity_min_partials;
  /// Largest per-partial misfit kept by a stretch fit, in bins. Default 0.5; positive.
  float inharmonicity_max_residual_bins;
  /// A fitted stretch above this is refused. Default 0.03125; positive.
  float inharmonicity_max_stretch;

  // Apportionment: what the fit refuses rather than guesses, at a bin two notes both claimed. A
  // refused bin keeps the equal share.

  /// Frames per apportionment fit. Default 8, in [4, 64].
  int32_t window_frames;
  /// Two partials must differ by at least this many radians per frame. Default 0.01, in
  /// (0, pi].
  float min_partial_separation;
  /// Relative misfit ceiling of an apportionment fit. Default 0.02, in (0, 1].
  float max_fit_residual;
  /// Ceiling on one weight's modulus. A weight is a fitted component over the observed bin, so
  /// where two partials nearly cancel it exceeds one and the residual carries several times the
  /// input there. While every edit is identity that is inaudible: the notes and the residual
  /// still sum to the input. Lowering it trades separation for a quieter residual. Default 8; at
  /// least 1.
  float max_weight_modulus;
  /// Highest partial frequency in Hz usable to refine f0. Default 0, which derives one;
  /// otherwise positive.
  float max_refine_hz;
  /// Worst f0 error tolerated, in cents. Default 50, in (0, 1200].
  float f0_tolerance_cents;

  // Note spans. One note per tracked ridge; the span itself is the ridge's, not cut by these
  // fields (see min_ridge_duration_ms above to filter short ridges instead).

  /// Scales the reported f0 stability figure only, in cents; a mid-ridge pitch jump is never
  /// split into two notes. Default 50; finite.
  float segmentation_threshold_cents;
  /// Not read: one note per ridge means nothing here filters a short one. Default 30; finite.
  float min_note_ms;
  /// Reference frequency for cents in Hz. Default 440; positive.
  float reference_hz;
} SonarePolyphonicConfig;

/// @brief Sets @c struct_size and the library default of every field of @p config.
/// @details NULL is a no-op. In a build without the pitch editor only @c struct_size
///          is set, since @ref sonare_polyphonic_analyze refuses every call there.
void sonare_polyphonic_config_init(SonarePolyphonicConfig* config);

/// @brief Analyses @p samples into editable notes and returns a handle to it.
/// @details One pass: one STFT, the multi-F0 extraction over it, a claim set per
///          tracked ridge, the apportionment of the bins two notes stand on, and the
///          measured fields of each note. Every note comes back with the identity
///          edit, so rendering the result unchanged reproduces the analysis's own
///          round trip.
///
///          An analysis finding no notes is not an error. Silence, or material the
///          register of the framing cannot resolve, tracks no ridge; rendering that
///          is the residual alone, which is the whole round trip.
/// @param samples Source audio; @p length must be non-zero and must fit in an int,
///        every stage below taking the length as one. Also floored by the STFT
///        framing: the multi-F0 stage needs at least two frames at the
///        configured n_fft/hop_length, so audio shorter than roughly one
///        hop_length (~512 samples at the default hop) is refused too.
/// @param config Optional configuration prepared by @ref sonare_polyphonic_config_init;
///        NULL selects the defaults.
/// @param out Receives a handle the caller owns and must release with
///        @ref sonare_polyphonic_analysis_destroy. Set to NULL before validation.
/// @note SONARE_ERROR_INVALID_PARAMETER when @p length is zero, or too short for
///       at least two STFT frames at the configured n_fft/hop_length, when
///       @c config->struct_size is below sizeof, or when a field is outside its range.
/// @note SONARE_ERROR_NOT_SUPPORTED when the library was built without the pitch
///       editor.
SonareError sonare_polyphonic_analyze(const float* samples, size_t length, int sample_rate,
                                      const SonarePolyphonicConfig* config,
                                      SonarePolyphonicAnalysis** out);

/// @brief Releases an analysis. NULL is a no-op.
void sonare_polyphonic_analysis_destroy(SonarePolyphonicAnalysis* analysis);

/// @brief Number of notes, which is also the number of claim sets.
SonareError sonare_polyphonic_note_count(const SonarePolyphonicAnalysis* analysis,
                                         size_t* out_count);

/// @brief Number of STFT frames the analysis ran over.
/// @details An @c int32_t rather than the @c size_t the note count uses, and the
///          difference is deliberate: a frame is a signed 32-bit quantity throughout
///          this library -- a note's @c frame_start and @c frame_end among them -- so
///          a host bounds-checking a note against this compares like with like
///          instead of signed against unsigned. The @c size_t counts here count
///          elements; this one counts frames.
SonareError sonare_polyphonic_frame_count(const SonarePolyphonicAnalysis* analysis,
                                          int32_t* out_count);

/// @brief Copies the notes out, in the order their claim sets are held in.
/// @details Each note carries its sample span, its frame span, its median pitch, its
///          steadiness and its pending edit. Three fields of the by-value door have
///          no offset to report through a handle and are stated rather than left to
///          be guessed at: @c amplitude_offset and the edit's @c envelope_offset are
///          always 0, and the edit's @c envelope_count is how many points the note's
///          envelope holds. The curves themselves come from
///          @ref sonare_polyphonic_note_f0 and its three siblings, the envelope
///          included.
/// @param out Receives up to @p capacity notes.
/// @param out_count Receives the number written, which is the note count clamped to
///        @p capacity. Query @ref sonare_polyphonic_note_count first to size the
///        buffer; a short buffer is not an error.
SonareError sonare_polyphonic_notes(const SonarePolyphonicAnalysis* analysis, SonareNoteObject* out,
                                    size_t capacity, size_t* out_count);

/// @brief Replaces one note's pending edit.
/// @details The only thing a host writes. Everything else on a note is a measurement,
///          and the order is the pairing with the claim sets, so neither is settable.
/// @param note Index below @ref sonare_polyphonic_note_count.
/// @param edit The new edit, or NULL for the identity edit. Its @c envelope_offset
///        and @c envelope_count are ignored: the envelope is @p envelope here, which
///        the handle copies, so the caller's array need not outlive this call.
/// @param envelope Per-frame linear gain points over the note's span, on top of
///        @c gain_db, or NULL for none. Stretched over whatever length the note
///        renders at, so it survives a time stretch and need not match the note's
///        frame count; one entry is a constant gain. Every value must be finite and
///        non-negative.
/// @param envelope_count Number of points, or 0 for no envelope.
SonareError sonare_polyphonic_set_note_edit(SonarePolyphonicAnalysis* analysis, size_t note,
                                            const SonareNoteEdit* edit, const float* envelope,
                                            size_t envelope_count);

/// @brief Per-frame voice count, before tracking dropped anything.
/// @details What the estimation saw rather than what survived: a frame reported as
///          three voices with two notes spanning it is the difference between the two
///          stages, which is the figure a host deciding what to edit wants.
/// @param out Receives up to @p capacity counts, one per frame from frame 0, only on success.
/// @param out_count Receives the number written.
SonareError sonare_polyphonic_polyphony(const SonarePolyphonicAnalysis* analysis, int32_t* out,
                                        size_t capacity, size_t* out_count);

/// @brief One note's F0 in Hz, per frame over its own span.
/// @details @c frame_end - @c frame_start entries, so the value at index i belongs to
///          frame @c frame_start + i. This is the curve the monophonic door makes the
///          caller pass back in; here the handle already holds it, so a curve edit
///          needs nothing from the caller.
/// @param out_count Receives the number written. 0 where the note spans no frame the
///        ridge covers, which a hand-built analysis cannot produce here.
SonareError sonare_polyphonic_note_f0(const SonarePolyphonicAnalysis* analysis, size_t note,
                                      float* out, size_t capacity, size_t* out_count);

/// @brief One note's linear RMS, per frame over its own span.
/// @details Indexed exactly as @ref sonare_polyphonic_note_f0.
SonareError sonare_polyphonic_note_amplitude(const SonarePolyphonicAnalysis* analysis, size_t note,
                                             float* out, size_t capacity, size_t* out_count);

/// @brief One note's salience, per frame over its own span.
/// @details Indexed exactly as @ref sonare_polyphonic_note_f0, and the one curve here
///          that is not the note's own: it is the tracked ridge's, so a frame of the
///          note the ridge does not reach reads 0. Salience is what the estimation
///          scored the candidate at, so it says how well the material supported this
///          note rather than how loud the note is -- the amplitude above is the loud.
SonareError sonare_polyphonic_note_salience(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count);

/// @brief One note's amplitude envelope points, as last set.
/// @details Indexed from 0 rather than over the note's span: an envelope is a set of
///          gain points stretched over whatever length the note renders at, not a
///          per-frame signal. The only one of the four curves here that is not a
///          measurement -- these are the points a caller handed
///          @ref sonare_polyphonic_set_note_edit. Readable because a note reports its
///          own @c envelope_count, and a count whose points cannot be fetched is a
///          field promising what it cannot deliver.
SonareError sonare_polyphonic_note_envelope(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count);

/// @brief The stretch fitted for each note, where the fit was asked for.
/// @details One entry per note, in @ref sonare_polyphonic_notes' order:
///          non-negative where the stretch was fitted, and exactly -1 where it was
///          refused, which means that note's claims were placed at the config's
///          @c inharmonicity instead. **0 is a fitted result and means the harmonic
///          series**, so it is not the refusal.
///
///          The refusal has to be reported rather than folded away, because the
///          value a refused note ends up using is the declared one and the declared
///          one defaults to 0 -- which is also what a genuine fit returns for an
///          unstretched note. A host handed only the effective stretch could not
///          tell a fit that reached its material from one that did not, and the fit
///          refuses a chord at the default framing.
/// @param out_count Receives the number written, which is 0 when
///        @c estimate_inharmonicity was not set. An analysis that asked for the fit
///        reports one entry per note whatever happened to each.
SonareError sonare_polyphonic_note_inharmonicity(const SonarePolyphonicAnalysis* analysis,
                                                 float* out, size_t capacity, size_t* out_count);

/// @brief Renders the analysis back to audio with whatever edits its notes carry.
/// @details Each note's claimed share is inverted, edited, and added to the residual
///          -- the part of the input no note claimed. With every edit identity the
///          result is the analysis's own round trip, not the source bit for bit, the
///          STFT round trip's error being neither added to nor removed here.
///
///          The render is additive per note with no cross-note term, so an unedited
///          note's contribution is identical between two renders. That is worth
///          stating because it is also the limit: it means a host cannot tell from
///          two renders whether a claim set divided the energy correctly.
/// @param config Optional versioned configuration; NULL selects the defaults. The
///        same struct @ref sonare_render_notes takes, and its @c vibrato_cutoff_hz
///        has to be whatever a curve edit was drawn at.
/// @param out Receives the rendered audio, which has the source's length.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_polyphonic_render(const SonarePolyphonicAnalysis* analysis,
                                     const SonareNoteRenderConfig* config, float** out,
                                     size_t* out_length);

#ifdef __cplusplus
}
#endif
