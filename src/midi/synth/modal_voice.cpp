#include "midi/synth/modal_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// Per-sample decay radius reaching -60 dB after @p t60_s.
float radius_for(double sample_rate, float t60_s) noexcept {
  return std::exp(-6.907755279f / (static_cast<float>(sample_rate) * std::max(0.01f, t60_s)));
}

}  // namespace

void ModalVoiceCore::start(const ModalPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float f0 = note_to_hz(note);

  // Mallet hardness: velocity opens the upper-mode excitation.
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float hardness =
      std::clamp(params.strike_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);

  // Decay stretching: big bars (low notes) ring longer.
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  const float octaves_below_a4 = (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f;
  const float t60 = std::max(0.01f, params.decay_s) * std::exp2(stretch * octaves_below_a4);

  VoiceRandomSequence scatter(seed);
  num_modes_ = std::clamp(params.num_modes, 0, kMaxModalModes);
  const float nyquist_limit = 0.45f * static_cast<float>(sample_rate_);
  for (int k = 0; k < num_modes_; ++k) {
    const ModalMode& src = params.modes[static_cast<size_t>(k)];
    Mode& mode = modes_[static_cast<size_t>(k)];
    const float freq = f0 * std::max(0.01f, src.ratio);
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    if (freq >= nyquist_limit) {
      mode.omega = 0.0f;
      mode.r = 0.0f;
      mode.gain = 0.0f;
      continue;
    }
    mode.omega = kTwoPi * freq / static_cast<float>(sample_rate_);
    mode.r = radius_for(sample_rate_, t60 * std::max(0.01f, src.decay_scale));
    // Mallet curve: soft strikes excite the fundamental only; the seeded
    // +-10% scatter keeps repeated strikes from sounding machine-identical.
    const float mallet = std::exp(-(1.0f - hardness) * 1.5f * static_cast<float>(k));
    const float jitter = 1.0f + 0.1f * scatter.bipolar_at(static_cast<uint64_t>(k));
    // sin(omega) normalizes the two-pole impulse response to ~unit amplitude.
    mode.gain = std::max(0.0f, src.gain) * mallet * jitter * std::sin(mode.omega);
  }
  for (int k = num_modes_; k < kMaxModalModes; ++k) modes_[static_cast<size_t>(k)] = Mode{};

  release_r_ = radius_for(sample_rate_, std::max(0.01f, params.release_damp_s));
  cached_ratio_ = 0.0f;  // force coefficient derivation on the first render
  excite_ = true;
}

void ModalVoiceCore::refresh_coefficients(float pitch_ratio) noexcept {
  cached_ratio_ = pitch_ratio;
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    if (mode.gain == 0.0f && mode.r == 0.0f) continue;
    const float w = std::min(mode.omega * pitch_ratio, 0.95f * 3.14159265359f);
    mode.a1 = 2.0f * mode.r * std::cos(w);
    mode.a2 = -mode.r * mode.r;
  }
}

float ModalVoiceCore::render(float pitch_ratio) noexcept {
  if (num_modes_ <= 0) return 0.0f;
  if (pitch_ratio != cached_ratio_) refresh_coefficients(pitch_ratio);
  const float x = excite_ ? 1.0f : 0.0f;
  excite_ = false;
  float mix = 0.0f;
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * x;
    mode.y2 = mode.y1;
    mode.y1 = y;
    mix += y;
  }
  return mix;
}

void ModalVoiceCore::release() noexcept {
  // Damp: cap every mode's radius at the release t60 and re-derive a1/a2.
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    if (mode.r > release_r_) {
      mode.r = release_r_;
    }
  }
  cached_ratio_ = 0.0f;  // re-derive on next render
}

void ModalVoiceCore::kill() noexcept {
  for (Mode& mode : modes_) {
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    mode.gain = 0.0f;
  }
  excite_ = false;
  num_modes_ = 0;
}

}  // namespace sonare::midi::synth
