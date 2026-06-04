#include "midi/synth/sf2_voice.h"

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

namespace {

// SF2 2.04 §8.1.3 generator defaults (raw units). Unlisted operators default
// to zero.
struct GenDefault {
  uint16_t oper;
  int32_t value;
};
constexpr GenDefault kGenDefaults[] = {
    {kGenInitialFilterFc, 13500}, {kGenDelayModLfo, -12000},
    {kGenDelayVibLfo, -12000},    {kGenDelayModEnv, -12000},
    {kGenAttackModEnv, -12000},   {kGenHoldModEnv, -12000},
    {kGenDecayModEnv, -12000},    {kGenReleaseModEnv, -12000},
    {kGenDelayVolEnv, -12000},    {kGenAttackVolEnv, -12000},
    {kGenHoldVolEnv, -12000},     {kGenDecayVolEnv, -12000},
    {kGenReleaseVolEnv, -12000},  {kGenScaleTuning, 100},
    {kGenOverridingRootKey, -1},  {kGenKeynum, -1},
    {kGenVelocity, -1},
};

/// Preset-level generators that are NOT additive (index / range / sample
/// addressing operators; SF2 2.04 §9.4).
bool preset_additive(uint16_t oper) noexcept {
  switch (oper) {
    case kGenStartAddrsOffset:
    case kGenEndAddrsOffset:
    case kGenStartloopAddrsOffset:
    case kGenEndloopAddrsOffset:
    case kGenStartAddrsCoarseOffset:
    case kGenEndAddrsCoarseOffset:
    case kGenStartloopAddrsCoarseOffset:
    case kGenEndloopAddrsCoarseOffset:
    case kGenInstrument:
    case kGenKeyRange:
    case kGenVelRange:
    case kGenKeynum:
    case kGenVelocity:
    case kGenSampleId:
    case kGenSampleModes:
    case kGenExclusiveClass:
    case kGenOverridingRootKey:
      return false;
    default:
      return true;
  }
}

}  // namespace

Sf2GenSet::Sf2GenSet() noexcept {
  for (int i = 0; i < kNumGens; ++i) values_[i] = 0;
  for (const GenDefault& d : kGenDefaults) values_[d.oper] = d.value;
}

void Sf2GenSet::apply_absolute(const Sf2Zone& zone) noexcept {
  for (const Sf2Gen& g : zone.gens) {
    if (g.oper < kNumGens) values_[g.oper] = g.amount;
  }
}

void Sf2GenSet::add_relative(const Sf2Zone& zone) noexcept {
  for (const Sf2Gen& g : zone.gens) {
    if (g.oper < kNumGens && preset_additive(g.oper)) values_[g.oper] += g.amount;
  }
}

float timecents_to_seconds(int32_t timecents) noexcept {
  // Spec floor: -12000 tc is 1 ms; treat anything at/below as instantaneous-ish.
  if (timecents <= -12000) return 0.001f;
  return std::exp2(static_cast<float>(timecents) / 1200.0f);
}

float centibels_to_gain(float centibels) noexcept {
  if (centibels <= 0.0f) return 1.0f;
  if (centibels >= 1440.0f) return 0.0f;
  return std::pow(10.0f, -centibels / 200.0f);
}

float abs_cents_to_hz(int32_t cents) noexcept {
  return 8.176f * std::exp2(static_cast<float>(cents) / 1200.0f);
}

Sf2VoiceParams resolve_voice_params(const Sf2GenSet& gens, const Sf2Sample& sample, uint8_t key,
                                    double output_sample_rate) noexcept {
  Sf2VoiceParams p;

  // --- sample addressing (header + fine/coarse offsets) ---
  const int64_t pool_start = static_cast<int64_t>(sample.start) + gens.get(kGenStartAddrsOffset) +
                             32768ll * gens.get(kGenStartAddrsCoarseOffset);
  const int64_t pool_end = static_cast<int64_t>(sample.end) + gens.get(kGenEndAddrsOffset) +
                           32768ll * gens.get(kGenEndAddrsCoarseOffset);
  const int64_t pool_loop_start = static_cast<int64_t>(sample.loop_start) +
                                  gens.get(kGenStartloopAddrsOffset) +
                                  32768ll * gens.get(kGenStartloopAddrsCoarseOffset);
  const int64_t pool_loop_end = static_cast<int64_t>(sample.loop_end) +
                                gens.get(kGenEndloopAddrsOffset) +
                                32768ll * gens.get(kGenEndloopAddrsCoarseOffset);

  const int64_t lo = static_cast<int64_t>(sample.start);
  const int64_t hi = static_cast<int64_t>(sample.end);
  p.start = static_cast<uint32_t>(std::clamp(pool_start, lo, hi));
  p.end = static_cast<uint32_t>(std::clamp(pool_end, static_cast<int64_t>(p.start), hi));
  p.loop_start = static_cast<uint32_t>(
      std::clamp(pool_loop_start, static_cast<int64_t>(p.start), static_cast<int64_t>(p.end)));
  p.loop_end = static_cast<uint32_t>(
      std::clamp(pool_loop_end, static_cast<int64_t>(p.loop_start), static_cast<int64_t>(p.end)));

  const int32_t mode = gens.get(kGenSampleModes) & 0x3;
  p.loop_mode = (mode == 1 || mode == 3) && p.loop_end > p.loop_start ? mode : 0;

  // --- pitch ---
  const int32_t root_override = gens.get(kGenOverridingRootKey);
  const int root = root_override >= 0 ? root_override : sample.original_pitch;
  const int32_t keynum_override = gens.get(kGenKeynum);
  const int effective_key = keynum_override >= 0 ? keynum_override : key;
  const double cents = static_cast<double>(effective_key - root) * gens.get(kGenScaleTuning) +
                       100.0 * gens.get(kGenCoarseTune) + gens.get(kGenFineTune) +
                       sample.correction;
  const double sr_ratio =
      output_sample_rate > 0.0 ? static_cast<double>(sample.sample_rate) / output_sample_rate : 1.0;
  p.pitch_increment = sr_ratio * std::exp2(cents / 1200.0);

  // --- attenuation / pan ---
  p.attenuation_gain = centibels_to_gain(static_cast<float>(gens.get(kGenInitialAttenuation)));
  const float pan = std::clamp(static_cast<float>(gens.get(kGenPan)), -500.0f, 500.0f);
  const float angle = (pan + 500.0f) / 1000.0f * 1.57079632679f;  // 0..pi/2
  p.gain_left = std::cos(angle);
  p.gain_right = std::sin(angle);

  // --- volume envelope (timecents -> ms, sustain cB -> level) ---
  p.volume_env.delay_ms = 1000.0f * timecents_to_seconds(gens.get(kGenDelayVolEnv));
  if (gens.get(kGenDelayVolEnv) <= -12000) p.volume_env.delay_ms = 0.0f;
  p.volume_env.attack_ms = 1000.0f * timecents_to_seconds(gens.get(kGenAttackVolEnv));
  p.volume_env.hold_ms = 1000.0f * timecents_to_seconds(gens.get(kGenHoldVolEnv));
  if (gens.get(kGenHoldVolEnv) <= -12000) p.volume_env.hold_ms = 0.0f;
  p.volume_env.decay_ms = 1000.0f * timecents_to_seconds(gens.get(kGenDecayVolEnv));
  p.volume_env.sustain = centibels_to_gain(static_cast<float>(gens.get(kGenSustainVolEnv)));
  // MVP floor: an SF2 with no release (default -12000 tc = 1 ms) still gets a
  // short click-free release.
  p.volume_env.release_ms =
      std::max(5.0f, 1000.0f * timecents_to_seconds(gens.get(kGenReleaseVolEnv)));

  p.exclusive_class = gens.get(kGenExclusiveClass);
  return p;
}

void Sf2Voice::start(const float* pool_data, const Sf2VoiceParams& p, double sample_rate,
                     float velocity_gain_in) noexcept {
  data = pool_data;
  params = p;
  pos = static_cast<double>(p.start);
  velocity_gain = velocity_gain_in;
  key_down = true;
  releasing = false;
  env.configure(sample_rate, p.volume_env);
  env.kill();
  env.note_on();
}

void Sf2Voice::release() noexcept {
  key_down = false;
  releasing = true;
  env.note_off();
}

float Sf2Voice::render() noexcept {
  if (!active || data == nullptr) return 0.0f;

  const bool looping = params.loop_mode == 1 || (params.loop_mode == 3 && key_down);

  // Wrap into the loop (also catches increments larger than the loop).
  if (looping) {
    const double loop_len =
        static_cast<double>(params.loop_end) - static_cast<double>(params.loop_start);
    while (pos >= static_cast<double>(params.loop_end) && loop_len > 0.0) pos -= loop_len;
  } else if (pos >= static_cast<double>(params.end)) {
    active = false;
    env.kill();
    return 0.0f;
  }

  // Linear interpolation; the second tap wraps across the loop seam.
  const uint32_t i0 = static_cast<uint32_t>(pos);
  const float mu = static_cast<float>(pos - static_cast<double>(i0));
  uint32_t i1 = i0 + 1;
  if (looping && i1 >= params.loop_end) i1 = params.loop_start;
  if (i1 >= params.end) i1 = params.end > 0 ? params.end - 1 : 0;
  const float y0 = data[i0];
  const float y1 = data[i1];
  const float sample = y0 + mu * (y1 - y0);

  pos += params.pitch_increment;

  const float level = env.next();
  if (!env.active()) {
    active = false;
    return 0.0f;
  }
  return sample * level * params.attenuation_gain * velocity_gain;
}

}  // namespace sonare::midi::synth
