#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Effects
// ============================================================================

SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, SonareHpssResult* out);
/// @brief Configurable HPSS wrapper.
/// @details The two-way form writes harmonic/percussive into @p out. When
///          @p with_residual is non-zero, @p out_residual is required and
///          receives the third signal; otherwise it is optional and remains
///          NULL. All output fields are reset before input/config validation.
/// @param n_fft      FFT size: an even integer >= 2. Any even size is accepted;
///                   the FFT is mixed-radix, not power-of-two only.
/// @param hop_length Hop in samples, in (0, n_fft/2]. The result is
///                   resynthesized by overlap-add and the phase-coherent
///                   spectral paths require at least half-window overlap; a
///                   sparser hop is rejected with
///                   SONARE_ERROR_INVALID_PARAMETER rather than returning a
///                   signal with periodic dropouts.
/// @note Free @p out with @ref sonare_free_hpss_result and @p out_residual with
///       @ref sonare_free_floats.
SonareError sonare_hpss_ex(const float* samples, size_t length, int sample_rate,
                           int kernel_harmonic, int kernel_percussive, int n_fft, int hop_length,
                           int use_soft_mask, int with_residual, SonareHpssResult* out,
                           float** out_residual);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate, float rate,
                                float** out, size_t* out_length);
/// @brief Native spectral time stretch with explicit FFT/hop configuration.
/// @details @p n_fft must be an even integer >= 2 and @p hop_length must lie in
///          (0, n_fft/2]; see @ref sonare_hpss_ex for why.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_time_stretch_ex(const float* samples, size_t length, int sample_rate, float rate,
                                   int n_fft, int hop_length, float** out, size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float semitones, float** out, size_t* out_length);
/// @brief Native spectral pitch shift with explicit FFT/hop configuration.
/// @details @p n_fft must be an even integer >= 2 and @p hop_length must lie in
///          (0, n_fft/2]; see @ref sonare_hpss_ex for why.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_pitch_shift_ex(const float* samples, size_t length, int sample_rate,
                                  float semitones, int n_fft, int hop_length, float** out,
                                  size_t* out_length);
/// Applies a single CONSTANT transposition: the whole buffer is treated as one
/// note at @p current_midi and shifted by (target_midi - current_midi). This is
/// an immediate transpose with no retune glide, and the returned buffer has
/// exactly the input length. The whole interval is applied however large it is:
/// both endpoints are validated to [0, 127], so a two-octave move such as
/// C3 -> C5 transposes by the full 24 semitones. This is not pitch tracking — it
/// does not follow a time-varying melody. For
/// contour-following correction use @ref sonare_pitch_correct_to_midi_timevarying
/// with a caller-supplied per-frame F0 track.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length);
/// @brief Per-frame ("time-varying") correction toward a fixed MIDI target.
/// @details Unlike @ref sonare_pitch_correct_to_midi (one constant transpose),
///          this follows a caller-supplied F0 contour: each of the @p n_frames
///          frames carries an @p f0_hz value (the measured pitch at that frame)
///          and the corrector retunes every voiced frame toward @p target_midi,
///          so vibrato/drift in the source is tracked rather than flattened.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
///                    A NaN is accepted only when the corresponding @p voiced
///                    flag is zero (matching pYIN's default unvoiced output).
///                    Finite values must be in [0, sample_rate/2].
/// @param voiced_prob Per-frame voicing probability [0,1] (@p n_frames entries),
///                    or NULL. It is used ONLY to derive voicing when @p voiced
///                    is NULL (>= 0.5 is voiced); when @p voiced is supplied it
///                    is ignored entirely. In particular it does NOT scale the
///                    per-frame correction amount, so passing
///                    @ref sonare_pitch_pyin's @c voiced_prob (a frequency-
///                    dependent observation mass, not a confidence) leaves the
///                    result identical to omitting it.
/// @param voiced      Per-frame voiced flags (non-zero = voiced; @p n_frames
///                    entries), or NULL to treat every frame as voiced.
/// @param hop_length  F0 hop in samples (> 0; frame i covers sample i*hop_length).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     int hop_length, float target_midi, float** out,
                                                     size_t* out_length);

/// @brief Target selector for @ref sonare_pitch_correct_timevarying.
typedef enum SONARE_ENUM_BASE {
  SONARE_PITCH_TARGET_FIXED_MIDI = 0,  ///< Retune every voiced frame toward @c target_midi.
  SONARE_PITCH_TARGET_SCALE = 1,       ///< Snap each voiced frame to the nearest scale degree.
} SonarePitchTargetMode;

/// @brief Tunable configuration for @ref sonare_pitch_correct_timevarying.
/// @details Zero-initialising this struct is NOT a valid default — populate it
///          via @ref sonare_pitch_correction_config_default first, then override
///          the fields you care about.
typedef struct {
  int32_t target_mode;         ///< @ref SonarePitchTargetMode.
  float target_midi;           ///< Target note when @c target_mode is FIXED_MIDI ([0,127]).
  int32_t scale_root;          ///< Scale root pitch class (0=C .. 11=B) when target_mode is SCALE.
  uint32_t scale_mode_mask;    ///< 12-bit degree mask, bit i = semitone i above the root enabled.
  float scale_reference_midi;  ///< Reference MIDI anchoring the scale grid (default 69 = A4).
  float retune_amount;         ///< Correction strength [0,1]; 1 = full snap, 0 = bypass.
  float max_correction_semitones;  ///< Hard clamp on per-frame correction magnitude.
  float retune_speed_ms;           ///< Retune IIR time constant (ms); larger = slower glide.
  float vibrato_threshold_cents;   ///< Corrections below this are bypassed to preserve vibrato.
} SonarePitchCorrectionConfig;

/// @brief Fills @p config with the library defaults (major scale, full retune).
/// @details Mirrors the core PitchCorrectionConfig defaults: FIXED_MIDI target,
///          C-major mask, reference A4, retune 1.0, 12-semitone clamp, 50 ms
///          glide, 20-cent vibrato threshold.
SonareError sonare_pitch_correction_config_default(SonarePitchCorrectionConfig* config);

/// @brief Per-frame pitch correction toward a fixed MIDI note OR a musical scale.
/// @details Generalises @ref sonare_pitch_correct_to_midi_timevarying: the same
///          caller-supplied F0 contour drives correction, but @p config selects
///          between a fixed-MIDI target and scale quantisation and exposes the
///          retune-strength / vibrato-preservation knobs. Pass NULL for @p config
///          to use the library defaults.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
///                    Unvoiced frames may contain NaN when @p voiced is zero;
///                    finite values must be in [0, sample_rate/2].
/// @param voiced_prob Per-frame voicing probability [0,1], or NULL. Used only
///                    to derive voicing when @p voiced is NULL; never a weight
///                    on the correction amount (see
///                    @ref sonare_pitch_correct_to_midi_timevarying).
/// @param voiced      Per-frame voiced flags (non-zero = voiced), or NULL.
/// @param hop_length  F0 hop in samples (> 0).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_timevarying(const float* samples, size_t length, int sample_rate,
                                             const float* f0_hz, const float* voiced_prob,
                                             const int32_t* voiced, size_t n_frames, int hop_length,
                                             const SonarePitchCorrectionConfig* config, float** out,
                                             size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
/// Move a note region to a new onset sample while preserving its duration.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_note_move(const float* samples, size_t length, int sample_rate, int onset_sample,
                             int offset_sample, int target_onset_sample, float** out,
                             size_t* out_length);
// ============================================================================
// Effects - Note objects
// ============================================================================

/// @brief Versioned configuration for @ref sonare_extract_notes.
/// @details Zero-initialize for the defaults (50 cents, 30 ms, A4 = 440 Hz,
///          voiced threshold 0.5). @c struct_version 0 and 1 both select the
///          version-1 layout. Every float takes its default at 0, so a zeroed
///          struct and a NULL pointer behave alike.
typedef struct {
  int32_t struct_version;
  float segmentation_threshold_cents;
  float min_note_ms;
  float reference_hz;
  /// Value of @c voiced_prob at or above which a frame counts as voiced. Read
  /// only when @c voiced is NULL. 0 keeps the default 0.5.
  ///
  /// pYIN's @c voiced_prob is a frame's voiced observation mass and rises with
  /// F0 for a fixed frame length, so the 0.5 default silently drops low
  /// registers: pass its @c voiced_flag through @c voiced instead.
  float voiced_threshold;
} SonareNoteExtractorConfig;

/// @brief Versioned configuration for @ref sonare_render_notes.
/// @details Zero-initialize for the defaults (5 ms fade, 3 Hz vibrato cutoff).
///          @c struct_version 0 and 1 both select the version-1 layout.
typedef struct {
  int32_t struct_version;
  /// Equal-power cross-fade at each edited note's edges. 0 keeps the default
  /// 5 ms; a hard cut is deliberately not selectable, because the seam it
  /// leaves behind is a click.
  float fade_ms;
  /// Boundary between the drift and the vibrato that @c vibrato_depth_change
  /// and @c drift_change act on, in Hz. 0 keeps the default 3 Hz.
  ///
  /// Pass whatever @ref sonare_decompose_note_pitch was called with. A host
  /// that draws the vibrato at one cutoff and edits it at another edits a
  /// curve it never showed anyone.
  float vibrato_cutoff_hz;
} SonareNoteRenderConfig;

/// @brief A pending, non-destructive change to one note.
/// @details Zero-initializing gives the identity edit: @c time_stretch_ratio 0
///          reads as 1 and an envelope of zero length is no envelope, so a
///          zeroed struct changes nothing.
typedef struct {
  /// Moves the note along the timeline. Negative moves it earlier.
  int64_t time_offset_samples;
  /// Index of this note's amplitude envelope in the envelope array that travels
  /// with the note set -- @c envelopes on a result, the @c envelopes argument
  /// on @ref sonare_render_notes. Ignored when @c envelope_count is 0.
  ///
  /// An offset rather than a pointer so that the field means the same thing on
  /// the way in and on the way out: the caller owns the array it passes, the
  /// library owns the one it returns, and neither ever frees the other's.
  int64_t envelope_offset;
  /// Number of envelope points, or 0 for no envelope.
  ///
  /// Per-frame linear gain over the note's span, on top of @c gain_db. It is a
  /// set of gain points rather than a signal: it is stretched over whatever
  /// length the note renders at, so it survives a time stretch and need not
  /// match the note's frame count. One entry is a constant gain.
  size_t envelope_count;
  float pitch_shift_semitones;
  float gain_db;
  /// >1 lengthens the note, <1 shortens it; pitch is preserved. 0 reads as 1.
  float time_stretch_ratio;
  /// Moves the spectral envelope, in semitones, on top of whatever the pitch
  /// shift already did to it.
  ///
  /// 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
  /// analysis-resynthesis round it did not ask for. A pitch shift drags the
  /// formants with it, so holding them still is -@c pitch_shift_semitones and
  /// the chipmunk is the default. Saturates near -10.3 and +8.7 semitones
  /// rather than being rejected.
  float formant_shift_semitones;
  /// Scales the vibrato measured over the note, stated as a change from it:
  /// 0 keeps it, -1 flattens it, +1 doubles it.
  ///
  /// Applying it needs a pitch curve, so a note carrying none is rejected
  /// rather than left alone. The curve is split at
  /// @c SonareNoteRenderConfig::vibrato_cutoff_hz.
  float vibrato_depth_change;
  /// The same, for the slow drift around the note's centre pitch.
  float drift_change;
  /// Non-zero silences the note's span; the other fields then do not apply.
  int32_t muted;
} SonareNoteEdit;

/// One editable note. Sample bounds are half-open into the source audio; frame
/// bounds are half-open into the caller's own F0 track.
typedef struct {
  int64_t onset_sample;
  int64_t offset_sample;
  /// Index of this note's first RMS value in the owning result's @c amplitude
  /// array; the note occupies @c frame_end - @c frame_start entries from there.
  /// Set on output and ignored by @ref sonare_render_notes.
  int64_t amplitude_offset;
  int32_t frame_start;
  int32_t frame_end;
  float median_hz;
  /// Median pitch in cents above the config's @c reference_hz.
  float median_cents;
  /// Pitch steadiness in [0, 1], from the median absolute deviation of the
  /// span's cents against the segmentation threshold. 1 is perfectly steady.
  ///
  /// The only quality figure a note carries. A voiced fraction would be one
  /// too, but the segmenter emits maximal voiced runs, so it is 1 for every
  /// note it can produce and measures nothing.
  float f0_stability;
  SonareNoteEdit edit;
} SonareNoteObject;

/// Heap-owned note-object output. Release with @ref sonare_free_note_objects.
/// @details The per-note F0 curve is not repeated here: it is the caller's own
///          @c f0_hz sliced by @c [frame_start, frame_end). The amplitude curve
///          is new, so it is returned -- one RMS per F0 frame over that frame's
///          samples, concatenated in note order.
typedef struct {
  SonareNoteObject* notes;
  size_t count;
  float* amplitude;
  size_t amplitude_count;
  /// Amplitude envelopes the notes' edits index through @c envelope_offset.
  /// NULL unless an entry point carried envelopes in: extraction produces
  /// identity edits, so only @ref sonare_split_note and @ref sonare_merge_notes
  /// ever fill this.
  float* envelopes;
  size_t envelope_count;
} SonareNoteObjectsResult;

/// @brief Extract editable note objects from audio and an F0 track.
/// @details Spans come from the same segmenter @ref sonare_note_segments uses.
///          Each note additionally carries its median pitch, its two measured
///          quality figures, and its slice of the amplitude curve. Every
///          returned note has the identity edit.
/// @param samples Source audio; @p length must be non-zero.
/// @param f0_hz Per-frame F0 in Hz, @p n_frames entries. Every value must be
///        finite and non-negative; zero denotes an unvoiced frame.
/// @param voiced_prob Per-frame voicing in [0, 1], or NULL. Read only when
///        @p voiced is NULL, and then required.
/// @param voiced Per-frame voiced flags (non-zero = voiced), or NULL.
/// @param n_frames Number of F0 frames; must be non-zero.
/// @param frame_rate F0 frames per second; must be finite and > 0.
/// @param config Optional versioned configuration; NULL selects the defaults.
/// @param out Receives a heap-owned result, cleared before validation. An empty
///        segmentation is returned as NULL pointers with zero counts.
SonareError sonare_extract_notes(const float* samples, size_t length, int sample_rate,
                                 const float* f0_hz, const float* voiced_prob,
                                 const int32_t* voiced, size_t n_frames, float frame_rate,
                                 const SonareNoteExtractorConfig* config,
                                 SonareNoteObjectsResult* out);
void sonare_free_note_objects(SonareNoteObjectsResult* result);

/// @brief Render edited note objects over their source audio.
/// @details Reads each note's sample span and its @c edit, plus the frame
///          bounds whenever @p f0_hz is given and @c median_hz when a curve
///          edit needs it; the metrics and the amplitude offset are ignored, so
///          a host may pass back exactly what @ref sonare_extract_notes
///          produced.
///          A note whose edit is the identity is not resynthesized, so a set
///          whose edits are all identity reproduces the input bit for bit.
///
///          Overlap is checked on the source spans only. Where
///          @c time_offset_samples lands a note is not, and a note lengthened
///          past its own span writes into its neighbours' samples, so two moved
///          or stretched notes may be written over each other.
///          Per note the order is: pitch curve, time stretch, pitch shift,
///          formant warp, amplitude envelope, then gain.
/// @param notes May be NULL when @p note_count is 0.
/// @param envelopes Amplitude envelope points the notes' edits index into, or
///        NULL when no note carries one. Caller-owned and only read here. Every
///        value must be finite and non-negative, and every note's
///        @c [envelope_offset, envelope_offset + envelope_count) must lie
///        inside @p envelope_count.
/// @param envelope_count Number of entries in @p envelopes.
/// @param f0_hz The F0 track the notes were extracted from, or NULL. Required
///        only by @c vibrato_depth_change and @c drift_change, which act on the
///        note's own pitch curve; every other edit ignores it. The curve is not
///        carried on @ref SonareNoteObject for the same reason it is not carried
///        on a result -- it is this array sliced by @c [frame_start,
///        frame_end), which the caller already holds.
///
///        Given one, every note is sliced and so every note's bounds are
///        checked, whatever its edit does with the result: an out-of-range
///        @c frame_end is rejected rather than read past the end of the track.
/// @param n_frames Number of F0 frames, or 0 when @p f0_hz is NULL.
/// @param frame_rate F0 frames per second; must be finite and > 0 when @p f0_hz
///        is given.
/// @param config Optional versioned configuration; NULL selects the defaults.
/// @param out Receives the rendered audio, which has the input's length.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_render_notes(const float* samples, size_t length, int sample_rate,
                                const SonareNoteObject* notes, size_t note_count,
                                const float* envelopes, size_t envelope_count, const float* f0_hz,
                                size_t n_frames, float frame_rate,
                                const SonareNoteRenderConfig* config, float** out,
                                size_t* out_length);

/// One note's pitch curve split into a centre, a slow drift and a vibrato.
/// Release with @ref sonare_free_pitch_decomposition.
typedef struct {
  /// The note's steady pitch in Hz. 0 when the note carries no usable pitch,
  /// and then both curves are empty.
  float centre_hz;
  /// Slow deviation from @c centre_hz in cents, @c count entries.
  float* drift_cents;
  /// Fast deviation in cents, over the same frames.
  float* vibrato_cents;
  size_t count;
} SonarePitchDecompositionResult;

/// @brief Split one note's pitch curve into a centre, a drift and a vibrato.
/// @details @c drift_cents[i] + @c vibrato_cents[i] is the note's own pitch at
///          frame i, in cents above @c centre_hz, to within float rounding, so
///          the three parts reconstruct the curve. The drift filter is zero
///          phase, so neither curve is shifted in time against the audio.
///
///          Frames whose F0 is unusable carry no measurement, so the curve is
///          held at the nearest usable neighbour across them. Both curves
///          therefore have an entry everywhere; a host marking the held ones
///          reads them off @p f0_hz, which is exact.
///
///          The note's own F0 curve is not returned by
///          @ref sonare_extract_notes, so pass the caller's own @c f0_hz sliced
///          by the note's @c [frame_start, frame_end) together with its
///          @c median_hz.
/// @param f0_hz The note's slice of the F0 track, @p n_frames entries. Every
///        value must be finite and non-negative; zero denotes an unvoiced
///        frame.
/// @param frame_rate F0 frames per second; must be finite and > 0.
/// @param median_hz The note's @c median_hz; must be finite and non-negative.
///        A note with no pitch is spelled 0, so a negative or non-finite value
///        is a caller bug rather than a second way of saying that.
/// @param vibrato_cutoff_hz Boundary between the two curves; must be finite and
///        non-negative. 0 keeps the default 3 Hz. Hand the same value to
///        @ref sonare_render_notes.
/// @param out Receives a heap-owned result, cleared before validation. A note
///        with no usable pitch is reported as a zero centre and NULL curves
///        rather than as an error.
SonareError sonare_decompose_note_pitch(const float* f0_hz, size_t n_frames, float frame_rate,
                                        float median_hz, float vibrato_cutoff_hz,
                                        SonarePitchDecompositionResult* out);
void sonare_free_pitch_decomposition(SonarePitchDecompositionResult* result);

/// @brief Split one note in two at a track frame.
/// @details Both halves are re-derived from the audio and the track the way
///          @ref sonare_extract_notes derives its own, rather than by patching
///          the fields of the note they replace.
///
///          Both inherit the source note's edit, and its amplitude envelope is
///          cut at the same proportion so each half keeps its own part of it,
///          which means a note whose edit is the identity still renders bit for
///          bit after being split. A one-entry envelope is a constant over the
///          span, so both halves get that same entry.
///          Every note in the set -- not just the two halves -- has its spans,
///          curves, medians and stability re-derived from @p samples and the
///          track, because @ref SonareNoteObject carries no curves for this
///          call to copy through. The frame bounds are therefore what a note is
///          identified by here, and the audio and track arguments must be the
///          ones the set was extracted from or the whole set is re-measured
///          against something else.
/// @param notes The current note set. Each note's @c [frame_start, frame_end)
///        must be non-empty and inside the track.
/// @param envelopes The envelope array @p notes index into, or NULL.
/// @param index Note to split.
/// @param frame Track frame to cut at, strictly inside the note's own span.
/// @param out Receives the whole new note set, cleared before validation, with
///        its own @c envelopes array.
SonareError sonare_split_note(const float* samples, size_t length, int sample_rate,
                              const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                              size_t n_frames, float frame_rate,
                              const SonareNoteExtractorConfig* config,
                              const SonareNoteObject* notes, size_t note_count,
                              const float* envelopes, size_t envelope_count, size_t index,
                              int32_t frame, SonareNoteObjectsResult* out);

/// @brief Join a run of notes into one.
/// @details The result spans from the first note's onset to the last note's
///          offset, including whatever the segmenter cut out between them, and
///          its measured fields are derived over that whole span -- the pitch
///          and amplitude of an unvoiced gap live in the track and the audio,
///          not in either neighbour.
///
///          It takes @p notes[first]'s edit, envelope included. Notes carrying
///          different edits have no single correct answer here, so the rule is
///          stated rather than guessed at; a host that cares sets the edit
///          afterwards.
///
///          Every note in the set is re-derived from @p samples and the track,
///          exactly as @ref sonare_split_note describes.
/// @param notes The current note set. Each note's @c [frame_start, frame_end)
///        must be non-empty and inside the track.
/// @param first Index of the first note to join; must be < @p last.
/// @param last Index of the last note to join, inclusive.
/// @param out Receives the whole new note set, cleared before validation, with
///        its own @c envelopes array.
SonareError sonare_merge_notes(const float* samples, size_t length, int sample_rate,
                               const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                               size_t n_frames, float frame_rate,
                               const SonareNoteExtractorConfig* config,
                               const SonareNoteObject* notes, size_t note_count,
                               const float* envelopes, size_t envelope_count, size_t first,
                               size_t last, SonareNoteObjectsResult* out);

// ============================================================================
// Effects - Percussive events
// ============================================================================

/// @brief Versioned configuration for @ref sonare_extract_percussive_events.
/// @details Zero-initialize for the defaults (2048-point FFT, 512 hop, 31-frame
///          median kernels, onset delta 0.06, one frame of minimum spacing, a
///          500 ms span cap and no ratio filter). @c struct_version 0 and 1 both
///          select the version-1 layout. Every field takes its default at 0, so
///          a zeroed struct and a NULL pointer behave alike.
typedef struct {
  int32_t struct_version;
  /// FFT size and hop the separation and the onset detector share. They cannot
  /// be set apart: an event measured on one framing and lifted out on another is
  /// not the same signal. 0 keeps 2048 and 512. The pair must overlap-add --
  /// @c n_fft even and at least 2, @c hop_length no more than half of it --
  /// because the separation inverts an STFT. A negative value is rejected on its
  /// own, before the zero-is-default rule could swallow it.
  int32_t n_fft;
  int32_t hop_length;
  /// Median filter lengths the separation runs, along time and along frequency.
  /// A longer harmonic kernel calls more of a sustained sound harmonic. 0 keeps
  /// the default 31; any other value must be odd and positive, so an even one is
  /// rejected rather than rounded. 1 is legal and degenerate -- a length-1
  /// median is the identity, so both components come back as the source.
  int32_t hpss_kernel_harmonic;
  int32_t hpss_kernel_percussive;
  /// Minimum frames between consecutive onsets. 0 keeps the default 1; negative
  /// is rejected.
  int32_t onset_wait;
  /// Offset added to the detector's adaptive threshold; raising it finds fewer,
  /// stronger hits and lowering it finds more. 0 keeps the default 0.06, so
  /// exactly zero is the one value not selectable from here -- a negative one is
  /// accepted and puts the threshold below the default, which is the direction
  /// a caller reaching for zero wanted anyway.
  ///
  /// It is in the units of @c SonarePercussiveEvent::strength, which is the
  /// onset envelope's own scale and is not normalized -- three isolated hits
  /// measured 37 to 51 on one fixture, where the 0.06 default selects nothing at
  /// all. So read a useful value off the strengths a default extraction returns
  /// rather than guessing one; a number chosen on the assumption that the scale
  /// is around 1 will look like a knob that does nothing.
  float onset_delta;
  /// Caps a span that no onset follows. It binds at the end of a phrase and at
  /// the end of the track; anywhere else the next onset closes the span first.
  /// 0 keeps the default 500 ms.
  float max_event_ms;
  /// Drops an event whose @c percussive_ratio falls below this; must be in
  /// [0, 1]. 0 is both the default and the meaningful "keep everything", so
  /// nothing is lost here to the rule that 0 selects the default. Raising it is
  /// useful on material that is mostly drums and wrong on a dense mix, where it
  /// also drops real hits sitting over a loud sustain.
  float min_percussive_ratio;
} SonarePercussiveEventConfig;

/// @brief Versioned configuration for @ref sonare_render_percussive_events.
/// @details Zero-initialize for the defaults (2048-point FFT, 512 hop, 31-frame
///          median kernels, 5 ms fade). @c struct_version 0 and 1 both select
///          the version-1 layout.
typedef struct {
  int32_t struct_version;
  /// The separation the events were measured against. Pass back what
  /// @ref sonare_extract_percussive_events was called with: a different
  /// separation lifts a different signal out of the span than the one the
  /// events describe. Validated even when every edit is the identity and no
  /// separation runs, so an unusable framing is an error on every set.
  int32_t n_fft;
  int32_t hop_length;
  int32_t hpss_kernel_harmonic;
  int32_t hpss_kernel_percussive;
  /// Fade-out at the tail of each lifted span. 0 keeps the default 5 ms, so a
  /// zero-length fade is unreachable from here rather than rejected -- a zeroed
  /// struct has to mean the defaults, and that outranks passing the core's own
  /// refusal through. A hard cut is not a thing to want anyway: what the fade
  /// shapes is the signal being subtracted, so squaring it off leaves a step.
  ///
  /// There is deliberately no matching fade-in: a span opens in front of its
  /// transient, where the percussive component is near-silent, so cutting square
  /// there costs nothing and keeps a muted hit's attack from surviving inside a
  /// fade.
  float fade_ms;
} SonarePercussiveRenderConfig;

/// @brief A pending, non-destructive change to one percussive event.
/// @details Zero-initializing gives the identity edit. A struck sound has no
///          steady pitch to edit, so the axes are time and amplitude and there
///          is deliberately nothing else here.
typedef struct {
  /// Moves the hit along the timeline. Negative moves it earlier.
  int64_t time_offset_samples;
  float gain_db;
  /// Non-zero silences the hit; the other fields then do not apply.
  int32_t muted;
} SonarePercussiveEventEdit;

/// One editable percussive event. Sample bounds are half-open into the source
/// audio.
/// @details It carries no pitch and is never associated with a note object: the
///          two models are produced by separate calls and do not refer to each
///          other. The signal an edit acts on is the percussive component of the
///          span rather than the span itself, which is why the three measured
///          figures are taken on that component.
typedef struct {
  int64_t onset_sample;
  int64_t offset_sample;
  /// Detector strength at the onset, on the onset envelope's own scale. It
  /// orders events against each other and carries no absolute meaning.
  float strength;
  /// Peak absolute sample of the percussive component over the span, linear.
  /// Measured on the signal @c gain_db scales rather than on the source.
  float peak_amplitude;
  /// Share of the span's energy the separation assigned to percussion, in
  /// [0, 1]. 0 when the span is silent.
  ///
  /// It describes the span rather than the onset that opened it. An isolated hit
  /// sits near 1, but a real hit over a loud sustain sits near 0, because the
  /// sustain owns the span's energy. So it separates a hit from a note attack
  /// only where nothing is sustaining through both, and it is not a test for
  /// whether a hit is there.
  float percussive_ratio;
  SonarePercussiveEventEdit edit;
} SonarePercussiveEvent;

/// Heap-owned percussive-event output. Release with
/// @ref sonare_free_percussive_events.
/// @details One allocation, unlike @ref SonareNoteObjectsResult: an event
///          carries three scalars and nothing per frame, so there is no
///          companion curve array to travel with the set.
typedef struct {
  SonarePercussiveEvent* events;
  size_t count;
} SonarePercussiveEventsResult;

/// @brief Extract editable percussive events from audio.
/// @details Onsets are detected on the percussive component rather than on the
///          source, so a harmonic attack is attenuated before the detector sees
///          it instead of being filtered out afterwards. Each onset opens a span
///          that the next one closes, and every returned event has the identity
///          edit.
///
///          Each onset is backtracked to the transient's start, which is not
///          optional and is why there is no knob for it here: peak-picking lands
///          after the attack, and a span that opened there would report the next
///          hit's peak and leave its own attack behind when muted.
///
///          Spans are fixed before @c min_percussive_ratio drops anything, so
///          raising the threshold selects events without lengthening the ones
///          that survive.
/// @param samples Source audio; @p length must be non-zero.
/// @param sample_rate Sample rate in Hz; must be > 0.
/// @param config Optional versioned configuration; NULL selects the defaults.
/// @param out Receives a heap-owned result, cleared before validation. Audio in
///        which nothing was detected is returned as a NULL pointer and a zero
///        count rather than as an error. Events come back in ascending sample
///        order with no two spans overlapping and every span inside @p samples,
///        so the set is renderable against the same audio without being sorted
///        or repaired first.
SonareError sonare_extract_percussive_events(const float* samples, size_t length, int sample_rate,
                                             const SonarePercussiveEventConfig* config,
                                             SonarePercussiveEventsResult* out);

/// @brief Release a result from @ref sonare_extract_percussive_events.
/// @details NULL-safe, and clears the struct it releases, so calling it twice on
///          the same result is harmless. A result that came back empty owns
///          nothing and may still be passed here.
void sonare_free_percussive_events(SonarePercussiveEventsResult* result);

/// @brief Render edited percussive events over their source audio.
/// @details Per event the lifted signal is the percussive component over
///          @c [onset_sample, offset_sample) under the tail fade. It is
///          subtracted where it sits and, unless the event is muted, added back
///          at the shifted position scaled by the gain. Only that signal moves,
///          so muting a hit leaves the harmonic content under it sounding and
///          moving one does not drag its neighbours' sustain along.
///
///          Reads each event's span and its @c edit; @c strength,
///          @c peak_amplitude and @c percussive_ratio are ignored, so a host may
///          pass back exactly what @ref sonare_extract_percussive_events
///          produced. A set whose edits are all identity reproduces the input
///          bit for bit and runs no separation at all.
///
///          Overlap is checked on the source spans only. Where
///          @c time_offset_samples lands an event is not, so two moved events
///          may be written over each other. A shift that pushes the signal past
///          either end is truncated there rather than wrapped.
/// @param events The event set; may be NULL when @p count is 0. Every span must
///        be non-empty and inside @p samples, and no two may overlap. That is
///        checked for every event including the ones whose edit is the identity:
///        an unrenderable set is unrenderable whether or not this call would
///        have touched the offending event.
/// @param config Optional versioned configuration; NULL selects the defaults.
/// @param out Receives the rendered audio, which has the input's length.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_render_percussive_events(const float* samples, size_t length, int sample_rate,
                                            const SonarePercussiveEvent* events, size_t count,
                                            const SonarePercussiveRenderConfig* config, float** out,
                                            size_t* out_length);

/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                float pitch_semitones, float formant_factor, float** out,
                                size_t* out_length);
/// @brief Convenience offline wrapper around the realtime voice changer chain.
/// @details Creates a temporary @ref SonareRealtimeVoiceChanger from @p preset,
///          processes the whole mono or interleaved stereo buffer in realtime-
///          sized blocks, and destroys the handle before returning. @p preset
///          may be NULL/empty (neutral-monitor), a preset id, or a full JSON
///          config document accepted by @ref sonare_realtime_voice_changer_create_json.
///          @p channels must be 1 (mono) or 2 (interleaved stereo).
/// @details The chain's processing latency (retune grain + ISP-limiter lookahead)
///          is compensated: an equal-length silent tail is flushed and the
///          leading @c latency_samples of pre-roll are dropped, so the returned
///          buffer is the same length as @p samples AND time-aligned with the
///          input (no head silence / tail truncation).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_voice_change_realtime(const float* samples, size_t length, int sample_rate,
                                         const char* preset, int channels, float** out,
                                         size_t* out_length);

/// @brief Flat POD mirror of @c editing::voice_changer::RealtimeVoiceChangerConfig
///        for C callers that want to avoid the JSON round-trip.
/// @details Field ordering follows the nested C++ struct (top-level →
///          retune → formant → eq → gate → compressor → deesser → reverb →
///          limiter). Values pass through @c normalize_realtime_voice_changer_config
///          before being applied, so out-of-range entries are clamped rather than
///          rejected (matching the JSON entry point).
typedef struct {
  float input_gain_db;
  float output_gain_db;
  float wet_mix;

  float retune_semitones;
  float retune_mix;
  int retune_grain_size;

  float formant_factor;
  float formant_amount;
  float formant_body;
  float formant_brightness;
  float formant_nasal;

  float eq_highpass_hz;
  float eq_body_db;
  float eq_presence_db;
  float eq_air_db;

  float gate_threshold_db;
  float gate_attack_ms;
  float gate_release_ms;
  float gate_range_db;

  float compressor_threshold_db;
  float compressor_ratio;
  float compressor_attack_ms;
  float compressor_release_ms;
  float compressor_makeup_gain_db;

  float deesser_frequency_hz;
  float deesser_threshold_db;
  float deesser_ratio;
  float deesser_range_db;

  float reverb_mix;
  float reverb_time_ms;
  float reverb_damping;
  int reverb_seed;

  float limiter_ceiling_db;
  float limiter_release_ms;

  /// @brief Enables the optional 4x-oversampled inter-sample peak (true-peak)
  ///        limiter as the final output stage (non-zero = enabled). Mirrors
  ///        editing::voice_changer::LimiterConfig::enable_isp_limiter. Defaults
  ///        to 1 (enabled).
  int limiter_enable_isp_limiter;
  /// @brief True-peak ceiling in dBTP applied by the ISP limiter when
  ///        @ref limiter_enable_isp_limiter is non-zero. Mirrors
  ///        editing::voice_changer::LimiterConfig::isp_ceiling_dbtp. Defaults to
  ///        -1.0 dBTP.
  float limiter_isp_ceiling_dbtp;
} SonareRealtimeVoiceChangerConfig;

// Verify the POD struct layout is stable. The ABI version constant
// SONARE_VOICE_CHANGER_ABI_VERSION below MUST be bumped whenever this size
// or any field offset changes. Bindings that rely on POD memcpy across the
// FFI boundary (Rust FFI, raw C ABI consumers) read this size at compile
// time and detect ABI drift before a single byte is exchanged.
//
// Layout: 33 float fields + 3 int fields, every member is 4 bytes and
// 4-byte aligned -> no struct padding on any target we ship. Exact equality
// (not >=) so silent padding insertion fails the check too.
#ifdef __cplusplus
static_assert(sizeof(SonareRealtimeVoiceChangerConfig) == 36u * sizeof(float),
              "SonareRealtimeVoiceChangerConfig unexpected size");
#endif

#include "sonare_c_voice_changer.h"

/// @brief Compile-time mirror of the runtime ABI version returned by
///        @ref sonare_voice_changer_abi_version. Bindings can `static_assert` /
///        `assertEqual` the runtime value against this at attach time.
#define SONARE_VOICE_CHANGER_ABI_VERSION 2u

/// @brief Returns the runtime ABI version of the
///        @ref SonareRealtimeVoiceChangerConfig POD layout.
/// @details Bindings that pass the POD struct across the C ABI (Rust, raw C
///          consumers) call this at attach time and compare against their
///          compile-time expectation; a mismatch means the host libsonare was
///          built against a different struct layout and the POD path would
///          corrupt memory. JSON-based bindings (Node/Python via
///          @ref sonare_realtime_voice_changer_create_json) are tolerant of
///          layout drift and do not need to gate on this.
///
///          Distinct from @ref sonare_engine_abi_version (which tracks the
///          realtime command queue layout) so that voice-changer-only
///          consumers can pin a narrower compatibility envelope.
///
///          Always equals @ref SONARE_VOICE_CHANGER_ABI_VERSION at the time
///          libsonare was built.
uint32_t sonare_voice_changer_abi_version(void);

#include "sonare_c_engine.h"

/// @brief Peak-normalize mono audio. @p target_db must be finite and <= 0 dBFS;
/// positive targets are rejected rather than hard-clipped.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        float** out, size_t* out_length);
/// @brief RMS-normalize mono audio with hard clipping to [-1, 1].
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_normalize_rms(const float* samples, size_t length, int sample_rate,
                                 float target_db, float** out, size_t* out_length);
/// @brief Trim leading/trailing silence using an absolute RMS dBFS threshold.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_trim_ex(const float* samples, size_t length, int sample_rate, float threshold_db,
                           int frame_length, int hop_length, float** out, size_t* out_length);

/// @brief Non-negative matrix factorisation of a non-negative spectrogram
///        (mirror of @c sonare::decompose / librosa.decompose.decompose).
/// @details Both output matrices are heap-allocated row-major and MUST be
///          released with @ref sonare_free_floats. @p out_w is the component
///          matrix [n_features x n_components] (length n_features*n_components)
///          and @p out_h is the activation matrix [n_components x n_frames]
///          (length n_components*n_frames). Multi-matrix shape: returned as two
///          flat buffers because sonare_c_types.h has no decompose result struct.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0
///        (n_iter==0 would return the raw init matrices and is rejected).
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
/// @note Uses deterministic random initialisation. For the SVD-based NNDSVD
///       warm-start (faster convergence), use @ref sonare_decompose_with_init.
SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, float** out_w, size_t* out_w_length,
                             float** out_h, size_t* out_h_length);

/// @brief Non-negative matrix factorisation with a selectable initialiser
///        (mirror of @c sonare::decompose with the @c init argument).
/// @details Identical to @ref sonare_decompose but exposes the initialisation
///          strategy so callers can opt into the NNDSVD warm-start, which tends
///          to converge in fewer iterations. Both output matrices are
///          heap-allocated row-major and MUST be released with
///          @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major (non-negative).
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param n_components Target number of components (k). Must be > 0.
/// @param n_iter Number of multiplicative-update iterations. Must be > 0.
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param init Initialiser: "random" (default if NULL) or "nndsvd".
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
SonareError sonare_decompose_with_init(const float* s, int n_features, int n_frames,
                                       int n_components, int n_iter, float beta, const char* init,
                                       float** out_w, size_t* out_w_length, float** out_h,
                                       size_t* out_h_length);

/// @brief Versioned options for @ref sonare_decompose_stems.
/// @details Zero-initialize for the defaults (4 components, 2048/512 STFT,
///          100 iterations, Frobenius beta, random init, magnitude mask).
typedef struct {
  int struct_version; /* 0 or 1 => version 1 */
  int n_components;   /* 0 => 4 */
  int n_fft;          /* 0 => 2048 */
  int hop_length;     /* 0 => 512 */
  int n_iter;         /* 0 => 100 */
  float beta;         /* 0 => 2 (Frobenius); 1 = KL, use a tiny value for IS */
  const char* init;   /* NULL => "random"; "nndsvd" also accepted */
  float mask_power;   /* 0 => 1 (magnitude ratio); 2 = Wiener-style power ratio */
} SonareDecomposeStemsConfig;

/// @brief NMF separation that CARRIES the original phase, so each component is
///        directly listenable.
/// @details @ref sonare_decompose returns W / H factors of a magnitude
///          spectrogram, which have no phase; reconstructing from them needs a
///          phase estimator (@ref sonare_griffin_lim), and an estimated phase
///          does not hold up as a stem. This builds a soft mask per component
///          from the factorisation and applies it to the ORIGINAL complex
///          spectrogram, so every component keeps the source's phase. The masks
///          sum to one wherever the model has energy and the inverse STFT is
///          linear, so the components sum back to the input.
/// @param samples Input signal (mono).
/// @param length Number of samples.
/// @param sample_rate Sample rate in Hz.
/// @param config Optional versioned options; NULL selects the defaults.
/// @param out Receives one heap buffer per component, laid out as a single
///        flat array of @p out_component_count * @p out_component_length
///        floats (component c starts at c * @p out_component_length). Release
///        with @ref sonare_free_floats.
/// @param out_component_count Receives the number of components.
/// @param out_component_length Receives the per-component sample count (equal
///        to @p length).
/// @param out_w Optional; receives the [n_bins x n_components] component matrix
///        that produced the masks. NULL skips it. Release with
///        @ref sonare_free_floats.
/// @param out_w_length Optional; receives n_bins * n_components. Required when
///        @p out_w is non-NULL.
/// @param out_h Optional; receives the [n_components x n_frames] activation
///        matrix. NULL skips it. Release with @ref sonare_free_floats.
/// @param out_h_length Optional; receives n_components * n_frames. Required
///        when @p out_h is non-NULL.
SonareError sonare_decompose_stems(const float* samples, size_t length, int sample_rate,
                                   const SonareDecomposeStemsConfig* config, float** out,
                                   size_t* out_component_count, size_t* out_component_length,
                                   float** out_w, size_t* out_w_length, float** out_h,
                                   size_t* out_h_length);

/// @brief Nearest-neighbour filter for spectrogram denoising
///        (mirror of @c sonare::nn_filter / librosa.decompose.nn_filter).
/// @details Output is the smoothed spectrogram [n_features x n_frames] row-major
///          (length n_features*n_frames); release with @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param aggregate Aggregator: "mean", "median", "min" or "max". NULL = "mean".
/// @param k Number of nearest neighbours.
/// @param width Time exclusion half-width. Must be >= 0 (negative widths are
///        rejected, mirroring librosa, instead of silently disabling exclusion).
/// @param out Receives the smoothed spectrogram buffer.
/// @param out_length Receives n_features * n_frames.
SonareError sonare_nn_filter(const float* s, int n_features, int n_frames, const char* aggregate,
                             int k, int width, float** out, size_t* out_length);

/// @brief Reorders / concatenates a signal by interval slices
///        (mirror of @c sonare::remix / librosa.effects.remix).
/// @details Each pair (intervals[2*i], intervals[2*i+1]) selects samples
///          [start, end). The output is the concatenation of all slices.
///          Output is heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input signal.
/// @param length Number of samples.
/// @param sample_rate Sample rate (validated, carried for API symmetry).
/// @param intervals Flat array of @p interval_count (start, end) pairs.
/// @param interval_count Number of (start, end) pairs.
/// @param align_zeros Snap boundaries to zero-crossings (non-zero = true).
/// @param out Receives the remixed signal buffer.
/// @param out_length Receives the remixed signal length.
SonareError sonare_remix(const float* samples, size_t length, int sample_rate, const int* intervals,
                         size_t interval_count, int align_zeros, float** out, size_t* out_length);

/// @brief Resolves the cut points @ref sonare_remix would use, without cutting.
/// @details Returns one clamped (start, end) pair per input interval, in order.
///          With @p align_zeros zero the pairs are the inputs clamped to
///          [0, length]; otherwise each boundary is snapped to the nearest
///          zero-crossing, with two guards that stop a slice from vanishing:
///          a signal with NO sign change at all (silence, a DC offset, any
///          constant) is not snapped, and a slice that had content but
///          collapses to empty after snapping keeps its unsnapped boundaries.
///
///          Snapping is a per-signal decision, so applying @ref sonare_remix
///          channel by channel snaps each channel to a different frame and
///          drifts a stereo take apart. Resolve the cut points once from one
///          channel here and apply them to every channel instead.
/// @param samples Input signal.
/// @param length Number of samples.
/// @param sample_rate Sample rate (validated, carried for API symmetry).
/// @param intervals Flat array of @p interval_count (start, end) pairs.
/// @param interval_count Number of (start, end) pairs.
/// @param align_zeros Snap boundaries to zero-crossings (non-zero = true).
/// @param out Receives a flat array of @p interval_count resolved (start, end)
///        pairs (2 * @p interval_count ints). Release with
///        @ref sonare_free_ints.
/// @param out_count Receives the number of resolved pairs.
SonareError sonare_remix_aligned_intervals(const float* samples, size_t length, int sample_rate,
                                           const int* intervals, size_t interval_count,
                                           int align_zeros, int** out, size_t* out_count);

/// @brief HPSS with residual: separates audio into harmonic, percussive and
///        residual signals (mirror of @c sonare::hpss_with_residual).
/// @details All three outputs share the same @p out_length and @p out_sample_rate
///          (residual = original - harmonic - percussive). Each buffer is
///          heap-allocated and MUST be released with @ref sonare_free_floats.
///          Three-signal shape: emitted as three flat buffers because
///          sonare_c_types.h has no with-residual HPSS result struct.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param kernel_harmonic Horizontal median filter size (odd and positive).
/// @param kernel_percussive Vertical median filter size (odd and positive).
/// @param out_harmonic Receives the harmonic signal.
/// @param out_percussive Receives the percussive signal.
/// @param out_residual Receives the residual signal.
/// @param out_length Receives the (shared) signal length.
/// @param out_sample_rate Receives the (shared) sample rate.
SonareError sonare_hpss_with_residual(const float* samples, size_t length, int sample_rate,
                                      int kernel_harmonic, int kernel_percussive,
                                      float** out_harmonic, float** out_percussive,
                                      float** out_residual, size_t* out_length,
                                      int* out_sample_rate);

/// @brief Phase-vocoder time-scale modification of audio
///        (STFT -> @c sonare::phase_vocoder -> iSTFT).
/// @details Faithful audio wrapper: computes the STFT, time-stretches the
///          spectrogram with phase coherence, and reconstructs audio. Output is
///          heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param rate Time stretch rate (< 1.0 = slower, > 1.0 = faster). Must be > 0.
/// @param n_fft FFT size used for analysis/synthesis: an even integer >= 2.
/// @param hop_length Hop length used for analysis/synthesis, in (0, n_fft/2];
///        see @ref sonare_hpss_ex for why the overlap bound applies.
/// @param out Receives the time-stretched audio buffer.
/// @param out_length Receives the output length.
/// @note Positional facades use the same order after the audio buffer:
/// sample_rate, rate, n_fft, hop_length. Prefer a request object where the
/// language binding provides one.
SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate, float rate,
                                 int n_fft, int hop_length, float** out, size_t* out_length);

/// @brief How a spectral region op modifies the masked STFT bins.
typedef enum SONARE_ENUM_BASE {
  SONARE_SPECTRAL_EDIT_MODE_GAIN = 0,      /* multiply magnitude by 10^(gain_db/20); phase kept */
  SONARE_SPECTRAL_EDIT_MODE_ATTENUATE = 1, /* gain with a (typically negative) gain_db */
  SONARE_SPECTRAL_EDIT_MODE_MUTE = 2,      /* hard zero the masked bins (gain_db ignored) */
  SONARE_SPECTRAL_EDIT_MODE_HEAL = 3,      /* tonal continuation from neighbouring time frames */
} SonareSpectralEditMode;

/// @brief STFT + heal parameters for @ref sonare_spectral_edit.
/// @details Zero-init friendly: every "0 => default" field below picks the
///          documented default so a memset(0) config is the all-defaults case.
typedef struct {
  int n_fft;              /* 0 => default 2048; must be a power of two (>= 2) */
  int hop_length;         /* 0 => default 512; must satisfy 0 < hop <= n_fft/2 */
  int window;             /* SonareWindowType (sonare_c_streaming.h); 0 = Hann */
  int heal_radius_frames; /* 0 => default 2; neighbour frames each side used by Heal */
} SonareSpectralEditConfig;

/// @brief One time x frequency rectangle edit op (POD; ops apply in array order).
typedef struct {
  int64_t start_sample; /* region time start (input samples); clamped to [0, length] */
  int64_t end_sample;   /* region time end, exclusive; clamped to [0, length] */
  float low_hz;         /* region frequency low edge (Hz); clamped to [0, nyquist] */
  float high_hz;        /* region frequency high edge (Hz); <=0 or >= nyquist => nyquist */
  float gain_db;        /* for GAIN/ATTENUATE; ignored by MUTE/HEAL */
  int mode;             /* SonareSpectralEditMode */
} SonareSpectralRegionOp;

/// @brief Region-based spectral editing: STFT -> per-op bin/frame masking -> iSTFT.
/// @details Stateless mono transform; output has the same length/sample rate as the
///          input. @p config may be NULL (all defaults). @p ops may be NULL iff
///          @p n_ops is 0 (identity transform that returns the input). Each op is a
///          time x frequency rectangle applied in order; see @ref SonareSpectralEditMode.
///          The returned array is heap-allocated and MUST be released with
///          @ref sonare_free_floats.
/// @param samples Input audio (mono).
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param config STFT + heal config, or NULL for all defaults.
/// @param ops Array of @p n_ops region ops, or NULL iff @p n_ops == 0.
/// @param n_ops Number of region ops.
/// @param out Receives the edited audio buffer.
/// @param out_length Receives the output length.
SonareError sonare_spectral_edit(const float* samples, size_t length, int sample_rate,
                                 const SonareSpectralEditConfig* config,
                                 const SonareSpectralRegionOp* ops, size_t n_ops, float** out,
                                 size_t* out_length);

#ifdef __cplusplus
}
#endif
