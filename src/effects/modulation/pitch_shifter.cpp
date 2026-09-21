#include "effects/modulation/pitch_shifter.h"

#include <algorithm>
#include <cmath>

#include "effects/modulation/mod_delay_line.h"
#include "rt/scoped_no_denormals.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

namespace {

// The grain is derived from the window, never the other way round: two taps one
// window apart splice once each per grain, so a grain is two windows.
constexpr double kWindowsPerGrain = 2.0;

// Window bounds. The floor keeps the window positive (prepare()'s own 64-sample
// floor takes over below it); the ceiling keeps the grain allocation bounded,
// well clear of the longest window a caller selects.
constexpr float kMinWindowMs = 0.5f;
constexpr float kMaxWindowMs = 1000.0f;

}  // namespace

PitchShifter::PitchShifter(PitchShifterConfig config) : config_(config) {
  config_.semitones = std::clamp(config_.semitones, -24.0f, 24.0f);
  // Rejected before the clamp below: std::clamp leaves NaN intact and the
  // window sizes an allocation (see delay_param_acceptable).
  if (!delay_param_acceptable(config_.window_ms)) {
    config_.window_ms = kDefaultWindowMs;
  }
  config_.window_ms = std::clamp(config_.window_ms, kMinWindowMs, kMaxWindowMs);
}

void PitchShifter::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const double grain_ms = kWindowsPerGrain * static_cast<double>(config_.window_ms);
  grain_ = std::max(64, static_cast<int>(sample_rate_ * grain_ms * 0.001));
  const size_t len = static_cast<size_t>(grain_ + 4);
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
  // Same fractional-read hazard as ModDelayLine::process: a non-finite delay
  // cannot produce an in-range index, so the tap contributes nothing.
  if (!delay_param_acceptable(delay)) return 0.0f;
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
  const float grain = static_cast<float>(grain_);
  const float half = 0.5f * grain;
  // The read position drifts relative to the write head at (1 - ratio) samples
  // per sample; wrapping it in [0, grain) is what repitches the grain.
  const float step = 1.0f - ratio;
  // Stereo-pair processor: grain buffers exist for two planes only, so planes
  // beyond the pair pass through dry (see the registry's stereoPairOnly
  // classification).
  const int active = std::min(num_channels, 2);
  if (ratio == 1.0f) {
    // At unity ratio the phase never advances, so both taps sit at a fixed
    // offset and the "no shift" default would delay the signal by one window
    // while reporting zero latency. Pass the input through instead, and keep
    // filling the grain buffers so a later shift starts from real history
    // rather than silence.
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
    while (phase >= grain) phase -= grain;
    while (phase < 0.0f) phase += grain;
    const float phase2 = phase >= half ? phase - half : phase + half;
    const float norm = phase / grain;
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
  // Reject before the clamp below: std::clamp leaves NaN intact and `semitones`
  // drives the read-tap phase (see delay_param_acceptable).
  if (!delay_param_acceptable(value)) return false;
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
