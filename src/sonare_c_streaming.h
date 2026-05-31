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
  int sample_rate;          /* Input sample rate in Hz (default 44100) */
  int n_fft;                /* FFT size (default 2048) */
  int hop_length;           /* Hop length between frames (default 512) */
  int n_mels;               /* Number of Mel bands (default 128) */
  float fmin;               /* Minimum Mel frequency */
  float fmax;               /* Maximum Mel frequency (0 = Nyquist) */
  float tuning_ref_hz;      /* A4 tuning reference */
  int compute_magnitude;    /* Non-zero to compute magnitude spectra */
  int compute_mel;          /* Non-zero to compute Mel spectra */
  int compute_chroma;       /* Non-zero to compute chroma */
  int compute_onset;        /* Non-zero to compute onset strength */
  int compute_spectral;     /* Non-zero to compute spectral scalar features */
  int emit_every_n_frames;  /* Emit every N frames (>=1, for throttling) */
  int magnitude_downsample; /* Downsample factor for magnitude output */
  float key_update_interval_sec;
  float bpm_update_interval_sec;
  int window;        /* SonareWindowType value (default Hann) */
  int output_format; /* SonareStreamOutputFormat value (default Float32) */
} SonareStreamConfig;

typedef enum {
  SONARE_WINDOW_HANN = 0,
  SONARE_WINDOW_HAMMING = 1,
  SONARE_WINDOW_BLACKMAN = 2,
  SONARE_WINDOW_RECTANGULAR = 3,
} SonareWindowType;

typedef enum {
  SONARE_STREAM_OUTPUT_FLOAT32 = 0,
  SONARE_STREAM_OUTPUT_INT16 = 1,
  SONARE_STREAM_OUTPUT_UINT8 = 2,
} SonareStreamOutputFormat;

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
  int n_mels;               /* Mel bands per frame (mel stride) */
  float* timestamps;        /* [n_frames] */
  float* mel;               /* [n_frames * n_mels] */
  float* chroma;            /* [n_frames * 12] */
  float* onset_strength;    /* [n_frames] */
  float* rms_energy;        /* [n_frames] */
  float* spectral_centroid; /* [n_frames] */
  float* spectral_flatness; /* [n_frames] */
  int* chord_root;          /* [n_frames] */
  int* chord_quality;       /* [n_frames] */
  float* chord_confidence;  /* [n_frames] */
} SonareStreamFrames;

typedef struct {
  int n_frames;
  int n_mels;
  float* timestamps;
  uint8_t* mel;
  uint8_t* chroma;
  uint8_t* onset_strength;
  uint8_t* rms_energy;
  uint8_t* spectral_centroid;
  uint8_t* spectral_flatness;
} SonareStreamFramesU8;

typedef struct {
  int n_frames;
  int n_mels;
  float* timestamps;
  int16_t* mel;
  int16_t* chroma;
  int16_t* onset_strength;
  int16_t* rms_energy;
  int16_t* spectral_centroid;
  int16_t* spectral_flatness;
} SonareStreamFramesI16;

/* Progressive estimate + counters snapshot. Mirrors the scalar fields of
   sonare::AnalyzerStats and ProgressiveEstimate. Variable-length progression
   arrays are intentionally omitted from this fixed-size struct; query the SOA
   per-frame chord fields for chord history. */
typedef struct {
  int total_frames;        /* Total frames processed */
  size_t total_samples;    /* Total samples processed */
  float duration_seconds;  /* Total audio processed (s) */
  float bpm;               /* Estimated BPM (0 if not yet estimated) */
  float bpm_confidence;    /* BPM confidence (0-1) */
  int bpm_candidate_count; /* Number of BPM candidates considered */
  int key;                 /* Estimated key (0-11, -1 = unknown) */
  int key_minor;           /* Non-zero if minor mode */
  float key_confidence;    /* Key confidence (0-1) */
  int chord_root;          /* Current chord root (0-11, -1 = unknown) */
  int chord_quality;       /* Current chord quality (0=Maj, 1=Min, ...) */
  float chord_confidence;  /* Current chord confidence (0-1) */
  float chord_start_time;  /* Start time of current chord (s) */
  int current_bar;         /* Current bar index (-1 if BPM not stable) */
  float bar_duration;      /* Duration of one bar (s, 0 if not stable) */
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
  float accumulated_seconds; /* Total audio processed for estimation (s) */
  int used_frames;           /* Number of frames used for estimation */
  int updated;               /* Non-zero if estimate was updated this read */
} SonareStreamStats;

/// @brief Fills @p config with real-time defaults (44100 Hz, n_fft 2048, etc.).
SonareError sonare_stream_analyzer_config_default(SonareStreamConfig* config);

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
SonareError sonare_stream_analyzer_process_with_offset(SonareStreamAnalyzer* analyzer,
                                                       const float* samples, size_t n_samples,
                                                       size_t sample_offset);

/// @brief Returns the number of frames available to read.
SonareError sonare_stream_analyzer_available_frames(SonareStreamAnalyzer* analyzer,
                                                    size_t* out_count);

/// @brief Reads up to @p max_frames frames into a SOA buffer (consumes them).
/// @param out Receives heap-allocated arrays (free with sonare_free_stream_frames).
SonareError sonare_stream_analyzer_read_frames(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                               SonareStreamFrames* out);

/// @brief Reads up to @p max_frames frames into an 8-bit quantized SOA buffer.
SonareError sonare_stream_analyzer_read_frames_u8(SonareStreamAnalyzer* analyzer, size_t max_frames,
                                                  SonareStreamFramesU8* out);

/// @brief Reads up to @p max_frames frames into a 16-bit quantized SOA buffer.
SonareError sonare_stream_analyzer_read_frames_i16(SonareStreamAnalyzer* analyzer,
                                                   size_t max_frames, SonareStreamFramesI16* out);

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
SonareError sonare_stream_analyzer_set_tuning_ref_hz(SonareStreamAnalyzer* analyzer, float ref_hz);

/// @brief Frees all arrays held by a SonareStreamFrames batch.
void sonare_free_stream_frames(SonareStreamFrames* frames);
void sonare_free_stream_frames_u8(SonareStreamFramesU8* frames);
void sonare_free_stream_frames_i16(SonareStreamFramesI16* frames);

#ifdef __cplusplus
}
#endif
