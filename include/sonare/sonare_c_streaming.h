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

/* Construction config for SonareStreamAnalyzer. Mirrors the relevant subset of
   sonare::StreamConfig exposed by the WASM StreamAnalyzer constructor. Use
   sonare_stream_analyzer_config_default to populate librosa/real-time defaults
   before overriding fields. */
typedef struct {
  int sample_rate;           /* Input sample rate in Hz (default 44100) */
  int n_fft;                 /* FFT size (default 2048) */
  int hop_length;            /* Hop length between frames (default 512) */
  int n_mels;                /* Number of Mel bands (default 128) */
  float fmin;                /* Minimum Mel frequency */
  float fmax;                /* Maximum Mel frequency (0 = Nyquist) */
  float tuning_ref_hz;       /* A4 tuning reference in Hz (default 440; must be
                                within 220..880, the same range
                                sonare_stream_analyzer_set_tuning_ref_hz
                                accepts) */
  int compute_magnitude;     /* Magnitude readout is not yet supported through the
                                C ABI; must be 0 (non-zero is rejected with
                                SONARE_ERROR_INVALID_PARAMETER). */
  int compute_mel;           /* Non-zero to compute Mel spectra */
  int compute_chroma;        /* Non-zero to compute chroma */
  int compute_onset;         /* Non-zero to compute onset strength */
  int compute_spectral;      /* Non-zero to compute spectral scalar features */
  int emit_every_n_frames;   /* Emit every N frames (>=1, for throttling) */
  int magnitude_downsample;  /* Downsample factor for magnitude output */
  size_t max_pending_frames; /* Unread-frame cap; overflow drops newest (default 4096) */
  float key_update_interval_sec;
  float bpm_update_interval_sec;
  int window;                     /* SonareWindowType value (default Hann) */
  int output_format;              /* Deprecated; must be SONARE_STREAM_OUTPUT_FLOAT32.
                                     Use an explicit U8/I16 read function instead. */
  size_t max_progression_entries; /* Per chord/bar progression cap; drops oldest (default 4096) */
} SonareStreamConfig;

typedef enum {
  SONARE_WINDOW_HANN = 0,
  SONARE_WINDOW_HAMMING = 1,
  SONARE_WINDOW_BLACKMAN = 2,
  SONARE_WINDOW_RECTANGULAR = 3,
} SonareWindowType;

typedef enum {
  SONARE_STREAM_OUTPUT_FLOAT32 = 0,
  SONARE_STREAM_OUTPUT_INT16 = 1, /* Legacy value; analyzer creation rejects it. */
  SONARE_STREAM_OUTPUT_UINT8 = 2, /* Legacy value; analyzer creation rejects it. */
} SonareStreamOutputFormat;

/* Feature arrays physically present in a SonareStreamFrames* result. RMS and
   timestamps are always present and therefore need no flag. */
typedef enum {
  SONARE_STREAM_FEATURE_MEL = 1u << 0,
  SONARE_STREAM_FEATURE_CHROMA = 1u << 1,
  SONARE_STREAM_FEATURE_ONSET = 1u << 2,
  SONARE_STREAM_FEATURE_SPECTRAL = 1u << 3,
} SonareStreamFeatureFlags;

typedef struct {
  int root;
  int quality;
  float start_time;
  float confidence;
} SonareStreamChordChange;

typedef struct {
  int bar_index;
  int root;
  int quality;
  float start_time;
  float confidence;
} SonareStreamBarChord;

typedef struct {
  char name[64];
  float score;
} SonareStreamPatternScore;

/* Structure-of-arrays frame buffer returned by sonare_stream_analyzer_read_frames.
   All arrays are heap-allocated; free the whole result with
   sonare_free_stream_frames. Matrix layouts are row-major [n_frames x stride]. */
typedef struct {
  int n_frames;             /* Number of frames in this batch */
  int n_mels;               /* Mel stride; 0 when MEL is absent */
  float* timestamps;        /* [n_frames] */
  float* mel;               /* [n_frames * n_mels] linear mel power (NOT dB; the
                               U8/I16 quantized variants pack dB instead) */
  float* chroma;            /* [n_frames * n_chroma], NULL when CHROMA is absent */
  float* onset_strength;    /* [n_frames], NULL when ONSET is absent */
  float* rms_energy;        /* [n_frames] */
  float* spectral_centroid; /* [n_frames], NULL when SPECTRAL is absent */
  float* spectral_flatness; /* [n_frames], NULL when SPECTRAL is absent */
  int* chord_root;          /* [n_frames], NULL when CHROMA is absent */
  int* chord_quality;       /* [n_frames], NULL when CHROMA is absent */
  float* chord_confidence;  /* [n_frames], NULL when CHROMA is absent */
  uint32_t feature_flags;   /* Bitwise SonareStreamFeatureFlags for populated arrays */
  int n_chroma;             /* Chroma bins per frame; 0 when chroma is disabled */
} SonareStreamFrames;

typedef struct {
  int n_frames;
  int n_mels;
  float* timestamps;
  uint8_t* mel; /* [n_frames * n_mels] mel in dB, quantized over [mel_db_min, mel_db_max] */
  uint8_t* chroma;
  uint8_t* onset_strength;
  uint8_t* rms_energy;
  uint8_t* spectral_centroid;
  uint8_t* spectral_flatness;
  uint32_t feature_flags; /* Bitwise SonareStreamFeatureFlags for populated arrays */
  int n_chroma;           /* Chroma bins per frame; 0 when chroma is disabled */
} SonareStreamFramesU8;

typedef struct {
  int n_frames;
  int n_mels;
  float* timestamps;
  int16_t* mel; /* [n_frames * n_mels] mel in dB, quantized over [mel_db_min, mel_db_max] */
  int16_t* chroma;
  int16_t* onset_strength;
  int16_t* rms_energy;
  int16_t* spectral_centroid;
  int16_t* spectral_flatness;
  uint32_t feature_flags; /* Bitwise SonareStreamFeatureFlags for populated arrays */
  int n_chroma;           /* Chroma bins per frame; 0 when chroma is disabled */
} SonareStreamFramesI16;

/* Quantization ranges for the u8/i16 bandwidth-reduction read paths. Mirrors
   sonare::QuantizeConfig. Populate with sonare_stream_quantize_config_default,
   then widen any range whose source values exceed the defaults: the quantizers
   clamp normalized values to [0,1], so a stream louder/quieter than these
   ranges otherwise saturates silently to the endpoints. */
typedef struct {
  float mel_db_min;   /* dB floor for mel quantization (default -80) */
  float mel_db_max;   /* dB ceiling for mel quantization (default 0) */
  float onset_max;    /* max expected onset strength (default 50) */
  float rms_max;      /* max expected RMS energy (default 1) */
  float centroid_max; /* max expected spectral centroid Hz (default 11025) */
} SonareStreamQuantizeConfig;

/* Progressive estimate + counters snapshot. Mirrors sonare::AnalyzerStats and
   ProgressiveEstimate. Variable-length arrays are owned by this result and
   released by sonare_free_stream_stats. */
typedef struct {
  int total_frames;             /* Total frames processed */
  size_t total_samples;         /* Total samples processed */
  float duration_seconds;       /* Total audio processed (s) */
  size_t pending_frames;        /* Unread frames currently retained */
  size_t dropped_output_frames; /* New output frames dropped at the configured cap */
  float bpm;                    /* Estimated BPM (0 if not yet estimated) */
  float bpm_confidence;         /* BPM confidence (0-1) */
  int bpm_candidate_count;      /* Number of BPM candidates considered */
  int key;                      /* Estimated key (0-11, -1 = unknown) */
  int key_minor;                /* Non-zero if minor mode */
  float key_confidence;         /* Key confidence (0-1) */
  int chord_root;               /* Current chord root (0-11, -1 = unknown) */
  int chord_quality;            /* Current chord quality (0=Maj, 1=Min, ...) */
  float chord_confidence;       /* Current chord confidence (0-1) */
  float chord_start_time;       /* Start time of current chord (s) */
  int current_bar;              /* Current bar index (-1 if BPM not stable) */
  float bar_duration;           /* Duration of one bar (s, 0 if not stable) */
  size_t chord_progression_count;
  SonareStreamChordChange* chord_progression;
  size_t bar_chord_progression_count;
  SonareStreamBarChord* bar_chord_progression;
  int pattern_length;
  size_t voted_pattern_count;
  SonareStreamBarChord* voted_pattern;
  char detected_pattern_name[64];
  float detected_pattern_score;
  size_t all_pattern_scores_count;
  SonareStreamPatternScore* all_pattern_scores;
  float accumulated_seconds;                /* Total audio processed for estimation (s) */
  int used_frames;                          /* Number of frames used for estimation */
  int updated;                              /* Non-zero if estimate was updated this read */
  size_t dropped_chord_progression_entries; /* Oldest chord changes dropped at cap */
  size_t dropped_bar_progression_entries;   /* Oldest bar chords dropped at cap */
} SonareStreamStats;

/// @brief Fills @p config with real-time defaults (44100 Hz, n_fft 2048, etc.).
SonareError sonare_stream_analyzer_config_default(SonareStreamConfig* config);

/// @par Threading and lifecycle
/// A handle supports one producer thread calling process, process_with_offset,
/// or finalize concurrently with one consumer thread calling available_frames,
/// a read_frames variant, stats, frame_count, or current_time. Calls within
/// either role must be serialized. Completed frames and stats snapshots use an
/// allocation-free release/acquire handoff; when the pending ring is full the
/// newly produced output frame is dropped, while analysis totals still advance.
/// reset, all set_* functions, destroy, and ownership transfer require both
/// roles to be stopped. sample_rate reads immutable construction state.

/// @brief Creates a streaming analyzer from the given config.
/// @param config Construction config (must be non-null).
/// @param out Receives the new handle (caller destroys with
///        sonare_stream_analyzer_destroy).
SonareError sonare_stream_analyzer_create(const SonareStreamConfig* config,
                                          SonareStreamAnalyzer** out);

/// @brief Destroys a streaming analyzer handle (null is a no-op).
void sonare_stream_analyzer_destroy(SonareStreamAnalyzer* analyzer);

/// @brief Feeds an audio chunk (internal cumulative offset tracking).
SonareError sonare_stream_analyzer_process(SonareStreamAnalyzer* analyzer, const float* samples,
                                           size_t n_samples);

/// @brief Feeds an audio chunk with an explicit external sample offset.
/// @details Offsets must be contiguous across non-empty calls. A gap, seek, or
///   switch from the internally tracked overload returns
///   SONARE_ERROR_INVALID_PARAMETER; call sonare_stream_analyzer_reset first to
///   discard the buffered partial frame and begin a new timeline segment.
SonareError sonare_stream_analyzer_process_with_offset(SonareStreamAnalyzer* analyzer,
                                                       const float* samples, size_t n_samples,
                                                       size_t sample_offset);

/// @brief Drains any high-rate resampler tail, then flushes the final partial
///        analysis frame with zero-padding.
SonareError sonare_stream_analyzer_finalize(SonareStreamAnalyzer* analyzer);

/// @brief Returns the number of frames available to read.
SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count);

/// @brief Reads up to @p max_frames frames into a SOA buffer (consumes them).
/// @param out Receives heap-allocated arrays (free with sonare_free_stream_frames).
SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out);

/// @brief Reads up to @p max_frames frames into an 8-bit quantized SOA buffer
///        using the default quantization ranges.
SonareError sonare_stream_analyzer_read_frames_u8(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                                  SonareStreamFramesU8* out);

/// @brief Reads up to @p max_frames frames into a 16-bit quantized SOA buffer
///        using the default quantization ranges.
SonareError sonare_stream_analyzer_read_frames_i16(SonareStreamAnalyzer* analyzer,
                                                   size_t max_frames, SonareStreamFramesI16* out);

/// @brief Fills @p config with the default quantization ranges.
SonareError sonare_stream_quantize_config_default(SonareStreamQuantizeConfig* config);

/// @brief Reads up to @p max_frames frames into an 8-bit quantized SOA buffer
///        using caller-supplied quantization ranges (NULL @p config = defaults).
SonareError sonare_stream_analyzer_read_frames_u8_ex(SonareStreamAnalyzer* analyzer,
                                                     const SonareStreamQuantizeConfig* config,
                                                     size_t max_frames, SonareStreamFramesU8* out);

/// @brief Reads up to @p max_frames frames into a 16-bit quantized SOA buffer
///        using caller-supplied quantization ranges (NULL @p config = defaults).
SonareError sonare_stream_analyzer_read_frames_i16_ex(SonareStreamAnalyzer* analyzer,
                                                      const SonareStreamQuantizeConfig* config,
                                                      size_t max_frames,
                                                      SonareStreamFramesI16* out);

/// @brief Resets analyzer state for a new stream.
SonareError sonare_stream_analyzer_reset(SonareStreamAnalyzer* analyzer, size_t base_sample_offset);

/// @brief Reads the current statistics and progressive estimate snapshot.
SonareError sonare_stream_analyzer_stats(SonareStreamAnalyzer* analyzer, SonareStreamStats* out);

/// @brief Frees variable-length arrays held by a SonareStreamStats.
void sonare_free_stream_stats(SonareStreamStats* stats);

/// @brief Returns the total number of frames processed.
SonareError sonare_stream_analyzer_frame_count(SonareStreamAnalyzer* analyzer, int* out_count);

/// @brief Returns the current stream time position in seconds.
SonareError sonare_stream_analyzer_current_time(SonareStreamAnalyzer* analyzer, float* out_seconds);

/// @brief Returns the configured input sample rate (Hz).
SonareError sonare_stream_analyzer_sample_rate(SonareStreamAnalyzer* analyzer,
                                               int* out_sample_rate);

/// @brief Sets the expected total duration (s) for pattern-lock timing.
SonareError sonare_stream_analyzer_set_expected_duration(SonareStreamAnalyzer* analyzer,
                                                         float duration_seconds);

/// @brief Sets a normalization gain applied to input samples.
SonareError sonare_stream_analyzer_set_normalization_gain(SonareStreamAnalyzer* analyzer,
                                                          float gain);

/// @brief Sets the A4 tuning reference (Hz) and rebuilds the chroma filterbank.
/// @details @p ref_hz must be within 220..880, the same range
///   SonareStreamConfig::tuning_ref_hz accepts at create time. A value outside
///   it returns SONARE_ERROR_INVALID_PARAMETER and leaves the filterbank alone;
///   it is not clamped, so a live change and a create-time setting of the same
///   value always bin the chromagram identically.
SonareError sonare_stream_analyzer_set_tuning_ref_hz(SonareStreamAnalyzer* analyzer, float ref_hz);

/// @brief Frees all arrays held by a SonareStreamFrames batch.
void sonare_free_stream_frames(SonareStreamFrames* frames);
void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames);
void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames);

#ifdef __cplusplus
}
#endif
