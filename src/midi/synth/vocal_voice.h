#pragma once

/// @file vocal_voice.h
/// @brief Source-filter vocal core for the NativeSynth voice — the choir /
///        voice family (GM 52-54) as a glottal source driving a formant bank,
///        the model an additive/subtractive "aah" pad cannot be: a real vowel
///        is a broadband glottal pulse train shaped by the vocal tract's
///        resonances (the formants), so the timbre is defined by WHERE the
///        formant peaks sit, not by a fixed partial recipe.
///
/// Two stages:
///   1. GLOTTAL SOURCE: a band-limited pulse train at the note fundamental with
///      a spectral tilt (the glottal flow's -12 dB/oct roll-off), plus a small
///      aspiration noise (breath). This is the voice's excitation.
///   2. FORMANT BANK: a parallel bank of resonant bandpass biquads tuned to a
///      vowel's formant frequencies / bandwidths (F1..F5). The vowel selector
///      picks the formant table; brightness tilts the source and opens the
///      upper formants.
///
/// A voice-local vibrato (a singer's own vibrato) modulates the source pitch.
/// No delay line is needed (source-filter is feed-forward), so the core owns no
/// host slab.
///
/// RT contract: start()/render() are allocation-free. Determinism: the
/// aspiration noise is the counter-based (voice_index, note, age) stream, so
/// identical event streams render bit-identically.

#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Formant bank size (F1..F5). Five poles cover a sung vowel's spectral
/// envelope; unused formants have zero gain.
inline constexpr int kVocalFormants = 5;

/// Source-filter vocal section of a NativeSynthPatch (used when mode == kVocal).
struct VocalPatchParams {
  /// Vowel selector: 0 = /a/ (ah), 1 = /e/ (eh), 2 = /i/ (ee), 3 = /o/ (oh),
  /// 4 = /u/ (oo). Out-of-range folds to /a/. Chooses the formant table.
  int vowel = 0;
  /// Source/formant brightness in [0,1]: tilts the glottal source and opens the
  /// upper formants (0 = dark/covered, 1 = bright/forward).
  float brightness = 0.5f;
  /// Aspiration (breath) noise in [0,1]: the breathy air component mixed into
  /// the glottal source (0 = a pressed/clean voice, higher = breathy).
  float breath_noise = 0.1f;
  /// Voice-local vibrato rate (Hz) — a singer's own vibrato. Read at note-on.
  float vibrato_rate_hz = 5.5f;
  /// Vibrato depth in [0,1] (0 = no vibrato; the LFO is skipped so an
  /// unmodulated note renders identically).
  float vibrato_depth = 0.3f;
  /// Onset rise (ms): the vowel swells in rather than stepping.
  float attack_ms = 30.0f;
  /// Release fall (ms): the voice fades on note-off.
  float release_ms = 120.0f;
};

/// Per-voice vocal state, embedded in NativeSynthVoice. The voice's amplitude
/// envelope / filter / mod matrix wrap around this core; render() returns the
/// raw vocal sample.
class VocalVoiceCore {
 public:
  /// Configures the source / formant bank for @p note / @p velocity and seeds
  /// the aspiration noise. @p seed drives the deterministic breath.
  void start(const VocalPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: ramp the source level to zero over release_ms.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  double sample_rate_ = 48000.0;
  float base_freq_hz_ = 220.0f;

  // Glottal source: a phase accumulator driving a band-limited pulse with a
  // one-pole spectral tilt (the glottal roll-off).
  float phase_ = 0.0f;
  float phase_inc_ = 0.0f;
  float tilt_state_ = 0.0f;
  float tilt_alpha_ = 1.0f;

  // Formant bank: parallel resonant bandpass biquads (Direct Form II transposed
  // state z1/z2 per formant). Coefficients are set at start() from the vowel
  // table and the note.
  int num_formants_ = 0;
  float form_b0_[kVocalFormants] = {};
  float form_b2_[kVocalFormants] = {};  // bandpass: b1 == 0, b2 == -b0
  float form_a1_[kVocalFormants] = {};
  float form_a2_[kVocalFormants] = {};
  float form_gain_[kVocalFormants] = {};
  float form_z1_[kVocalFormants] = {};
  float form_z2_[kVocalFormants] = {};

  // Level contour: a one-pole ramp toward the target (1 while singing, 0 once
  // released).
  float level_target_ = 1.0f;
  float level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Voice-local vibrato LFO. depth_ == 0 -> skipped (bit-identical).
  float vib_depth_ = 0.0f;
  float vib_phase_ = 0.0f;
  float vib_inc_ = 0.0f;

  // Aspiration (breath) noise.
  float breath_ = 0.0f;
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  /// Output trim bringing the raw vocal sample up to a musical voice level.
  float output_scale_ = 1.0f;
};

}  // namespace sonare::midi::synth
