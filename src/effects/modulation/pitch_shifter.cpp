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

// The pre-delays lengthen the delay line, so they are bounded like the window.
constexpr float kMaxPreDelayMs = 1000.0f;
constexpr float kMaxFeedback = 0.95f;
constexpr float kMaxCents = 100.0f;
constexpr float kCentsPerSemitone = 100.0f;

/// The two crossfade gains and tap positions of one voice's grain.
struct Grain {
  float phase;
  float phase2;
  float g1;
  float g2;
};

/// Advances a voice's grain phase by @p step and derives its two taps and gains.
Grain advance_grain(float& state, float step, float grain, float half) noexcept {
  float phase = state + step;
  while (phase >= grain) phase -= grain;
  while (phase < 0.0f) phase += grain;
  const float phase2 = phase >= half ? phase - half : phase + half;
  const float norm = phase / grain;
  const float g1 = std::sin(::sonare::constants::kPi * norm);
  const float g2 = std::sin(::sonare::constants::kPi * (norm >= 0.5f ? norm - 0.5f : norm + 0.5f));
  state = phase;
  return {phase, phase2, g1, g2};
}

/// Per-side gains of a balance control: unity on the near side, falling on the far one.
std::array<float, 2> balance_gains(float pan) noexcept {
  const float p = std::clamp(pan, -1.0f, 1.0f);
  return {std::min(1.0f, 1.0f - p), std::min(1.0f, 1.0f + p)};
}

float semitone_ratio(float semitones, float cents) noexcept {
  return std::exp2((semitones + cents / kCentsPerSemitone) / 12.0f);
}

float finite_or_zero(float value) noexcept { return delay_param_acceptable(value) ? value : 0.0f; }

}  // namespace

PitchShifter::PitchShifter(PitchShifterConfig config) : config_(config) {
  config_.semitones = std::clamp(config_.semitones, -24.0f, 24.0f);
  config_.cents = std::clamp(finite_or_zero(config_.cents), -kMaxCents, kMaxCents);
  config_.pan = std::clamp(finite_or_zero(config_.pan), -1.0f, 1.0f);
  config_.semitones2 = std::clamp(finite_or_zero(config_.semitones2), -24.0f, 24.0f);
  config_.cents2 = std::clamp(finite_or_zero(config_.cents2), -kMaxCents, kMaxCents);
  config_.level2 = std::clamp(finite_or_zero(config_.level2), 0.0f, 1.0f);
  config_.pan2 = std::clamp(finite_or_zero(config_.pan2), -1.0f, 1.0f);
  config_.pre_delay_ms = std::clamp(finite_or_zero(config_.pre_delay_ms), 0.0f, kMaxPreDelayMs);
  config_.pre_delay2_ms = std::clamp(finite_or_zero(config_.pre_delay2_ms), 0.0f, kMaxPreDelayMs);
  config_.feedback = std::clamp(finite_or_zero(config_.feedback), -kMaxFeedback, kMaxFeedback);
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
  const float pre_ms = std::max(config_.pre_delay_ms, config_.pre_delay2_ms);
  const float sr = static_cast<float>(sample_rate_);
  pre_delay_samples_ = {config_.pre_delay_ms * 0.001f * sr, config_.pre_delay2_ms * 0.001f * sr};
  const int pre_max = static_cast<int>(std::ceil(pre_ms * 0.001f * sr));
  const size_t len = static_cast<size_t>(grain_ + pre_max + 4);
  for (auto& buffer : buffers_) {
    buffer.assign(len, 0.0f);
  }
  reset();
}

void PitchShifter::reset() {
  phase_ = 0.0f;
  phase2_ = 0.0f;
  feedback_state_ = {0.0f, 0.0f};
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
  const float dry_wet = std::clamp(config_.dry_wet, 0.0f, 1.0f);
  const common::MixGains mix = common::mix_gains(config_.mix_law, dry_wet);
  const float dry = mix.dry;
  const float wet = mix.wet;
  const float ratio = semitone_ratio(config_.semitones, config_.cents);
  const float ratio2 = semitone_ratio(config_.semitones2, config_.cents2);
  const float grain = static_cast<float>(grain_);
  const float half = 0.5f * grain;
  // The read position drifts relative to the write head at (1 - ratio) samples
  // per sample; wrapping it in [0, grain) is what repitches the grain.
  const float step = 1.0f - ratio;
  const float step2 = 1.0f - ratio2;
  const bool unity = ratio == 1.0f;
  const bool unity2 = ratio2 == 1.0f;
  const float pre = pre_delay_samples_[0];
  const float pre2 = pre_delay_samples_[1];
  const float level2 = config_.level2;
  const bool second = level2 > 0.0f;
  const float feedback = config_.feedback;
  const std::array<float, 2> pan = balance_gains(config_.pan);
  const std::array<float, 2> pan2 = balance_gains(config_.pan2);
  // Stereo-pair processor: grain buffers exist for two planes only, so planes
  // beyond the pair pass through dry (see the registry's stereoPairOnly
  // classification).
  const int active = std::min(num_channels, 2);
  if (unity && !second && config_.pan == 0.0f && pre == 0.0f && feedback == 0.0f) {
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
  bool non_finite = false;
  for (int i = 0; i < num_samples; ++i) {
    // A voice at unity ratio reads one fixed tap instead of a drifting grain.
    const Grain g =
        unity ? Grain{0.0f, 0.0f, 0.0f, 0.0f} : advance_grain(phase_, step, grain, half);
    Grain g_2{0.0f, 0.0f, 0.0f, 0.0f};
    if (second && !unity2) g_2 = advance_grain(phase2_, step2, grain, half);
    for (int ch = 0; ch < active; ++ch) {
      if (channels[ch] == nullptr) continue;
      const size_t c = static_cast<size_t>(ch);
      const float in = channels[ch][i];
      buffers_[c][static_cast<size_t>(write_pos_[c])] = in;
      const float voice =
          unity ? read_tap(ch, pre)
                : g.g1 * read_tap(ch, g.phase + pre) + g.g2 * read_tap(ch, g.phase2 + pre);
      float shifted = voice * pan[c];
      if (second) {
        const float voice2 = unity2 ? read_tap(ch, pre2)
                                    : g_2.g1 * read_tap(ch, g_2.phase + pre2) +
                                          g_2.g2 * read_tap(ch, g_2.phase2 + pre2);
        shifted += level2 * pan2[c] * voice2;
      }
      if (feedback != 0.0f) {
        // Written after the taps have read, so the loop is one pre-delay long.
        buffers_[c][static_cast<size_t>(write_pos_[c])] += feedback * shifted;
        feedback_state_[c] = shifted;
        non_finite = non_finite || !std::isfinite(shifted);
      }
      channels[ch][i] = dry * in + wet * shifted;
      write_pos_[c] = (write_pos_[c] + 1) % static_cast<int>(buffers_[c].size());
    }
  }
  // A non-finite sample would otherwise circulate through the feedback path forever.
  if (non_finite) reset();
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
    case 2:
      config_.cents = std::clamp(value, -kMaxCents, kMaxCents);
      return true;
    case 3:
      config_.pan = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 4:
      config_.semitones2 = std::clamp(value, -24.0f, 24.0f);
      return true;
    case 5:
      config_.cents2 = std::clamp(value, -kMaxCents, kMaxCents);
      return true;
    case 6:
      config_.level2 = std::clamp(value, 0.0f, 1.0f);
      return true;
    case 7:
      config_.pan2 = std::clamp(value, -1.0f, 1.0f);
      return true;
    case 8:
      config_.feedback = std::clamp(value, -kMaxFeedback, kMaxFeedback);
      return true;
    case 9:
      // An unnamed law is refused rather than rounded onto a neighbour.
      if (value < 0.0f || value != std::floor(value) ||
          value >= static_cast<float>(common::kMixLawCount)) {
        return false;
      }
      config_.mix_law = static_cast<common::MixLaw>(static_cast<int>(value));
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> PitchShifter::parameter_descriptors() const {
  return {{"semitones", 0}, {"dryWet", 1}, {"cents", 2}, {"pan", 3},      {"semitones2", 4},
          {"cents2", 5},    {"level2", 6}, {"pan2", 7},  {"feedback", 8}, {"mixLaw", 9}};
}

}  // namespace sonare::effects::modulation
