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

/// Default modulator: velocity -> initialFilterFc darkening (soft notes lose
/// brightness). Linearised form of the spec's -2400 cent modulator.
constexpr float kVelToFilterCents = -2400.0f;

/// Converts a DAHDSR generator block (timecents / sustain unit) to ms config.
DahdsrConfig envelope_from_gens(const Sf2GenSet& gens, uint16_t delay_op, uint16_t attack_op,
                                uint16_t hold_op, uint16_t decay_op, uint16_t sustain_op,
                                uint16_t release_op, bool sustain_is_centibels) noexcept {
  DahdsrConfig cfg;
  cfg.delay_ms =
      gens.get(delay_op) <= -12000 ? 0.0f : 1000.0f * timecents_to_seconds(gens.get(delay_op));
  cfg.attack_ms = 1000.0f * timecents_to_seconds(gens.get(attack_op));
  cfg.hold_ms =
      gens.get(hold_op) <= -12000 ? 0.0f : 1000.0f * timecents_to_seconds(gens.get(hold_op));
  cfg.decay_ms = 1000.0f * timecents_to_seconds(gens.get(decay_op));
  if (sustain_is_centibels) {
    cfg.sustain = centibels_to_gain(static_cast<float>(gens.get(sustain_op)));
  } else {
    // Modulation envelope sustain: 0.1% DECREASE units (0 = full, 1000 = none).
    cfg.sustain = std::clamp(1.0f - static_cast<float>(gens.get(sustain_op)) / 1000.0f, 0.0f, 1.0f);
  }
  cfg.release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(gens.get(release_op)));
  return cfg;
}

float pan_angle(float pan_units) noexcept {
  const float pan = std::clamp(pan_units, -500.0f, 500.0f);
  return (pan + 500.0f) / 1000.0f * 1.57079632679f;  // 0..pi/2
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

float abs_cents_to_hz(float cents) noexcept { return 8.176f * std::exp2(cents / 1200.0f); }

float sf2_velocity_gain(uint8_t velocity) noexcept {
  // The spec's concave 960 cB velocity->attenuation modulator reduces to
  // exactly (vel/127)^2 in linear gain.
  const float v = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  return v * v;
}

float sf2_cc_gain(uint8_t value) noexcept {
  const float v = static_cast<float>(value & 0x7Fu) / 127.0f;
  return v * v;
}

Sf2VoiceParams resolve_voice_params(const Sf2GenSet& gens, const Sf2Sample& sample, uint8_t key,
                                    uint8_t velocity, double output_sample_rate) noexcept {
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
  p.pan_units = std::clamp(static_cast<float>(gens.get(kGenPan)), -500.0f, 500.0f);

  // --- volume / modulation envelopes ---
  p.volume_env = envelope_from_gens(gens, kGenDelayVolEnv, kGenAttackVolEnv, kGenHoldVolEnv,
                                    kGenDecayVolEnv, kGenSustainVolEnv, kGenReleaseVolEnv,
                                    /*sustain_is_centibels=*/true);
  p.mod_env = envelope_from_gens(gens, kGenDelayModEnv, kGenAttackModEnv, kGenHoldModEnv,
                                 kGenDecayModEnv, kGenSustainModEnv, kGenReleaseModEnv,
                                 /*sustain_is_centibels=*/false);
  p.mod_env_to_pitch = static_cast<float>(gens.get(kGenModEnvToPitch));
  p.mod_env_to_filter_fc = static_cast<float>(gens.get(kGenModEnvToFilterFc));

  // --- filter: velocity darkening (default modulator) on top of the zone Fc ---
  const float vel_offset =
      kVelToFilterCents * (1.0f - static_cast<float>(velocity & 0x7Fu) / 127.0f);
  p.filter_fc_cents = static_cast<float>(gens.get(kGenInitialFilterFc)) + vel_offset;
  // initialFilterQ is the resonance peak height in centibels: Q = 10^(cB/200).
  const float q_cb = static_cast<float>(gens.get(kGenInitialFilterQ));
  p.filter_q = std::max(0.707f, std::pow(10.0f, q_cb / 200.0f));

  // --- LFOs ---
  p.mod_lfo_delay_s =
      gens.get(kGenDelayModLfo) <= -12000 ? 0.0f : timecents_to_seconds(gens.get(kGenDelayModLfo));
  p.mod_lfo_freq_hz = abs_cents_to_hz(static_cast<float>(gens.get(kGenFreqModLfo)));
  p.mod_lfo_to_pitch = static_cast<float>(gens.get(kGenModLfoToPitch));
  p.mod_lfo_to_filter_fc = static_cast<float>(gens.get(kGenModLfoToFilterFc));
  p.mod_lfo_to_volume_cb = static_cast<float>(gens.get(kGenModLfoToVolume));
  p.vib_lfo_delay_s =
      gens.get(kGenDelayVibLfo) <= -12000 ? 0.0f : timecents_to_seconds(gens.get(kGenDelayVibLfo));
  p.vib_lfo_freq_hz = abs_cents_to_hz(static_cast<float>(gens.get(kGenFreqVibLfo)));
  p.vib_lfo_to_pitch = static_cast<float>(gens.get(kGenVibLfoToPitch));

  // The filter section is bypassed only when fully open and unmodulated.
  p.filter_bypass = p.filter_fc_cents >= 13499.0f && q_cb <= 0.0f &&
                    p.mod_env_to_filter_fc == 0.0f && p.mod_lfo_to_filter_fc == 0.0f;

  // --- effect sends (0.1% units -> [0,1]) ---
  p.reverb_send =
      std::clamp(static_cast<float>(gens.get(kGenReverbEffectsSend)) / 1000.0f, 0.0f, 1.0f);
  p.chorus_send =
      std::clamp(static_cast<float>(gens.get(kGenChorusEffectsSend)) / 1000.0f, 0.0f, 1.0f);

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
  mod_env.configure(sample_rate, p.mod_env);
  mod_env.kill();
  mod_env.note_on();
  mod_lfo.start(sample_rate, p.mod_lfo_delay_s, p.mod_lfo_freq_hz);
  vib_lfo.start(sample_rate, p.vib_lfo_delay_s, p.vib_lfo_freq_hz);
  filter.prepare(sample_rate);
  cached_pan_units = 1.0e9f;  // force pan recompute on first render
}

void Sf2Voice::release() noexcept {
  key_down = false;
  releasing = true;
  env.note_off();
  mod_env.note_off();
}

float Sf2Voice::render(const Sf2ChannelMod& mod) noexcept {
  if (!active || data == nullptr) return 0.0f;

  // Refresh the cached stereo pan gains when zone pan + CC10 changed.
  const float pan_units = params.pan_units + mod.pan_units;
  if (pan_units != cached_pan_units) {
    cached_pan_units = pan_units;
    const float angle = pan_angle(pan_units);
    gain_left = std::cos(angle);
    gain_right = std::sin(angle);
  }

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
  float sample = y0 + mu * (y1 - y0);

  // --- modulation sources ---
  const float mod_env_level = mod_env.next();
  const float mod_lfo_value = mod_lfo.next();
  const float vib_lfo_value = vib_lfo.next();

  // --- pitch: bend + modEnv + LFOs (cents) ---
  const float pitch_cents = mod.pitch_cents + mod_env_level * params.mod_env_to_pitch +
                            mod_lfo_value * params.mod_lfo_to_pitch +
                            vib_lfo_value * (params.vib_lfo_to_pitch + mod.extra_vibrato_cents);
  if (pitch_cents != 0.0f) {
    pos += params.pitch_increment * std::exp2(static_cast<double>(pitch_cents) / 1200.0);
  } else {
    pos += params.pitch_increment;
  }

  // --- filter: Fc = zone Fc + modEnv + modLFO (cents) ---
  if (!params.filter_bypass) {
    const float fc_cents = params.filter_fc_cents + mod_env_level * params.mod_env_to_filter_fc +
                           mod_lfo_value * params.mod_lfo_to_filter_fc;
    filter.set(abs_cents_to_hz(fc_cents), params.filter_q);
    sample = filter.process(sample).lp;
  }

  // --- amplitude ---
  const float level = env.next();
  if (!env.active()) {
    active = false;
    return 0.0f;
  }
  float gain = level * params.attenuation_gain * velocity_gain * mod.gain;
  if (params.mod_lfo_to_volume_cb != 0.0f) {
    gain *= centibels_to_gain(mod_lfo_value * params.mod_lfo_to_volume_cb);
  }
  return sample * gain;
}

}  // namespace sonare::midi::synth
