#include "effects/modulation/pitch_shifter.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

namespace {

constexpr float kWindowMs = 45.0f;

}  // namespace

PitchShifter::PitchShifter(PitchShifterConfig config) : config_(config) {
  config_.semitones = std::clamp(config_.semitones, -24.0f, 24.0f);
}

void PitchShifter::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  window_ = std::max(64, static_cast<int>(sample_rate_ * static_cast<double>(kWindowMs) * 0.001));
  const size_t len = static_cast<size_t>(window_ + 4);
  for (auto& buffer : buffers_) {
    buffer.assign(len, 0.0f);
  }
  reset();
}

void PitchShifter::reset() {
  phase_ = 0.0f;
  write_pos_ = {0, 0};
  for (auto& buffer : buffers_) {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
  }
}

float PitchShifter::read_tap(int channel, float delay) const noexcept {
  const auto& buffer = buffers_[static_cast<size_t>(channel)];
  const float size = static_cast<float>(buffer.size());
  float read_pos = static_cast<float>(write_pos_[static_cast<size_t>(channel)]) - delay;
  while (read_pos < 0.0f) read_pos += size;
  const int i0 = static_cast<int>(std::floor(read_pos)) % static_cast<int>(buffer.size());
  const int i1 = (i0 + 1) % static_cast<int>(buffer.size());
  const float frac = read_pos - std::floor(read_pos);
  return buffer[static_cast<size_t>(i0)] * (1.0f - frac) + buffer[static_cast<size_t>(i1)] * frac;
}

void PitchShifter::process(float* const* channels, int num_channels, int num_samples) {
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  rt::ScopedNoDenormals no_denormals;
  const float wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  const float ratio = std::exp2(config_.semitones / 12.0f);
  const float window = static_cast<float>(window_);
  const float half = 0.5f * window;
  // The read position drifts relative to the write head at (1 - ratio) samples
  // per sample; wrapping it in [0, window) is what repitches the grain.
  const float step = 1.0f - ratio;
  // Stereo-pair processor: grain buffers exist for two planes only, so planes
  // beyond the pair pass through dry (see the registry's stereoPairOnly
  // classification).
  const int active = std::min(num_channels, 2);
  if (ratio == 1.0f) {
    // At unity ratio the phase never advances, so both taps sit at a fixed
    // offset and the "no shift" default would delay the signal by half a grain
    // window (22.5 ms) while reporting zero latency. Pass the input through
    // instead, and keep filling the grain buffers so a later shift starts from
    // real history rather than silence.
    for (int i = 0; i < num_samples; ++i) {
      for (int ch = 0; ch < active; ++ch) {
        if (channels[ch] == nullptr) continue;
        auto& buffer = buffers_[static_cast<size_t>(ch)];
        auto& write_pos = write_pos_[static_cast<size_t>(ch)];
        buffer[static_cast<size_t>(write_pos)] = channels[ch][i];
        write_pos = (write_pos + 1) % static_cast<int>(buffer.size());
      }
    }
    return;
  }
  for (int i = 0; i < num_samples; ++i) {
    // Advance the shared grain phase and derive the two tap positions/gains.
    float phase = phase_ + step;
    while (phase >= window) phase -= window;
    while (phase < 0.0f) phase += window;
    const float phase2 = phase >= half ? phase - half : phase + half;
    const float norm = phase / window;
    const float g1 = std::sin(::sonare::constants::kPi * norm);
    const float g2 =
        std::sin(::sonare::constants::kPi * (norm >= 0.5f ? norm - 0.5f : norm + 0.5f));
    phase_ = phase;
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const float in = channels[ch][i];
      buffers_[static_cast<size_t>(ch)][static_cast<size_t>(write_pos_[static_cast<size_t>(ch)])] =
          in;
      const float shifted = g1 * read_tap(ch, phase) + g2 * read_tap(ch, phase2);
      channels[ch][i] = dry * in + wet * shifted;
      write_pos_[static_cast<size_t>(ch)] =
          (write_pos_[static_cast<size_t>(ch)] + 1) %
          static_cast<int>(buffers_[static_cast<size_t>(ch)].size());
    }
  }
}

bool PitchShifter::set_parameter(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      config_.semitones = std::clamp(value, -24.0f, 24.0f);
      return true;
    case 1:
      config_.dry_wet = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> PitchShifter::parameter_descriptors() const {
  return {{"semitones", 0}, {"dryWet", 1}};
}

}  // namespace sonare::effects::modulation
