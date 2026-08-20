#pragma once

#include <stddef.h>
#include <stdint.h>

/* Fixed underlying type for every public enum, applied as `typedef enum
   SONARE_ENUM_BASE { ... } Name;`.

   An enum without a fixed underlying type may only hold the values its
   enumerators span, so loading anything else is undefined behaviour. Every C
   ABI entry point that range-checks an enum parameter has to load the value in
   order to reject it, which makes the validation itself the undefined
   operation -- exactly what a foreign caller is expected to trigger, and what
   UndefinedBehaviorSanitizer's `enum` check reports. Fixing the underlying type
   makes the whole int32_t range representable, so the check has nothing to
   report and the rejection path stays as written.

   A fixed underlying type is C++11 but only C23, so C consumers keep the
   ordinary enum. Both spellings occupy 4 bytes and are laid out identically;
   the guards in src/c_api/core_common.cpp hold that claim mechanically. */
#ifdef __cplusplus
#define SONARE_ENUM_BASE : int32_t
#else
#define SONARE_ENUM_BASE
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum SONARE_ENUM_BASE {
  SONARE_OK = 0,
  SONARE_ERROR_FILE_NOT_FOUND = 1,
  SONARE_ERROR_INVALID_FORMAT = 2,
  SONARE_ERROR_DECODE_FAILED = 3,
  SONARE_ERROR_INVALID_PARAMETER = 4,
  SONARE_ERROR_OUT_OF_MEMORY = 5,
  SONARE_ERROR_NOT_SUPPORTED = 6,
  SONARE_ERROR_INVALID_STATE = 7,
  SONARE_ERROR_CANCELLED = 8,
  /// Producing or writing an output artefact failed. The read-side sibling is
  /// SONARE_ERROR_DECODE_FAILED.
  SONARE_ERROR_ENCODE_FAILED = 9,
  SONARE_ERROR_UNKNOWN = 99
} SonareError;

// Pitch class enum
typedef enum SONARE_ENUM_BASE {
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
typedef enum SONARE_ENUM_BASE {
  SONARE_MODE_MAJOR = 0,
  SONARE_MODE_MINOR = 1,
  SONARE_MODE_DORIAN = 2,
  SONARE_MODE_PHRYGIAN = 3,
  SONARE_MODE_LYDIAN = 4,
  SONARE_MODE_MIXOLYDIAN = 5,
  SONARE_MODE_LOCRIAN = 6
} SonareMode;

typedef enum SONARE_ENUM_BASE {
  SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER = 0,
  SONARE_KEY_PROFILE_TEMPERLEY = 1,
  SONARE_KEY_PROFILE_SHAATH = 2,
  SONARE_KEY_PROFILE_FARALDO_EDMT = 3,
  SONARE_KEY_PROFILE_FARALDO_EDMA = 4,
  SONARE_KEY_PROFILE_FARALDO_EDMM = 5,
  SONARE_KEY_PROFILE_BELLMAN_BUDGE = 6
} SonareKeyProfileType;

typedef enum SONARE_ENUM_BASE {
  SONARE_TEMPOGRAM_AUTOCORRELATION = 0,
  SONARE_TEMPOGRAM_COSINE = 1
} SonareTempogramMode;

/* Ordinals mirror sonare::editing::voice_changer::VoiceCharacterPreset; do not
   reorder. The string ids returned by sonare_voice_character_preset_id() are
   exactly the entries (in this order) of SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS. */
typedef enum SONARE_ENUM_BASE {
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
typedef struct SonareClipPageProvider SonareClipPageProvider;

#define SONARE_EQ_MAX_BANDS 24
#define SONARE_EQ_SPECTRUM_STREAM_CAPACITY 256
#define SONARE_EQ_SPECTRUM_PROFILE_BANDS 16

// Values match the offline `phaseMode` param and eq::PhaseMode ordinals;
// 0 (Inherit) is invalid for a global phase mode.
typedef enum SONARE_ENUM_BASE {
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

/* Relationship between a tempo candidate and SonareAnalysisResult.bpm. */
typedef enum SONARE_ENUM_BASE {
  SONARE_BPM_CANDIDATE_PRIMARY = 0,
  SONARE_BPM_CANDIDATE_HALF = 1,
  SONARE_BPM_CANDIDATE_DOUBLE = 2,
  SONARE_BPM_CANDIDATE_OTHER = 3
} SonareBpmCandidateRelation;

typedef struct {
  float value;
  float confidence;
  SonareBpmCandidateRelation relation;
} SonareAnalysisBpmCandidate;

// Flat analysis result structure. Producing this result runs the full quick
// analysis pipeline because the flat result still includes meter/beat data;
// use sonare_detect_bpm/key/beats for cheaper single-purpose queries.
typedef struct {
  float bpm;
  float bpm_confidence;
  SonareKey key;
  SonareTimeSignature time_signature;
  float* beat_times;
  size_t beat_count;
  SonareAnalysisBpmCandidate* bpm_candidates;
  size_t bpm_candidate_count;
  SonareTimeSignature* time_signature_candidates;
  size_t time_signature_candidate_count;
} SonareAnalysisResult;

#ifdef __cplusplus
}
#endif
