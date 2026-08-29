#include "midi/synth/gs_system_effects.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

constexpr float kMax7Bit = 127.0f;

/// The reset value of the three return levels, which is where unity sits.
constexpr float kReturnLevelUnity = 64.0f;

/// Largest coefficient a feedback byte reaches. Matches the clamp the delay
/// bus already applies, so a full-scale byte saturates nowhere downstream.
constexpr float kMaxDelayFeedback = 0.9f;
constexpr float kMaxUnsignedFeedback = 0.95f;

/// REVERB TIME. The manual gives 0-127 with no time scale; a squared law over
/// 0.2-12 s puts the power-on Hall 2 (64) at 3.2 s and keeps the short end
/// usable, which an exponential between the same endpoints cannot.
constexpr float kReverbTimeMinSeconds = 0.2f;
constexpr float kReverbTimeSpanSeconds = 11.8f;

/// CHORUS RATE. No curve given; 0.122 Hz per step spans 0-15.5 Hz and puts the
/// power-on default (3) at a 0.37 Hz chorus.
constexpr float kChorusRateHzPerStep = 0.122f;

/// CHORUS DEPTH and CHORUS DELAY share Roland's 1/3.2 ms step, spanning
/// 0.31-40 ms. The power-on depth (19) is 6.25 ms.
constexpr float kChorusMsSteps = 3.2f;

/// PRE-LPF 0-7. Entry 0 is THRU; the rest halve or nearly halve each step,
/// which is the shape the manual's cutoff list has.
constexpr std::array<float, 8> kPreLpfCutoffHz{
    {kGsPreLpfThruHz, 8000.0f, 6400.0f, 3200.0f, 1600.0f, 800.0f, 400.0f, 200.0f}};

template <typename T, size_t N>
uint8_t table_index(const std::array<T, N>&, uint8_t index) noexcept {
  return std::min<uint8_t>(index, static_cast<uint8_t>(N - 1));
}

template <typename T, size_t N>
const T& table_entry(const std::array<T, N>& table, uint8_t index) noexcept {
  return table[table_index(table, index)];
}

}  // namespace

float gs_delay_time_ms(uint8_t value) noexcept {
  const auto& table = kGsDelayTimeBreakpoints;
  const uint8_t clamped = std::clamp(value, table.front().value, table.back().value);
  for (size_t i = 1; i < table.size(); ++i) {
    if (clamped > table[i].value) continue;
    const GsDelayTimeBreakpoint& lo = table[i - 1];
    const GsDelayTimeBreakpoint& hi = table[i];
    const double steps = static_cast<double>(clamped) - lo.value;
    const double span = static_cast<double>(hi.value) - lo.value;
    const double step_ms = (static_cast<double>(hi.ms) - lo.ms) / span;
    return static_cast<float>(lo.ms + steps * step_ms);
  }
  return table.back().ms;
}

float gs_delay_time_ratio_percent(uint8_t value) noexcept {
  // 100 % lands on 24 and 500 % on 0x78; the manual's "4 %" at 01 is that same
  // 100/24 step rounded for display.
  const uint8_t clamped = std::clamp<uint8_t>(value, 0x01, 0x78);
  return static_cast<float>(clamped * 100.0 / 24.0);
}

int gs_delay_feedback_signed(uint8_t value) noexcept {
  return static_cast<int>(value & 0x7Fu) - 64;
}

float gs_delay_feedback_coefficient(uint8_t value) noexcept {
  return static_cast<float>(gs_delay_feedback_signed(value)) / 64.0f * kMaxDelayFeedback;
}

float gs_reverb_time_seconds(uint8_t value) noexcept {
  const float t = static_cast<float>(value & 0x7Fu) / kMax7Bit;
  return kReverbTimeMinSeconds + kReverbTimeSpanSeconds * t * t;
}

float gs_reverb_predelay_ms(uint8_t value) noexcept { return static_cast<float>(value & 0x7Fu); }

float gs_reverb_delay_feedback_coefficient(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) / kMax7Bit * kMaxUnsignedFeedback;
}

float gs_chorus_rate_hz(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) * kChorusRateHzPerStep;
}

float gs_chorus_depth_ms(uint8_t value) noexcept {
  return static_cast<float>((value & 0x7Fu) + 1) / kChorusMsSteps;
}

float gs_chorus_delay_ms(uint8_t value) noexcept {
  return static_cast<float>((value & 0x7Fu) + 1) / kChorusMsSteps;
}

float gs_chorus_feedback_coefficient(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) / kMax7Bit * kMaxUnsignedFeedback;
}

float gs_effect_level(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) / kMax7Bit;
}

float gs_return_level(uint8_t value) noexcept {
  return static_cast<float>(value & 0x7Fu) / kReturnLevelUnity;
}

float gs_pre_lpf_cutoff_hz(uint8_t value) noexcept { return table_entry(kPreLpfCutoffHz, value); }

GsReverbMacroParams gs_reverb_macro_params(uint8_t macro) noexcept {
  return table_entry(kGsReverbMacros, macro);
}

GsChorusMacroParams gs_chorus_macro_params(uint8_t macro) noexcept {
  return table_entry(kGsChorusMacros, macro);
}

GsDelayMacroParams gs_delay_macro_params(uint8_t macro) noexcept {
  return table_entry(kGsDelayMacros, macro);
}

void gs_apply_reverb_macro(GsSystemEffects& fx, uint8_t macro) noexcept {
  const GsReverbMacroParams p = gs_reverb_macro_params(macro);
  fx.reverb_macro = table_index(kGsReverbMacros, macro);
  fx.reverb_character = p.character;
  fx.reverb_pre_lpf = p.pre_lpf;
  fx.reverb_level = p.level;
  fx.reverb_time = p.time;
  fx.reverb_delay_feedback = p.delay_feedback;
  fx.reverb_predelay = p.predelay;
}

void gs_apply_chorus_macro(GsSystemEffects& fx, uint8_t macro) noexcept {
  const GsChorusMacroParams p = gs_chorus_macro_params(macro);
  fx.chorus_macro = table_index(kGsChorusMacros, macro);
  fx.chorus_pre_lpf = p.pre_lpf;
  fx.chorus_level = p.level;
  fx.chorus_feedback = p.feedback;
  fx.chorus_delay = p.delay;
  fx.chorus_rate = p.rate;
  fx.chorus_depth = p.depth;
  fx.chorus_send_to_reverb = p.send_to_reverb;
  fx.chorus_send_to_delay = p.send_to_delay;
}

void gs_apply_delay_macro(GsSystemEffects& fx, uint8_t macro) noexcept {
  const GsDelayMacroParams p = gs_delay_macro_params(macro);
  fx.delay_macro = table_index(kGsDelayMacros, macro);
  fx.delay_pre_lpf = p.pre_lpf;
  fx.delay_time_center = p.time_center;
  fx.delay_time_ratio_left = p.time_ratio_left;
  fx.delay_time_ratio_right = p.time_ratio_right;
  fx.delay_level_center = p.level_center;
  fx.delay_level_left = p.level_left;
  fx.delay_level_right = p.level_right;
  fx.delay_level = p.level;
  fx.delay_feedback = p.feedback;
  fx.delay_send_to_reverb = p.send_to_reverb;
}

bool gs_reverb_character_is_delay(uint8_t character) noexcept {
  return table_entry(kGsReverbCharacters, character).delay_type;
}

float gs_reverb_character_damping(uint8_t character) noexcept {
  return table_entry(kGsReverbCharacters, character).damping;
}

}  // namespace sonare::midi::synth
