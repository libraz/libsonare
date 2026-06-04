#include "midi/synth/percussion_voice.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
/// Noise draws live far above any other per-voice index range.
constexpr uint64_t kNoiseIndexBase = 1ull << 20;

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

float radius_for(double sample_rate, float t60_s) noexcept {
  return std::exp(-6.907755279f / (static_cast<float>(sample_rate) * std::max(0.005f, t60_s)));
}

}  // namespace

void PercussionVoiceCore::start(const PercussionPatchParams& params, double sample_rate,
                                uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  noise_index_ = 0;

  const float base_hz = params.base_freq_hz > 0.0f ? params.base_freq_hz : note_to_hz(note);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;

  // Membrane modes: harder hits excite the upper ring modes a bit more.
  num_modes_ = std::clamp(params.num_modes, 0, kMaxPercussionModes);
  tone_gain_ = std::max(0.0f, params.tone_gain);
  const float nyquist_limit = 0.45f * static_cast<float>(sr);
  for (int k = 0; k < num_modes_; ++k) {
    Mode& mode = modes_[static_cast<size_t>(k)];
    const float ratio = params.mode_ratios[static_cast<size_t>(k)];
    const float freq = base_hz * std::max(0.01f, ratio);
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    if (ratio <= 0.0f || freq >= nyquist_limit) {
      mode = Mode{};
      continue;
    }
    mode.omega = kTwoPi * freq / static_cast<float>(sr);
    // Upper membrane modes die faster than the fundamental (1/ratio scaling).
    mode.r = radius_for(sr, std::max(0.005f, params.mode_decay_s) / std::max(1.0f, ratio));
    const float strike = k == 0 ? 1.0f : (0.4f + 0.4f * vel01) / static_cast<float>(k + 1);
    mode.gain = strike * std::sin(mode.omega);
  }
  for (int k = num_modes_; k < kMaxPercussionModes; ++k) modes_[static_cast<size_t>(k)] = Mode{};

  // Descending pitch envelope.
  drop_state_ = std::max(0.0f, params.pitch_drop);
  drop_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.pitch_drop_ms) * 0.001f * static_cast<float>(sr)));
  cached_ratio_ = 0.0f;
  excite_ = num_modes_ > 0;

  // Noise layer.
  noise_level_ = std::max(0.0f, params.noise_gain) * (0.6f + 0.4f * vel01);
  noise_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.noise_decay_ms) * 0.001f * static_cast<float>(sr)));
  noise_output_ = params.noise_output;
  noise_filter_.prepare(sr);
  noise_filter_.set(params.noise_cutoff_hz, std::max(0.5f, params.noise_q));
  noise_filter_.reset();
}

float PercussionVoiceCore::render(float pitch_ratio) noexcept {
  float mix = 0.0f;

  if (num_modes_ > 0) {
    // Tone layer with the descending strike pitch folded into the ratio.
    float ratio = pitch_ratio * (1.0f + drop_state_);
    if (drop_state_ > 0.0f) {
      drop_state_ *= drop_coeff_;
      if (drop_state_ < 1.0e-3f) drop_state_ = 0.0f;
    }
    if (ratio != cached_ratio_) {
      cached_ratio_ = ratio;
      for (int k = 0; k < num_modes_; ++k) {
        Mode& mode = modes_[static_cast<size_t>(k)];
        if (mode.gain == 0.0f && mode.r == 0.0f) continue;
        const float w = std::min(mode.omega * ratio, 0.95f * 3.14159265359f);
        mode.a1 = 2.0f * mode.r * std::cos(w);
        mode.a2 = -mode.r * mode.r;
      }
    }
    const float x = excite_ ? 1.0f : 0.0f;
    excite_ = false;
    float tone = 0.0f;
    for (int k = 0; k < num_modes_; ++k) {
      Mode& mode = modes_[static_cast<size_t>(k)];
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * x;
      mode.y2 = mode.y1;
      mode.y1 = y;
      tone += y;
    }
    mix += tone_gain_ * tone;
  }

  if (noise_level_ > 1.0e-5f) {
    const float burst = noise_.bipolar_at(kNoiseIndexBase + noise_index_++) * noise_level_;
    noise_level_ *= noise_coeff_;
    const TptSvf::Outputs out = noise_filter_.process(burst);
    switch (noise_output_) {
      case SynthFilterOutput::kHighpass:
        mix += out.hp;
        break;
      case SynthFilterOutput::kBandpass:
        mix += out.bp;
        break;
      case SynthFilterOutput::kLowpass:
        mix += out.lp;
        break;
    }
  }

  return mix;
}

void PercussionVoiceCore::kill() noexcept {
  for (Mode& mode : modes_) {
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    mode.gain = 0.0f;
  }
  num_modes_ = 0;
  noise_level_ = 0.0f;
  excite_ = false;
}

}  // namespace sonare::midi::synth
