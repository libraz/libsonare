#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Streaming - StreamAnalyzer (stateful real-time frame analyzer)
// ============================================================================

/// @brief Construction config for SonareStreamAnalyzer.
///
/// Mirrors the relevant subset of sonare::StreamConfig exposed by the WASM StreamAnalyzer
/// constructor. Start from sonare_stream_config_init, which writes the librosa/real-time defaults.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_stream_config_init.
  int sample_rate;       ///< Input sample rate in Hz (default 44100)
  /// Window in samples at 44100 Hz (default 2048); rescaled below that rate, never shorter than
  /// the hop.
  int n_fft;
  size_t hop_length;  ///< Hop in analysis-rate samples (default 512, at most n_fft)
  int n_mels;         ///< Number of Mel bands (default 128)
  float fmin_hz;      ///< Lowest Mel filterbank frequency in Hz (default 0)
  /// Highest Mel filterbank frequency in Hz, greater than fmin_hz; NaN (the default) is the
  /// Nyquist frequency. At most 22050 effective above 44100 Hz input, where the analyzer works
  /// at 44100 Hz.
  float fmax_hz;
  /// A4 tuning reference in Hz (default 440; must be within 220..880, the same range
  /// sonare_stream_analyzer_set_reference_hz accepts).
  float reference_hz;
  /// Magnitude frames are not exposed by the C read paths; non-zero returns
  /// SONARE_ERROR_NOT_SUPPORTED for config.compute_magnitude.
  int compute_magnitude;
  int compute_mel;                ///< Non-zero to compute Mel spectra
  int compute_chroma;             ///< Non-zero to compute chroma and per-frame chord estimates
  int compute_onset;              ///< Non-zero to compute onset strength
  int compute_spectral;           ///< Non-zero to compute spectral centroid and flatness
  int emit_every_n_frames;        ///< Emit one output frame per N analysis frames (>= 1, default 1)
  int magnitude_downsample;       ///< Magnitude downsample factor (>= 1, default 1); unused while
                                  ///< compute_magnitude must be 0
  size_t max_pending_frames;      ///< Unread-frame cap; overflow drops newest (default 4096)
  float key_update_interval_sec;  ///< Seconds between key re-estimates (> 0, default 5)
  float bpm_update_interval_sec;  ///< Seconds between BPM re-estimates (> 0, default 10)
  int window;  ///< A @ref SonareWindowType selecting the analysis window (default Hann)
  /// A @ref SonareStreamOutputFormat; deprecated and must be SONARE_STREAM_OUTPUT_FLOAT32. Use an
  /// explicit U8/I16 read function instead.
  int output_format;
  /// Per chord/bar progression cap; drops oldest (default 4096)
  size_t max_progression_entries;
} SonareStreamConfig;

/// @brief Fills @p config with sizeof and the real-time defaults (44100 Hz, n_fft 2048, etc.).
///        A NULL @p config is ignored.
void sonare_stream_config_init(SonareStreamConfig* config);

/// @brief Analysis window applied to each frame before the FFT.
typedef enum SONARE_ENUM_BASE {
  SONARE_WINDOW_HANN = 0,         ///< Hann window (default)
  SONARE_WINDOW_HAMMING = 1,      ///< Hamming window
  SONARE_WINDOW_BLACKMAN = 2,     ///< Blackman window
  SONARE_WINDOW_RECTANGULAR = 3,  ///< No tapering
} SonareWindowType;

/// @brief Sample type of the generic frame read; only Float32 is accepted.
typedef enum SONARE_ENUM_BASE {
  SONARE_STREAM_OUTPUT_FLOAT32 = 0,  ///< 32-bit float output (the only accepted value)
  SONARE_STREAM_OUTPUT_INT16 = 1,    ///< Legacy value; analyzer creation rejects it.
  SONARE_STREAM_OUTPUT_UINT8 = 2,    ///< Legacy value; analyzer creation rejects it.
} SonareStreamOutputFormat;

/// @brief Feature arrays physically present in a SonareStreamFrames* result.
///
/// RMS and timestamps_sec are always present and therefore need no flag.
typedef enum SONARE_ENUM_BASE {
  SONARE_STREAM_FEATURE_MEL = 1u << 0,       ///< Mel array is populated
  SONARE_STREAM_FEATURE_CHROMA = 1u << 1,    ///< Chroma and chord arrays are populated
  SONARE_STREAM_FEATURE_ONSET = 1u << 2,     ///< Onset strength array is populated
  SONARE_STREAM_FEATURE_SPECTRAL = 1u << 3,  ///< Spectral centroid and flatness are populated
} SonareStreamFeatureFlags;

/// @brief One chord change in the frame-level chord progression.
typedef struct {
  int root;          ///< Chord root pitch class (0-11, C = 0)
  int quality;       ///< A @ref SonareChordQuality of the chord
  float start_sec;   ///< Time the chord begins, in seconds on the stream timeline
  float confidence;  ///< Detection confidence (0-1)
} SonareStreamChordChange;

/// @brief One bar of the beat-synchronized chord progression.
///
/// bar_index is the bar number, and it is NOT the index of this entry in the
/// array: a bar whose frames were all below the chord-detection threshold is not
/// recorded, and the oldest entries are dropped once max_progression_entries is
/// reached. Anything that groups bars by their position in a repeating pattern
/// must key on bar_index.
///
/// start_sec is on the same timeline as SonareStreamFrames.timestamps_sec, offset
/// included, and consecutive bars are spaced by bar_duration_sec rather than snapped
/// to the analysis frame grid.
///
/// In voted_pattern, root is in [0,11] or -1 (unknown) and quality is a valid
/// chord-quality index; bar_index is the pattern position and start_sec is
/// unused.
typedef struct {
  int bar_index;     ///< Bar number (not the array position; see above)
  int root;          ///< Chord root pitch class (0-11, C = 0)
  int quality;       ///< A @ref SonareChordQuality of the bar's chord
  float start_sec;   ///< Bar start in seconds on the stream timeline
  float confidence;  ///< Detection confidence (0-1)
} SonareStreamBarChord;

/// @brief Match score of one chord-progression pattern template.
typedef struct {
  char name[64];  ///< NUL-terminated pattern name (truncated to fit)
  float score;    ///< Match score of the pattern against the detected bars
} SonareStreamPatternScore;

/// @brief Structure-of-arrays frame buffer returned by sonare_stream_analyzer_read_frames.
///
/// All arrays are heap-allocated; free the whole result with sonare_free_stream_frames. Matrix
/// layouts are row-major [n_frames x stride].
typedef struct {
  int n_frames;           ///< Number of frames in this batch
  int n_mels;             ///< Mel stride; 0 when MEL is absent
  float* timestamps_sec;  ///< [n_frames] frame times in seconds on the stream timeline
  /// [n_frames * n_mels] linear mel power (NOT dB; the U8/I16 quantized variants pack dB instead)
  float* mel;
  float* chroma;             ///< [n_frames * n_chroma], NULL when CHROMA is absent
  float* onset_strength;     ///< [n_frames], NULL when ONSET is absent
  float* rms_energy;         ///< [n_frames] frame RMS (linear amplitude)
  float* spectral_centroid;  ///< [n_frames] in Hz, NULL when SPECTRAL is absent
  float* spectral_flatness;  ///< [n_frames] (0-1), NULL when SPECTRAL is absent
  int* chord_root;           ///< [n_frames] root pitch class, NULL when CHROMA is absent
  int* chord_quality;        ///< [n_frames] @ref SonareChordQuality, NULL when CHROMA is absent
  float* chord_confidence;   ///< [n_frames] (0-1), NULL when CHROMA is absent
  uint32_t feature_flags;    ///< Bitwise SonareStreamFeatureFlags for populated arrays
  int n_chroma;              ///< Chroma bins per frame; 0 when chroma is disabled
} SonareStreamFrames;

/// @brief 8-bit quantized frame buffer from sonare_stream_analyzer_read_quantized_frames_u8.
///
/// Every array holds values scaled to 0..255 over the quantization range noted per field. Free
/// with sonare_free_stream_frames_u8.
typedef struct {
  int n_frames;           ///< Number of frames in this batch
  int n_mels;             ///< Mel stride; 0 when MEL is absent
  float* timestamps_sec;  ///< [n_frames] frame times in seconds (not quantized)
  /// [n_frames * n_mels] mel in dB, quantized over [mel_db_min, mel_db_max]
  uint8_t* mel;
  uint8_t* chroma;             ///< [n_frames * n_chroma] chroma quantized over [0, 1]
  uint8_t* onset_strength;     ///< [n_frames] quantized over [0, onset_max]
  uint8_t* rms_energy;         ///< [n_frames] quantized over [0, rms_max]
  uint8_t* spectral_centroid;  ///< [n_frames] quantized over [0, centroid_max]
  uint8_t* spectral_flatness;  ///< [n_frames] quantized over [0, 1]
  int8_t* chord_root;          ///< [n_frames] root pitch class, NULL when CHROMA is absent
  int8_t* chord_quality;       ///< [n_frames] @ref SonareChordQuality, NULL when CHROMA is absent
  uint8_t* chord_confidence;   ///< [n_frames] quantized over [0, 1], NULL when CHROMA is absent
  uint32_t feature_flags;      ///< Bitwise SonareStreamFeatureFlags for populated arrays
  int n_chroma;                ///< Chroma bins per frame; 0 when chroma is disabled
} SonareStreamFramesU8;

/// @brief 16-bit quantized frame buffer from sonare_stream_analyzer_read_quantized_frames_i16.
///
/// Same layout and quantization ranges as SonareStreamFramesU8 with int16 samples. Free with
/// sonare_free_stream_frames_i16.
typedef struct {
  int n_frames;           ///< Number of frames in this batch
  int n_mels;             ///< Mel stride; 0 when MEL is absent
  float* timestamps_sec;  ///< [n_frames] frame times in seconds (not quantized)
  /// [n_frames * n_mels] mel in dB, quantized over [mel_db_min, mel_db_max]
  int16_t* mel;
  int16_t* chroma;             ///< [n_frames * n_chroma] chroma quantized over [0, 1]
  int16_t* onset_strength;     ///< [n_frames] quantized over [0, onset_max]
  int16_t* rms_energy;         ///< [n_frames] quantized over [0, rms_max]
  int16_t* spectral_centroid;  ///< [n_frames] quantized over [0, centroid_max]
  int16_t* spectral_flatness;  ///< [n_frames] quantized over [0, 1]
  int8_t* chord_root;          ///< [n_frames] root pitch class, NULL when CHROMA is absent
  int8_t* chord_quality;       ///< [n_frames] @ref SonareChordQuality, NULL when CHROMA is absent
  int16_t* chord_confidence;   ///< [n_frames] quantized over [0, 1], NULL when CHROMA is absent
  uint32_t feature_flags;      ///< Bitwise SonareStreamFeatureFlags for populated arrays
  int n_chroma;                ///< Chroma bins per frame; 0 when chroma is disabled
} SonareStreamFramesI16;

/// @brief Quantization ranges for the u8/i16 bandwidth-reduction read paths.
///
/// Mirrors sonare::QuantizeConfig. Populate with sonare_stream_quantize_config_init, then widen
/// any range whose source values exceed the defaults: the quantizers clamp normalized values to
/// [0,1], so a stream louder/quieter than these ranges otherwise saturates silently to the
/// endpoints. Every range must be finite and ordered / positive or the read is refused.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_stream_quantize_config_init.
  float mel_db_min;      ///< dB floor for mel quantization (default -80)
  float mel_db_max;      ///< dB ceiling for mel quantization (default 0)
  float onset_max;       ///< max expected onset strength (default 50)
  float rms_max;         ///< max expected RMS energy (default 1)
  float centroid_max;    ///< max expected spectral centroid Hz (default 11025)
} SonareStreamQuantizeConfig;

/// @brief Fills @p config with sizeof and the default quantization ranges. A NULL @p config is
///        ignored.
void sonare_stream_quantize_config_init(SonareStreamQuantizeConfig* config);

/// @brief Progressive estimate + counters snapshot.
///
/// Mirrors sonare::AnalyzerStats and ProgressiveEstimate. Variable-length arrays are owned by this
/// result and released by sonare_free_stream_stats.
typedef struct {
  int total_frames;                ///< Analysis frames processed since reset
  size_t total_samples;            ///< Input-sample cursor, including the caller's timeline origin
  float duration_sec;              ///< Stream position in seconds, including the timeline origin
  size_t pending_frames;           ///< Unread frames currently retained
  uint64_t dropped_output_frames;  ///< New output frames dropped at the configured cap
  float bpm;                       ///< Estimated BPM (0 if not yet estimated)
  float bpm_confidence;            ///< BPM confidence (0-1)
  /// Tempo candidates the most recent BPM estimate chose from; 0 until one has run.
  size_t bpm_candidate_count;
  int key;                         ///< Estimated key root pitch class (0-11, -1 = unknown)
  int key_minor;                   ///< Non-zero if minor mode
  float key_confidence;            ///< Key confidence (0-1)
  int chord_root;                  ///< Current chord root (0-11, -1 = unknown)
  int chord_quality;               ///< A @ref SonareChordQuality of the current chord
  float chord_confidence;          ///< Current chord confidence (0-1)
  float chord_start_sec;           ///< Start time of current chord (s)
  int current_bar;                 ///< Current bar index (-1 if BPM not stable)
  float bar_duration_sec;          ///< Duration of one bar (s, 0 if not stable)
  size_t chord_progression_count;  ///< Number of entries in chord_progression
  /// Frame-level chord changes, oldest first; capped at max_progression_entries
  SonareStreamChordChange* chord_progression;
  size_t bar_chord_progression_count;  ///< Number of entries in bar_chord_progression
  /// Beat-synchronized chord per bar, oldest first; capped at max_progression_entries
  SonareStreamBarChord* bar_chord_progression;
  size_t pattern_length;  ///< Length in bars of the repeating chord pattern (see voted_pattern)
  size_t voted_pattern_count;  ///< Number of entries in voted_pattern
  /// Majority-voted chord per position of the repeating pattern; see SonareStreamBarChord
  SonareStreamBarChord* voted_pattern;
  char detected_pattern_name[64];   ///< NUL-terminated name of the best-matching pattern
  float detected_pattern_score;     ///< Match score of the best-matching pattern
  size_t all_pattern_scores_count;  ///< Number of entries in all_pattern_scores
  /// Score of every candidate pattern
  SonareStreamPatternScore* all_pattern_scores;
  float accumulated_sec;  ///< Total audio processed for estimation (s)
  int used_frames;        ///< Number of frames used for estimation
  /// Non-zero if the key or BPM was re-estimated since the previous snapshot; one change sets it
  /// on exactly one snapshot
  int updated;
  size_t dropped_chord_progression_entries;  ///< Oldest chord changes dropped at cap
  size_t dropped_bar_progression_entries;    ///< Oldest bar chords dropped at cap
  /// Blocks in which a non-finite input sample was replaced before it could reach the analyzer's
  /// recursive state. Unlike the drops above nothing is missing from the output: every estimate
  /// is produced as usual and simply stops describing the input, so this is the only report that
  /// the stream was degraded. The unit is one process() call, never a sample, so a block carrying
  /// a thousand NaNs adds one. Cleared by sonare_stream_analyzer_reset alongside the drop counts,
  /// because that call rebuilds the timeline and the count describes a segment rather than the
  /// analyzer.
  size_t non_finite_discard_blocks;
} SonareStreamStats;

/// @par Threading and lifecycle
/// A handle supports one producer thread calling process,
/// or finalize concurrently with one consumer thread calling available_frames,
/// a read_frames or read_quantized_frames variant, stats, frame_count, or current_time_sec. Calls
/// within either role must be serialized. Completed frames and stats snapshots use an
/// allocation-free release/acquire handoff; when the pending ring is full the
/// newly produced output frame is dropped, while analysis totals still advance.
/// reset, all set_* functions, destroy, and ownership transfer require both
/// roles to be stopped. sample_rate reads immutable construction state.

/// @brief Creates a streaming analyzer from the given config.
/// @param config Construction config (must be non-null). A sample_rate outside
///        [8000, 384000], an n_fft above 524288 or an n_mels above 4096 is
///        SONARE_ERROR_INVALID_PARAMETER. Non-zero compute_magnitude is
///        SONARE_ERROR_NOT_SUPPORTED because no C read path exposes its frames.
/// @param out Receives the new handle on success and NULL on failure; destroy it with
///        sonare_stream_analyzer_destroy.
/// @return SONARE_OK or the error code describing the failure.
/// @note @p out is a handle, not a sonare_free_* buffer.
SonareError sonare_stream_analyzer_create(const SonareStreamConfig* config,
                                          SonareStreamAnalyzer** out);

/// @brief Destroys a streaming analyzer handle (null is a no-op).
void sonare_stream_analyzer_destroy(SonareStreamAnalyzer* analyzer);

/// @brief Feeds an audio chunk.
/// @details A NULL @p sample_offset tracks the cumulative offset internally. A non-NULL
///   @p sample_offset is the explicit external offset of this chunk: offsets must be
///   contiguous across non-empty calls. A non-empty call cannot switch between internal
///   and explicit offset tracking; a gap, seek, or mode switch returns
///   SONARE_ERROR_INVALID_PARAMETER. Call sonare_stream_analyzer_reset first to discard
///   the buffered partial frame and begin a new timeline segment. A zero-sample call is
///   a no-op: it neither changes the tracking mode nor the timeline, including on a finalized
///   analyzer. Feeding non-empty audio to a finalized analyzer returns
///   SONARE_ERROR_INVALID_STATE; call sonare_stream_analyzer_reset first to begin a new stream.
SonareError sonare_stream_analyzer_process(SonareStreamAnalyzer* analyzer, const float* samples,
                                           size_t n_samples, const size_t* sample_offset);

/// @brief Drains any high-rate resampler tail, then flushes the final partial
///        analysis frame with zero-padding.
/// @details Repeating a successful call is a no-op. A failed call leaves the stream
///   un-finalized and may have advanced its analysis state or published frames.
///   Call sonare_stream_analyzer_reset before starting a new stream.
SonareError sonare_stream_analyzer_finalize(SonareStreamAnalyzer* analyzer);

/// @brief Returns the number of frames available to read.
SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count);

/// @brief Reads up to @p max_frames frames into a SOA buffer (consumes them).
/// @param out Receives heap-allocated arrays (free with sonare_free_stream_frames).
SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out);

/// @brief Reads up to @p max_frames frames into an 8-bit quantized SOA buffer
///        using caller-supplied quantization ranges.
/// @param config Quantization ranges; started from @ref sonare_stream_quantize_config_init.
///        NULL = those defaults.
/// @param out Receives heap-allocated arrays (free with sonare_free_stream_frames_u8).
SonareError sonare_stream_analyzer_read_quantized_frames_u8(
    SonareStreamAnalyzer* analyzer, const SonareStreamQuantizeConfig* config, size_t max_frames,
    SonareStreamFramesU8* out);

/// @brief Reads up to @p max_frames frames into a 16-bit quantized SOA buffer
///        using caller-supplied quantization ranges.
/// @param config Quantization ranges; started from @ref sonare_stream_quantize_config_init.
///        NULL = those defaults.
/// @param out Receives heap-allocated arrays (free with sonare_free_stream_frames_i16).
SonareError sonare_stream_analyzer_read_quantized_frames_i16(
    SonareStreamAnalyzer* analyzer, const SonareStreamQuantizeConfig* config, size_t max_frames,
    SonareStreamFramesI16* out);

/// @brief Resets analyzer state for a new stream.
SonareError sonare_stream_analyzer_reset(SonareStreamAnalyzer* analyzer, size_t base_sample_offset);

/// @brief Reads the current statistics and progressive estimate snapshot.
SonareError sonare_stream_analyzer_stats(SonareStreamAnalyzer* analyzer, SonareStreamStats* out);

/// @brief Frees variable-length arrays held by a SonareStreamStats.
void sonare_free_stream_stats(SonareStreamStats* stats);

/// @brief Returns the total number of frames processed.
SonareError sonare_stream_analyzer_frame_count(SonareStreamAnalyzer* analyzer, size_t* out_count);

/// @brief Returns the current stream time position in seconds, including the caller's timeline
/// origin.
SonareError sonare_stream_analyzer_current_time_sec(SonareStreamAnalyzer* analyzer, float* out_sec);

/// @brief Returns the configured input sample rate (Hz).
SonareError sonare_stream_analyzer_sample_rate(SonareStreamAnalyzer* analyzer,
                                               int* out_sample_rate);

/// @brief Sets the expected total duration (s) for pattern-lock timing.
SonareError sonare_stream_analyzer_set_expected_duration(SonareStreamAnalyzer* analyzer,
                                                         float duration_sec);

/// @brief Sets a normalization gain applied to input samples.
/// @details @p gain is a linear factor within 0.01..100 (±40 dB), assuming
///   input in the conventional ±1 float domain. A non-finite, non-positive or
///   out-of-range value returns SONARE_ERROR_INVALID_PARAMETER and leaves the
///   previous gain in place; it is not clamped. The usual recipe
///   (gain = target_level / measured_level) can land outside the range for a
///   buffer on another scale — an integer-scaled one asks for about 3e-4 — and
///   since no getter exposes the effective gain, a clamped request would leave
///   the analysis running far off target with nothing to detect it by. Convert
///   such a buffer before feeding it instead.
SonareError sonare_stream_analyzer_set_normalization_gain(SonareStreamAnalyzer* analyzer,
                                                          float gain);

/// @brief Sets the A4 tuning reference (Hz) and rebuilds the chroma filterbank.
/// @details @p reference_hz must be within 220..880, the same range
///   SonareStreamConfig::reference_hz accepts at create time. A value outside
///   it returns SONARE_ERROR_INVALID_PARAMETER and leaves the filterbank alone;
///   it is not clamped, so a live change and a create-time setting of the same
///   value always bin the chromagram identically.
SonareError sonare_stream_analyzer_set_reference_hz(SonareStreamAnalyzer* analyzer,
                                                    float reference_hz);

/// @brief Frees all arrays held by a SonareStreamFrames batch.
void sonare_free_stream_frames(SonareStreamFrames* frames);

/// @brief Frees all arrays held by a SonareStreamFramesU8 batch.
void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames);

/// @brief Frees all arrays held by a SonareStreamFramesI16 batch.
void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames);

// ===========================================================================
// Streaming retune (block-by-block grain overlap-add pitch shifter)
// ===========================================================================

/// @brief Opaque handle for a streaming retune stage.
///
/// A stateful, block-by-block pitch shifter with the same lifecycle as
/// SonareStreamingMasteringChain: create, prepare, then process one block at a
/// time. All controls are scalars, so there is no public struct to keep in sync
/// across the bindings.
typedef struct SonareStreamingRetune SonareStreamingRetune;

/// @brief Creates a streaming retune stage. Fails with SONARE_ERROR_INVALID_PARAMETER for a
///        control outside its domain, which is refused by name and never moved onto a nearby
///        value, and with SONARE_ERROR_NOT_SUPPORTED when the library was built without the
///        voice changer.
/// @param pitch_semitones Pitch shift in semitones, finite and in [-24, 24].
/// @param mix Dry/wet blend, finite and in [0, 1].
/// @param grain_size_samples Grain length in samples, a multiple of 4 in [4, 8192], or 0 to
///        derive roughly a 46 ms grain from the sample rate at prepare() time. Structural: it
///        takes effect at the NEXT prepare(), and re-preparing at a different sample rate
///        re-derives it when it is 0.
/// @param out Receives the handle on success, NULL on failure; destroy with
///        @ref sonare_streaming_retune_destroy, which is what invalidates it; no other call does.
/// @return SONARE_OK, or the error code describing the failure.
SonareError sonare_streaming_retune_create(float pitch_semitones, float mix, int grain_size_samples,
                                           SonareStreamingRetune** out);

/// @brief Destroys a streaming retune stage. NULL is a no-op.
void sonare_streaming_retune_destroy(SonareStreamingRetune* retune);

/// @brief Allocates the grain / ring buffers for @p sample_rate and resolves the
///        grain size. Must succeed before processing a non-empty block.
/// @details A rejected re-prepare leaves the previous prepared stage usable.
/// @return SONARE_ERROR_INVALID_PARAMETER for a NULL handle, a @p sample_rate outside
///         [8000, 384000], or a @p max_block_size outside [1, 1048576]; there is no default
///         block size, so 0 is refused.
SonareError sonare_streaming_retune_prepare(SonareStreamingRetune* retune, int sample_rate,
                                            int max_block_size);

/// @brief Clears all overlap-add state without reallocating. NULL handle is
///        SONARE_ERROR_INVALID_PARAMETER.
SonareError sonare_streaming_retune_reset(SonareStreamingRetune* retune);

/// @brief Updates the live controls. @p grain_size_samples is remembered but only
///        applied by the next prepare() (see create); the size in use, read by
///        sonare_streaming_retune_grain_size_samples, stays the prepared one until then.
/// @return SONARE_ERROR_INVALID_PARAMETER for a NULL handle or a control outside the domain
///         create states; the previous controls stay in force.
SonareError sonare_streaming_retune_set_config(SonareStreamingRetune* retune, float pitch_semitones,
                                               float mix, int grain_size_samples);

/// @brief Reads the controls as configured. Any out pointer may be NULL.
/// @details An automatic grain reads back as 0; the size in use is
///          sonare_streaming_retune_grain_size_samples. Feeding the values back to
///          set_config leaves behaviour unchanged.
SonareError sonare_streaming_retune_config(SonareStreamingRetune* retune,
                                           float* out_pitch_semitones, float* out_mix,
                                           int* out_grain_size_samples);

/// @brief Processes one mono block in place.
/// @details @p samples is read and overwritten with the retuned output. A zero-length block
///          is a no-op on a valid handle, even before prepare().
/// @return SONARE_ERROR_INVALID_STATE for a non-empty block when prepare() has not succeeded;
///         SONARE_ERROR_INVALID_PARAMETER for a NULL handle, a NULL @p samples with a non-zero
///         count, a block longer than the prepared max_block_size, or any non-finite input sample.
///         A non-finite sample is refused rather than zeroed: it would otherwise persist in the
///         grain history and poison every later block, and the caller would never learn its input
///         was altered.
SonareError sonare_streaming_retune_process_mono(SonareStreamingRetune* retune, float* samples,
                                                 size_t num_samples);

/// @brief Effective grain length in samples, 0 before prepare().
SonareError sonare_streaming_retune_grain_size_samples(SonareStreamingRetune* retune,
                                                       int* out_grain_size_samples);

/// @brief Fixed overlap-add latency in samples (one grain), 0 before prepare().
SonareError sonare_streaming_retune_latency_samples(SonareStreamingRetune* retune,
                                                    int* out_latency_samples);

#ifdef __cplusplus
}
#endif
