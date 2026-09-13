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

/// @brief Versioned configuration for @ref sonare_polyphonic_analyze.
/// @details Zero-initialize for the defaults; @c struct_version 0 and 1 both select
///          the version-1 layout. Every field takes its default at 0, so a zeroed
///          struct and a NULL pointer behave alike.
///
///          Four fields accept 0 as a value as well as reading it as their default,
///          and are marked below. **Pass a negative number to select 0 on those.**
///          Every one of them rejects a negative otherwise, so the meaning cannot
///          collide with a setting a caller meant literally.
///
///          The window function and the centred framing are not settable. Every
///          other entry point in this library states an STFT as a size and a hop,
///          and every span, claim and mask offset in this chain is derived against
///          one framing, so a second way to state it would be a second thing to keep
///          in agreement.
typedef struct {
  int32_t struct_version;

  /* --- Framing. One STFT serves the extraction, the claims, the apportionment and
     the render, so it is stated once. --- */
  int32_t n_fft;      /* 0 => 4096, the size this chain is tuned at */
  int32_t hop_length; /* 0 => 512 */
  int32_t win_length; /* 0 => n_fft */

  /* --- The cent axis the salience is folded onto. --- */
  float cent_ref_hz;    /* bottom of the axis; 0 => 55 */
  float cents_per_bin;  /* axis resolution; 0 => 100/3, and under 1 cent is rejected */
  float cent_max_hz;    /* top of the axis; 0 => 8000 */
  int32_t tonality_off; /* non-zero stops weighting bins by tonality, which is on */

  /* --- Salience: what an f0 candidate is worth. --- */
  int32_t salience_harmonics;   /* partials summed per candidate; 0 => 20, at most 128 */
  float f0_min_hz;              /* 0 => 55 */
  float f0_max_hz;              /* 0 => 1760 */
  float salience_alpha_hz;      /* harmonic weighting offset; 0 => 27 */
  float salience_beta_hz;       /* harmonic weighting scale; 0 => 320 */
  float salience_inharmonicity; /* stretch assumed while scoring; 0 is the default and a value */

  /* --- Estimation: how many voices a frame is allowed and when to stop. --- */
  int32_t max_polyphony;      /* 0 => 4, at most 64 */
  float min_frame_peak_ratio; /* stop iterating below this share; 0 => 0.20, negative => 0 */
  float min_separation_cents; /* closest two candidates may sit; 0 => 50, negative => 0 */
  float subtraction_factor;   /* share of a found voice removed; 0 => 1 */

  /* --- Tracking: which frames become one ridge. --- */
  float max_jump_cents;        /* a larger jump breaks the ridge; 0 => 50 */
  float min_ridge_peak_ratio;  /* a fade below this share of the ridge's own peak breaks it;
                                  0 => 0.10, negative => 0 */
  float min_ridge_duration_ms; /* shorter ridges are dropped; 0 => 140, negative => 0 */

  /* --- Claims: which bins a note asks for. A claim is predicted from the note's f0
     and harmonic number and never read from the spectrum, so a note claims bins its
     own partials never reached. --- */
  int32_t mask_harmonics; /* partials claimed per note; 0 => 20, at most 128 */
  float claim_lobes;      /* claim half-width in Hann main lobes; 0 => 1 */
  /* Stretch of the partial series, B in f_h = h*f0*sqrt(1 + B*h^2). 0 is the default
     and a value. Leaving it at 0 for material that is stretched costs more than a
     widened claim would: at a piano's 1e-4 the highest partial of a twenty-harmonic
     claim sits outside the claim entirely, and a partial outside every claim is
     residual, which is carried unedited and so keeps sounding at the old pitch after
     its note is moved. */
  float inharmonicity;
  /* Fit a stretch per note from the spectrum instead of spending the field above on
     every one of them; non-zero turns it on. Off by default because of what it
     reaches rather than what it costs: at this framing the fit takes an isolated
     note in the middle register and refuses a chord. A refused note keeps the
     declared stretch, so the fit only ever replaces a guess with a measurement --
     @ref sonare_polyphonic_note_inharmonicity reports which notes it reached. */
  int32_t estimate_inharmonicity;
  int32_t inharmonicity_min_partials;    /* usable partials a fit needs; 0 => 3 */
  float inharmonicity_max_residual_bins; /* largest per-partial misfit kept; 0 => 0.5 */
  float inharmonicity_max_stretch;       /* a fit above this is refused; 0 => 0.03125 */

  /* --- Apportionment: what the fit refuses rather than guesses, at a bin two notes
     both claimed. A refused bin keeps the equal share. --- */
  int32_t window_frames;        /* frames per fit; 0 => 8, between 4 and 64 */
  float min_partial_separation; /* rad/frame two partials must differ by; 0 => 0.01 */
  float max_fit_residual;       /* relative misfit ceiling; 0 => 0.02 */
  /* Ceiling on one weight's modulus. A weight is a fitted component over the observed
     bin, so where two partials nearly cancel it exceeds one and the residual carries
     several times the input there. While every edit is identity that is inaudible --
     the notes and the residual still sum to the input. Lowering it trades separation
     for a quieter residual. 0 => 8. */
  float max_weight_modulus;
  float max_refine_hz;      /* highest partial usable to refine f0; 0 derives one */
  float f0_tolerance_cents; /* worst f0 error tolerated; 0 => 50 */

  /* --- Note spans, cut from each ridge. --- */
  float segmentation_threshold_cents; /* 0 => 50 */
  float min_note_ms;                  /* 0 => 30 */
  float reference_hz;                 /* cents reference; 0 => 440 */
} SonarePolyphonicConfig;

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
///        every stage below taking the length as one.
/// @param config Optional versioned configuration; NULL selects the defaults.
/// @param out Receives a handle the caller owns and must release with
///        @ref sonare_polyphonic_analysis_destroy. Set to NULL before validation.
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
/// @param out Receives up to @p capacity counts, one per frame from frame 0.
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
