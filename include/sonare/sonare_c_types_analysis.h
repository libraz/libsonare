#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

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
  float momentary_lufs;      /* final complete 400 ms window, not Max-M */
  float short_term_lufs;     /* final complete 3 s window, not Max-S */
  float max_momentary_lufs;  /* maximum 400 ms window (EBU R128 Max-M) */
  float max_short_term_lufs; /* maximum 3 s window (EBU R128 Max-S) */
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
  float threshold; /* final-template correlation threshold [0, 1]; below => UNKNOWN / N.C. */
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

/* ============================================================================
 * NativeSynth patch (versioned) — the patch-driven synthesizer surface
 * ========================================================================= */

/* Synthesis engine selector for SonareSynthPatch.engine_mode. 0 keeps the
   base patch's engine (the named preset's, or subtractive for an empty
   preset); explicit values select a mode. The field is int-wide so future
   modes extend the enum without a layout change. */
typedef enum {
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
  SONARE_SYNTH_ENGINE_FREE_REED = 15
} SonareSynthEngineMode;
#define SONARE_SYNTH_ENGINE_MODE_COUNT 16

/* Oscillator waveform (subtractive mode). 0 keeps the base patch's value. */
typedef enum {
  SONARE_SYNTH_OSC_DEFAULT = 0,
  SONARE_SYNTH_OSC_SINE = 1,
  SONARE_SYNTH_OSC_SAW = 2,
  SONARE_SYNTH_OSC_SQUARE = 3,
  SONARE_SYNTH_OSC_TRIANGLE = 4,
  SONARE_SYNTH_OSC_NOISE = 5
} SonareSynthOscWaveform;
#define SONARE_SYNTH_OSC_WAVEFORM_COUNT 6

/* Filter model (the character core). 0 keeps the base patch's value. */
typedef enum {
  SONARE_SYNTH_FILTER_DEFAULT = 0,
  SONARE_SYNTH_FILTER_SVF = 1,
  SONARE_SYNTH_FILTER_MOOG_LADDER = 2,
  SONARE_SYNTH_FILTER_DIODE_LADDER = 3,
  SONARE_SYNTH_FILTER_SALLEN_KEY = 4
} SonareSynthFilterModel;
#define SONARE_SYNTH_FILTER_MODEL_COUNT 5

/* Which filter output the voice mixes (SVF only; the ladder and Sallen-Key
   models are lowpass-only). 0 keeps the base patch's value. */
typedef enum {
  SONARE_SYNTH_FILTER_OUT_DEFAULT = 0,
  SONARE_SYNTH_FILTER_OUT_LOWPASS = 1,
  SONARE_SYNTH_FILTER_OUT_BANDPASS = 2,
  SONARE_SYNTH_FILTER_OUT_HIGHPASS = 3
} SonareSynthFilterOutput;
#define SONARE_SYNTH_FILTER_OUTPUT_COUNT 4

/* Body/formant resonance voicing. 0 keeps the base patch's value;
   SONARE_SYNTH_BODY_NONE explicitly disables a preset's body. */
typedef enum {
  SONARE_SYNTH_BODY_DEFAULT = 0,
  SONARE_SYNTH_BODY_NONE = 1,
  SONARE_SYNTH_BODY_GUITAR = 2,
  SONARE_SYNTH_BODY_VIOLIN = 3,
  SONARE_SYNTH_BODY_WOOD_TUBE = 4,
  SONARE_SYNTH_BODY_BRASS_BELL = 5,
  SONARE_SYNTH_BODY_VOCAL = 6
} SonareSynthBodyType;
#define SONARE_SYNTH_BODY_TYPE_COUNT 7

#define SONARE_SYNTH_MOD_SOURCE_COUNT 9
#define SONARE_SYNTH_MOD_DESTINATION_COUNT 5

/* One mod-matrix routing. Source/destination mirror the core ordinals
   directly; a slot with source or destination 0 (none) is disabled. */
typedef struct {
  int source;      /* 0=none 1=ampEnv 2=filterEnv 3=lfo1 4=lfo2 5=velocity
                      6=keyTrack 7=modWheel 8=random */
  int destination; /* 0=none 1=pitchCents 2=cutoffCents 3=ampGain 4=panUnits */
  float depth;     /* destination units at full source deflection */
} SonareSynthModRouting;

#define SONARE_SYNTH_PATCH_MOD_ROUTINGS 8
#define SONARE_SYNTH_PRESET_NAME_MAX 32

/* Versioned NativeSynth patch for
   @ref sonare_project_bounce_with_synth_instruments and
   @ref sonare_engine_set_synth_instrument.

   Zero-initialize then override. The patch starts from a BASE: the named
   preset in @c preset (see @ref sonare_synth_preset_names) or, when @c preset
   is empty, the default subtractive patch. Every numeric field then uses
   "0 => keep the base value"; non-zero values override (and are clamped to
   their audible ranges). Enum fields reserve 0 as "keep" (see the enums
   above). Because struct_version 1 has no per-field presence bits, explicit
   zero values for numeric fields (for example @c amp_sustain = 0) cannot be
   represented through this C ABI; pass a non-zero value or choose a base preset
   that already contains the desired zero. A non-empty @c num_mod_routings
   REPLACES the base mod matrix.

   Mode-specific deep parameters (FM operator stacks, modal mode tables,
   drawbar registrations, kit pieces, piano strings) travel inside the named
   presets — struct_version 1 deliberately exposes the wrapper sections every
   engine shares (oscillator / filter / envelopes / LFO / glide / realism /
   mod matrix / bus). */
typedef struct {
  int struct_version;                        /* 0 or 1 => version 1; 2 => present_fields honoured */
  char preset[SONARE_SYNTH_PRESET_NAME_MAX]; /* base preset name; "" = init patch */
  int engine_mode;                           /* SonareSynthEngineMode; 0 => base */

  /* --- oscillator section (subtractive mode) --- */
  int waveform;       /* SonareSynthOscWaveform; 0 => base preset. NOTE: a distinct
                         enum from SonareSynthWaveform (sonare_c_project_instruments.h),
                         whose 0 means sine, not "keep base". Do not mix the two. */
  int unison;         /* detuned-stack width [1,7]; 0 => base */
  float detune_cents; /* unison spread; 0 => base */
  float drift_cents;  /* per-voice slow pitch drift depth; 0 => base */
  float drive;        /* pre-filter drive [0,1]; 0 => base */

  /* --- filter section --- */
  int filter_model;          /* SonareSynthFilterModel; 0 => base */
  int filter_output;         /* SonareSynthFilterOutput; 0 => base */
  float cutoff_hz;           /* 0 => base */
  float resonance_q;         /* 0 => base */
  float key_track;           /* cutoff keyboard tracking [0,1]; 0 => base */
  float env_to_cutoff_cents; /* filter-envelope depth; 0 => base */
  float vel_to_cutoff_cents; /* velocity->brightness depth; 0 => base */

  /* --- envelopes (ms / sustain in [0,1]) --- */
  float amp_attack_ms;
  float amp_decay_ms;
  float amp_sustain; /* 0 => base, or an explicit zero with its presence bit */
  float amp_release_ms;
  float filter_attack_ms;
  float filter_decay_ms;
  float filter_sustain; /* 0 => base, or an explicit zero with its presence bit */
  float filter_release_ms;

  /* --- LFOs / glide --- */
  float lfo_rate_hz;        /* vibrato LFO rate; 0 => base */
  float lfo_to_pitch_cents; /* hardwired vibrato depth; 0 => base */
  float lfo2_rate_hz;       /* matrix-routed LFO2 rate; 0 => base */
  float glide_ms;           /* portamento; 0 => base */

  /* --- realism polish --- */
  int body;            /* SonareSynthBodyType; 0 => base */
  float body_mix;      /* body resonance mix [0,1]; 0 => base */
  float stereo_spread; /* seeded per-voice pan scatter [0,1]; 0 => base */

  /* --- mod matrix (REPLACES the base matrix when num_mod_routings > 0) --- */
  int num_mod_routings;
  SonareSynthModRouting mod_routings[SONARE_SYNTH_PATCH_MOD_ROUTINGS];

  /* --- voice pool / bus --- */
  float gain;      /* master output gain (linear); 0 => base */
  int polyphony;   /* max voices [1,64]; 0 => base */
  float bus_drive; /* gain-neutral bus saturation [0,1]; 0 => base */

  /* --- explicit-value presence (struct_version 2) --- */
  /* Bitmask of SONARE_SYNTH_FIELD_* naming the fields the caller set on
     purpose. A set bit overrides the base with the field's value even when that
     value is zero, which the "0 => base" rule above cannot express; a clear bit
     keeps the version-1 behaviour, so a caller that only fills the fields it
     wants to change needs no mask at all. Ignored unless struct_version is 2.
     32 bits with 27 in use; a further extension appends a second word under a
     new struct_version rather than widening this one. */
  uint32_t present_fields;
} SonareSynthPatch;

/* Bit positions for SonareSynthPatch.present_fields. The enum fields are absent
   on purpose: their zero is already the reserved "keep base" value and every
   real value is non-zero, so they have nothing to disambiguate. */
#define SONARE_SYNTH_FIELD_UNISON (1u << 0)
#define SONARE_SYNTH_FIELD_DETUNE_CENTS (1u << 1)
#define SONARE_SYNTH_FIELD_DRIFT_CENTS (1u << 2)
#define SONARE_SYNTH_FIELD_DRIVE (1u << 3)
#define SONARE_SYNTH_FIELD_CUTOFF_HZ (1u << 4)
#define SONARE_SYNTH_FIELD_RESONANCE_Q (1u << 5)
#define SONARE_SYNTH_FIELD_KEY_TRACK (1u << 6)
#define SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS (1u << 7)
#define SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS (1u << 8)
#define SONARE_SYNTH_FIELD_AMP_ATTACK_MS (1u << 9)
#define SONARE_SYNTH_FIELD_AMP_DECAY_MS (1u << 10)
#define SONARE_SYNTH_FIELD_AMP_SUSTAIN (1u << 11)
#define SONARE_SYNTH_FIELD_AMP_RELEASE_MS (1u << 12)
#define SONARE_SYNTH_FIELD_FILTER_ATTACK_MS (1u << 13)
#define SONARE_SYNTH_FIELD_FILTER_DECAY_MS (1u << 14)
#define SONARE_SYNTH_FIELD_FILTER_SUSTAIN (1u << 15)
#define SONARE_SYNTH_FIELD_FILTER_RELEASE_MS (1u << 16)
#define SONARE_SYNTH_FIELD_LFO_RATE_HZ (1u << 17)
#define SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS (1u << 18)
#define SONARE_SYNTH_FIELD_LFO2_RATE_HZ (1u << 19)
#define SONARE_SYNTH_FIELD_GLIDE_MS (1u << 20)
#define SONARE_SYNTH_FIELD_BODY_MIX (1u << 21)
#define SONARE_SYNTH_FIELD_STEREO_SPREAD (1u << 22)
#define SONARE_SYNTH_FIELD_GAIN (1u << 23)
#define SONARE_SYNTH_FIELD_POLYPHONY (1u << 24)
#define SONARE_SYNTH_FIELD_BUS_DRIVE (1u << 25)
/* Set with num_mod_routings == 0 to clear the base mod matrix rather than keep
   it; a non-empty table replaces the base matrix with or without the bit. */
#define SONARE_SYNTH_FIELD_MOD_ROUTINGS (1u << 26)

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

static_assert(SONARE_SYNTH_ENGINE_FREE_REED + 1 == SONARE_SYNTH_ENGINE_MODE_COUNT,
              "SonareSynthEngineMode count changed");
static_assert(SONARE_SYNTH_OSC_NOISE + 1 == SONARE_SYNTH_OSC_WAVEFORM_COUNT,
              "SonareSynthOscWaveform count changed");
static_assert(SONARE_SYNTH_FILTER_SALLEN_KEY + 1 == SONARE_SYNTH_FILTER_MODEL_COUNT,
              "SonareSynthFilterModel count changed");
static_assert(SONARE_SYNTH_FILTER_OUT_HIGHPASS + 1 == SONARE_SYNTH_FILTER_OUTPUT_COUNT,
              "SonareSynthFilterOutput count changed");
static_assert(SONARE_SYNTH_BODY_VOCAL + 1 == SONARE_SYNTH_BODY_TYPE_COUNT,
              "SonareSynthBodyType count changed");

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
static_assert(SONARE_SYNTH_FIELD_MOD_ROUTINGS ==
                  1u << 26,  // Highest bit in use; widening needs a struct_version bump.
              "SonareSynthPatch presence bit range changed");
#endif

#ifdef __cplusplus
}
#endif
