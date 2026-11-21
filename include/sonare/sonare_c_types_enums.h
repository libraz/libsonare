#pragma once

#include <stddef.h>
#include <stdint.h>

/** Fixed underlying type for every public enum, applied as `typedef enum
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
/// The 32-bit underlying type of every public enum (C++).
#define SONARE_ENUM_BASE : int32_t
#else
/// Empty in C, whose enums keep the default 4-byte layout.
#define SONARE_ENUM_BASE
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Result code of every fallible C entry point; the binding error classes carry
/// the same numbers.
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
  /// A binding found that the loaded native library implements a different ABI
  /// than the binding was built for. The C library never returns it itself.
  SONARE_ERROR_ABI_MISMATCH = 10,
  /// An operation was attempted on a result its source has since invalidated.
  SONARE_ERROR_STALE_RESULT = 11,
  SONARE_ERROR_UNKNOWN = 99
} SonareError;

/// Severity of one entry in the structured diagnostic channel
/// (sonare_last_diagnostic_*). Ordinals mirror sonare::Diagnostic::Severity; do
/// not reorder.
typedef enum SONARE_ENUM_BASE {
  SONARE_DIAGNOSTIC_INFO = 0,
  SONARE_DIAGNOSTIC_WARNING = 1,
  SONARE_DIAGNOSTIC_ERROR = 2
} SonareDiagnosticSeverity;

/// Vector norm selector of sonare_vector_normalize; ordinals mirror sonare::NormType.
typedef enum SONARE_ENUM_BASE {
  SONARE_NORM_MAX = 0,
  SONARE_NORM_L1 = 1,
  SONARE_NORM_L2 = 2,
  SONARE_NORM_POWER = 3
} SonareNormType;

/// Chroma front end of the chord-detection options.
typedef enum SONARE_ENUM_BASE {
  SONARE_CHROMA_METHOD_STFT = 0,
  SONARE_CHROMA_METHOD_NNLS = 1,
  SONARE_CHROMA_METHOD_CQT = 2
} SonareChromaMethod;

/// Pitch class, C = 0 through B = 11; the `S` suffix is a sharp.
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

/// Scale mode of a detected key.
typedef enum SONARE_ENUM_BASE {
  SONARE_MODE_MAJOR = 0,
  SONARE_MODE_MINOR = 1,
  SONARE_MODE_DORIAN = 2,
  SONARE_MODE_PHRYGIAN = 3,
  SONARE_MODE_LYDIAN = 4,
  SONARE_MODE_MIXOLYDIAN = 5,
  SONARE_MODE_LOCRIAN = 6
} SonareMode;

/// Key-finding profile the chroma is correlated against.
typedef enum SONARE_ENUM_BASE {
  /** No profile pinned: the genre hint chooses one, or the chroma when the hint is "auto". */
  SONARE_KEY_PROFILE_AUTO = -1,
  SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER = 0,
  SONARE_KEY_PROFILE_TEMPERLEY = 1,
  SONARE_KEY_PROFILE_SHAATH = 2,
  SONARE_KEY_PROFILE_FARALDO_EDMT = 3,
  SONARE_KEY_PROFILE_FARALDO_EDMA = 4,
  SONARE_KEY_PROFILE_FARALDO_EDMM = 5,
  SONARE_KEY_PROFILE_BELLMAN_BUDGE = 6
} SonareKeyProfileType;

/// How a tempogram is computed from the onset envelope: autocorrelation or cosine (Fourier).
typedef enum SONARE_ENUM_BASE {
  SONARE_TEMPOGRAM_AUTOCORRELATION = 0,
  SONARE_TEMPOGRAM_COSINE = 1
} SonareTempogramMode;

/// Built-in voice changer character presets. Ordinals mirror
/// sonare::editing::voice_changer::VoiceCharacterPreset; do not reorder. The
/// string ids returned by sonare_voice_character_preset_id() are exactly the
/// entries (in this order) of SONARE_REALTIME_VOICE_CHANGER_PRESET_IDS.
typedef enum SONARE_ENUM_BASE {
  SONARE_VC_PRESET_NEUTRAL_MONITOR = 0,
  SONARE_VC_PRESET_BRIGHT_IDOL = 1,
  SONARE_VC_PRESET_SOFT_WHISPER = 2,
  SONARE_VC_PRESET_DEEP_NARRATOR = 3,
  SONARE_VC_PRESET_ROBOT_MASCOT = 4,
  SONARE_VC_PRESET_DARK_VILLAIN = 5,
  SONARE_VC_PRESET_HARD_TUNE = 6,
  SONARE_VC_PRESET_ROBOT_MONOTONE = 7,
  SONARE_VC_PRESET_CHOIR_HARMONY = 8,
  SONARE_VC_PRESET_VOCODER_STRINGS = 9,
  SONARE_VC_PRESET_MEGAPHONE = 10,
  SONARE_VC_PRESET_RADIO_VOICE = 11
} SonareVoiceCharacterPreset;

/* Realtime voice changer selectors. Ordinals mirror the enums of
   sonare::editing::voice_changer (voice_transform_types.h); do not reorder. */

/// Pitch-shifting algorithm of the voice changer: granular or PSOLA.
typedef enum SONARE_ENUM_BASE {
  SONARE_PITCH_ENGINE_GRAIN = 0,
  SONARE_PITCH_ENGINE_PSOLA = 1
} SonarePitchEngine;

/// How the voice changer chooses its target pitch. AUTO pulls toward the nearest note of
/// the scale by the correction amount; ROBOT holds the scale's tonic in the chosen octave.
typedef enum SONARE_ENUM_BASE {
  SONARE_PITCH_CORRECTION_OFF = 0,
  SONARE_PITCH_CORRECTION_AUTO = 1,
  SONARE_PITCH_CORRECTION_ROBOT = 2
} SonarePitchCorrectionMode;

/// Scale used by automatic pitch correction and harmony voices.
typedef enum SONARE_ENUM_BASE {
  SONARE_SCALE_MAJOR = 0,
  SONARE_SCALE_NATURAL_MINOR = 1,
  SONARE_SCALE_CHROMATIC = 2
} SonareScaleKind;

/// Where the vocoder takes its carrier from: its own oscillator or a second input.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCODER_CARRIER_INTERNAL = 0,
  SONARE_VOCODER_CARRIER_EXTERNAL = 1
} SonareVocoderCarrierSource;

/// Waveform of the vocoder's internal carrier.
typedef enum SONARE_ENUM_BASE {
  SONARE_VOCODER_WAVE_SAW = 0,
  SONARE_VOCODER_WAVE_PULSE = 1,
  SONARE_VOCODER_WAVE_ENSEMBLE = 2
} SonareVocoderCarrierWave;

/// Character effect of the voice changer's output stage.
typedef enum SONARE_ENUM_BASE {
  SONARE_CHARACTER_NONE = 0,
  SONARE_CHARACTER_MEGAPHONE = 1,
  SONARE_CHARACTER_RADIO = 2,
  SONARE_CHARACTER_BBD_CHORUS = 3,
  SONARE_CHARACTER_STROBO = 4
} SonareCharacterType;

/// Variation of the voice changer's reverb send.
typedef enum SONARE_ENUM_BASE {
  SONARE_REVERB_VARIATION_REVERB = 0,
  SONARE_REVERB_VARIATION_ECHO = 1,
  SONARE_REVERB_VARIATION_TEMPO_DELAY = 2,
  SONARE_REVERB_VARIATION_DUB_ECHO = 3
} SonareReverbVariation;

/// Note value a tempo-synced delay repeats at.
typedef enum SONARE_ENUM_BASE {
  SONARE_NOTE_DIVISION_QUARTER = 0,
  SONARE_NOTE_DIVISION_EIGHTH = 1,
  SONARE_NOTE_DIVISION_DOTTED_EIGHTH = 2,
  SONARE_NOTE_DIVISION_SIXTEENTH = 3
} SonareNoteDivision;

/// What MIDI-driven voices do once every held note is released: HOLD keeps the last
/// note's pitch, INPUT returns to the pitch the input and correction give.
typedef enum SONARE_ENUM_BASE {
  SONARE_MIDI_RELEASE_HOLD = 0,
  SONARE_MIDI_RELEASE_INPUT = 1
} SonareMidiRelease;

// Opaque types
/// Decoded mono audio and its sample rate; released with sonare_audio_destroy.
typedef struct SonareAudio SonareAudio;
/// Mixer of channel strips and buses; released with sonare_mixer_destroy.
typedef struct SonareMixer SonareMixer;
/// One mixer channel strip, borrowed from its SonareMixer.
typedef struct SonareStrip SonareStrip;
/// Realtime multi-band equalizer.
typedef struct SonareEq SonareEq;
/// Realtime playback and mixing engine; released with sonare_engine_destroy.
typedef struct SonareEngine SonareEngine;
/// Realtime voice changer; released with sonare_realtime_voice_changer_destroy.
typedef struct SonareRealtimeVoiceChanger SonareRealtimeVoiceChanger;
/// Block-by-block analyzer of a live stream; released with sonare_stream_analyzer_destroy.
typedef struct SonareStreamAnalyzer SonareStreamAnalyzer;
/// Supplies audio pages of a clip on demand; released with sonare_clip_page_provider_destroy.
typedef struct SonareClipPageProvider SonareClipPageProvider;

/// Most bands one SonareEq holds.
#define SONARE_EQ_MAX_BANDS 24
/// Capacity of each decimated time-domain scope array in SonareEqSnapshot.
#define SONARE_EQ_SPECTRUM_STREAM_CAPACITY 256
/// Number of 20 Hz to 20 kHz bands in SonareEqSnapshot.profile_db.
#define SONARE_EQ_SPECTRUM_PROFILE_BANDS 16

/// Phase response of a SonareEq. Values match the offline `phaseMode` param and
/// eq::PhaseMode ordinals; 0 (Inherit) is invalid for a global phase mode.
typedef enum SONARE_ENUM_BASE {
  SONARE_EQ_PHASE_ZERO_LATENCY = 1,
  SONARE_EQ_PHASE_NATURAL = 2,
  SONARE_EQ_PHASE_LINEAR = 3
} SonareEqPhaseMode;

/** Detected key.

   confidence is the share of the model's belief that this key is the answer,
   in [0, 1): a softmax over the profile correlations of every candidate that
   was scored, so it falls as the runner-up closes in and two keys that split
   the evidence -- a relative major and minor, typically -- each report about
   half.

   It is the model's own belief, NOT a measured accuracy. It says how decisively
   the chroma picked this key out of the candidate set; it does not say how
   often that pick is right, and nothing here has been fitted against annotated
   recordings. A confident wrong answer is entirely possible, so a pipeline that
   branches on it must choose its own threshold against its own material. */
typedef struct {
  SonarePitchClass root;  ///< Tonic pitch class.
  SonareMode mode;        ///< Scale mode.
  float confidence;       ///< Belief in this key over the scored candidates, in [0, 1); see above.
} SonareKey;

/// One scored key hypothesis.
typedef struct {
  SonareKey key;      ///< The candidate key.
  float correlation;  ///< Pearson correlation of the mean chroma with the key profile, in [-1, 1].
} SonareKeyCandidate;

/// Detected time signature.
typedef struct {
  int numerator;     ///< Beats per bar.
  int denominator;   ///< Note value of one beat (4 = quarter note).
  float confidence;  ///< Belief in this meter over the other candidates, in [0, 1].
} SonareTimeSignature;

#ifdef __cplusplus
}
#endif
