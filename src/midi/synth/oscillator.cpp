#include "midi/synth/oscillator.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// 2-sample polynomial band-limited step residual at the phase discontinuity.
/// @p t is the phase in [0,1), @p dt the per-sample phase increment.
float poly_blep(float t, float dt) noexcept {
  if (t < dt) {
    const float x = t / dt;
    return x + x - x * x - 1.0f;  // rising edge just passed
  }
  if (t > 1.0f - dt) {
    const float x = (t - 1.0f) / dt;
    return x * x + x + x + 1.0f;  // rising edge imminent
  }
  return 0.0f;
}

/// Ideal triangle for @p phase in [0,1): -1 at 0, +1 at 0.5 (the integral of
/// the +1-first square). Used to pre-charge the leaky integrator at start so
/// the first cycle is not a DC-settling thump.
float ideal_triangle(float phase) noexcept {
  return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

}  // namespace

void VaOscillator::start(double sample_rate, VaWaveform waveform, float phase01,
                         uint64_t noise_seed) noexcept {
  sample_rate_ = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  waveform_ = waveform;
  phase_ = phase01 - std::floor(phase01);
  inc_ = 0.0f;
  tri_state_ = ideal_triangle(phase_);
  noise_seed_ = noise_seed;
  noise_counter_ = 0;
}

void VaOscillator::set_frequency(float freq_hz) noexcept {
  const float nyquist = 0.5f * sample_rate_;
  inc_ = std::clamp(freq_hz, 0.0f, nyquist - 1.0f) / sample_rate_;
}

float VaOscillator::next() noexcept {
  const float t = phase_;
  phase_ += inc_;
  if (phase_ >= 1.0f) phase_ -= 1.0f;

  switch (waveform_) {
    case VaWaveform::kSine:
      return std::sin(kTwoPi * t);
    case VaWaveform::kSaw:
      return 2.0f * t - 1.0f - poly_blep(t, inc_);
    case VaWaveform::kSquare: {
      float s = t < 0.5f ? 1.0f : -1.0f;
      s += poly_blep(t, inc_);
      // The falling edge at phase 0.5 is the rising edge of phase + 0.5.
      float t2 = t + 0.5f;
      if (t2 >= 1.0f) t2 -= 1.0f;
      s -= poly_blep(t2, inc_);
      return s;
    }
    case VaWaveform::kTriangle: {
      // Integrate the PolyBLEP square through a leaky integrator. The leak
      // (time constant ~4 cycles) keeps DC bounded without audible droop.
      float s = t < 0.5f ? 1.0f : -1.0f;
      s += poly_blep(t, inc_);
      float t2 = t + 0.5f;
      if (t2 >= 1.0f) t2 -= 1.0f;
      s -= poly_blep(t2, inc_);
      tri_state_ = tri_state_ * (1.0f - 0.25f * inc_) + 4.0f * inc_ * s;
      return tri_state_;
    }
    case VaWaveform::kNoise:
      return voice_random_bipolar(noise_seed_ ^ noise_counter_++);
  }
  return 0.0f;
}

}  // namespace sonare::midi::synth
