#pragma once

#include <stddef.h>
#include <stdint.h>

/// @brief ABI version of the flat analysis / feature POD structs declared in
///        this header (SonareKey, SonareAnalysisResult, the feature result
///        structs, SonareChordDetectionOptions, ...). Bump on ANY layout change
///        to one of those structs. Mirrors the project / engine / voice /
///        acoustic per-subsystem versioning pattern. Exposed at runtime through
///        the aggregate sonare_abi_version() so a prebuilt binding can detect a
///        struct-layout mismatch before exchanging a single byte.
#define SONARE_FEATURE_ABI_VERSION 1u

/// @brief Single aggregate C-ABI version. Encodes the per-subsystem versions so a
///        prebuilt binding linked against a different libsonare can detect a
///        layout/contract mismatch with one comparison. The byte layout is:
///          bits  0..7  : SONARE_FEATURE_ABI_VERSION
///          bits  8..15 : SONARE_PROJECT_ABI_VERSION
///          bits 16..23 : SONARE_VOICE_CHANGER_ABI_VERSION
///          bits 24..31 : SONARE_ACOUSTIC_ABI_VERSION
///        The RT command-queue / engine ABI keeps its own dedicated accessor
///        (sonare_engine_abi_version) because it versions a SharedArrayBuffer
///        record layout independent of these PODs.
/// @note  Defined in the top-level sonare_c.h once every subsystem macro is in
///        scope; do not redefine it here.

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum {
  SONARE_OK = 0,
  SONARE_ERROR_FILE_NOT_FOUND = 1,
  SONARE_ERROR_INVALID_FORMAT = 2,
  SONARE_ERROR_DECODE_FAILED = 3,
  SONARE_ERROR_INVALID_PARAMETER = 4,
  SONARE_ERROR_OUT_OF_MEMORY = 5,
  SONARE_ERROR_NOT_SUPPORTED = 6,
  SONARE_ERROR_INVALID_STATE = 7,
  SONARE_ERROR_UNKNOWN = 99
} SonareError;

// Pitch class enum
typedef enum {
  SONARE_PITCH_C = 0,
  SONARE_PITCH_CS = 1,
  SONARE_PITCH_D = 2,
  SONARE_PITCH_DS = 3,
  SONARE_PITCH_E = 4,
  SONARE_PITCH_F = 5,
  SONARE_PITCH_FS = 6,
  SONARE_PITCH_G = 7,
  SONARE_PITCH_GS = 8,
  SONARE_PITCH_A = 9,
  SONARE_PITCH_AS = 10,
  SONARE_PITCH_B = 11
} SonarePitchClass;

// Mode enum
typedef enum {
  SONARE_MODE_MAJOR = 0,
  SONARE_MODE_MINOR = 1,
  SONARE_MODE_DORIAN = 2,
  SONARE_MODE_PHRYGIAN = 3,
  SONARE_MODE_LYDIAN = 4,
  SONARE_MODE_MIXOLYDIAN = 5,
  SONARE_MODE_LOCRIAN = 6
} SonareMode;

typedef enum {
  SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER = 0,
  SONARE_KEY_PROFILE_TEMPERLEY = 1,
  SONARE_KEY_PROFILE_SHAATH = 2,
  SONARE_KEY_PROFILE_FARALDO_EDMT = 3,
  SONARE_KEY_PROFILE_FARALDO_EDMA = 4,
  SONARE_KEY_PROFILE_FARALDO_EDMM = 5,
  SONARE_KEY_PROFILE_BELLMAN_BUDGE = 6
} SonareKeyProfileType;

typedef enum {
  SONARE_TEMPOGRAM_AUTOCORRELATION = 0,
  SONARE_TEMPOGRAM_COSINE = 1
} SonareTempogramMode;

/* Ordinals mirror sonare::editing::voice_changer::VoiceCharacterPreset; do not
   reorder. The string ids returned by sonare_voice_character_preset_id() are
   exactly the entries (in this order) of SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS. */
typedef enum {
  SONARE_VC_PRESET_NEUTRAL_MONITOR = 0,
  SONARE_VC_PRESET_BRIGHT_IDOL = 1,
  SONARE_VC_PRESET_SOFT_WHISPER = 2,
  SONARE_VC_PRESET_DEEP_NARRATOR = 3,
  SONARE_VC_PRESET_ROBOT_MASCOT = 4,
  SONARE_VC_PRESET_DARK_VILLAIN = 5
} SonareVoiceCharacterPreset;

// Opaque types
typedef struct SonareAudio SonareAudio;
typedef struct SonareAnalyzer SonareAnalyzer;
typedef struct SonareMixer SonareMixer;
typedef struct SonareStrip SonareStrip;
typedef struct SonareEq SonareEq;
typedef struct SonareRealtimeEngine SonareRealtimeEngine;
typedef struct SonareRealtimeVoiceChanger SonareRealtimeVoiceChanger;
typedef struct SonareStreamAnalyzer SonareStreamAnalyzer;

#define SONARE_EQ_MAX_BANDS 24
#define SONARE_EQ_SPECTRUM_STREAM_CAPACITY 256
#define SONARE_EQ_SPECTRUM_PROFILE_BANDS 16

// Values match the offline `phaseMode` param and eq::PhaseMode ordinals;
// 0 (Inherit) is invalid for a global phase mode.
typedef enum {
  SONARE_EQ_PHASE_ZERO_LATENCY = 1,
  SONARE_EQ_PHASE_NATURAL = 2,
  SONARE_EQ_PHASE_LINEAR = 3
} SonareEqPhaseMode;

// Key structure
typedef struct {
  SonarePitchClass root;
  SonareMode mode;
  float confidence;
} SonareKey;

typedef struct {
  SonareKey key;
  float correlation;
} SonareKeyCandidate;

// Time signature structure
typedef struct {
  int numerator;
  int denominator;
  float confidence;
} SonareTimeSignature;

// Analysis result structure
typedef struct {
  float bpm;
  float bpm_confidence;
  SonareKey key;
  SonareTimeSignature time_signature;
  float* beat_times;
  size_t beat_count;
} SonareAnalysisResult;

typedef struct {
  int type;
  int error;
  int64_t render_frame;
  int64_t timeline_sample;
  int64_t audible_timeline_sample;
  int32_t graph_latency_samples_q8;
  uint32_t value;
} SonareEngineTelemetry;

/* Mirrors engine::MeterTelemetryRecord: a fixed-size meter snapshot published by
   the engine's meter tap. Drained with sonare_engine_drain_meter_telemetry. */
typedef struct {
  uint32_t target_id;
  int64_t render_frame;
  uint64_t seq;
  float peak_db_l;
  float peak_db_r;
  float rms_db_l;
  float rms_db_r;
  float true_peak_db_l;
  float true_peak_db_r;
  float max_true_peak_db;
  float correlation;
  float mono_compat_width;
  float momentary_lufs;
  float short_term_lufs;
  float integrated_lufs;
  float gain_reduction_db;
  uint32_t dropped_records;
} SonareMeterTelemetryRecord;

/* Read-only snapshot of the engine transport state. */
typedef struct {
  int playing;
  int looping;
  int64_t render_frame;
  int64_t sample_position;
  double ppq_position;
  double bpm;
  double loop_start_ppq;
  double loop_end_ppq;
  double sample_rate;
  /* Musical position derived from the tempo map (computed every block).
     Appended after the original fields to preserve struct layout. */
  double bar_start_ppq;               /* PPQ of the current bar's downbeat. */
  int64_t bar_count;                  /* Zero-based index of the current bar. */
  SonareTimeSignature time_signature; /* Time signature in effect at this PPQ. */
} SonareTransportState;

typedef struct {
  uint32_t id;
  char name[64];
  char unit[16];
  float min_value;
  float max_value;
  float default_value;
  int rt_safe;
  int default_curve;
} SonareParameterInfo;

typedef struct {
  double ppq;
  float value;
  /* PPQ-domain curve to the next breakpoint. Used by
     sonare_engine_set_automation_lane. The canonical ordinal mapping is:
       0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve
     This matches the sample-accurate mixer curves accepted by
     sonare_strip_schedule_*_automation in sonare_c_mixing.h and the
     AutomationCurve enums in the Node / Python / WASM bindings. The
     mapping is pinned by static_assert against sonare::AutomationCurve
     in src/util/automation_curve.h. */
  int curve_to_next;
} SonareAutomationPoint;

typedef struct {
  uint32_t id;
  double ppq;
  char name[64];
} SonareEngineMarker;

typedef struct {
  int enabled;
  float beat_gain;
  float accent_gain;
  /* Explicit click length in samples. 0 means "use the sample-rate-derived
     default" (the engine derives the length from click_seconds and the prepared
     sample rate). A negative value is rejected. */
  int click_samples;
  /* Click duration in seconds, used when click_samples is 0 to derive the click
     length from the prepared sample rate. Defaults to 0.002 (2 ms). */
  double click_seconds;
} SonareEngineMetronomeConfig;

typedef struct {
  uint32_t id;
  const float* const* channels;
  int num_channels;
  int64_t num_samples;
  double start_ppq;
  int64_t clip_offset_samples;
  int64_t length_samples;
  int loop;
  float gain;
  int64_t fade_in_samples;
  int64_t fade_out_samples;
} SonareEngineClip;

typedef struct {
  float* const* channels;
  int num_channels;
  int64_t capacity_frames;
} SonareEngineCaptureBuffer;

typedef struct {
  int64_t captured_frames;
  uint32_t overflow_count;
  int armed;
  int punch_enabled;
} SonareEngineCaptureStatus;

/* Canonical fallback target loudness used by sonare_engine_bounce_offline when
   normalize_lufs is enabled but target_lufs is left at 0.0f (its zero-init
   sentinel). Matches streaming-platform reference loudness (Spotify/YouTube).
   Bindings (WASM, Node, Python) MUST surface this same default; do not
   hardcode a different value. */
#define SONARE_DEFAULT_BOUNCE_TARGET_LUFS (-14.0f)

typedef struct {
  int64_t total_frames;
  int block_size;
  int num_channels;
  int target_sample_rate;
  int source_sample_rate;
  int normalize_lufs;
  /* Target integrated loudness in LUFS when normalize_lufs != 0. The value
     0.0f is treated as a "use default" sentinel and is normalized to
     SONARE_DEFAULT_BOUNCE_TARGET_LUFS (-14.0 LUFS). Pass a non-zero value
     to override. Call sonare_engine_bounce_options_default() to obtain a
     fully-initialized options struct with documented defaults. */
  float target_lufs;
  int dither; /* 0 = none, 1 = RPDF, 2 = TPDF, 3 = noise-shaped */
  int dither_bits;
  uint32_t dither_seed;
} SonareEngineBounceOptions;

typedef struct {
  float* interleaved; /* heap-allocated; free with sonare_free_bounce_result */
  size_t sample_count;
  int64_t frames;
  int num_channels;
  int sample_rate;
  float integrated_lufs;
} SonareEngineBounceResult;

typedef struct {
  int64_t total_frames;
  int block_size;
  int num_channels;
  uint32_t clip_id;
  double start_ppq;
  float gain;
} SonareEngineFreezeOptions;

typedef struct {
  uint32_t clip_id;
  int64_t frames;
  int num_channels;
} SonareEngineFreezeResult;

typedef struct {
  char id[64];
  int type; /* 0 = pass-through, 1 = gain */
  float gain_db;
  int num_ports;
} SonareEngineGraphNode;

typedef struct {
  char source_node[64];
  int source_port;
  char dest_node[64];
  int dest_port;
  int mix; /* 0 = replace, 1 = add */
} SonareEngineGraphConnection;

typedef struct {
  uint32_t param_id;
  char node_id[64];
} SonareEngineGraphParameterBinding;

typedef struct {
  const SonareEngineGraphNode* nodes;
  size_t node_count;
  const SonareEngineGraphConnection* connections;
  size_t connection_count;
  const SonareEngineGraphParameterBinding* parameter_bindings;
  size_t parameter_binding_count;
  char input_node[64];
  char output_node[64];
  int num_channels;
} SonareEngineGraphSpec;

typedef enum {
  SONARE_GROOVE_STRAIGHT = 0,
  SONARE_GROOVE_SHUFFLE = 1,
  SONARE_GROOVE_SWING = 2
} SonareGrooveType;

typedef enum {
  SONARE_CHORD_MAJOR = 0,
  SONARE_CHORD_MINOR = 1,
  SONARE_CHORD_DIMINISHED = 2,
  SONARE_CHORD_AUGMENTED = 3,
  SONARE_CHORD_DOMINANT7 = 4,
  SONARE_CHORD_MAJOR7 = 5,
  SONARE_CHORD_MINOR7 = 6,
  SONARE_CHORD_SUS2 = 7,
  SONARE_CHORD_SUS4 = 8,
  SONARE_CHORD_UNKNOWN = 9,
  SONARE_CHORD_ADD9 = 10,
  SONARE_CHORD_MINOR_ADD9 = 11,
  SONARE_CHORD_DIM7 = 12,
  SONARE_CHORD_HALF_DIM7 = 13,
  SONARE_CHORD_MAJOR9 = 14,
  SONARE_CHORD_DOMINANT9 = 15,
  SONARE_CHORD_SUS2_ADD4 = 16
} SonareChordQuality;

// Audio functions
SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out);
SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out);

#ifndef __EMSCRIPTEN__
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
#endif

void sonare_audio_free(SonareAudio* audio);
const float* sonare_audio_data(const SonareAudio* audio);
size_t sonare_audio_length(const SonareAudio* audio);
int sonare_audio_sample_rate(const SonareAudio* audio);
float sonare_audio_duration(const SonareAudio* audio);
SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float* out_bpm);
SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key);
SonareError sonare_audio_detect_beats(const SonareAudio* audio, float** out_times,
                                      size_t* out_count);
SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count);
SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count);
SonareError sonare_audio_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Quick detection functions
SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float* out_bpm);
SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              SonareKey* out_key);
SonareError sonare_detect_key_with_options(const float* samples, size_t length, int sample_rate,
                                           int n_fft, int hop_length, int use_hpss,
                                           int loudness_weighted, float high_pass_hz,
                                           SonareKey* out_key);
SonareError sonare_detect_key_with_options_and_modes(const float* samples, size_t length,
                                                     int sample_rate, int n_fft, int hop_length,
                                                     int use_hpss, int loudness_weighted,
                                                     float high_pass_hz, const SonareMode* modes,
                                                     size_t mode_count, SonareKey* out_key);
SonareError sonare_detect_key_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKey* out_key);
SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         int n_fft, int hop_length, int use_hpss,
                                         int loudness_weighted, float high_pass_hz,
                                         SonareKeyCandidate** out_candidates, size_t* out_count);
SonareError sonare_detect_key_candidates_with_modes(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyCandidate** out_candidates, size_t* out_count);
SonareError sonare_detect_key_candidates_with_extended_options(
    const float* samples, size_t length, int sample_rate, int n_fft, int hop_length, int use_hpss,
    int loudness_weighted, float high_pass_hz, const SonareMode* modes, size_t mode_count,
    SonareKeyProfileType profile_type, const char* genre_hint, SonareKeyCandidate** out_candidates,
    size_t* out_count);
SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate,
                                float** out_times, size_t* out_count);
SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count);
SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 float** out_times, size_t* out_count);

// Full analysis
SonareError sonare_analyze(const float* samples, size_t length, int sample_rate,
                           SonareAnalysisResult* out);

/* Progress callback for the JSON analysis variants: progress in [0,1] plus a
   stage label. user_data is the opaque pointer passed to the analyze call. */
typedef void (*SonareAnalyzeProgressCallback)(float progress, const char* stage, void* user_data);

/* Full analysis serialized to a camelCase JSON object. Unlike sonare_analyze
   (which fills only the flat bpm/key/beats struct), this returns the complete
   result: chords, sections, timbre, dynamics, rhythm, melody and form, with
   per-beat strength. *out_json is heap-allocated and MUST be released with
   sonare_free_string. */
SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                char** out_json);

/* Same as sonare_analyze_json but reports per-stage progress. A null callback
   runs silently. The callback fires on the calling thread before return. */
SonareError sonare_analyze_json_with_progress(const float* samples, size_t length, int sample_rate,
                                              SonareAnalyzeProgressCallback callback,
                                              void* user_data, char** out_json);

// Memory management
void sonare_free_floats(float* ptr);
void sonare_free_ints(int* ptr);
void sonare_free_string(char* ptr);
void sonare_free_key_candidates(SonareKeyCandidate* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// Error handling
const char* sonare_error_message(SonareError error);

/// @brief Returns the detailed message for the most recent error on the calling thread.
/// @details The returned string is owned by libsonare and valid until the next API call on
///   the same thread. The pointer is never NULL; returns an empty string ("") when no
///   detailed message is currently recorded.
///
///   CONTRACT (read carefully):
///   - Every public C-ABI call CLEARS the thread-local message on entry, so a
///     detailed message can never leak from an earlier, unrelated call.
///   - A message is recorded ONLY on the caught-C++-exception return path (the
///     library mapped a thrown sonare::SonareException / std::exception to a
///     SonareError). For those, this returns the exception's what() text.
///   - Validation early-returns that produce an error code WITHOUT throwing
///     (e.g. a NULL out-pointer, an out-of-range sample rate, a non-finite input
///     sample) record NO message: this returns "" even though the call failed.
///     Use sonare_error_message(SonareError) for a human-readable string for ANY
///     error code; use this only for the extra detail of exception-path errors.
///   - EXCEPTION (non-fatal diagnostic): the mixer graph-compile path
///     (sonare_mixer_compile / sonare_mixer_process_stereo / the lazy compile in
///     sonare_mixer_from_scene_json) may record a WARNING message on a SUCCESS
///     return (SONARE_OK) when it detects a likely routing mistake — e.g. an
///     explicit submix/aux bus that is fed by strips but has no onward connection
///     to the master (its audio is silently dropped). Check this after a
///     successful mixer compile to surface such warnings. The message is still
///     cleared on the next API call on the thread.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_error_message(void);

// Version
const char* sonare_version(void);
uint32_t sonare_engine_abi_version(void);

/// @brief Returns 1 if libsonare was compiled with FFmpeg-backed decoding for
///        M4A/AAC/FLAC/OGG, 0 otherwise.
/// @details This reflects the value of the @c SONARE_WITH_FFMPEG CMake option at
///   build time. Language bindings expose this so test suites can conditionally
///   exercise the FFmpeg decode path without false failures.
int sonare_has_ffmpeg_support(void);

// ============================================================================
// Result structures for feature/effect functions
// ============================================================================

// STFT result
typedef struct {
  int n_bins;
  int n_frames;
  int n_fft;
  int hop_length;
  int sample_rate;
  float* magnitude;  // n_bins * n_frames, caller frees with sonare_free_floats
  float* power;      // n_bins * n_frames, caller frees with sonare_free_floats
} SonareStftResult;

// Mel spectrogram result
typedef struct {
  int n_mels;
  int n_frames;
  int sample_rate;
  int hop_length;
  float* power;  // n_mels * n_frames
  float* db;     // n_mels * n_frames
} SonareMelResult;

// MFCC result
typedef struct {
  int n_mfcc;
  int n_frames;
  float* coefficients;  // n_mfcc * n_frames
} SonareMfccResult;

// Chroma result
typedef struct {
  int n_chroma;
  int n_frames;
  int sample_rate;
  int hop_length;
  float* features;     // n_chroma * n_frames
  float* mean_energy;  // n_chroma
} SonareChromaResult;

// Pitch result
typedef struct {
  int n_frames;
  float* f0;           // n_frames
  float* voiced_prob;  // n_frames
  int* voiced_flag;    // n_frames (0 or 1)
  float median_f0;
  float mean_f0;
} SonarePitchResult;

// HPSS result
typedef struct {
  float* harmonic;    // length samples
  float* percussive;  // length samples
  size_t length;
  int sample_rate;
} SonareHpssResult;

typedef struct {
  float bpm;
  float confidence;
} SonareBpmCandidate;

typedef struct {
  float rt60;
  float edt;
  float c50;
  float c80;
  float d50;
  float* rt60_bands;
  float* edt_bands;
  float* c50_bands;
  float* c80_bands;
  size_t band_count;
  float confidence;
  int is_blind;
} SonareAcousticResult;

// LUFS loudness result (no heap pointers; no free function required).
typedef struct {
  float integrated_lufs;
  float momentary_lufs;
  float short_term_lufs;
  float loudness_range;
} SonareLufsResult;

typedef struct {
  float bpm;
  float confidence;
  SonareBpmCandidate* candidates;
  size_t candidate_count;
  float* autocorrelation;
  size_t autocorrelation_count;
  float* tempogram;
  size_t tempogram_count;
} SonareBpmAnalysisResult;

typedef struct {
  float bpm;
  SonareTimeSignature time_signature;
  SonareGrooveType groove_type;
  float syncopation;
  float pattern_regularity;
  float tempo_stability;
  float* beat_intervals;
  size_t beat_interval_count;
} SonareRhythmResult;

typedef struct {
  float dynamic_range_db;
  float peak_db;
  float rms_db;
  float crest_factor;
  float loudness_range_db;
  int is_compressed;
  float* loudness_times;
  float* loudness_rms_db;
  size_t loudness_count;
} SonareDynamicsResult;

/// @brief Timbre metrics for one analysis window.
typedef struct {
  float brightness;
  float warmth;
  float density;
  float roughness;
  float complexity;
} SonareTimbreFrame;

typedef struct {
  float brightness;
  float warmth;
  float density;
  float roughness;
  float complexity;
  float* spectral_centroid;
  size_t spectral_centroid_count;
  float* spectral_flatness;
  size_t spectral_flatness_count;
  float* spectral_rolloff;
  size_t spectral_rolloff_count;
  /// @brief Time-varying timbre metrics, one entry per analysis window.
  /// Owned by the result; released by sonare_free_timbre_result.
  SonareTimbreFrame* timbre_over_time;
  size_t timbre_over_time_count;
} SonareTimbreResult;

typedef struct {
  SonarePitchClass root;
  SonareChordQuality quality;
  float start;
  float end;
  float confidence;
  SonarePitchClass bass;
} SonareChord;

typedef struct {
  SonareChord* chords;
  size_t chord_count;
} SonareChordAnalysisResult;

/* Heap-owned array of NUL-terminated C strings. @c items has @c count entries,
   each a separately allocated string. Free the whole array (items and the
   container array) with sonare_free_string_array. An empty result yields
   @c items == NULL and @c count == 0. */
typedef struct {
  char** items;
  size_t count;
} SonareStringArray;

/* Song-structure section types (mirrors sonare::SectionType ordinals). */
typedef enum {
  SONARE_SECTION_INTRO = 0,
  SONARE_SECTION_VERSE = 1,
  SONARE_SECTION_PRE_CHORUS = 2,
  SONARE_SECTION_CHORUS = 3,
  SONARE_SECTION_BRIDGE = 4,
  SONARE_SECTION_INSTRUMENTAL = 5,
  SONARE_SECTION_OUTRO = 6,
  SONARE_SECTION_UNKNOWN = 7
} SonareSectionType;

typedef struct {
  SonareSectionType type;
  float start;        /* seconds */
  float end;          /* seconds */
  float energy_level; /* [0, 1] */
  float confidence;   /* [0, 1] */
} SonareSection;

typedef struct {
  SonareSection* sections; /* free with sonare_free_section_result */
  size_t section_count;
} SonareSectionResult;

typedef struct {
  float time;       /* seconds */
  float frequency;  /* Hz (0 if unvoiced) */
  float confidence; /* [0, 1] */
} SonareMelodyPoint;

typedef struct {
  SonareMelodyPoint* points; /* free with sonare_free_melody_result */
  size_t point_count;
  float pitch_range_octaves;
  float pitch_stability;
  float mean_frequency;
  float vibrato_rate;
} SonareMelodyResult;

typedef struct {
  float min_duration;
  float smoothing_window;
  float threshold;
  int use_triads_only;
  int n_fft;
  int hop_length;
  int use_beat_sync;
  int use_hmm;
  int hmm_beam_width;
  int use_key_context;
  SonarePitchClass key_root;
  SonareMode key_mode;
  int detect_inversions;
  int chroma_method;  // 0 = STFT, 1 = NNLS
} SonareChordDetectionOptions;

#ifdef __cplusplus
// Layout guards for the previously-unversioned analysis / feature PODs. Any
// padding / reorder / member add/remove trips one of these, forcing a bump of
// SONARE_FEATURE_ABI_VERSION (and thus a change in the aggregate
// sonare_abi_version()). Sizes are spelled in terms of the dominant member so
// the asserts survive benign ABI-equivalent typedef differences across targets.
static_assert(sizeof(SonareKey) == 3u * sizeof(int), "SonareKey layout changed");
static_assert(sizeof(SonareKeyCandidate) == sizeof(SonareKey) + sizeof(float),
              "SonareKeyCandidate layout changed");
static_assert(sizeof(SonareTimeSignature) == 2u * sizeof(int) + sizeof(float),
              "SonareTimeSignature layout changed");
static_assert(offsetof(SonareAnalysisResult, beat_times) ==
                  sizeof(float) + sizeof(float) + sizeof(SonareKey) + sizeof(SonareTimeSignature),
              "SonareAnalysisResult scalar prefix layout changed");
static_assert(offsetof(SonareAnalysisResult, beat_count) ==
                  offsetof(SonareAnalysisResult, beat_times) + sizeof(float*),
              "SonareAnalysisResult tail layout changed");
#endif

#ifdef __cplusplus
}
#endif
