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

/// @brief Options for @ref sonare_hpss. Start from @ref sonare_hpss_options_init.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_hpss_options_init.
  uint32_t struct_size;
  /// FFT size: an even integer >= 4; the FFT is mixed-radix, not power-of-two only. Default 2048.
  int32_t n_fft;
  /// Hop in samples, in [1, n_fft/2]; 0 (the default) is a quarter of @c n_fft, at least 1.
  /// The result is resynthesized by overlap-add, which needs at least half-window overlap; a
  /// sparser hop is refused rather than returning a signal with periodic dropouts.
  size_t hop_length;
  /// Non-zero uses soft (Wiener) masks; zero uses thresholded binary masks. Default 1.
  int32_t use_soft_mask;
} SonareHpssOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_hpss_options_init(SonareHpssOptions* options);

/// @brief Harmonic/percussive source separation.
/// @details Writes harmonic/percussive into @p out. When @p out_residual is non-NULL it
///          receives the third signal, the band where neither component dominates. That signal
///          is silent under soft masks, which sum to one; set @c use_soft_mask to zero for a
///          residual that holds signal. All outputs are reset before validation.
/// @param kernel_harmonic Horizontal median filter size, in STFT frames: a positive odd integer
///   at most 524287. The ceiling is 524288 and an even kernel is refused, so 524287 is the
///   largest legal size.
/// @param kernel_percussive Vertical median filter size, in STFT bins, under the same rule.
/// @param options NULL for the defaults (@ref sonare_hpss_options_init).
/// @note Free @p out with @ref sonare_free_hpss_result and @p out_residual with
///       @ref sonare_free_floats.
SonareError sonare_hpss(const float* samples, size_t length, int sample_rate, int kernel_harmonic,
                        int kernel_percussive, const SonareHpssOptions* options,
                        SonareHpssResult* out, float** out_residual);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_harmonic(const float* samples, size_t length, int sample_rate, float** out,
                            size_t* out_length);
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_percussive(const float* samples, size_t length, int sample_rate, float** out,
                              size_t* out_length);

/// @brief Options for @ref sonare_time_stretch. Start from @ref sonare_time_stretch_options_init.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_time_stretch_options_init.
  uint32_t struct_size;
  /// FFT size: an even integer >= 4. Default 2048.
  int32_t n_fft;
  /// Hop in samples; 0 (the default) is a quarter of @c n_fft. See
  /// @ref SonareHpssOptions::hop_length.
  size_t hop_length;
} SonareTimeStretchOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_time_stretch_options_init(SonareTimeStretchOptions* options);

/// @brief Native spectral time stretch.
/// @param options NULL for the defaults (@ref sonare_time_stretch_options_init).
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_time_stretch(const float* samples, size_t length, int sample_rate,
                                float speed_ratio, const SonareTimeStretchOptions* options,
                                float** out, size_t* out_length);

/// @brief Options for @ref sonare_pitch_shift. Start from @ref sonare_pitch_shift_options_init.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_pitch_shift_options_init.
  uint32_t struct_size;
  /// FFT size: an even integer >= 4. Default 2048.
  int32_t n_fft;
  /// Hop in samples; 0 (the default) is a quarter of @c n_fft. See
  /// @ref SonareHpssOptions::hop_length.
  size_t hop_length;
} SonarePitchShiftOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_pitch_shift_options_init(SonarePitchShiftOptions* options);

/// @brief Native spectral pitch shift.
/// @param options NULL for the defaults (@ref sonare_pitch_shift_options_init).
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_pitch_shift(const float* samples, size_t length, int sample_rate,
                               float pitch_semitones, const SonarePitchShiftOptions* options,
                               float** out, size_t* out_length);
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
///          A valid voiced frame is passed through unchanged when its source
///          period exceeds the input buffer length, or its corrected pitch lies
///          outside [sample_rate / length, sample_rate / 2]. These frames do not
///          fail the request. Invalid source F0 and non-finite parameters are
///          rejected by the core; all bindings inherit this validation.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
///                    Voiced frames require finite F0 in (0, sample_rate/2].
///                    Unvoiced frames accept NaN or finite F0 in
///                    [0, sample_rate/2], matching pYIN's unvoiced output.
/// @param voiced_prob Per-frame voicing probability [0,1] (@p n_frames entries),
///                    or NULL. It only derives voicing (>= 0.5 is voiced) and
///                    does NOT scale the per-frame correction amount: it is a
///                    voicing decision, so @ref sonare_pitch_pyin's @c voiced_flag
///                    belongs in @p voiced rather than its @c voiced_prob (a
///                    frequency-dependent observation mass, not a confidence).
/// @param voiced      Per-frame voiced flags (non-zero = voiced; @p n_frames
///                    entries), or NULL. At most one of @p voiced_prob and
///                    @p voiced is given -- both together is refused; with
///                    both NULL, every frame is treated as voiced.
/// @param hop_length  F0 hop in samples (> 0). Frame i is centered at sample
///                    i*hop_length; nearest-frame voicing switches at half-hop
///                    boundaries.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     size_t hop_length, float target_midi,
                                                     float** out, size_t* out_length);

/// @brief Target selector for @ref sonare_pitch_correct_timevarying.
typedef enum SONARE_ENUM_BASE {
  SONARE_PITCH_TARGET_FIXED_MIDI = 0,  ///< Retune every voiced frame toward @c target_midi.
  SONARE_PITCH_TARGET_SCALE = 1,       ///< Snap each voiced frame to the nearest scale degree.
} SonarePitchTargetMode;

/// @brief Tunable configuration for @ref sonare_pitch_correct_timevarying.
/// @details Prepare one with @ref sonare_pitch_correction_config_init, then override the fields
///          you care about; every field is read literally.
typedef struct {
  uint32_t struct_size;        ///< sizeof(SonarePitchCorrectionConfig); set by the init function.
  int32_t target_mode;         ///< A @ref SonarePitchTargetMode selecting how the target is chosen.
  float target_midi;           ///< Target note when @c target_mode is FIXED_MIDI ([0,127]).
  int32_t scale_root;          ///< Scale root pitch class (0=C .. 11=B) when target_mode is SCALE.
  uint32_t scale_mode_mask;    ///< 12-bit degree mask, bit i = semitone i above the root enabled.
  float scale_reference_midi;  ///< Finite reference MIDI anchoring the scale grid; default 69 (A4).
  float retune_amount;         ///< Correction strength [0,1]; 1 = full snap, 0 = bypass.
  float max_correction_semitones;  ///< Hard clamp on per-frame correction magnitude, >= 0.
  float retune_speed_ms;           ///< Retune IIR time constant (ms), >= 0; larger = slower glide.
  float vibrato_threshold_cents;   ///< Corrections below this are bypassed; >= 0.
} SonarePitchCorrectionConfig;

/// @brief Fills @p config with sizeof and the library defaults (major scale, full retune).
/// @details Mirrors the core PitchCorrectionConfig defaults: FIXED_MIDI target,
///          C-major mask, reference A4, retune 1.0, 12-semitone clamp, 50 ms
///          glide, 20-cent vibrato threshold. A NULL @p config is ignored.
void sonare_pitch_correction_config_init(SonarePitchCorrectionConfig* config);

/// @brief Per-frame pitch correction toward a fixed MIDI note OR a musical scale.
/// @details Generalises @ref sonare_pitch_correct_to_midi_timevarying: the same
///          caller-supplied F0 contour drives correction, but @p config selects
///          between a fixed-MIDI target and scale quantisation and exposes the
///          retune-strength / vibrato-preservation knobs. Pass NULL for @p config
///          to use the library defaults.
///          A valid voiced frame is passed through unchanged when its source
///          period exceeds the input buffer length, or its corrected pitch lies
///          outside [sample_rate / length, sample_rate / 2]. These frames do not
///          fail the request. Invalid source F0 and non-finite parameters are
///          rejected by the core; all bindings inherit this validation.
/// @param f0_hz       Per-frame measured F0 in Hz (@p n_frames entries, required).
///                    Voiced frames require finite F0 in (0, sample_rate/2].
///                    Unvoiced frames accept NaN or finite F0 in
///                    [0, sample_rate/2].
/// @param voiced_prob Per-frame voicing probability [0,1], or NULL. Only
///                    derives voicing (>= 0.5 is voiced); never a weight on
///                    the correction amount (see
///                    @ref sonare_pitch_correct_to_midi_timevarying).
/// @param voiced      Per-frame voiced flags (non-zero = voiced), or NULL. At
///                    most one of @p voiced_prob and @p voiced is given --
///                    both together is refused; with both NULL, every frame
///                    is treated as voiced.
/// @param hop_length  F0 hop in samples (> 0), with the centered-frame voicing
///                    rule of @ref sonare_pitch_correct_to_midi_timevarying.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_pitch_correct_timevarying(const float* samples, size_t length, int sample_rate,
                                             const float* f0_hz, const float* voiced_prob,
                                             const int32_t* voiced, size_t n_frames,
                                             size_t hop_length,
                                             const SonarePitchCorrectionConfig* config, float** out,
                                             size_t* out_length);

/// @brief Offline auto-tune: snaps the voiced pitch of a monophonic recording to
///        the scale of a key, detected or named.
/// @details Chains key detection (when @p key is NULL), @ref sonare_scale_mask_for_mode,
///          pYIN pitch tracking (2048-sample frames, 512-sample hop, the hop
///          @ref sonare_pitch_correct_timevarying defaults to) and the
///          time-varying scale corrector. The output has the input's length.
///          Key detection measures the recording's tuning and uses the default
///          major/minor candidates, exactly as @ref sonare_detect_key does; the
///          correction grid stays at concert A440.
/// @param key    The key to tune to, or NULL to detect it. A named key is taken
///               as certain: @p out_key reports confidence 1.
/// @param config Correction knobs, or NULL for the library defaults. Only
///               @c scale_reference_midi, @c retune_amount,
///               @c max_correction_semitones, @c retune_speed_ms and
///               @c vibrato_threshold_cents are read; the key supplies the scale,
///               so @c target_mode, @c target_midi, @c scale_root and
///               @c scale_mode_mask are ignored.
/// @param out_key Receives the key used (required).
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_auto_tune(const float* samples, size_t length, int sample_rate,
                             const SonareKey* key, const SonarePitchCorrectionConfig* config,
                             float** out, size_t* out_length, SonareKey* out_key);
/// @param stretch_ratio Duration multiplier for the region; 1 leaves it as it
///        is. Must be finite and > 0; there is no spelling of "unspecified".
///        The refusal lives in the core rather than in this call, so an
///        out-of-domain value comes back as SONARE_ERROR_INVALID_PARAMETER with
///        the core's own text on sonare_last_error_message().
/// @note Free @p out with @ref sonare_free_floats.
/// @note A @p stretch_ratio that would make the stretched note pass 2147483647 samples is refused
/// with @c SONARE_ERROR_INVALID_PARAMETER naming it.
SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int64_t onset_sample, int64_t offset_sample, float stretch_ratio,
                                float** out, size_t* out_length);
/// Move a note region to a new onset sample while preserving its duration.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_note_move(const float* samples, size_t length, int sample_rate,
                             int64_t onset_sample, int64_t offset_sample,
                             int64_t target_onset_sample, float** out, size_t* out_length);
// ============================================================================
// Effects - Note objects
// ============================================================================

/// @brief Configuration for @ref sonare_extract_notes.
/// @details Prepare one with @ref sonare_note_extractor_config_init (50 cents, 30 ms,
///          A4 = 440 Hz, voiced threshold 0.5); every field is then read literally. A NULL
///          config pointer takes the same defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareNoteExtractorConfig); set by the init function.
  /// Distance in cents from a note's pitch beyond which a frame leaves the note; finite and
  /// > 0, default 50.
  float segmentation_threshold_cents;
  /// Milliseconds a deviation must persist before it starts a new note; finite and >= 0
  /// (0 keeps no minimum beyond two frames), default 30.
  float min_note_ms;
  /// Tuning reference in Hz that @c median_cents is measured from; finite and > 0, default 440.
  float reference_hz;
  /// Value of @c voiced_prob at or above which a frame counts as voiced, in [0, 1], default
  /// 0.5. Read only when @c voiced is NULL.
  ///
  /// pYIN's @c voiced_prob is a frame's voiced observation mass and rises with
  /// F0 for a fixed frame length, so the 0.5 default silently drops low
  /// registers: pass its @c voiced_flag through @c voiced instead.
  float voiced_threshold;
} SonareNoteExtractorConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
void sonare_note_extractor_config_init(SonareNoteExtractorConfig* config);

/// @brief Configuration for @ref sonare_render_notes.
/// @details Prepare one with @ref sonare_note_render_config_init (5 ms fade, 3 Hz vibrato
///          cutoff); every field is then read literally. A NULL config pointer takes the same
///          defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareNoteRenderConfig); set by the init function.
  /// Cross-fade at each edited note's edges, weighted to hold the level whether
  /// the note is coherent with the audio under it or not; finite and > 0, default 5 ms. A
  /// hard cut is deliberately not selectable, because the seam it leaves behind is a click.
  float fade_ms;
  /// Boundary between the drift and the vibrato that @c vibrato_depth_change
  /// and @c drift_change act on, in Hz; finite and > 0, default 3.
  ///
  /// Pass whatever @ref sonare_decompose_note_pitch was called with. A host
  /// that draws the vibrato at one cutoff and edits it at another edits a
  /// curve it never showed anyone.
  float vibrato_cutoff_hz;
} SonareNoteRenderConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
void sonare_note_render_config_init(SonareNoteRenderConfig* config);

/// @brief A pending, non-destructive change to one note.
/// @details @ref sonare_note_edit_init gives the identity edit: ratio 1 and an envelope of zero
///          length, which is no envelope, so the edit changes nothing. Every note an entry
///          point takes carries its own edit, so each one is started from the init function.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareNoteEdit); set by the init function.
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
  /// Pitch shift of the note in semitones; 0 leaves the pitch alone.
  float pitch_semitones;
  /// Gain applied to the note in dB; 0 leaves the level alone.
  float gain_db;
  /// >1 lengthens the note, <1 shortens it; pitch is preserved. Finite and > 0, default 1.
  float time_stretch_ratio;
  /// Moves the spectral envelope, in semitones, on top of whatever the pitch
  /// shift already did to it.
  ///
  /// 0 runs no warp at all, so a pitch-only edit is not charged for an LPC
  /// analysis-resynthesis round it did not ask for. A pitch shift drags the
  /// formants with it, so holding them still is -@c pitch_semitones and
  /// the chipmunk is the default. Saturates near -10.3 and +8.7 semitones
  /// rather than being rejected.
  float formant_semitones;
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

/// @brief Fills @p edit with sizeof and the identity edit (time_stretch_ratio 1, all else 0).
///        A NULL @p edit is ignored.
void sonare_note_edit_init(SonareNoteEdit* edit);

/// One editable note. Sample bounds are half-open into the source audio; frame
/// bounds are half-open into the caller's own F0 track.
/// @details A data record: the measured fields are the library's and the edit carries its own
///          struct_size, so a note built by hand sets @c edit with @ref sonare_note_edit_init.
typedef struct {
  /// First sample of the note in the source audio.
  int64_t onset_sample;
  /// One past the last sample of the note in the source audio.
  int64_t offset_sample;
  /// Index of this note's first RMS value in the owning result's @c amplitude
  /// array; the note occupies @c frame_end - @c frame_start entries from there.
  /// Set on output and ignored by @ref sonare_render_notes.
  int64_t amplitude_offset;
  /// First F0 frame of the note in the caller's F0 track.
  int32_t frame_start;
  /// One past the last F0 frame of the note.
  int32_t frame_end;
  /// Median pitch of the note in Hz.
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
  /// Pending edit; the identity edit on a freshly extracted note.
  SonareNoteEdit edit;
} SonareNoteObject;

/// Heap-owned note-object output. Release with @ref sonare_free_note_objects_result.
/// @details The per-note F0 curve is not repeated here: it is the caller's own
///          @c f0_hz sliced by @c [frame_start, frame_end). The amplitude curve
///          is new, so it is returned -- one RMS per F0 frame over that frame's
///          samples, concatenated in note order.
typedef struct {
  /// Heap array of @c count notes, or NULL when empty.
  SonareNoteObject* notes;
  /// Number of entries in @c notes.
  size_t count;
  /// Per-frame RMS values of all notes concatenated in note order, or NULL when empty.
  float* amplitude;
  /// Number of entries in @c amplitude.
  size_t amplitude_count;
  /// Amplitude envelopes the notes' edits index through @c envelope_offset.
  /// NULL unless an entry point carried envelopes in: extraction produces
  /// identity edits, so only @ref sonare_split_note and @ref sonare_merge_notes
  /// ever fill this.
  float* envelopes;
  /// Number of entries in @c envelopes.
  size_t envelope_count;
} SonareNoteObjectsResult;

/// @brief Extract editable note objects from audio and an F0 track.
/// @details Spans come from the same segmenter @ref sonare_note_segments uses.
///          Each note additionally carries its median pitch, its two measured
///          quality figures, and its slice of the amplitude curve. Every
///          returned note has the identity edit.
/// @param samples Source audio; @p length must be non-zero.
/// @param f0_hz Per-frame F0 in Hz, @p n_frames entries. A frame carrying no
///        pitch is spelled as zero, a negative value or a non-finite one --
///        sonare_pitch_pyin emits NaN there unless asked to fill it -- and all
///        three read the same: that frame contributes no measurement.
/// @param voiced_prob Per-frame voicing in [0, 1], or NULL. Exactly one of
///        @p voiced_prob and @p voiced is given; both or neither is refused.
/// @param voiced Per-frame voiced flags (non-zero = voiced), or NULL.
/// @param n_frames Number of F0 frames; must be non-zero.
/// @param frame_rate F0 frames per second; must be finite and > 0.
/// @param config Optional configuration started from its init function; NULL selects the defaults.
/// @param out Receives a heap-owned result, cleared before validation. An empty
///        segmentation is returned as NULL pointers with zero counts.
SonareError sonare_extract_notes(const float* samples, size_t length, int sample_rate,
                                 const float* f0_hz, const float* voiced_prob,
                                 const int32_t* voiced, size_t n_frames, float frame_rate,
                                 const SonareNoteExtractorConfig* config,
                                 SonareNoteObjectsResult* out);
/// @brief Releases the notes, amplitude and envelope arrays of a result and empties it.
/// @details NULL and an already-released result are no-ops.
void sonare_free_note_objects_result(SonareNoteObjectsResult* result);

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
///          or stretched notes may be written over each other. Edited source
///          spans are cleared before rendering destinations. Notes are rendered
///          in array order; later notes overwrite earlier destination samples,
///          with the configured cross-fade at their edges. Muted notes clear
///          their source span and do not erase rendered destinations.
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
/// @param voiced Optional per-frame flags, @p n_frames entries. Zero suppresses
///        the F0 measurement; nonzero still requires a positive finite F0.
///        NULL infers voicing from F0 alone. Requires @p f0_hz when given.
/// @param n_frames Number of F0 frames, or 0 when @p f0_hz is NULL.
/// @param frame_rate F0 frames per second; must be finite and > 0 when @p f0_hz
///        is given.
/// @param config Optional configuration started from its init function; NULL selects the defaults.
/// @param out Receives the rendered audio, which has the input's length.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_render_notes(const float* samples, size_t length, int sample_rate,
                                const SonareNoteObject* notes, size_t note_count,
                                const float* envelopes, size_t envelope_count, const float* f0_hz,
                                const int32_t* voiced, size_t n_frames, float frame_rate,
                                const SonareNoteRenderConfig* config, float** out,
                                size_t* out_length);

/// One note's pitch curve split into a centre, a slow drift and a vibrato.
/// Release with @ref sonare_free_pitch_decomposition_result.
typedef struct {
  /// The note's steady pitch in Hz. 0 when the note carries no usable pitch,
  /// and then both curves are empty.
  float center_hz;
  /// Slow deviation from @c center_hz in cents, @c count entries.
  float* drift_cents;
  /// Fast deviation in cents, over the same frames.
  float* vibrato_cents;
  /// Number of entries in each of the two curves.
  size_t count;
} SonarePitchDecompositionResult;

/// @brief Split one note's pitch curve into a centre, a drift and a vibrato.
/// @details @c drift_cents[i] + @c vibrato_cents[i] is the note's own pitch at
///          frame i, in cents above @c center_hz, to within float rounding, so
///          the three parts reconstruct the curve. The drift filter is zero
///          phase, so neither curve is shifted in time against the audio.
///
///          Unvoiced frames and frames whose F0 is unusable carry no measurement.
///          The curve is held at the nearest usable neighbour across them. Both curves
///          therefore have an entry everywhere; a host marking the held ones
///          identifies them from @p f0_hz and @p voiced when given.
///
///          The note's own F0 curve is not returned by
///          @ref sonare_extract_notes, so pass the caller's own @c f0_hz sliced
///          by the note's @c [frame_start, frame_end) together with its
///          @c median_hz.
/// @param f0_hz The note's slice of the F0 track, @p n_frames entries. Positive
///        finite values carry pitch; zero, negative and non-finite values carry
///        no measurement. @ref sonare_pitch_pyin emits NaN in unvoiced frames
///        unless asked to fill them.
/// @param voiced Optional per-frame flags for the same note slice. Zero suppresses
///        the F0 measurement; nonzero still requires a positive finite F0.
///        NULL infers voicing from F0 alone.
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
SonareError sonare_decompose_note_pitch(const float* f0_hz, const int32_t* voiced, size_t n_frames,
                                        float frame_rate, float median_hz, float vibrato_cutoff_hz,
                                        SonarePitchDecompositionResult* out);
/// @brief Releases both curves of a @ref SonarePitchDecompositionResult and empties it.
/// @details NULL and an already-released result are no-ops.
void sonare_free_pitch_decomposition_result(SonarePitchDecompositionResult* result);

/// @brief Split one note in two at a track frame.
/// @details Both halves are re-derived from the audio and the track the way
///          @ref sonare_extract_notes derives its own, rather than by patching
///          the fields of the note they replace.
///
///          Both inherit the source note's edit, and the tail's
///          @c time_offset_samples absorbs the duration change of the stretched
///          head, so the two halves occupy the destination timeline the unsplit
///          note did; a note whose edit is the identity still renders bit for
///          bit after being split. A half with no sample span at that boundary
///          is omitted and the other keeps the edit unchanged, so the result
///          then has as many notes as @p notes. A one-entry envelope is a
///          constant over the span, so both halves get that same entry; a
///          longer one is resampled onto a grid that preserves the rendered
///          gain at every integer source sample.
///          Every note in the set -- not just the two halves -- has its spans,
///          curves, medians and stability re-derived from @p samples and the
///          track, because @ref SonareNoteObject carries no curves for this
///          call to copy through. The frame bounds are therefore what a note is
///          identified by here, and the audio and track arguments must be the
///          ones the set was extracted from or the whole set is re-measured
///          against something else. A set whose re-derived notes would include
///          one with no sample span is rejected with
///          @c SONARE_ERROR_INVALID_PARAMETER rather than shortened.
///
///          The same code rejects a note whose sample span runs past the end of
///          @p samples, an edit whose gain times envelope cannot be represented
///          as a finite value, non-finite @p samples or output values, and an
///          invalid edit on any note in the set.
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
///          exactly as @ref sonare_split_note describes, and the rejections it
///          lists (span past the audio end, unrepresentable gain times envelope,
///          non-finite input or output, invalid edit on any note) apply here too.
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
// Effects - Note targets
// ============================================================================

/// One note of a reference melody. Times are seconds from the start of the audio
/// the notes were extracted from.
typedef struct {
  /// Note start in seconds.
  double start_sec;
  /// Note end in seconds.
  double end_sec;
  /// Target pitch as a MIDI note number.
  float target_midi;
} SonareNoteTarget;

/// @brief What to do with a note that has a measurable pitch but no target.
typedef enum SONARE_ENUM_BASE {
  SONARE_NOTE_TARGET_UNMATCHED_LEAVE = 0,  ///< Leave the edit alone; the note renders as recorded.
  SONARE_NOTE_TARGET_UNMATCHED_MUTE = 1,   ///< Mute the note's span.
  SONARE_NOTE_TARGET_UNMATCHED_NEAREST = 2,  ///< Take the nearest target in time, however far away.
} SonareNoteTargetUnmatchedPolicy;

/// @brief Tunable configuration for @ref sonare_assign_note_targets.
/// @details Prepare one with @ref sonare_note_target_assign_config_init, or pass NULL to take the
///          same library defaults. Every field is read literally: a zeroed
///          @c max_correction_semitones saturates every correction to nothing, so the call
///          would assign targets and move no note.
typedef struct {
  uint32_t struct_size;      ///< sizeof(SonareNoteTargetAssignConfig); set by the init function.
  int32_t unmatched_policy;  ///< A @ref SonareNoteTargetUnmatchedPolicy for notes with no target.
  /// Fraction of the note that must overlap a target for it to count, in [0, 1]; default 0.5.
  /// 0 means any overlap at all counts.
  float min_overlap_ratio;
  /// The assigned shift saturates here rather than being refused, matching
  /// @c SonareNoteEdit::formant_semitones: a reference an octave out is a
  /// wrong reference, and a rejected call tells the caller less than a bounded
  /// correction does. Finite and >= 0, default 12; 0 allows no correction.
  float max_correction_semitones;
} SonareNoteTargetAssignConfig;

/// @brief Fills @p config with sizeof and the library defaults (unmatched notes left alone,
///        half the note must overlap, corrections saturate at an octave). A NULL @p config is
///        ignored.
void sonare_note_target_assign_config_init(SonareNoteTargetAssignConfig* config);

/// @brief Reads one track of an in-memory Standard MIDI File as a reference melody.
/// @details Each note-on is paired with the next note-off of the same note number
///          on the same channel, and the pair becomes one target at the note's own
///          pitch. A note-on the track never closes is dropped: it has no end, and
///          the track's end is not a substitute for one -- the file's length is
///          derived from its last event, which is the open note-on itself when the
///          file stops there. With events after it, the note would instead span the
///          whole remainder and, being the longest overlap everywhere, take the
///          assignment away from every note that follows.
///
///          Times follow the file's tempo map, so a tempo change or a tempo ramp
///          inside it is honoured rather than the initial tempo being scaled.
///          Zero-length notes are skipped -- they overlap nothing, so they could
///          never be assigned.
/// @param track_index Index into the tracks that carried MIDI events, NOT the
///        SMF's own track numbering: a track holding only meta events -- a
///        conductor track carrying the tempo map is the usual one -- is not
///        counted here. A file whose first track is a conductor track therefore
///        has its melody at index 0, not 1.
/// @param out Receives a heap-owned array of @p out_count targets, sorted by
///        @c start_sec. A track with no closed note is reported as NULL and 0
///        rather than as an error.
/// @note The returned array MUST be released with @ref sonare_free_note_targets.
SonareError sonare_note_targets_from_smf(const uint8_t* bytes, size_t len, int track_index,
                                         SonareNoteTarget** out, size_t* out_count);
/// @brief Releases an array returned by @ref sonare_note_targets_from_smf.
/// @details NULL is a no-op.
void sonare_free_note_targets(SonareNoteTarget* targets);

/// @brief Writes each note's @c pitch_semitones from the target it overlaps.
/// @details Rewrites @p notes in place, so @c edit means the same field going in
///          and coming out and no second note-object result is produced.
///
///          A note is matched to the target it overlaps longest, provided that
///          overlap is at least @c min_overlap_ratio of the note's own span; ties
///          go to the target that starts first. The shift is
///          @c target_midi minus the note's own @c median_hz as a MIDI number,
///          saturated at @c max_correction_semitones.
///
///          A note whose @c median_hz is not finite and positive is never assigned
///          and never edited, whatever the policy says. Such a note has no
///          measured pitch to correct from -- the extractor spells that the way an
///          F0 track spells an unvoiced frame -- so NEAREST would compute a shift
///          from a pitch that does not exist. The policy governs notes that have a
///          pitch and no target, which is a different thing from having no pitch.
/// @param notes The note set to edit, in place. May be NULL when @p note_count
///        is 0. Only @c median_hz, the sample bounds and @c edit are read.
/// @param sample_rate Converts each note's sample span to seconds; must be > 0.
/// @param targets May be NULL when @p target_count is 0, which assigns nothing
///        and applies the policy to every note carrying a pitch.
/// @param config Optional configuration; NULL selects the library defaults.
/// @param out_assigned_count Receives how many notes got a target. Zero is a
///        legitimate answer -- a reference that does not line up with the take --
///        and the caller has to be able to see it, which is why it is reported
///        rather than left implicit in the notes.
SonareError sonare_assign_note_targets(SonareNoteObject* notes, size_t note_count, int sample_rate,
                                       const SonareNoteTarget* targets, size_t target_count,
                                       const SonareNoteTargetAssignConfig* config,
                                       size_t* out_assigned_count);

// ============================================================================
// Effects - Percussive events
// ============================================================================

/// @brief Configuration for @ref sonare_extract_percussive_events.
/// @details Prepare one with @ref sonare_percussive_event_config_init (2048-point FFT, a hop of
///          a quarter of it, 31-frame median kernels, onset delta 0.06, one frame of minimum
///          spacing, a 500 ms span cap and no ratio filter); every field is then read
///          literally. A NULL config pointer takes the same defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonarePercussiveEventConfig); set by the init function.
  /// FFT size and hop the separation and the onset detector share. They cannot
  /// be set apart: an event measured on one framing and lifted out on another is
  /// not the same signal. @c n_fft defaults to 2048 and @c hop_length to
  /// 0, a quarter of the n_fft in use. The pair must overlap-add --
  /// @c n_fft even and at least 2, @c hop_length no more than half of it --
  /// because the separation inverts an STFT.
  int32_t n_fft;
  /// Hop in samples, or 0 for a quarter of @c n_fft; see @c n_fft.
  size_t hop_length;
  /// Median filter lengths the separation runs, along time and along frequency.
  /// A longer harmonic kernel calls more of a sustained sound harmonic. Odd and positive,
  /// default 31, so an even one is rejected rather than rounded. 1 is legal and degenerate --
  /// a length-1 median is the identity, so both components come back as the source.
  int32_t hpss_kernel_harmonic;
  /// Percussive median kernel length (frequency axis); see @c hpss_kernel_harmonic.
  int32_t hpss_kernel_percussive;
  /// Minimum frames between consecutive onsets, >= 0; default 1.
  int32_t onset_wait;
  /// Offset added to the detector's adaptive threshold; raising it finds fewer,
  /// stronger hits and lowering it finds more. Finite, default 0.06; 0 and negative values
  /// are ordinary.
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
  /// Finite and > 0, default 500 ms.
  float max_event_ms;
  /// Drops an event whose @c percussive_ratio falls below this; must be in
  /// [0, 1], default 0 (keep everything). Raising it is
  /// useful on material that is mostly drums and wrong on a dense mix, where it
  /// also drops real hits sitting over a loud sustain.
  float min_percussive_ratio;
} SonarePercussiveEventConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
void sonare_percussive_event_config_init(SonarePercussiveEventConfig* config);

/// @brief Configuration for @ref sonare_render_percussive_events.
/// @details Prepare one with @ref sonare_percussive_render_config_init (2048-point FFT, a hop of
///          a quarter of it, 31-frame median kernels, 5 ms fade); every field is then read
///          literally. A NULL config pointer takes the same defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonarePercussiveRenderConfig); set by the init function.
  /// The separation the events were measured against; even and at least 2, default 2048.
  /// Pass back what @ref sonare_extract_percussive_events was called with: a different
  /// separation lifts a different signal out of the span than the one the events describe.
  /// Validated even when every edit is the identity and no separation runs, so an unusable
  /// framing is an error on every set.
  int32_t n_fft;
  /// Hop in samples of the separation, or 0 (the default), a quarter of @c n_fft.
  size_t hop_length;
  /// Harmonic median kernel length of the separation; odd and positive, default 31.
  int32_t hpss_kernel_harmonic;
  /// Percussive median kernel length of the separation; odd and positive, default 31.
  int32_t hpss_kernel_percussive;
  /// Fade-out at the tail of each lifted span; finite and > 0, default 5 ms. A hard cut is not
  /// selectable: what the fade shapes is the signal being subtracted, so squaring it off
  /// leaves a step.
  ///
  /// There is deliberately no matching fade-in: a span opens in front of its
  /// transient, where the percussive component is near-silent, so cutting square
  /// there costs nothing and keeps a muted hit's attack from surviving inside a
  /// fade.
  float fade_ms;
} SonarePercussiveRenderConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
void sonare_percussive_render_config_init(SonarePercussiveRenderConfig* config);

/// @brief A pending, non-destructive change to one percussive event.
/// @details Zero-initializing gives the identity edit. A struck sound has no
///          steady pitch to edit, so the axes are time and amplitude and there
///          is deliberately nothing else here.
typedef struct {
  /// Moves the hit along the timeline. Negative moves it earlier.
  int64_t time_offset_samples;
  /// Gain applied to the hit in dB; 0 leaves the level alone.
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
  /// First sample of the event in the source audio.
  int64_t onset_sample;
  /// One past the last sample of the event in the source audio.
  int64_t offset_sample;
  /// Detector strength at the onset, on the onset envelope's own scale. It
  /// orders events against each other and carries no absolute meaning.
  float strength;
  /// Peak absolute sample of the percussive component over the span, linear.
  /// Measured on the signal @c gain_db scales rather than on the source.
  float peak_amplitude;
  /// Share of the span's energy the separation assigned to percussion, in
  /// [0, 1]. NaN (not measured) when the span is silent; such an event is kept only
  /// when @c min_percussive_ratio is 0.
  ///
  /// It describes the span rather than the onset that opened it. An isolated hit
  /// sits near 1, but a real hit over a loud sustain sits near 0, because the
  /// sustain owns the span's energy. So it separates a hit from a note attack
  /// only where nothing is sustaining through both, and it is not a test for
  /// whether a hit is there.
  float percussive_ratio;
  /// Pending edit; the identity edit on a freshly extracted event.
  SonarePercussiveEventEdit edit;
} SonarePercussiveEvent;

/// Heap-owned percussive-event output. Release with
/// @ref sonare_free_percussive_events_result.
/// @details One allocation, unlike @ref SonareNoteObjectsResult: an event
///          carries three scalars and nothing per frame, so there is no
///          companion curve array to travel with the set.
typedef struct {
  /// Heap array of @c count events, or NULL when empty.
  SonarePercussiveEvent* events;
  /// Number of entries in @c events.
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
/// @param config Optional configuration started from its init function; NULL selects the defaults.
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
void sonare_free_percussive_events_result(SonarePercussiveEventsResult* result);

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
/// @param config Optional configuration started from its init function; NULL selects the defaults.
/// @param out Receives the rendered audio, which has the input's length.
/// @note The returned array is heap-allocated and MUST be released with
///       @ref sonare_free_floats.
SonareError sonare_render_percussive_events(const float* samples, size_t length, int sample_rate,
                                            const SonarePercussiveEvent* events, size_t count,
                                            const SonarePercussiveRenderConfig* config, float** out,
                                            size_t* out_length);

/// @brief How @ref SonareVoiceChangeConfig::formant_factor relates to the pitch shift.
typedef enum {
  /// The factor is the warp applied after the pitch shift, so the formants end up at the
  /// factor times the pitch ratio 2^(semitones/12). This is what @ref sonare_voice_change does.
  SONARE_FORMANT_MODE_RELATIVE = 0,
  /// The factor is the formant shift relative to the input: the warp applied is the factor
  /// divided by 2^(semitones/12), so 1 keeps the formants where they were.
  SONARE_FORMANT_MODE_ABSOLUTE = 1
} SonareFormantMode;

/// @brief Configuration for @ref sonare_voice_change. Start from
///        @ref sonare_voice_change_config_init; every field is then read literally.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareVoiceChangeConfig); set by the init function.
  /// Transpose in semitones; 0 leaves the pitch alone. Must be finite. Default 0.
  float pitch_semitones;
  /// Formant scale; its meaning is chosen by @c formant_mode. Must be finite. Default 1.
  float formant_factor;
  /// A @ref SonareFormantMode. Default @ref SONARE_FORMANT_MODE_RELATIVE.
  int32_t formant_mode;
} SonareVoiceChangeConfig;

/// @brief Writes every default into @p config and sets its @c struct_size; NULL is ignored.
void sonare_voice_change_config_init(SonareVoiceChangeConfig* config);

/// @brief Pitch shift with a formant warp.
/// @param config NULL for the defaults (@ref sonare_voice_change_config_init: no transpose, no
///        warp). A @c formant_mode outside @ref SonareFormantMode, a relative-mode factor outside
///        [0.55, 1.65], a non-finite transpose or factor, or an absolute-mode request whose warp
///        (the factor divided by 2^(semitones/12)) falls outside [0.55, 1.65], is
///        SONARE_ERROR_INVALID_PARAMETER; for the latter sonare_last_error_message() names the
///        reachable formant-factor range for the given semitones. The request is refused rather
///        than clamped.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_voice_change(const float* samples, size_t length, int sample_rate,
                                const SonareVoiceChangeConfig* config, float** out,
                                size_t* out_length);

#include "sonare_c_engine.h"
#include "sonare_c_voice_changer.h"

/// @brief Peak-normalize mono audio. @p target_db must be finite.
/// @details The result is not clipped; a caller writing to an integer format fits or limits it.
///   A target above 0 dB leaves the peak above full scale.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_normalize(const float* samples, size_t length, int sample_rate, float target_db,
                             float** out, size_t* out_length);
/// @brief RMS-normalize mono audio. @p target_db must be finite.
/// @details One gain brings the signal RMS exactly to target_db. The result is not clipped; an
///   RMS target can leave peaks above full scale.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_normalize_rms(const float* samples, size_t length, int sample_rate,
                                 float target_db, float** out, size_t* out_length);

/// @brief Applies a cosine fade-in and fade-out to mono audio.
/// @details Each length is clamped to the input; 0 leaves that end untouched.
///   The fade-in is applied first, then the fade-out.
/// @param fade_in_sec  Fade-in length in seconds; finite and >= 0.
/// @param fade_out_sec Fade-out length in seconds; finite and >= 0.
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_fade(const float* samples, size_t length, int sample_rate, float fade_in_sec,
                        float fade_out_sec, float** out, size_t* out_length);

/// @brief Response selector for @ref sonare_biquad_filter.
typedef enum SONARE_ENUM_BASE {
  SONARE_BIQUAD_FILTER_HIGH_PASS = 0,  ///< Butterworth high-pass at @c cutoff_hz.
  SONARE_BIQUAD_FILTER_LOW_PASS = 1,   ///< Butterworth low-pass at @c cutoff_hz.
  SONARE_BIQUAD_FILTER_BAND_PASS = 2,  ///< Band-pass at @c center_hz, @c bandwidth_hz wide.
  SONARE_BIQUAD_FILTER_NOTCH = 3,      ///< Notch at @c center_hz, @c bandwidth_hz wide.
} SonareBiquadFilterType;

/// @brief Configuration for @ref sonare_biquad_filter.
/// @details Fill with @ref sonare_biquad_filter_config_init, then set the
///   frequency fields @c type reads. A frequency field @c type does not read
///   must stay NaN (not given); any other value is refused, and so is NaN in a
///   field @c type reads.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareBiquadFilterConfig); set by the init function.
  int32_t type;          ///< A @ref SonareBiquadFilterType (default HIGH_PASS).
  int32_t order;         ///< 2, or 4 for HIGH_PASS / LOW_PASS (a two-section cascade); default 2.
  int32_t zero_phase;    ///< Non-zero runs the filter forward and backward; default 0.
  float cutoff_hz;       ///< HIGH_PASS / LOW_PASS corner, in (0, sample_rate / 2).
  float center_hz;       ///< BAND_PASS / NOTCH center, in (0, sample_rate / 2).
  float bandwidth_hz;    ///< BAND_PASS / NOTCH bandwidth, > 0.
} SonareBiquadFilterConfig;

/// @brief Fills @p config with sizeof and the defaults; every frequency field is NaN, so
///   the caller sets the ones @c type reads. A NULL @p config is ignored.
void sonare_biquad_filter_config_init(SonareBiquadFilterConfig* config);

/// @brief Filters mono audio with a biquad (or a cascade of two for order 4).
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_biquad_filter(const float* samples, size_t length, int sample_rate,
                                 const SonareBiquadFilterConfig* config, float** out,
                                 size_t* out_length);

/// @brief Peak-normalize a stereo pair on a gain measured across both channels.
/// @details The peak is taken across the pair and the resulting gain goes to
///   both channels, because a per-channel gain is a balance change: it lifts the
///   quieter side until the two peaks match, which is not what normalizing a
///   stereo recording means. The louder channel reaches @p target_db and the
///   other keeps its level relative to it. Same argument policy as
///   @ref sonare_normalize -- @p target_db must be finite, and the result is not clipped -- plus
///   the pair itself must be processable together: neither channel empty, one
///   @p length, one @p sample_rate.
/// @note @p out_left and @p out_right each receive @p length samples, reported once in
///   @p out_length. Free each with @ref sonare_free_floats. On failure all three are cleared.
SonareError sonare_normalize_stereo(const float* left, const float* right, size_t length,
                                    int sample_rate, float target_db, float** out_left,
                                    float** out_right, size_t* out_length);

/// @brief RMS-normalize a stereo pair on a gain measured across both channels.
/// @details The level driven to @p target_db is the root mean square over the
///   two channels' samples together -- the quadratic mean of the per-channel
///   figures, not their average. Like @ref sonare_normalize_rms the result is
///   not clipped, and it shares @ref sonare_normalize_stereo's argument and pair
///   policy.
/// @note Free @p out_left and @p out_right with @ref sonare_free_floats.
SonareError sonare_normalize_rms_stereo(const float* left, const float* right, size_t length,
                                        int sample_rate, float target_db, float** out_left,
                                        float** out_right, size_t* out_length);

/// @brief Options for @ref sonare_trim. Start from @ref sonare_trim_options_init.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_trim_options_init.
  uint32_t struct_size;
  /// RMS frame length in samples. Default 2048.
  size_t frame_length;
  /// RMS hop in samples. Default 512.
  size_t hop_length;
} SonareTrimOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_trim_options_init(SonareTrimOptions* options);

/// @brief Trim leading/trailing silence using an absolute RMS dBFS threshold.
/// @param options NULL for the defaults (@ref sonare_trim_options_init).
/// @note Free @p out with @ref sonare_free_floats.
SonareError sonare_trim(const float* samples, size_t length, int sample_rate, float threshold_db,
                        const SonareTrimOptions* options, float** out, size_t* out_length);

/// @brief Options for @ref sonare_decompose. Start from @ref sonare_decompose_options_init.
/// @note Pointer members stay the caller's; the call does not transfer ownership.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_decompose_options_init.
  uint32_t struct_size;
  /// Initialiser: "random" (deterministic seeded) or "nndsvd" (SVD warm start, which tends to
  /// converge in fewer iterations). NULL (the default) is "random".
  const char* init;
} SonareDecomposeOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_decompose_options_init(SonareDecomposeOptions* options);

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
/// @param n_iter Number of multiplicative-update iterations, >= 1; 0 selects the
///        library default (50). Negative values are rejected.
/// @param beta Beta divergence (2 = Frobenius, 1 = KL, 0 = Itakura-Saito).
/// @param options NULL for the defaults (@ref sonare_decompose_options_init).
/// @param out_w Receives the [n_features x n_components] component matrix.
/// @param out_w_length Receives n_features * n_components.
/// @param out_h Receives the [n_components x n_frames] activation matrix.
/// @param out_h_length Receives n_components * n_frames.
/// @note An @p n_components that would make the component weights or activations pass 500000000
/// elements is refused with @c SONARE_ERROR_INVALID_PARAMETER naming it.
SonareError sonare_decompose(const float* s, int n_features, int n_frames, int n_components,
                             int n_iter, float beta, const SonareDecomposeOptions* options,
                             float** out_w, size_t* out_w_length, float** out_h,
                             size_t* out_h_length);

/// @brief Options for @ref sonare_decompose_stems.
/// @details Prepare one with @ref sonare_decompose_stems_config_init (4 components, 2048/512
///          STFT, 100 iterations, Frobenius beta, random init, magnitude mask); every field is
///          then read literally. A NULL config pointer takes the same defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareDecomposeStemsConfig); set by the init function.
  int n_components;      ///< Number of NMF components (stems), >= 1; default 4.
  int n_fft;             ///< STFT size in samples, even and at least 4; default 2048.
  /// STFT hop in samples, at most n_fft / 2; 0 (the default) is a quarter of n_fft.
  size_t hop_length;
  int n_iter;        ///< Multiplicative-update iterations, >= 1; default 100.
  float beta;        ///< Finite beta divergence: 2 = Frobenius (default), 1 = KL, 0 = IS.
  const char* init;  ///< Initialiser "random" (default) or "nndsvd"; never NULL once initialised.
  float mask_power;  ///< Soft-mask exponent, finite and >= 1; default 1 (magnitude ratio), 2 is
                     ///< Wiener-style.
} SonareDecomposeStemsConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
/// @details Allocates nothing and does not transfer ownership; @c init points at a static string.
void sonare_decompose_stems_config_init(SonareDecomposeStemsConfig* config);

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
/// @param config Optional options started from the init function; NULL selects the defaults.
/// @param out Receives one heap buffer holding every component, a flat array
///        of @p out_n_components * @p out_frames floats (component c starts at
///        c * @p out_frames). Release with @ref sonare_free_floats.
/// @param out_frames Receives the sample frames per component (equal to @p length).
/// @param out_n_components Receives the number of components.
/// @param out_w Optional; receives the [n_bins x n_components] component matrix
///        that produced the masks, n_bins being config n_fft / 2 + 1. NULL skips
///        it. Release with @ref sonare_free_floats.
/// @param out_w_length Optional; receives n_bins * n_components. Required when
///        @p out_w is non-NULL.
/// @param out_h Optional; receives the [n_components x n_frames] activation
///        matrix, n_frames being the STFT analysis frames of @p length at the
///        config hop. NULL skips it. Release with @ref sonare_free_floats.
/// @param out_h_length Optional; receives n_components * n_frames. Required
///        when @p out_h is non-NULL.
/// @note An @p n_components that would make the component weights or activations pass 500000000
/// elements is refused with @c SONARE_ERROR_INVALID_PARAMETER naming it.
SonareError sonare_decompose_stems(const float* samples, size_t length, int sample_rate,
                                   const SonareDecomposeStemsConfig* config, float** out,
                                   size_t* out_frames, int* out_n_components, float** out_w,
                                   size_t* out_w_length, float** out_h, size_t* out_h_length);

/// @brief Multi-channel form of @ref sonare_decompose_stems: one NMF model and
///        one soft mask shared across every channel.
/// @details The per-channel complex spectrograms are averaged into a single
///          magnitude plane before the NMF factorisation, and the resulting
///          per-component soft mask is applied UNCHANGED to every channel's own
///          complex spectrum, so no interchannel level or phase difference
///          moves -- the same guarantee @ref sonare_mastering_repair_denoise_classical_linked's
///          gain mask gives its channel set.
///
///          A @p num_channels of 1 reproduces @ref sonare_decompose_stems bit
///          for bit: the average over one channel is a division by 1, which
///          does not change the bits.
/// @param channels @p num_channels buffers of @p length samples each; none
///        NULL. At least one and at most 64 channels.
/// @param length Samples per channel. One length for the set.
/// @param sample_rate Sample rate in Hz, shared by every channel.
/// @param config Optional options started from the init function; NULL selects the defaults.
/// @param out Receives one heap buffer holding every (component, channel)
///        signal, laid out as a single flat array of @p out_n_components *
///        @p num_channels * @p out_frames floats: component c, channel ch starts
///        at (c * @p num_channels + ch) * @p out_frames. Release with
///        @ref sonare_free_floats.
/// @param out_frames Receives the sample frames per (component, channel) (equal to @p length).
/// @param out_n_components Receives the number of components.
/// @param out_w Optional; receives the [n_bins x n_components] component matrix
///        that produced the masks, n_bins being config n_fft / 2 + 1. NULL skips
///        it. Release with @ref sonare_free_floats.
/// @param out_w_length Optional; receives n_bins * n_components. Required when
///        @p out_w is non-NULL.
/// @param out_h Optional; receives the [n_components x n_frames] activation
///        matrix, n_frames being the STFT analysis frames of @p length at the
///        config hop. NULL skips it. Release with @ref sonare_free_floats.
/// @param out_h_length Optional; receives n_components * n_frames. Required
///        when @p out_h is non-NULL.
/// @note An @p n_components that would make the component weights or activations pass 500000000
/// elements is refused with @c SONARE_ERROR_INVALID_PARAMETER naming it.
SonareError sonare_decompose_stems_linked(const float* const* channels, size_t num_channels,
                                          size_t length, int sample_rate,
                                          const SonareDecomposeStemsConfig* config, float** out,
                                          size_t* out_frames, int* out_n_components, float** out_w,
                                          size_t* out_w_length, float** out_h,
                                          size_t* out_h_length);

/// @brief Nearest-neighbour filter for spectrogram denoising
///        (mirror of @c sonare::nn_filter / librosa.decompose.nn_filter).
/// @details Output is the smoothed spectrogram [n_features x n_frames] row-major
///          (length n_features*n_frames); release with @ref sonare_free_floats.
/// @param s Input spectrogram [n_features x n_frames] row-major.
/// @param n_features Feature dimension (rows). Must be > 0.
/// @param n_frames Number of time frames. Must be > 0.
/// @param aggregate Aggregator: "mean", "median", "min" or "max". NULL = "mean".
/// @param k Number of nearest neighbours, >= 1; -1 selects the library default (7).
///        0 and any other negative value are rejected.
/// @param width Time exclusion half-width, >= 0; -1 selects the library default (1).
///        Any other negative value is rejected, mirroring librosa, instead of
///        silently disabling exclusion.
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
/// @note @p intervals whose spans would make the remixed signal pass 500000000 samples is refused
/// with @c SONARE_ERROR_INVALID_PARAMETER naming them.
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

/// @brief Phase-vocoder time-scale modification of audio
///        (STFT -> @c sonare::phase_vocoder -> iSTFT).
/// @details Faithful audio wrapper: computes the STFT, time-stretches the
///          spectrogram with phase coherence, and reconstructs audio. Output is
///          heap-allocated; release with @ref sonare_free_floats.
/// @param samples Input audio.
/// @param length Number of samples.
/// @param sample_rate Sample rate.
/// @param speed_ratio Time stretch rate (< 1.0 = slower, > 1.0 = faster). Must be > 0.
/// @param n_fft FFT size used for analysis/synthesis: an even integer >= 4.
/// @param hop_length Hop length used for analysis/synthesis, in (0, n_fft/2], or
///        0 for a quarter of @p n_fft (at least 1). See
///        @ref SonareHpssOptions::hop_length for why the overlap bound applies.
/// @param out Receives the time-stretched audio buffer.
/// @param out_length Receives the output length.
/// @note Positional facades use the same order after the audio buffer:
/// sample_rate, rate, n_fft, hop_length. Prefer a request object where the
/// language binding provides one.
/// @note A @p speed_ratio that would make the stretched spectrogram pass 500000000 elements is
/// refused with @c SONARE_ERROR_INVALID_PARAMETER naming it.
SonareError sonare_phase_vocoder(const float* samples, size_t length, int sample_rate,
                                 float speed_ratio, int n_fft, size_t hop_length, float** out,
                                 size_t* out_length);

/// @brief How a spectral region op modifies the masked STFT bins.
typedef enum SONARE_ENUM_BASE {
  SONARE_SPECTRAL_EDIT_MODE_GAIN = 0,      /* multiply magnitude by 10^(gain_db/20); phase kept */
  SONARE_SPECTRAL_EDIT_MODE_ATTENUATE = 1, /* gain with a (typically negative) gain_db */
  SONARE_SPECTRAL_EDIT_MODE_MUTE = 2,      /* hard zero the masked bins (gain_db ignored) */
  SONARE_SPECTRAL_EDIT_MODE_HEAL = 3,      /* tonal continuation from neighbouring time frames */
} SonareSpectralEditMode;

/// @brief STFT + heal parameters for @ref sonare_spectral_edit.
/// @details Prepare one with @ref sonare_spectral_edit_config_init; every field is then read
///          literally. A NULL config pointer takes the same defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareSpectralEditConfig); set by the init function.
  int n_fft;             ///< STFT size in samples, a power of two in [4, 262144]; default 2048.
  /// STFT hop in samples, at most n_fft/2; 0 (the default) is a quarter of n_fft.
  size_t hop_length;
  int window;              ///< A @ref SonareWindowType (sonare_c_streaming.h); default Hann.
  int heal_radius_frames;  ///< Neighbour frames on each side that heal reads, >= 1; default 2.
} SonareSpectralEditConfig;

/// @brief Fills @p config with sizeof and the library defaults. A NULL @p config is ignored.
void sonare_spectral_edit_config_init(SonareSpectralEditConfig* config);

/// @brief One time x frequency rectangle edit op (ops apply in array order).
/// @details Prepare each op with @ref sonare_spectral_region_op_init, then set the fields.
typedef struct {
  uint32_t struct_size;  ///< sizeof(SonareSpectralRegionOp); set by the init function.
  int64_t start_sample;  ///< Region start in input samples; clamped to [0, length].
  int64_t end_sample;    ///< Region end in input samples, exclusive; clamped to [0, length].
  float low_hz;          ///< Region low edge in Hz; clamped to [0, nyquist].
  /// Region high edge in Hz, finite and > 0, clamped to nyquist; NaN (the init value) means
  /// nyquist.
  float high_hz;
  float gain_db;  ///< Gain in dB for GAIN and ATTENUATE; ignored by MUTE and HEAL.
  int mode;       ///< A @ref SonareSpectralEditMode selecting how the region is modified.
} SonareSpectralRegionOp;

/// @brief Fills @p op with sizeof, an empty region at 0, high_hz NaN (nyquist), gain 0 dB and
///        mode GAIN. A NULL @p op is ignored.
void sonare_spectral_region_op_init(SonareSpectralRegionOp* op);

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
