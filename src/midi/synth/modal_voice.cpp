#include "midi/synth/modal_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
#include "midi/synth/voice_random.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kMidiA4;
using sonare::constants::kPiD;
using sonare::constants::kSemitonesPerOctave;
using sonare::constants::kTwoPiD;

/// Per-sample decay radius reaching -60 dB after @p t60_s.
double radius_for(double sample_rate, double t60_s) noexcept {
  return std::exp(-6.9077552789821370521 / (sample_rate * std::max(0.01, t60_s)));
}

}  // namespace

void ModalVoiceCore::start(const ModalPatchParams& params, double sample_rate, uint8_t note,
                           Velocity16 velocity, uint64_t seed) noexcept {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const double f0 = static_cast<double>(note_to_hz(note));

  // Mallet hardness: velocity opens the upper-mode excitation.
  const float vel01 = velocity.f7() / 127.0f;
  const float vel_amount = std::clamp(params.vel_to_brightness, 0.0f, 1.0f);
  const float hardness =
      std::clamp(params.strike_brightness, 0.0f, 1.0f) * ((1.0f - vel_amount) + vel_amount * vel01);

  // Decay stretching: big bars (low notes) ring longer.
  const double stretch = std::clamp(static_cast<double>(params.decay_stretch), 0.0, 1.0);
  const double octaves_below_a4 =
      (static_cast<double>(kMidiA4) - static_cast<double>(note & 0x7Fu)) /
      static_cast<double>(kSemitonesPerOctave);
  const double t60 =
      std::max(0.01, static_cast<double>(params.decay_s)) * std::exp2(stretch * octaves_below_a4);

  VoiceRandomSequence scatter(seed);
  num_modes_ = std::clamp(params.num_modes, 0, kMaxModalModes);
  for (int k = 0; k < num_modes_; ++k) {
    const ModalMode& src = params.modes[static_cast<size_t>(k)];
    Mode& mode = modes_[static_cast<size_t>(k)];
    const double freq = f0 * std::max(0.01, static_cast<double>(src.ratio));
    mode.y_s = 0.0;
    mode.y_c = 0.0;
    mode.omega = kTwoPiD * freq / sample_rate_;
    mode.r = radius_for(sample_rate_, t60 * std::max(0.01, static_cast<double>(src.decay_scale)));
    // Mallet curve: soft strikes excite the fundamental only; the seeded
    // +-10% scatter keeps repeated strikes from sounding machine-identical.
    const float mallet = std::exp(-(1.0f - hardness) * 1.5f * static_cast<float>(k));
    const float jitter = 1.0f + 0.1f * scatter.bipolar_at(static_cast<uint64_t>(k));
    // Preserve the strike weight; quadrature initialization applies sin(w).
    const float excitation = std::max(0.0f, src.gain) * mallet * jitter;
    mode.excitation = static_cast<double>(excitation);
    mode.sin_w = 0.0;
    mode.cos_w = 1.0;
    mode.started = false;
    mode.audible = false;
  }
  for (int k = num_modes_; k < kMaxModalModes; ++k) modes_[static_cast<size_t>(k)] = Mode{};

  release_r_ = radius_for(sample_rate_, std::max(0.01, static_cast<double>(params.release_damp_s)));
  cached_ratio_ = 0.0f;  // force coefficient derivation on the first render
  excite_ = true;
}

void ModalVoiceCore::refresh_coefficients(float pitch_ratio) noexcept {
  const double ratio = pitch_ratio > 0.01f ? static_cast<double>(pitch_ratio) : 0.01;
  cached_ratio_ = static_cast<float>(ratio);
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    const double w = mode.omega * ratio;
    if (mode.excitation == 0.0 || mode.r == 0.0 || w <= 0.0 || w >= 0.9 * kPiD) {
      // Clear the resonator when its current pitch is out of band. This keeps
      // a later bend from resurrecting a mode that was already struck while it
      // was inactive; excite_ is consumed by render() exactly once.
      mode.audible = false;
      mode.sin_w = 0.0;
      mode.cos_w = 1.0;
      mode.y_s = 0.0;
      mode.y_c = 0.0;
      continue;
    }
    mode.audible = true;
    mode.sin_w = std::sin(w);
    mode.cos_w = std::cos(w);
  }
}

float ModalVoiceCore::render(float pitch_ratio) noexcept {
  if (num_modes_ <= 0) return 0.0f;
  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  if (ratio != cached_ratio_) refresh_coefficients(ratio);
  const bool strike = excite_;
  excite_ = false;
  double mix = 0.0;
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    if (!mode.audible) {
      if (strike) mode.started = true;
      continue;
    }
    if (!mode.started) {
      if (!strike) continue;
      mode.y_s = mode.excitation * mode.sin_w;
      mode.y_c = mode.excitation * mode.cos_w;
      mode.started = true;
    } else {
      const double old_s = mode.y_s;
      const double old_c = mode.y_c;
      mode.y_s = mode.r * (old_s * mode.cos_w + old_c * mode.sin_w);
      mode.y_c = mode.r * (old_c * mode.cos_w - old_s * mode.sin_w);
    }
    mix += mode.y_s;
  }
  return static_cast<float>(mix);
}

void ModalVoiceCore::release() noexcept {
  // Damp: cap every mode's radius at the release t60.
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
    mode.y_s = 0.0;
    mode.y_c = 0.0;
    mode.excitation = 0.0;
    mode.sin_w = 0.0;
    mode.cos_w = 1.0;
    mode.started = false;
    mode.audible = false;
  }
  excite_ = false;
  num_modes_ = 0;
}

}  // namespace sonare::midi::synth
