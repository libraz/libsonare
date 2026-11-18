#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Rhythmic feel of the beat subdivision.
typedef enum SONARE_ENUM_BASE {
  SONARE_GROOVE_STRAIGHT = 0,  ///< Even subdivisions.
  SONARE_GROOVE_SHUFFLE = 1,   ///< Shuffle feel (swing ratio above 0.55).
  SONARE_GROOVE_SWING = 2      ///< Swing feel (swing ratio above 0.6).
} SonareGrooveType;

/// @brief Chord quality, the type of a chord independent of its root.
typedef enum SONARE_ENUM_BASE {
  SONARE_CHORD_MAJOR = 0,        ///< Major triad.
  SONARE_CHORD_MINOR = 1,        ///< Minor triad.
  SONARE_CHORD_DIMINISHED = 2,   ///< Diminished triad.
  SONARE_CHORD_AUGMENTED = 3,    ///< Augmented triad.
  SONARE_CHORD_DOMINANT7 = 4,    ///< Dominant seventh.
  SONARE_CHORD_MAJOR7 = 5,       ///< Major seventh.
  SONARE_CHORD_MINOR7 = 6,       ///< Minor seventh.
  SONARE_CHORD_SUS2 = 7,         ///< Suspended second.
  SONARE_CHORD_SUS4 = 8,         ///< Suspended fourth.
  SONARE_CHORD_UNKNOWN = 9,      ///< No chord matched, or no chord sounding.
  SONARE_CHORD_ADD9 = 10,        ///< Major triad with an added ninth.
  SONARE_CHORD_MINOR_ADD9 = 11,  ///< Minor triad with an added ninth.
  SONARE_CHORD_DIM7 = 12,        ///< Diminished seventh.
  SONARE_CHORD_HALF_DIM7 = 13,   ///< Half-diminished seventh.
  SONARE_CHORD_MAJOR9 = 14,      ///< Major ninth.
  SONARE_CHORD_DOMINANT9 = 15,   ///< Dominant ninth.
  SONARE_CHORD_SUS2_ADD4 = 16,   ///< Suspended second with an added fourth.
  /** Extended vocabulary. Only produced when the analyzer runs with its full
     template set (use_triads_only = 0). Values are appended, never reordered. */
  SONARE_CHORD_MAJOR6 = 17,
  SONARE_CHORD_MINOR6 = 18,
  SONARE_CHORD_MINOR_MAJOR7 = 19,
  SONARE_CHORD_DOMINANT7_SUS4 = 20,
  SONARE_CHORD_DOMINANT11 = 21,
  SONARE_CHORD_DOMINANT13 = 22,
  SONARE_CHORD_DOMINANT7_FLAT9 = 23,
  SONARE_CHORD_DOMINANT7_SHARP9 = 24
} SonareChordQuality;
/** Number of SonareChordQuality enumerators. A table indexed by chord quality
   sizes itself from this rather than from a literal. */
#define SONARE_CHORD_QUALITY_COUNT 25

/// @brief Short-time Fourier transform spectrogram, laid out as n_bins * n_frames.
typedef struct {
  int n_bins;        ///< Frequency bins per frame.
  int n_frames;      ///< Time frames.
  int n_fft;         ///< FFT length in samples.
  int hop_length;    ///< Hop between frames in samples.
  int sample_rate;   ///< Sample rate in Hz.
  float* magnitude;  ///< n_bins * n_frames, caller frees with sonare_free_floats
  float* power;      ///< n_bins * n_frames, caller frees with sonare_free_floats
} SonareStftResult;

/** Layout version of SonareMelResult and SonareMfccResult, written by the
   function that fills them. */
#define SONARE_MEL_RESULT_VERSION 2
#define SONARE_MFCC_RESULT_VERSION 2  ///< Layout version of SonareMfccResult.

/** Mel spectrogram result. Besides the matrices it carries the parameters of
   the forward transform, so the result is enough to invert it: the inverse
   entry points take the same values as arguments. */
typedef struct {
  /** SONARE_MEL_RESULT_VERSION. */
  int struct_version;
  int n_mels;       ///< Mel bands per frame.
  int n_frames;     ///< Time frames.
  int sample_rate;  ///< Sample rate in Hz.
  int hop_length;   ///< Hop between frames in samples.
  /** FFT size of the source STFT. */
  int n_fft;
  /** Mel range in Hz as applied: fmax is sr/2 when the request left it at 0. */
  float fmin;
  float fmax;  ///< Upper edge of the Mel range in Hz.
  /** Non-zero for the HTK Mel formula, 0 for Slaney. */
  int htk;
  /** Non-zero when the matrix being described is in dB. Always 0 here: @c power
     is linear power and @c db is its dB form. */
  int is_db;
  float* power;  ///< n_mels * n_frames
  float* db;     ///< n_mels * n_frames
} SonareMelResult;

/** MFCC result. Carries the Mel parameters its coefficients were derived with,
   plus the lifter, so the result is enough to invert it. */
typedef struct {
  /** SONARE_MFCC_RESULT_VERSION. */
  int struct_version;
  int n_mfcc;       ///< Coefficients per frame.
  int n_frames;     ///< Time frames.
  int sample_rate;  ///< Sample rate in Hz.
  int hop_length;   ///< Hop between frames in samples.
  int n_fft;        ///< FFT size of the source STFT.
  /** Mel bands the coefficients were taken from. */
  int n_mels;
  float fmin_hz;  ///< Lower edge of the Mel range in Hz.
  float fmax_hz;  ///< Upper edge of the Mel range in Hz.
  int htk;        ///< Non-zero for the HTK Mel formula, 0 for Slaney.
  /** Always 0: the coefficients are a cepstrum, not a dB spectrogram. */
  int is_db;
  /** Cepstral lifter applied to the coefficients; 0 = none. */
  float lifter;
  float* coefficients;  ///< n_mfcc * n_frames
} SonareMfccResult;

/// Chroma result
typedef struct {
  int n_chroma;        ///< Chroma bins per frame.
  int n_frames;        ///< Time frames.
  int sample_rate;     ///< Sample rate in Hz.
  int hop_length;      ///< Hop between frames in samples.
  float* features;     ///< Chromagram, [n_chroma x n_frames] row-major.
  float* mean_energy;  ///< n_chroma
} SonareChromaResult;

/// Pitch result
typedef struct {
  int n_frames;        ///< Time frames.
  float* f0;           ///< n_frames
  float* voiced_prob;  ///< n_frames
  int* voiced_flag;    ///< n_frames (0 or 1)
  float median_f0;     ///< Median f0 of the voiced frames in Hz; 0 when no frame is voiced.
  float mean_f0;       ///< Mean f0 of the voiced frames in Hz; 0 when no frame is voiced.
} SonarePitchResult;

/// HPSS result
typedef struct {
  float* harmonic;    ///< length samples
  float* percussive;  ///< length samples
  size_t length;      ///< Samples in each of harmonic and percussive.
  int sample_rate;    ///< Sample rate in Hz.
} SonareHpssResult;

/// @brief One tempo hypothesis with its confidence.
typedef struct {
  float bpm;         ///< Tempo in BPM.
  float confidence;  ///< Confidence of this tempo, [0, 1].
} SonareBpmCandidate;

/** Room-acoustic measures.

   Two fields need a known direct-sound arrival time, which only a measured
   impulse response supplies, and are therefore not computed when is_blind is
   nonzero:
     - the clarity family (c50, c80, d50, c50_bands, c80_bands), and
     - edt / edt_bands, which measure the 0 to -10 dB decay. The blind
       estimator only fits the late decay that rt60 comes from, so it has
       nothing independent to report here.
   Only rt60 / rt60_bands are estimated in both modes.

   "Not computed" has exactly two representations, and every binding maps them
   to one observable form each:
     - a scalar reports NaN (c50, c80, d50, edt);
     - a band pointer is NULL (c50_bands, c80_bands), which every binding
       surfaces as an EMPTY array, never as a zero-filled one -- a zero-filled
       clarity array is indistinguishable from a genuine 0 dB measurement.
   edt_bands is the one exception to the second rule: it stays a non-NULL array
   of band_count NaNs so that rt60_bands and edt_bands can always be indexed by
   the same band index. */
typedef struct {
  float rt60;         ///< Reverberation time in seconds.
  float edt;          ///< Early decay time in seconds.
  float c50;          ///< Clarity of the first 50 ms, in dB.
  float c80;          ///< Clarity of the first 80 ms, in dB.
  float d50;          ///< Definition: share of the energy arriving in the first 50 ms.
  float* rt60_bands;  ///< Reverberation time per frequency band, in seconds; band_count entries.
  float* edt_bands;   ///< Early decay time per frequency band, in seconds; band_count entries.
  float* c50_bands;   ///< Clarity of the first 50 ms per band, in dB; band_count entries, or NULL.
  float* c80_bands;   ///< Clarity of the first 80 ms per band, in dB; band_count entries, or NULL.
  size_t band_count;  ///< Entries in each non-NULL band array.
  float confidence;   ///< Confidence of the estimate.
  /// Non-zero when estimated from a recording rather than a measured impulse response.
  int is_blind;
} SonareAcousticResult;

/// LUFS loudness result (no heap pointers; no free function required).
typedef struct {
  float integrated_lufs;      ///< Integrated loudness over the whole input, in LUFS.
  float momentary_lufs;       ///< final complete 400 ms window, not Max-M
  float short_term_lufs;      ///< final complete 3 s window, not Max-S
  float max_momentary_lufs;   ///< maximum 400 ms window (EBU R128 Max-M)
  float max_short_term_lufs;  ///< maximum 3 s window (EBU R128 Max-S)
  float loudness_range_lu;    ///< Loudness range, in LU.
} SonareLufsResult;

/// @brief Tempo estimate with its candidates and the curves it was derived from.
typedef struct {
  float bpm;  ///< Estimated tempo in BPM.
  /** Winning tempo cluster's vote share within [bpm_min, bpm_max], searched at the input rate;
     can differ from sonare_analyze's bpm_confidence (60-200 BPM at 22050 Hz) on the same input. */
  float confidence;
  /** Chosen tempo first (candidates[0].bpm == bpm), then other bins by descending vote share. */
  SonareBpmCandidate* candidates;
  size_t candidate_count;  ///< Entries in candidates.
  /// Autocorrelation of the onset evidence over lag; autocorrelation_count entries.
  float* autocorrelation;
  size_t autocorrelation_count;  ///< Entries in autocorrelation.
  float* tempogram;  ///< Tempogram values, tempo bins by time frames; tempogram_count entries.
  size_t tempogram_count;  ///< Entries in tempogram.
} SonareBpmAnalysisResult;

/// @brief Rhythm characteristics; free with sonare_free_rhythm_result.
typedef struct {
  float bpm;                           ///< Tempo in BPM.
  SonareTimeSignature time_signature;  ///< Detected meter.
  SonareGrooveType groove_type;        ///< Detected groove.
  float syncopation;                   ///< Syncopation level, [0, 1].
  float pattern_regularity;            ///< Rhythmic regularity, [0, 1]; 0 with no beat interval.
  float tempo_stability;       ///< Tempo stability over time, [0, 1]; 0 below two beat intervals.
  float* beat_intervals;       ///< Seconds between consecutive beats; beat_interval_count entries.
  size_t beat_interval_count;  ///< Entries in beat_intervals.
} SonareRhythmResult;

/// @brief Dynamics summary; free with sonare_free_dynamics_result.
typedef struct {
  float dynamic_range_db;   ///< Dynamic range, in dB.
  float peak_db;            ///< Peak level, in dB.
  float rms_db;             ///< RMS level, in dB.
  float crest_factor;       ///< Ratio of peak to RMS level.
  float loudness_range_lu;  ///< Loudness range, in LU.
  int is_compressed;        ///< Non-zero when the audio appears heavily compressed; 0 for silence.
  float* loudness_times_sec;  ///< Time of each loudness point in seconds; loudness_count entries.
  float* loudness_rms_db;     ///< RMS level at each loudness point, in dB; loudness_count entries.
  size_t loudness_count;      ///< Entries in each loudness array.
} SonareDynamicsResult;

/// @brief Timbre metrics for one analysis window.
typedef struct {
  float brightness;  ///< Spectral brightness, [0, 1]; high is bright.
  float warmth;      ///< Spectral warmth, [0, 1]; high is warm.
  float density;     ///< Spectral density, [0, 1]; 0 without spectral energy.
  float roughness;   ///< Spectral roughness, [0, 1]; high is rough.
  float complexity;  ///< Harmonic complexity, [0, 1].
} SonareTimbreFrame;

/// @brief Timbre of the whole input plus per-frame spectral curves.
typedef struct {
  float brightness;                ///< Overall spectral brightness, [0, 1]; high is bright.
  float warmth;                    ///< Overall spectral warmth, [0, 1]; high is warm.
  float density;                   ///< Overall spectral density, [0, 1]; 0 without spectral energy.
  float roughness;                 ///< Overall spectral roughness, [0, 1]; high is rough.
  float complexity;                ///< Overall harmonic complexity, [0, 1].
  float* spectral_centroid;        ///< Spectral centroid in Hz, one per frame.
  size_t spectral_centroid_count;  ///< Entries in spectral_centroid.
  float* spectral_flatness;        ///< Spectral flatness (0 to 1), one per frame.
  size_t spectral_flatness_count;  ///< Entries in spectral_flatness.
  float* spectral_rolloff;         ///< 85% energy rolloff in Hz, one per frame.
  size_t spectral_rolloff_count;   ///< Entries in spectral_rolloff.
  /// @brief Time-varying timbre metrics, one entry per analysis window.
  /// Owned by the result; released by sonare_free_timbre_result.
  SonareTimbreFrame* timbre_over_time;
  size_t timbre_over_time_count;  ///< Entries in timbre_over_time.
} SonareTimbreResult;

/// @brief One detected chord span.
typedef struct {
  SonarePitchClass root;       ///< Root pitch class.
  SonareChordQuality quality;  ///< Chord quality.
  float start_sec;             ///< Start time in seconds.
  float end_sec;               ///< End time in seconds.
  float confidence;            ///< Detection confidence, [0, 1].
  SonarePitchClass bass;       ///< Bass pitch class, for inversion notation.
} SonareChord;

#define SONARE_CHORD_ANALYSIS_RESULT_VERSION 2  ///< Layout version of SonareChordAnalysisResult.

/// @brief Detected chord sequence; free with sonare_free_chord_analysis_result.
typedef struct {
  /** SONARE_CHORD_ANALYSIS_RESULT_VERSION, written by the filling function. */
  int struct_version;
  /** Tuning the chroma was built with, in cents: the options' value, or the
     measured one when tuning_auto was set. The entry points that take no
     tuning measure it. */
  float tuning_cents;
  SonareChord* chords;  ///< Detected chords; chord_count entries.
  size_t chord_count;   ///< Entries in chords.
} SonareChordAnalysisResult;

/** Heap-owned array of NUL-terminated C strings. @c items has @c count entries,
   each a separately allocated string. Free the whole array (items and the
   container array) with sonare_free_string_array. An empty result yields
   @c items == NULL and @c count == 0. */
typedef struct {
  char** items;  ///< Separately allocated NUL-terminated strings; count entries.
  size_t count;  ///< Entries in items.
} SonareStringArray;

/** Song-structure section types (mirrors sonare::SectionType ordinals). Every
 * value is reachable. SONARE_SECTION_PRE_CHORUS is a repeated group that always
 * leads into the Chorus and is shorter than it. SONARE_SECTION_UNKNOWN means
 * the segment matched no labelling rule, including a track with no structure at
 * all; it is always reported with confidence 0. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SECTION_INTRO = 0,
  SONARE_SECTION_VERSE = 1,
  SONARE_SECTION_PRE_CHORUS = 2,
  SONARE_SECTION_CHORUS = 3,
  SONARE_SECTION_BRIDGE = 4,
  SONARE_SECTION_INSTRUMENTAL = 5,
  SONARE_SECTION_OUTRO = 6,
  SONARE_SECTION_UNKNOWN = 7
} SonareSectionType;

/// @brief One labelled span of the song structure.
typedef struct {
  SonareSectionType type;  ///< Section label.
  float start_sec;         ///< seconds
  float end_sec;           ///< seconds
  float energy_level;      ///< median frame RMS in the section over the loudest section's, [0, 1]
  float confidence;        ///< [0, 1]; 0 exactly when type is SONARE_SECTION_UNKNOWN
  /**
   * material group: sections sharing a value repeat the same material,
   * numbered 0, 1, ... by first appearance
   */
  int32_t group;
} SonareSection;

/// @brief Song-structure sections in time order.
typedef struct {
  SonareSection* sections;  ///< free with sonare_free_section_result
  size_t section_count;     ///< Entries in sections.
} SonareSectionResult;

/** Zeroing this struct is NOT equivalent to the defaults: `n_fft` and `hop_length`
 * 0 are refused. Start from sonare_section_options_default(). */
typedef struct {
  /** Window length in samples at 22050 Hz, rescaled to the analysis rate. The analysis rate is
   * 22050 Hz for input above it and the input rate otherwise. Default 2048. */
  int n_fft;
  /** Hop between analysis frames in samples of the analysed signal: the input resampled to
   * 22050 Hz when its rate is above 22050 Hz, the input itself otherwise. Default 512. */
  int hop_length;
  /** Minimum section length in seconds; it also sets the analysis scale. Default 4. */
  float min_section_sec;
  /** Relative boundary threshold, as SonareBoundaryOptions::threshold. Default 0.22, below the
   * boundary layer's 0.3 because sections snap to downbeats and enforce a minimum length. */
  float threshold;
} SonareSectionOptions;

/** Structural boundary detection. This is the layer sonare_analyze_sections is
 * built on and is not a coarser view of it: sections are labelled spans, these
 * are the unlabelled transitions plus the continuous novelty curve they were
 * picked from, so a caller that wants the curve needs this and cannot derive it
 * from the section list. */
typedef struct {
  float time_sec;  ///< seconds; the authoritative output
  /** Index into the ANALYSIS grid, which is not the source's STFT grid: input
   * above 22.05 kHz is resampled before any feature is computed, and a long
   * input is additionally mean-pooled (see frame_stride). Use `time` for any
   * sample or second mapping. */
  int frame;
  float strength;  ///< novelty score
} SonareBoundary;

/// @brief Structural boundaries and the novelty curve they were picked from.
typedef struct {
  SonareBoundary* boundaries;  ///< free with sonare_free_boundary_result
  size_t boundary_count;       ///< Entries in boundaries.
  /** The novelty curve scaled by its own maximum, so values are in [0, 1] and a
   * peak of 1.0 means "the most novel frame here" rather than "a large change".
   * Multiply by novelty_peak to recover the raw response `absolute_threshold`
   * is compared against. Freed by the same call as `boundaries`. */
  float* novelty_curve;
  size_t novelty_length;  ///< Entries in novelty_curve.
  float novelty_peak;     ///< 0 when nothing rose above the numerical floor
  int sample_rate;        ///< the rate the ANALYSIS ran at, not the input's
  int hop_length;         ///< Hop between analysis frames in samples at the analysis rate.
  int n_frames;           ///< analysis frames the similarity band was built on
  int frame_stride;       ///< raw frames averaged per analysis frame; 1 unless pooled
} SonareBoundaryResult;

/** Zeroing this struct is NOT equivalent to the defaults and changes what is
 * detected: `absolute_threshold` 0 disables the floor that asks whether the
 * features changed at all, and without it a stationary input segments anyway,
 * because the relative threshold is applied to a curve scaled by its local
 * maximum. Start from sonare_boundary_options_default(). */
typedef struct {
  int n_fft;        ///< Window length in samples at 22050 Hz, rescaled to the analysis rate.
  int hop_length;   ///< Hop between analysis frames in samples.
  int kernel_size;  ///< checkerboard kernel, in frames
  float threshold;  ///< relative to the local novelty maximum, lone dominant event set aside
  float absolute_threshold;  ///< applied to the raw response, before scaling
  int n_mfcc;                ///< MFCC coefficients per frame when use_mfcc is set.
  int n_chroma;              ///< Chroma bins per frame when use_chroma is set.
  float peak_distance_sec;   ///< minimum spacing between peaks, in seconds
  int use_mfcc;              ///< Non-zero includes MFCC features in the similarity measure.
  int use_chroma;            ///< Non-zero includes chroma features in the similarity measure.
  /** One-sided span in seconds of the neighbourhood `threshold` is measured
   * against. Default 60. 0 makes the reference the frame itself, so `threshold`
   * no longer applies; a value at least the track length makes the
   * reference whole-track, still with a lone dominant event (one no other event
   * in the window comes within 30% of) set aside. */
  float reference_window_sec;
} SonareBoundaryOptions;

/// @brief One melody contour point.
typedef struct {
  float time_sec;      ///< seconds
  float frequency_hz;  ///< Hz (0 if unvoiced)
  float confidence;    ///< [0, 1]
} SonareMelodyPoint;

/// @brief Melody contour and its summary; free with sonare_free_melody_result.
typedef struct {
  SonareMelodyPoint* points;  ///< free with sonare_free_melody_result
  size_t point_count;         ///< Entries in points.
  float pitch_range_octaves;  ///< Range of the melody, in octaves.
  float pitch_stability;      ///< Pitch stability, [0, 1]; 1 is very stable.
  float mean_frequency_hz;    ///< Mean pitch frequency in Hz.
  float vibrato_rate_hz;      ///< Vibrato rate in Hz; 0 with no vibrato.
} SonareMelodyResult;

/** Layout version of SonareChordDetectionOptions; the caller sets it. 0 reads as
   the current version, so a zero-filled struct with the fields set is valid. */
#define SONARE_CHORD_DETECTION_OPTIONS_VERSION 2

/// @brief Options for chord detection.
typedef struct {
  /** SONARE_CHORD_DETECTION_OPTIONS_VERSION, or 0 for the same. */
  int struct_version;
  float min_duration_sec;      ///< Minimum chord duration in seconds.
  float smoothing_window_sec;  ///< Smoothing window in seconds.
  float threshold;      ///< final-template correlation threshold [0, 1]; below => UNKNOWN / N.C.
  int use_triads_only;  ///< Non-zero restricts the vocabulary to triads.
  int n_fft;            ///< STFT chroma window in samples at 22050 Hz, rescaled to the input rate
  int hop_length;       ///< Hop between frames in samples.
  int use_beat_sync;    ///< Non-zero decides chords per beat.
  int use_hmm;          ///< Non-zero smooths candidates with a Viterbi HMM.
  int hmm_beam_width;   ///< Candidate beam width for the HMM smoothing.
  int use_key_context;  ///< Non-zero biases HMM transitions toward key_root and key_mode.
  SonarePitchClass key_root;  ///< Tonic of the key context.
  SonareMode key_mode;        ///< Mode of the key context.
  int detect_inversions;      ///< Non-zero estimates the bass pitch class and reports slash chords.
  SonareChromaMethod chroma_method;  ///< STFT, NNLS or CQT (the library default)
  /** Tuning offset of the recording in cents; 0 is concert A440. Must be in
     [-50, 50). sonare_reference_hz_to_tuning converts a reference pitch to
     this unit. */
  float tuning_cents;
  /** Non-zero measures the tuning from the audio instead of reading
     @c tuning_cents; a zeroed struct reads @c tuning_cents. The value used,
     given or measured, is SonareChordAnalysisResult.tuning_cents. */
  int tuning_auto;
} SonareChordDetectionOptions;

/* ============================================================================
 * NativeSynth patch (versioned) — the patch-driven synthesizer surface
 * ========================================================================= */

/** Synthesis engine selector for SonareSynthPatch.engine_mode. 0 keeps the
   base patch's engine (the named preset's, or subtractive for an empty
   preset); explicit values select a mode. The field is int-wide so future
   modes extend the enum without a layout change. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_ENGINE_DEFAULT = 0,
  SONARE_SYNTH_ENGINE_SUBTRACTIVE = 1,
  SONARE_SYNTH_ENGINE_FM = 2,
  SONARE_SYNTH_ENGINE_KARPLUS_STRONG = 3,
  SONARE_SYNTH_ENGINE_MODAL = 4,
  SONARE_SYNTH_ENGINE_ADDITIVE = 5,
  SONARE_SYNTH_ENGINE_PERCUSSION = 6,
  SONARE_SYNTH_ENGINE_PIANO = 7,
  SONARE_SYNTH_ENGINE_PIPE_ORGAN = 8,
  SONARE_SYNTH_ENGINE_BOWED_STRING = 9,
  SONARE_SYNTH_ENGINE_REED = 10,
  SONARE_SYNTH_ENGINE_BRASS = 11,
  SONARE_SYNTH_ENGINE_FLUTE = 12,
  SONARE_SYNTH_ENGINE_PLUCKED_STRING = 13,
  SONARE_SYNTH_ENGINE_VOCAL = 14,
  SONARE_SYNTH_ENGINE_FREE_REED = 15,
  SONARE_SYNTH_ENGINE_HARPSICHORD = 16,
  SONARE_SYNTH_ENGINE_SAMPLE = 17,
  SONARE_SYNTH_ENGINE_SINGING = 18
} SonareSynthEngineMode;
#define SONARE_SYNTH_ENGINE_MODE_COUNT 19  ///< Number of SonareSynthEngineMode enumerators.

/** Loop behaviour override for the sample engine. 0 keeps what the bank
   recorded for the sample the zone names. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SAMPLE_LOOP_DEFAULT = 0,
  SONARE_SAMPLE_LOOP_NONE = 1,
  SONARE_SAMPLE_LOOP_CONTINUOUS = 2,
  SONARE_SAMPLE_LOOP_KEY_DOWN = 3
} SonareSampleLoopMode;
#define SONARE_SAMPLE_LOOP_MODE_COUNT 4  ///< Number of SonareSampleLoopMode enumerators.

/** Whether a sample follows the played key. 0 keeps the base patch's value. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SAMPLE_KEY_TRACK_DEFAULT = 0,
  SONARE_SAMPLE_KEY_TRACK_ON = 1,
  SONARE_SAMPLE_KEY_TRACK_OFF = 2
} SonareSampleKeyTrack;
#define SONARE_SAMPLE_KEY_TRACK_COUNT 3  ///< Number of SonareSampleKeyTrack enumerators.

/** Oscillator waveform (subtractive mode). 0 keeps the base patch's value. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_OSC_DEFAULT = 0,
  SONARE_SYNTH_OSC_SINE = 1,
  SONARE_SYNTH_OSC_SAW = 2,
  SONARE_SYNTH_OSC_SQUARE = 3,
  SONARE_SYNTH_OSC_TRIANGLE = 4,
  SONARE_SYNTH_OSC_NOISE = 5
} SonareSynthOscWaveform;
#define SONARE_SYNTH_OSC_WAVEFORM_COUNT 6  ///< Number of SonareSynthOscWaveform enumerators.

/** Filter model (the character core). 0 keeps the base patch's value. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_FILTER_DEFAULT = 0,
  SONARE_SYNTH_FILTER_SVF = 1,
  SONARE_SYNTH_FILTER_TRANSISTOR_LADDER = 2,
  SONARE_SYNTH_FILTER_DIODE_LADDER = 3,
  SONARE_SYNTH_FILTER_SALLEN_KEY = 4,
  SONARE_SYNTH_FILTER_SVF_4POLE = 5
} SonareSynthFilterModel;
#define SONARE_SYNTH_FILTER_MODEL_COUNT 6  ///< Number of SonareSynthFilterModel enumerators.

/** Which filter output the voice mixes (SVF only; the ladder, Sallen-Key and
   4-pole SVF models are lowpass-only). 0 keeps the base patch's value. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_FILTER_OUT_DEFAULT = 0,
  SONARE_SYNTH_FILTER_OUT_LOWPASS = 1,
  SONARE_SYNTH_FILTER_OUT_BANDPASS = 2,
  SONARE_SYNTH_FILTER_OUT_HIGHPASS = 3
} SonareSynthFilterOutput;
#define SONARE_SYNTH_FILTER_OUTPUT_COUNT 4  ///< Number of SonareSynthFilterOutput enumerators.

/** Body/formant resonance voicing. 0 keeps the base patch's value;
   SONARE_SYNTH_BODY_NONE explicitly disables a preset's body. */
typedef enum SONARE_ENUM_BASE {
  SONARE_SYNTH_BODY_DEFAULT = 0,
  SONARE_SYNTH_BODY_NONE = 1,
  SONARE_SYNTH_BODY_GUITAR = 2,
  SONARE_SYNTH_BODY_VIOLIN = 3,
  SONARE_SYNTH_BODY_WOOD_TUBE = 4,
  SONARE_SYNTH_BODY_BRASS_BELL = 5,
  SONARE_SYNTH_BODY_VOCAL = 6
} SonareSynthBodyType;
#define SONARE_SYNTH_BODY_TYPE_COUNT 7  ///< Number of SonareSynthBodyType enumerators.

/** What a note-on restarts. 0 keeps the base patch's choice. */
typedef enum {
  SONARE_SYNTH_RETRIGGER_BASE = 0,
  SONARE_SYNTH_RETRIGGER_FREE = 1,  ///< start phases and per-voice random streams vary per note
  SONARE_SYNTH_RETRIGGER_NOTE = 2   ///< both are derived from the note number alone
} SonareSynthRetrigger;
#define SONARE_SYNTH_RETRIGGER_COUNT 3  ///< Number of SonareSynthRetrigger enumerators.

/** The patch's LFO waveform (vibrato and tremolo LFOs), at every destination either
   reaches. 0 keeps the base patch's choice. */
typedef enum {
  SONARE_SYNTH_LFO_SHAPE_BASE = 0,
  SONARE_SYNTH_LFO_SHAPE_TRIANGLE = 1,  ///< every catalog preset's shape
  SONARE_SYNTH_LFO_SHAPE_SINE = 2,   ///< no harmonics: sidebands sit where a player's or motor's do
  SONARE_SYNTH_LFO_SHAPE_RANDOM = 3  ///< a seeded target per cycle, glided to: no line at the rate
} SonareSynthLfoShape;
#define SONARE_SYNTH_LFO_SHAPE_COUNT 4  ///< Number of SonareSynthLfoShape enumerators.

#define SONARE_SYNTH_MOD_SOURCE_COUNT 13  ///< Number of mod-matrix source ordinals, none included.
/// Number of mod-matrix destination ordinals, none included.
#define SONARE_SYNTH_MOD_DESTINATION_COUNT 13

/** One mod-matrix routing. Source/destination mirror the core ordinals
   directly; a slot with source or destination 0 (none) is refused: a routing
   that reaches nothing is a caller mistake, not an empty slot. */
typedef struct {
  /**
   * 0=none 1=ampEnv 2=filterEnv 3=lfo1 4=lfo2 5=velocity
   * 6=keyTrack 7=modWheel 8=random 9=breath 10=aftertouch
   * 11=expressionCc 12=pitchBend. 9-12 are the live
   * controllers: CC2, channel plus polyphonic aftertouch
   * summed and saturated, CC11, and the bend wheel's
   * position. Only 12 is bipolar; a route's depth carries the
   * sign, so none of them is spelled for a polarity.
   * 11 is already folded into the channel gain, so routing it
   * to ampGain applies it twice.
   */
  int source;
  /**
   * 0=none 1=pitchCents 2=cutoffCents 3=ampGain 4=panUnits
   * 5=resonanceQ 6=vibratoDepthCents 7=filterEnvDepth
   * 8=lfo1RateScale 9=excitationForce 10=excitationPosition
   * 11=excitationBrightness 12=spectrumMorph. depth's unit and
   * clamp are per destination: panUnits is SF2 pan units
   * clamped to [-500,500] (1 unit moves the image about 0.2%
   * of a side); ampGain and filterEnvDepth are multiplicative,
   * each routing's amount composing onto a running multiplier
   * clamped to [0,4] -- filterEnvDepth has nothing to scale
   * while the patch's env_to_cutoff_cents is 0; resonanceQ
   * adds onto the patch's own Q and then floors at a fixed
   * 0.5, not at that Q; lfo1RateScale is multiplicative,
   * clamped to [1/16,16], and retunes LFO1 rather than
   * sounding on its own, so it is audible only once something
   * else already reads LFO1's output (vibrato depth, the
   * filter's LFO amount, tremolo). 9-12 reach inside the
   * engine at an offset in its own normalized [0,1] axis
   * units (the same scale the live-control CCs drive), summed
   * and clamped to [-1,1]. 9-11 are the physical model's
   * exciter — its drive (bow force, breath pressure), its
   * spatial excitation coordinate (a bow's contact point)
   * and its brightness — and reach differs per axis: 9 (force)
   * reaches pipe organ, bowed string, reed, brass, flute, free
   * reed; 10 (position) reaches bowed string only; 11
   * (brightness) reaches pipe organ, reed, brass, flute, free
   * reed, vocal. An engine outside an axis's list ignores it
   * — its exciter is finished before the second sample
   * renders, or it has none. 12 travels the engine's timbral
   * continuum — between the two spectral tables an additive
   * patch carries; a patch needs
   * an explicit drawbars_b distinct from drawbars to have a
   * second table to travel to, and no catalog preset ships
   * one, so 12 is currently unreachable from a preset name
   * alone. Percussion reaches none of the above: a drum
   * channel's voice runs the per-note kit patch instead of
   * this one, so the whole mod matrix is discarded before the
   * voice starts.
   */
  int destination;
  /**
   * destination units at full source deflection; see
   * `destination` above for the per-destination unit and
   * clamp. Non-finite or beyond +-9600 is refused. An
   * ordinal outside either list above is refused too.
   */
  float depth;
} SonareSynthModRouting;

#define SONARE_SYNTH_PATCH_MOD_ROUTINGS 8  ///< Capacity of SonareSynthPatch.mod_routings.
#define SONARE_SYNTH_PRESET_NAME_MAX 32    ///< Size in bytes of SonareSynthPatch.preset.

/** One field of the selected engine's own model section, set by public key
   (see @ref sonare_synth_engine_param_info for the keys an engine accepts). */
typedef struct {
  const char* key;  ///< public key, e.g. "bowForce" or "ranks2.level"
  double value;     ///< integer and switch fields take a whole number
} SonareSynthEngineParam;

/** Versioned NativeSynth patch for
   @ref sonare_project_bounce_with_synth_instruments and
   @ref sonare_engine_set_synth_instrument.

   Zero-initialize then override. The patch starts from a BASE: the named
   preset in @c preset (see @ref sonare_synth_preset_names) or, when @c preset
   is empty, the default subtractive patch. Its engine section (the selected
   engine's own model: FM operator stacks, modal mode tables, drawbar
   registrations, kit pieces, piano strings, bow / reed / lip / jet fields) is
   the named preset's when that preset uses the selected engine, and otherwise
   the engine's base preset's, so every engine but sample sounds from
   @c engine_mode alone. Base presets alias the GM bank, so such a render
   follows the bank's version. Every numeric field then uses
   "0 => keep the base value"; non-zero values override. A value that is non-finite
   or outside the range @ref sonare_synth_patch_param_info publishes for its
   field, and a @c num_mod_routings outside [0, 8], is refused with
   SONARE_ERROR_INVALID_PARAMETER rather than clamped. Enum fields reserve 0 as "keep" (see the
   enums above). Because struct_version 1 has no per-field presence bits, explicit zero values for
   numeric fields (for example @c amp_sustain = 0) cannot be represented through this C ABI; pass a
   non-zero value or choose a base preset that already contains the desired zero. A non-empty @c
   num_mod_routings REPLACES the base mod matrix.

   The struct fields are the wrapper sections; a field of the engine section
   is set by key through @c engine_params (struct_version 8). Which engines
   honour each wrapper field:
   - subtractive only: waveform, unison, detune_cents.
   - sample only: the sample block (sample_set through sample_key_track).
   - every engine: pitch_offset_cents, the LFOs, drift_cents / drift_rate_hz,
     glide_ms, the filter and its envelope, the amplitude envelope (an
     undamped harpsichord tail outlasts it), one_shot, body / body_mix, drive,
     hp_cutoff_hz, sample_hold_hz / bit_depth, stereo_spread.
   - every engine and a percussion channel: gain, polyphony, bus_drive. On a
     percussion channel (a percussion patch voicing a GM kit, as the drum-kit
     preset does) each note runs its per-note kit patch and every other field
     is discarded.
   The Node / WASM SynthPatch TSDoc and the Python SynthPatch docstring carry
   this table by hand; nothing checks the copies against each other. */
typedef struct {
  /**
   * 0 or 1 => version 1; 2 => present_fields honoured;
   * 3 => the sample-engine block at the tail is read too;
   * 4 => the series highpass at the tail is read too;
   * 5 => the converter block at the tail is read too;
   * 6 => the pitch offset at the tail is read too;
   * 7 => the retrigger mode at the tail is read too;
   * 8 => the engine params at the tail are read too;
   * 9 => present_fields2 and its fields are read too;
   * 10 => the LFO shape at the tail is read too
   */
  int struct_version;
  char preset[SONARE_SYNTH_PRESET_NAME_MAX];  ///< base preset name; "" = init patch
  int engine_mode;                            ///< SonareSynthEngineMode; 0 => base

  /* --- oscillator section (subtractive mode) --- */
  /**
   * SonareSynthOscWaveform; 0 => base preset. Read by the
   * subtractive engine only; every other engine ignores it.
   * NOTE: a distinct enum from SonareSynthWaveform
   * (sonare_c_project_instruments.h), whose 0 means sine,
   * not "keep base". Do not mix the two.
   */
  int waveform;
  int unison;          ///< detuned-stack width [1,7]; 0 => base
  float detune_cents;  ///< unison spread; 0 => base
  float drift_cents;   ///< per-voice slow pitch drift depth; 0 => base
  float drive;         ///< pre-filter drive [0,1]; 0 => base

  /* --- filter section --- */
  int filter_model;   ///< SonareSynthFilterModel; 0 => base
  int filter_output;  ///< SonareSynthFilterOutput; 0 => base
  float cutoff_hz;    ///< 0 => base
  float resonance_q;  ///< 0 => base
  float key_track;    ///< cutoff keyboard tracking [0,1]; 0 => base
  /**
   * filter envelope's cutoff depth, in cents at
   * full envelope; 0 => base, and leaves the
   * envelope's cutoff contribution off. The cutoff
   * is scaled by 2^(env * cents / 1200), exact
   * wherever the corner is well below Nyquist.
   * cutoff_hz is the -3 dB corner only for the SVF
   * at resonance_q 0.707; the ladders and
   * Sallen-Key put their resonant peak on it, with
   * the -3 dB point one to three octaves lower
   */
  float env_to_cutoff_cents;
  /**
   * velocity's cutoff depth, in cents; 0 => base.
   * Rendered term is
   * vel_to_cutoff_cents * (velocity/127 - 1), so
   * velocity 127 is the anchor where it is exactly
   * zero: positive darkens softer notes, negative
   * brightens them, both growing with distance
   * below 127
   */
  float vel_to_cutoff_cents;

  /* --- envelopes (ms / sustain in [0,1]) --- */
  /** Attack reaches full level at the set time; decay and release are three time
     constants, about -26 dB of the remaining distance at the set time. */
  float amp_attack_ms;
  float amp_decay_ms;       ///< Amplitude envelope decay time in ms; 0 => base.
  float amp_sustain;        ///< 0 => base, or an explicit zero with its presence bit
  float amp_release_ms;     ///< Amplitude envelope release time in ms; 0 => base.
  float filter_attack_ms;   ///< Filter envelope attack time in ms; 0 => base.
  float filter_decay_ms;    ///< Filter envelope decay time in ms; 0 => base.
  float filter_sustain;     ///< 0 => base, or an explicit zero with its presence bit
  float filter_release_ms;  ///< Filter envelope release time in ms; 0 => base.

  /* --- LFOs / glide --- */
  float lfo_rate_hz;         ///< vibrato LFO rate; 0 => base
  float lfo_to_pitch_cents;  ///< hardwired vibrato depth; 0 => base
  float lfo2_rate_hz;        ///< matrix-routed LFO2 rate; 0 => base
  float glide_ms;            ///< portamento; 0 => base

  /* --- realism polish --- */
  int body;             ///< SonareSynthBodyType; 0 => base
  float body_mix;       ///< body resonance mix [0,1]; 0 => base
  float stereo_spread;  ///< seeded per-voice pan scatter [0,1]; 0 => base

  /* --- mod matrix (REPLACES the base matrix when num_mod_routings > 0) --- */
  int num_mod_routings;  ///< Used entries in mod_routings, [0, SONARE_SYNTH_PATCH_MOD_ROUTINGS].
  /// Mod-matrix routings; the first num_mod_routings are read.
  SonareSynthModRouting mod_routings[SONARE_SYNTH_PATCH_MOD_ROUTINGS];

  /* --- voice pool / bus --- */
  float gain;       ///< master output gain (linear); 0 => base
  int polyphony;    ///< max voices [1,64]; 0 => base
  float bus_drive;  ///< gain-neutral bus saturation [0,1]; 0 => base

  /* --- explicit-value presence (struct_version 2) --- */
  /** Bitmask of SONARE_SYNTH_FIELD_* naming the fields the caller set on
     purpose. A set bit overrides the base with the field's value even when that
     value is zero, which the "0 => base" rule above cannot express; a clear bit
     keeps the version-1 behaviour, so a caller that only fills the fields it
     wants to change needs no mask at all. Honoured from struct_version 2 on.
     32 bits with 31 in use; a further extension appends a second word under a
     new struct_version rather than widening this one. */
  uint32_t present_fields;

  /* --- sample engine (struct_version 3) --- */
  /** Read only when the resolved engine is SONARE_SYNTH_ENGINE_SAMPLE, which is
     what lets sample_set keep its natural zero: a patch voicing another engine
     never consults this block, so set 0 stays addressable. The bank itself is
     bound alongside the patch (see SonareSynthInstrumentBinding). */
  int sample_set;             ///< keymap set in the bound bank; negative = none
  float sample_level;         ///< linear gain on the sample; 0 => base
  int sample_loop;            ///< SonareSampleLoopMode; 0 => what the bank recorded
  float sample_start_offset;  ///< attack skip as a fraction of the region; 0 => base
  int sample_key_track;       ///< A @ref SonareSampleKeyTrack; 0 => base

  /* --- series highpass (struct_version 4) --- */
  /** 12 dB/oct highpass after the main filter, the other end of a band the
     lowpass alone cannot make. 0 => base; set SONARE_SYNTH_FIELD_HP_CUTOFF_HZ
     in present_fields to ask for 0 as a value and switch the stage off. It
     runs at Butterworth Q: resonance_q belongs to the main filter. */
  float hp_cutoff_hz;

  /* --- converter (struct_version 5) --- */
  /** The voice's own output stage, ahead of its amplitude envelope: a sample and
     hold followed by a uniform quantizer. Per voice rather than per bus, so a
     patch layering several voices can convert some and leave others alone.
     Percussion never reads this block: a drum channel's voice runs the
     per-note kit patch instead of this one, so it is discarded before the
     voice starts. Each is 0 => base; set the matching present_fields bit to
     ask for 0 as a value and switch that half off. */
  float sample_hold_hz;  ///< rate the output is held at; below 100 Hz is raised to it
  float bit_depth;       ///< word length the held value is quantized to, [1,24]; fractional is fine

  /* --- pitch offset (struct_version 6) --- */
  /** Constant transposition of the voice's own pitch, in cents, [-4800, 4800].
     Carried in the per-sample pitch factor every engine's render already takes,
     so it applies the same amount on all of them — except percussion, whose
     per-note kit patch replaces this whole section and never reads it — and a
     lane moving it reaches a voice that is already sounding.
     0 => base; set SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS in present_fields to
     ask for 0 as a value. Automatable under the same name, which is why it is
     here: every other name @ref sonare_engine_resolve_instrument_automation_id
     accepts is a field of this struct. */
  float pitch_offset_cents;

  /* --- note-on retrigger (struct_version 7) --- */
  /** SonareSynthRetrigger; 0 => base, and every catalog preset is free. Free
     seeds each voice's oscillator start phases, unison jitter, drift and engine
     noise from its pool slot and a running note count, so a repeated note
     generally differs. Note seeds them from the note number alone, so a note
     played again after its tail has ended renders the same samples. State
     outside the voice is reset by neither: controllers, the bus DC blocker, a
     piano's shared soundboard, a plucked string's sympathetic halo, an organ's
     wind chest, and effect tails. A GM kit's per-note pieces keep their own. */
  int retrigger;

  /* --- engine params (struct_version 8) --- */
  /** Fields of the selected engine's section, applied after the base and the
     struct fields above; @ref sonare_synth_engine_param_info lists each
     engine's keys, ranges and units. Borrowed for the call: the array and its
     key strings are read before the call returns and never kept.
     engine_param_count 0 => none (engine_params may then be NULL). Unlike a
     SonareMasteringParam list, nothing here is ignored or clamped: an unknown
     key, a key outside the selected engine's section, a key given twice, a
     non-finite value, a fractional value for an integer or switch field, or a
     value outside the field's range fails the call with
     SONARE_ERROR_INVALID_PARAMETER and a last-error message naming the key and
     the range. */
  const SonareSynthEngineParam* engine_params;
  size_t engine_param_count;  ///< Entries in engine_params.

  /* --- second presence word (struct_version 9) --- */
  /** Bitmask of SONARE_SYNTH_FIELD2_* for the fields below, meaning what
     present_fields means above: a set bit makes a zero an explicit value. Each
     field below is 0 => base otherwise. */
  uint32_t present_fields2;
  float drift_rate_hz;    ///< per-voice drift rate, [0.01, 20] Hz
  float amp_delay_ms;     ///< amplitude envelope delay before the attack, [0, 5000] ms
  float amp_hold_ms;      ///< amplitude envelope hold at the peak, [0, 5000] ms
  float filter_delay_ms;  ///< filter envelope delay before the attack, [0, 5000] ms
  float filter_hold_ms;   ///< filter envelope hold at the peak, [0, 5000] ms
  /**
   * non-zero: note-off is ignored and the voice ends with its
   * envelope's decay or its engine's own end
   */
  int one_shot;

  /* --- LFO shape (struct_version 10) --- */
  /** SonareSynthLfoShape; 0 => base. Shapes both the vibrato and the tremolo
     LFO wherever they are read: the vibrato depth, the GS LFO cutoff and
     amplitude depths and mod sources lfo1 and lfo2. */
  int lfo_shape;
} SonareSynthPatch;

/** Newest SonareSynthPatch layout. Named rather than written out at each site,
   because every one of them — the reader's upper bound, the writer's stamp, and
   the tests that pin "one past the newest is refused" — has to move together,
   and a literal in any of them goes stale silently. */
#define SONARE_SYNTH_PATCH_STRUCT_VERSION 10

/** Bit positions for SonareSynthPatch.present_fields. The enum fields are absent
   on purpose: their zero is already the reserved "keep base" value and every
   real value is non-zero, so they have nothing to disambiguate. */
#define SONARE_SYNTH_FIELD_UNISON (1u << 0)               ///< Marks unison as set.
#define SONARE_SYNTH_FIELD_DETUNE_CENTS (1u << 1)         ///< Marks detune_cents as set.
#define SONARE_SYNTH_FIELD_DRIFT_CENTS (1u << 2)          ///< Marks drift_cents as set.
#define SONARE_SYNTH_FIELD_DRIVE (1u << 3)                ///< Marks drive as set.
#define SONARE_SYNTH_FIELD_CUTOFF_HZ (1u << 4)            ///< Marks cutoff_hz as set.
#define SONARE_SYNTH_FIELD_RESONANCE_Q (1u << 5)          ///< Marks resonance_q as set.
#define SONARE_SYNTH_FIELD_KEY_TRACK (1u << 6)            ///< Marks key_track as set.
#define SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS (1u << 7)  ///< Marks env_to_cutoff_cents as set.
#define SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS (1u << 8)  ///< Marks vel_to_cutoff_cents as set.
#define SONARE_SYNTH_FIELD_AMP_ATTACK_MS (1u << 9)        ///< Marks amp_attack_ms as set.
#define SONARE_SYNTH_FIELD_AMP_DECAY_MS (1u << 10)        ///< Marks amp_decay_ms as set.
#define SONARE_SYNTH_FIELD_AMP_SUSTAIN (1u << 11)         ///< Marks amp_sustain as set.
#define SONARE_SYNTH_FIELD_AMP_RELEASE_MS (1u << 12)      ///< Marks amp_release_ms as set.
#define SONARE_SYNTH_FIELD_FILTER_ATTACK_MS (1u << 13)    ///< Marks filter_attack_ms as set.
#define SONARE_SYNTH_FIELD_FILTER_DECAY_MS (1u << 14)     ///< Marks filter_decay_ms as set.
#define SONARE_SYNTH_FIELD_FILTER_SUSTAIN (1u << 15)      ///< Marks filter_sustain as set.
#define SONARE_SYNTH_FIELD_FILTER_RELEASE_MS (1u << 16)   ///< Marks filter_release_ms as set.
#define SONARE_SYNTH_FIELD_LFO_RATE_HZ (1u << 17)         ///< Marks lfo_rate_hz as set.
#define SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS (1u << 18)  ///< Marks lfo_to_pitch_cents as set.
#define SONARE_SYNTH_FIELD_LFO2_RATE_HZ (1u << 19)        ///< Marks lfo2_rate_hz as set.
#define SONARE_SYNTH_FIELD_GLIDE_MS (1u << 20)            ///< Marks glide_ms as set.
#define SONARE_SYNTH_FIELD_BODY_MIX (1u << 21)            ///< Marks body_mix as set.
#define SONARE_SYNTH_FIELD_STEREO_SPREAD (1u << 22)       ///< Marks stereo_spread as set.
#define SONARE_SYNTH_FIELD_GAIN (1u << 23)                ///< Marks gain as set.
#define SONARE_SYNTH_FIELD_POLYPHONY (1u << 24)           ///< Marks polyphony as set.
#define SONARE_SYNTH_FIELD_BUS_DRIVE (1u << 25)           ///< Marks bus_drive as set.
/** Set with num_mod_routings == 0 to clear the base mod matrix rather than keep
   it; a non-empty table replaces the base matrix with or without the bit. */
#define SONARE_SYNTH_FIELD_MOD_ROUTINGS (1u << 26)        ///< Marks mod_routings as set.
#define SONARE_SYNTH_FIELD_HP_CUTOFF_HZ (1u << 27)        ///< Marks hp_cutoff_hz as set.
#define SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ (1u << 28)      ///< Marks sample_hold_hz as set.
#define SONARE_SYNTH_FIELD_BIT_DEPTH (1u << 29)           ///< Marks bit_depth as set.
#define SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS (1u << 30)  ///< Marks pitch_offset_cents as set.

/** Bit positions for SonareSynthPatch.present_fields2 (struct_version 9). */
#define SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ (1u << 0)    ///< Marks drift_rate_hz as set.
#define SONARE_SYNTH_FIELD2_AMP_DELAY_MS (1u << 1)     ///< Marks amp_delay_ms as set.
#define SONARE_SYNTH_FIELD2_AMP_HOLD_MS (1u << 2)      ///< Marks amp_hold_ms as set.
#define SONARE_SYNTH_FIELD2_FILTER_DELAY_MS (1u << 3)  ///< Marks filter_delay_ms as set.
#define SONARE_SYNTH_FIELD2_FILTER_HOLD_MS (1u << 4)   ///< Marks filter_hold_ms as set.
#define SONARE_SYNTH_FIELD2_ONE_SHOT (1u << 5)         ///< Marks one_shot as set.

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
static_assert(offsetof(SonareAnalysisResult, bpm_candidates) ==
                  offsetof(SonareAnalysisResult, beat_count) + sizeof(size_t),
              "SonareAnalysisResult candidate layout changed");

static_assert(SONARE_SYNTH_ENGINE_SINGING + 1 == SONARE_SYNTH_ENGINE_MODE_COUNT,
              "SonareSynthEngineMode count changed");
static_assert(SONARE_SAMPLE_LOOP_KEY_DOWN + 1 == SONARE_SAMPLE_LOOP_MODE_COUNT,
              "SonareSampleLoopMode count changed");
static_assert(SONARE_SAMPLE_KEY_TRACK_OFF + 1 == SONARE_SAMPLE_KEY_TRACK_COUNT,
              "SonareSampleKeyTrack count changed");
static_assert(SONARE_SYNTH_OSC_NOISE + 1 == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
              "SonareSynthOscWaveform count changed");
static_assert(SONARE_SYNTH_FILTER_SVF_4POLE + 1 == SONARE_SYNTH_FILTER_MODEL_COUNT,
              "SonareSynthFilterModel count changed");
static_assert(SONARE_SYNTH_FILTER_OUT_HIGHPASS + 1 == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
              "SonareSynthFilterOutput count changed");
static_assert(SONARE_SYNTH_BODY_VOCAL + 1 == SONARE_SYNTH_BODY_TYPE_COUNT,
              "SonareSynthBodyType count changed");
static_assert(SONARE_SYNTH_RETRIGGER_NOTE + 1 == SONARE_SYNTH_RETRIGGER_COUNT,
              "SonareSynthRetrigger count changed");
static_assert(SONARE_SYNTH_LFO_SHAPE_RANDOM + 1 == SONARE_SYNTH_LFO_SHAPE_COUNT,
              "SonareSynthLfoShape count changed");

static_assert(sizeof(SonareSynthModRouting) == 2u * sizeof(int) + sizeof(float),
              "SonareSynthModRouting layout changed");
static_assert(offsetof(SonareSynthPatch, engine_mode) == sizeof(int) + SONARE_SYNTH_PRESET_NAME_MAX,
              "SonareSynthPatch engine_mode offset changed");
static_assert(offsetof(SonareSynthPatch, gain) ==
                  offsetof(SonareSynthPatch, mod_routings) +
                      SONARE_SYNTH_PATCH_MOD_ROUTINGS * sizeof(SonareSynthModRouting),
              "SonareSynthPatch gain offset changed");
static_assert(offsetof(SonareSynthPatch, present_fields) ==
                  offsetof(SonareSynthPatch, bus_drive) + sizeof(float),
              "SonareSynthPatch present_fields offset changed");
static_assert(SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS ==
                  1u << 30,  // Highest bit in use; widening needs a struct_version bump.
              "SonareSynthPatch presence bit range changed");
static_assert(SONARE_SYNTH_FIELD2_ONE_SHOT == 1u << 5,  // Highest bit in use in the second word.
              "SonareSynthPatch second presence bit range changed");
static_assert(offsetof(SonareSynthPatch, present_fields2) ==
                  offsetof(SonareSynthPatch, engine_param_count) + sizeof(size_t),
              "SonareSynthPatch present_fields2 offset changed");
#endif

#ifdef __cplusplus
}
#endif
