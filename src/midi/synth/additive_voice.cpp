#include "midi/synth/additive_voice.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

/// Hammond drawbar pitches as ratios to the played note.
constexpr std::array<float, kAdditivePartials> kDrawbarRatios = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f,
                                                                 4.0f, 5.0f, 6.0f, 8.0f};

float note_to_hz(uint8_t note) noexcept {
  return 440.0f * std::exp2((static_cast<float>(note & 0x7Fu) - 69.0f) / 12.0f);
}

/// Drawbar stop digit (0-8) -> linear gain (~3 dB per stop, 0 = off).
float drawbar_gain(float level) noexcept {
  const float stops = std::clamp(level, 0.0f, 8.0f);
  if (stops <= 0.0f) return 0.0f;
  return std::pow(10.0f, (stops - 8.0f) * 3.0f / 20.0f);
}

}  // namespace

void AdditiveVoiceCore::start(const AdditivePatchParams& params, double sample_rate, uint8_t note,
                              uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  const float f0 = note_to_hz(note);
  noise_ = VoiceRandomSequence(seed);

  float norm = 0.0f;
  for (int k = 0; k < kAdditivePartials; ++k) {
    norm += drawbar_gain(params.drawbars[static_cast<size_t>(k)]);
  }
  norm = norm > 0.0f ? 1.0f / norm : 1.0f;

  for (int k = 0; k < kAdditivePartials; ++k) {
    Partial& partial = partials_[static_cast<size_t>(k)];
    const float freq = f0 * kDrawbarRatios[static_cast<size_t>(k)];
    // Tonewheels above the generator range simply do not exist.
    if (freq >= 0.45f * static_cast<float>(sr)) {
      partial = Partial{};
      continue;
    }
    partial.base_inc = static_cast<float>(static_cast<double>(freq) / sr);
    partial.gain = drawbar_gain(params.drawbars[static_cast<size_t>(k)]) * norm;
    // Seeded start phase: free-running tonewheels are never phase-locked.
    partial.phase = noise_.unipolar_at(static_cast<uint64_t>(k));
  }

  // Key click: contact transient scaled a little by velocity.
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  click_level_ = std::clamp(params.key_click, 0.0f, 1.0f) * (0.5f + 0.5f * vel01) * 0.6f;
  const float decay_ms = std::max(0.5f, params.click_decay_ms);
  click_coeff_ = std::exp(-1.0f / (decay_ms * 0.001f * static_cast<float>(sr)));
  click_index_ = 0;
}

float AdditiveVoiceCore::render(float pitch_ratio) noexcept {
  float mix = 0.0f;
  for (Partial& partial : partials_) {
    if (partial.gain <= 0.0f) continue;
    mix += std::sin(kTwoPi * static_cast<float>(partial.phase)) * partial.gain;
    partial.phase += static_cast<double>(partial.base_inc * pitch_ratio);
    if (partial.phase >= 1.0) partial.phase -= std::floor(partial.phase);
  }
  if (click_level_ > 1.0e-5f) {
    mix += click_level_ * noise_.bipolar_at((1ull << 16) + click_index_++);
    click_level_ *= click_coeff_;
  }
  return mix;
}

void AdditiveVoiceCore::kill() noexcept {
  for (Partial& partial : partials_) partial.gain = 0.0f;
  click_level_ = 0.0f;
}

}  // namespace sonare::midi::synth
