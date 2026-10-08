#include "effects/modulation/pitch_shifter.h"

#include <algorithm>
#include <cmath>

#include "effects/common/control_ranges.h"
#include "rt/scoped_no_denormals.h"
#include "rt/tail_budget.h"
#include "util/constants.h"

namespace sonare::effects::modulation {

using common::kMaxFeedback;

namespace {

// The grain is derived from the window, never the other way round: two taps one
// window apart splice once each per grain, so a grain is two windows.
constexpr double kWindowsPerGrain = 2.0;

// Window bounds. The floor keeps the window positive (prepare()'s own 64-sample
// floor takes over below it); the ceiling bounds a configured window.
constexpr float kMinWindowMs = 0.5f;
constexpr float kMaxWindowMs = 1000.0f;
constexpr float kMaxPreDelayMs = 1000.0f;

// Live ceilings: the largest values the GS EFX bindings can request (the splice
// window table in gs_efx_tables.h tops at 128 ms, the pre-delay ladder at the
// shared pre-delay ceiling). prepare() sizes the delay line for max(live
// ceiling, configured value).
constexpr float kLiveMaxWindowMs = 128.0f;
constexpr float kLiveMaxPreDelayMs = common::kMaxModulationPreDelayMs;
constexpr float kMaxCents = 100.0f;
constexpr float kCentsPerSemitone = 100.0f;
// The corner stays below this fraction of the rate so a ratio just above 1
// keeps the section's design away from Nyquist.
constexpr double kMaxAntiAliasCornerFraction = 0.45;

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

/// True for exactly 0 or 1, the two values a switch accepts.
bool is_switch_value(float value) noexcept { return value == 0.0f || value == 1.0f; }

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
  max_window_ms_ = std::max(kLiveMaxWindowMs, config_.window_ms);
  max_pre_delay_ms_ = std::max({kLiveMaxPreDelayMs, config_.pre_delay_ms, config_.pre_delay2_ms});
}

void PitchShifter::update_grain() noexcept {
  const double grain_ms = kWindowsPerGrain * static_cast<double>(config_.window_ms);
  grain_ = std::max(64, static_cast<int>(sample_rate_ * grain_ms * 0.001));
  if (grain_ > 0) {
    phase_ = std::fmod(phase_, static_cast<float>(grain_));
    phase2_ = std::fmod(phase2_, static_cast<float>(grain_));
    if (phase_ < 0.0f) phase_ += static_cast<float>(grain_);
    if (phase2_ < 0.0f) phase2_ += static_cast<float>(grain_);
  }
}

void PitchShifter::update_pre_delay_samples() noexcept {
  const float sr = static_cast<float>(sample_rate_);
  pre_delay_samples_ = {config_.pre_delay_ms * 0.001f * sr, config_.pre_delay2_ms * 0.001f * sr};
}

void PitchShifter::prepare(double sample_rate, int) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  anti_alias_corner_hz_ = 0.0f;
  const int max_grain =
      std::max(64, static_cast<int>(sample_rate_ * kWindowsPerGrain * max_window_ms_ * 0.001));
  const int max_pre = static_cast<int>(std::ceil(max_pre_delay_ms_ * 0.001 * sample_rate_));
  const size_t len = static_cast<size_t>(max_grain + max_pre + 4);
  for (auto& buffer : buffers_) {
    buffer.assign(len, 0.0f);
  }
  update_grain();
  update_pre_delay_samples();
  reset();
}

void PitchShifter::reset() {
  phase_ = 0.0f;
  phase2_ = 0.0f;
  feedback_state_ = {0.0f, 0.0f};
  write_pos_ = {0, 0};
  for (auto& channel : anti_alias_) {
    for (auto& section : channel) section.reset();
  }
  for (auto& buffer : buffers_) {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
  }
}

int PitchShifter::tail_samples() const noexcept {
  const common::MixGains mix =
      common::mix_gains(config_.mix_law, std::clamp(config_.dry_wet, 0.0f, 1.0f));
  if (!(mix.wet > 0.0f)) return 0;
  // A grain reads back at most one grain plus the longer pre-delay; feedback writes the
  // shifted sum into the same line, so each pass costs that read again.
  const double longest = static_cast<double>(grain_) +
                         std::max(pre_delay_samples_[0], pre_delay_samples_[1]) +
                         kDelayReadStencilSamples;
  rt::TailBudget tail;
  if (anti_alias_corner_hz_ > 0.0f) {
    for (const auto& section : anti_alias_[0]) {
      tail.biquad(std::fabs(section.c.b0) + std::fabs(section.c.b1) + std::fabs(section.c.b2),
                  section.c.a1, section.c.a2);
    }
  }
  tail.delay(longest).recirculation(longest, config_.feedback);
  return tail.samples();
}

float PitchShifter::read_tap(int channel, float delay) const noexcept {
  // Same fractional-read hazard as ModDelayLine::process: a non-finite delay
  // cannot produce an in-range index, so the tap contributes nothing.
  if (!delay_param_acceptable(delay)) return 0.0f;
  const auto& buffer = buffers_[static_cast<size_t>(channel)];
  if (buffer.empty()) return 0.0f;
  if (config_.interpolation == DelayInterpolation::kLagrange3) {
    return read_tap_lagrange3(channel, delay);
  }
  const float size = static_cast<float>(buffer.size());
  float read_pos = static_cast<float>(write_pos_[static_cast<size_t>(channel)]) - delay;
  while (read_pos < 0.0f) read_pos += size;
  const int i0 = static_cast<int>(std::floor(read_pos)) % static_cast<int>(buffer.size());
  const int i1 = (i0 + 1) % static_cast<int>(buffer.size());
  const float frac = read_pos - std::floor(read_pos);
  return buffer[static_cast<size_t>(i0)] * (1.0f - frac) + buffer[static_cast<size_t>(i1)] * frac;
}

float PitchShifter::read_tap_lagrange3(int channel, float delay) const noexcept {
  const auto& buffer = buffers_[static_cast<size_t>(channel)];
  const int size = static_cast<int>(buffer.size());
  if (size < 4) return 0.0f;
  // Nodes sit at delays D-1, D, D+1, D+2; the buffer holds grain + pre-delay + 4
  // samples, so D + 2 stays inside it.
  const float clamped = std::clamp(delay, 1.0f, static_cast<float>(size - 3));
  const int whole = static_cast<int>(clamped);
  const float f = clamped - static_cast<float>(whole);
  const float w0 = -f * (f - 1.0f) * (f - 2.0f) * (1.0f / 6.0f);
  const float w1 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
  const float w2 = -(f + 1.0f) * f * (f - 2.0f) * 0.5f;
  const float w3 = (f + 1.0f) * f * (f - 1.0f) * (1.0f / 6.0f);
  const int head = write_pos_[static_cast<size_t>(channel)];
  auto tap = [&](int back) {
    return buffer[static_cast<size_t>(((head - back) % size + size) % size)];
  };
  return w0 * tap(whole - 1) + w1 * tap(whole) + w2 * tap(whole + 1) + w3 * tap(whole + 2);
}

void PitchShifter::update_anti_alias(float max_ratio) noexcept {
  if (!(max_ratio > 1.0f)) {
    anti_alias_corner_hz_ = 0.0f;
    return;
  }
  const double corner_hz = std::min(sample_rate_ / (2.0 * static_cast<double>(max_ratio)),
                                    kMaxAntiAliasCornerFraction * sample_rate_);
  if (static_cast<float>(corner_hz) == anti_alias_corner_hz_) return;
  anti_alias_corner_hz_ = static_cast<float>(corner_hz);
  const float w0 = static_cast<float>(sonare::constants::kTwoPiD * corner_hz / sample_rate_);
  const rt::BiquadCoeffs coeffs = rt::rbj_lowpass(w0, sonare::constants::kInvSqrt2);
  for (auto& channel : anti_alias_) {
    for (auto& section : channel) section.set(coeffs);
  }
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
  // Under the two-ramp law level2 balances the voices as dry_wet balances the
  // mix; otherwise the first voice stays whole and level2 is the second's gain.
  const common::MixGains voices = config_.mix_law == common::MixLaw::kTwoRamps
                                      ? common::mix_gains(config_.mix_law, config_.level2)
                                      : common::MixGains{1.0f, config_.level2};
  const float level1 = voices.dry;
  const float level2 = voices.wet;
  const bool second = level2 > 0.0f;
  const bool anti_alias = config_.anti_alias;
  if (anti_alias) {
    update_anti_alias(std::max(ratio, second ? ratio2 : 1.0f));
  }
  // The anti-alias sections re-enter the path from rest, never from the history
  // they froze with while the flag was off or the ratio was at or below 1.
  const bool filtering = anti_alias_gate_.admit(anti_alias && anti_alias_corner_hz_ > 0.0f, [this] {
    for (auto& channel : anti_alias_) {
      for (auto& section : channel) section.reset();
    }
  });
  const float feedback = config_.feedback;
  const std::array<float, 2> pan = balance_gains(config_.pan);
  const std::array<float, 2> pan2 = balance_gains(config_.pan2);
  // A shifting voice sums two sin/cos-faded taps, which reads up to sqrt(2) of a
  // correlated line; a unity voice reads one tap. The fed-back share is divided
  // by the channel's worst-case voice sum, so the loop gain stays within
  // |feedback| for every voice configuration.
  const float peak1 = unity ? 1.0f : ::sonare::constants::kSqrt2;
  const float peak2 = unity2 ? 1.0f : ::sonare::constants::kSqrt2;
  std::array<float, 2> feedback_gain{};
  for (size_t c = 0; c < feedback_gain.size(); ++c) {
    const float voice_sum =
        peak1 * std::abs(level1 * pan[c]) + (second ? peak2 * std::abs(level2 * pan2[c]) : 0.0f);
    feedback_gain[c] = feedback / std::max(1.0f, voice_sum);
  }
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
    anti_alias_corner_hz_ = 0.0f;
    anti_alias_gate_.close();
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
      float written = in;
      if (filtering) {
        written = anti_alias_[c][1].process(anti_alias_[c][0].process(in));
        if (!std::isfinite(written)) {
          // The sections are recursive: one non-finite input would stay in them for good.
          for (auto& section : anti_alias_[c]) section.reset();
          written = 0.0f;
        }
      }
      buffers_[c][static_cast<size_t>(write_pos_[c])] = written;
      const float voice =
          unity ? read_tap(ch, pre)
                : g.g1 * read_tap(ch, g.phase + pre) + g.g2 * read_tap(ch, g.phase2 + pre);
      float shifted = level1 * voice * pan[c];
      if (second) {
        const float voice2 = unity2 ? read_tap(ch, pre2)
                                    : g_2.g1 * read_tap(ch, g_2.phase + pre2) +
                                          g_2.g2 * read_tap(ch, g_2.phase2 + pre2);
        shifted += level2 * pan2[c] * voice2;
      }
      if (feedback != 0.0f) {
        // Written after the taps have read, so the loop is one pre-delay long.
        buffers_[c][static_cast<size_t>(write_pos_[c])] += feedback_gain[c] * shifted;
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

bool PitchShifter::set_parameter_impl(unsigned int param_id, float value) {
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
      return common::mix_law_from_value(value, &config_.mix_law);
    case 10:
      if (!delay_interpolation_acceptable(value)) return false;
      config_.interpolation = static_cast<DelayInterpolation>(static_cast<int>(value));
      return true;
    case 11:
      if (!is_switch_value(value)) return false;
      config_.anti_alias = value != 0.0f;
      return true;
    case 12:
      config_.window_ms = std::clamp(value, kMinWindowMs, max_window_ms_);
      update_grain();
      return true;
    case 13:
      config_.pre_delay_ms = std::clamp(value, 0.0f, max_pre_delay_ms_);
      update_pre_delay_samples();
      return true;
    case 14:
      config_.pre_delay2_ms = std::clamp(value, 0.0f, max_pre_delay_ms_);
      update_pre_delay_samples();
      return true;
    default:
      return false;
  }
}

bool PitchShifter::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return param_id <= 14u;
}

std::vector<rt::ParamDescriptor> PitchShifter::parameter_descriptors() const {
  return {{"semitones", 0},  {"dryWet", 1},      {"cents", 2},          {"pan", 3},
          {"semitones2", 4}, {"cents2", 5},      {"level2", 6},         {"pan2", 7},
          {"feedback", 8},   {"mixLaw", 9},      {"interpolation", 10}, {"antiAlias", 11},
          {"windowMs", 12},  {"preDelayMs", 13}, {"preDelay2Ms", 14}};
}

}  // namespace sonare::effects::modulation
