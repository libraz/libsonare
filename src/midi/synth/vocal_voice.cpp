#include "midi/synth/vocal_voice.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// One vowel formant: centre frequency, level relative to F1, and bandwidth.
struct VowelFormant {
  float freq_hz;
  float amp_db;
  float bw_hz;
};

// Bass-voice vowel formant tables (F1..F5), indexed by the vowel selector
// (0 = /a/, 1 = /e/, 2 = /i/, 3 = /o/, 4 = /u/). Frequencies / relative levels /
// bandwidths follow the classic sung-bass measurements used by the Csound
// FOF vowel corpus.
constexpr VowelFormant kVowelTable[5][kVocalFormants] = {
    // /a/
    {{600.0f, 0.0f, 60.0f},
     {1040.0f, -7.0f, 70.0f},
     {2250.0f, -9.0f, 110.0f},
     {2450.0f, -9.0f, 120.0f},
     {2750.0f, -20.0f, 130.0f}},
    // /e/
    {{400.0f, 0.0f, 40.0f},
     {1620.0f, -12.0f, 80.0f},
     {2400.0f, -9.0f, 100.0f},
     {2800.0f, -12.0f, 120.0f},
     {3100.0f, -18.0f, 120.0f}},
    // /i/
    {{250.0f, 0.0f, 60.0f},
     {1750.0f, -30.0f, 90.0f},
     {2600.0f, -16.0f, 100.0f},
     {3050.0f, -22.0f, 120.0f},
     {3340.0f, -28.0f, 120.0f}},
    // /o/
    {{400.0f, 0.0f, 40.0f},
     {750.0f, -11.0f, 80.0f},
     {2400.0f, -21.0f, 100.0f},
     {2600.0f, -20.0f, 120.0f},
     {2900.0f, -40.0f, 120.0f}},
    // /u/
    {{350.0f, 0.0f, 40.0f},
     {600.0f, -20.0f, 60.0f},
     {2400.0f, -32.0f, 100.0f},
     {2675.0f, -28.0f, 120.0f},
     {2950.0f, -36.0f, 120.0f}},
};

// Glottal-source tilt corner: the one-pole lowpass shaping the raw sawtooth
// into the glottal flow's spectral roll-off. Brightness slides the corner up
// (a forward, ringing voice keeps more source harmonics for the upper formants
// to catch); the range spans a covered voice to a bright one.
constexpr float kTiltCornerBaseHz = 350.0f;
constexpr float kTiltCornerOctSpan = 3.0f;  // corner = base * 2^(span*brightness)

// Brightness also opens the upper formants: F5 moves by up to this much (dB)
// around the table value, scaled linearly down to 0 at F1 so the vowel identity
// (carried by F1/F2) is untouched.
constexpr float kBrightFormantSpanDb = 12.0f;

// Aspiration depth at breath_noise == 1: the noise is mixed into the excitation
// BEFORE the formant bank, so the breath is vowel-coloured like real
// aspiration, not a flat hiss on top.
constexpr float kBreathDepth = 0.25f;

// Vibrato depth scale: depth 1 modulates the pitch by about +/-50 cents
// (a wide operatic vibrato); typical patch depths sit far below.
constexpr float kVibratoMaxFrac = 0.03f;

// Output trim and velocity-to-level floor: the summed formant-bank response to
// the tilted source lands well under unity, so the trim brings a forte note
// into the other engines' [0.3, 0.8] range; a pianissimo strike keeps a body.
constexpr float kOutputScale = 2.0f;
constexpr float kVelFloor = 0.4f;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// One-pole ramp coefficient reaching ~95% of the target in @p ms.
float ramp_coeff(float ms, double sample_rate) noexcept {
  const double t = std::max(0.5f, ms) * 0.001 * sample_rate;
  return static_cast<float>(1.0 - std::exp(-3.0 / std::max(1.0, t)));
}

}  // namespace

void VocalVoiceCore::start(const VocalPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  sample_rate_ = sr;
  noise_ = VoiceRandomSequence(seed);
  drive_index_ = 0;

  // Glottal source pitch.
  base_freq_hz_ = note_to_hz(note);
  phase_ = 0.0f;
  phase_inc_ = base_freq_hz_ / static_cast<float>(sr);

  // Source tilt: brightness slides the glottal roll-off corner up, keeping more
  // of the source's upper harmonics for the high formants.
  const float bright = std::clamp(params.brightness, 0.0f, 1.0f);
  const float corner = std::min(kTiltCornerBaseHz * std::exp2(kTiltCornerOctSpan * bright),
                                0.45f * static_cast<float>(sr));
  tilt_alpha_ = 1.0f - std::exp(-kTwoPi * corner / static_cast<float>(sr));
  tilt_state_ = 0.0f;

  // Formant bank: RBJ constant-0dB-peak bandpass biquads at the vowel's F1..F5.
  // form_a1_/form_a2_ hold the NEGATED normalized denominator so render() only
  // accumulates. All poles lie strictly inside the unit circle (alpha > 0), so
  // the bank is unconditionally stable.
  const int vowel = (params.vowel >= 0 && params.vowel < 5) ? params.vowel : 0;
  num_formants_ = kVocalFormants;
  for (int i = 0; i < kVocalFormants; ++i) {
    const VowelFormant& fm = kVowelTable[vowel][i];
    const float srf = static_cast<float>(sr);
    const float f = std::min(fm.freq_hz, 0.45f * srf);
    const float q = f / std::max(1.0f, fm.bw_hz);
    const float w = kTwoPi * f / srf;
    const float alpha = std::sin(w) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    form_b0_[i] = alpha / a0;
    form_b2_[i] = -form_b0_[i];
    form_a1_[i] = 2.0f * std::cos(w) / a0;
    form_a2_[i] = -(1.0f - alpha) / a0;
    // Brightness opens the upper formants; F1 (the vowel anchor) stays put.
    const float open_db = (bright - 0.5f) * kBrightFormantSpanDb *
                          (static_cast<float>(i) / static_cast<float>(kVocalFormants - 1));
    form_gain_[i] = std::pow(10.0f, (fm.amp_db + open_db) / 20.0f);
    form_z1_[i] = 0.0f;
    form_z2_[i] = 0.0f;
  }

  // Aspiration and vibrato.
  breath_ = std::clamp(params.breath_noise, 0.0f, 1.0f) * kBreathDepth;
  vib_depth_ = std::clamp(params.vibrato_depth, 0.0f, 1.0f) * kVibratoMaxFrac;
  vib_phase_ = 0.0f;
  vib_inc_ = vib_depth_ > 0.0f
                 ? kTwoPi * std::max(0.1f, params.vibrato_rate_hz) / static_cast<float>(sr)
                 : 0.0f;

  // Level contour.
  level_ = 0.0f;
  level_target_ = 1.0f;
  releasing_ = false;
  attack_coeff_ = ramp_coeff(params.attack_ms, sr);
  release_coeff_ = ramp_coeff(params.release_ms, sr);

  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  output_scale_ = kOutputScale * (kVelFloor + (1.0f - kVelFloor) * vel01);
}

float VocalVoiceCore::render(float pitch_ratio) noexcept {
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;

  // Level contour: one-pole ramp toward 1 while singing, 0 once released.
  const float coeff = releasing_ ? release_coeff_ : attack_coeff_;
  level_ += coeff * (level_target_ - level_);

  // Voice-local vibrato: a small multiplicative wobble on the pitch. Depth 0
  // skips the LFO entirely so an unmodulated note renders bit-identically.
  float vib = 1.0f;
  if (vib_depth_ > 0.0f) {
    vib = 1.0f + vib_depth_ * std::sin(vib_phase_);
    vib_phase_ += vib_inc_;
    if (vib_phase_ >= kTwoPi) vib_phase_ -= kTwoPi;
  }

  // Glottal source: a naive sawtooth through the one-pole tilt (the glottal
  // flow's spectral roll-off), plus the seeded aspiration noise. Source-filter
  // is feed-forward, so any aliasing of the raw saw is further shaped by the
  // narrow formant bands rather than accumulated.
  float inc = phase_inc_ * ratio * vib;
  if (inc > 0.45f) inc = 0.45f;
  phase_ += inc;
  phase_ -= std::floor(phase_);
  const float saw = 2.0f * phase_ - 1.0f;
  tilt_state_ += tilt_alpha_ * (saw - tilt_state_);
  float exc = tilt_state_;
  if (breath_ > 0.0f) {
    exc += breath_ * noise_.bipolar_at(drive_index_);
  }
  ++drive_index_;

  // Formant bank: parallel bandpass biquads (direct form II transposed; the
  // stored a1/a2 are negated, so the recurrence is a pure accumulate).
  float sum = 0.0f;
  for (int i = 0; i < num_formants_; ++i) {
    const float y = form_b0_[i] * exc + form_z1_[i];
    form_z1_[i] = form_a1_[i] * y + form_z2_[i];
    form_z2_[i] = form_b2_[i] * exc + form_a2_[i] * y;
    sum += form_gain_[i] * y;
  }

  return sum * level_ * output_scale_;
}

void VocalVoiceCore::release() noexcept {
  releasing_ = true;
  level_target_ = 0.0f;
}

void VocalVoiceCore::kill() noexcept {
  level_ = 0.0f;
  level_target_ = 0.0f;
  releasing_ = true;
  phase_ = 0.0f;
  tilt_state_ = 0.0f;
  vib_phase_ = 0.0f;
  for (int i = 0; i < kVocalFormants; ++i) {
    form_z1_[i] = 0.0f;
    form_z2_[i] = 0.0f;
  }
}

}  // namespace sonare::midi::synth
