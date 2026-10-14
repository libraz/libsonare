#include "midi/synth/sf2_voice.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

constexpr bool kAdditive = true;
constexpr bool kAbsolute = false;
constexpr Sf2GenRole kVoice = Sf2GenRole::kVoice;
constexpr Sf2GenRole kZoneLink = Sf2GenRole::kZoneLink;
constexpr Sf2GenRole kReserved = Sf2GenRole::kReserved;

/// SF2 2.04 section 8.1.3 generator contract, one row per operator number. Preset-level
/// index / range / sample addressing operators are not additive (section 9.4).
constexpr Sf2GenSpec kGenSpecs[Sf2GenSet::kNumGens] = {
    {kGenStartAddrsOffset, 0, kAbsolute, kVoice},
    {kGenEndAddrsOffset, 0, kAbsolute, kVoice},
    {kGenStartloopAddrsOffset, 0, kAbsolute, kVoice},
    {kGenEndloopAddrsOffset, 0, kAbsolute, kVoice},
    {kGenStartAddrsCoarseOffset, 0, kAbsolute, kVoice},
    {kGenModLfoToPitch, 0, kAdditive, kVoice},
    {kGenVibLfoToPitch, 0, kAdditive, kVoice},
    {kGenModEnvToPitch, 0, kAdditive, kVoice},
    {kGenInitialFilterFc, 13500, kAdditive, kVoice},
    {kGenInitialFilterQ, 0, kAdditive, kVoice},
    {kGenModLfoToFilterFc, 0, kAdditive, kVoice},
    {kGenModEnvToFilterFc, 0, kAdditive, kVoice},
    {kGenEndAddrsCoarseOffset, 0, kAbsolute, kVoice},
    {kGenModLfoToVolume, 0, kAdditive, kVoice},
    {14, 0, kAbsolute, kReserved},  // unused1
    {kGenChorusEffectsSend, 0, kAdditive, kVoice},
    {kGenReverbEffectsSend, 0, kAdditive, kVoice},
    {kGenPan, 0, kAdditive, kVoice},
    {18, 0, kAbsolute, kReserved},  // unused2
    {19, 0, kAbsolute, kReserved},  // unused3
    {20, 0, kAbsolute, kReserved},  // unused4
    {kGenDelayModLfo, -12000, kAdditive, kVoice},
    {kGenFreqModLfo, 0, kAdditive, kVoice},
    {kGenDelayVibLfo, -12000, kAdditive, kVoice},
    {kGenFreqVibLfo, 0, kAdditive, kVoice},
    {kGenDelayModEnv, -12000, kAdditive, kVoice},
    {kGenAttackModEnv, -12000, kAdditive, kVoice},
    {kGenHoldModEnv, -12000, kAdditive, kVoice},
    {kGenDecayModEnv, -12000, kAdditive, kVoice},
    {kGenSustainModEnv, 0, kAdditive, kVoice},
    {kGenReleaseModEnv, -12000, kAdditive, kVoice},
    {kGenKeynumToModEnvHold, 0, kAdditive, kVoice},
    {kGenKeynumToModEnvDecay, 0, kAdditive, kVoice},
    {kGenDelayVolEnv, -12000, kAdditive, kVoice},
    {kGenAttackVolEnv, -12000, kAdditive, kVoice},
    {kGenHoldVolEnv, -12000, kAdditive, kVoice},
    {kGenDecayVolEnv, -12000, kAdditive, kVoice},
    {kGenSustainVolEnv, 0, kAdditive, kVoice},
    {kGenReleaseVolEnv, -12000, kAdditive, kVoice},
    {kGenKeynumToVolEnvHold, 0, kAdditive, kVoice},
    {kGenKeynumToVolEnvDecay, 0, kAdditive, kVoice},
    {kGenInstrument, 0, kAbsolute, kZoneLink},
    {42, 0, kAbsolute, kReserved},  // reserved1
    {kGenKeyRange, 0, kAbsolute, kZoneLink},
    {kGenVelRange, 0, kAbsolute, kZoneLink},
    {kGenStartloopAddrsCoarseOffset, 0, kAbsolute, kVoice},
    {kGenKeynum, -1, kAbsolute, kVoice},
    {kGenVelocity, -1, kAbsolute, kVoice},
    {kGenInitialAttenuation, 0, kAdditive, kVoice},
    {49, 0, kAbsolute, kReserved},  // reserved2
    {kGenEndloopAddrsCoarseOffset, 0, kAbsolute, kVoice},
    {kGenCoarseTune, 0, kAdditive, kVoice},
    {kGenFineTune, 0, kAdditive, kVoice},
    {kGenSampleId, 0, kAbsolute, kZoneLink},
    {kGenSampleModes, 0, kAbsolute, kVoice},
    {55, 0, kAbsolute, kReserved},  // reserved3
    {kGenScaleTuning, 100, kAdditive, kVoice},
    {kGenExclusiveClass, 0, kAbsolute, kVoice},
    {kGenOverridingRootKey, -1, kAbsolute, kVoice},
    {59, 0, kAbsolute, kReserved},  // unused5
    {60, 0, kAbsolute, kReserved},  // endOper
};

constexpr bool gen_specs_indexed_by_oper() noexcept {
  for (int i = 0; i < Sf2GenSet::kNumGens; ++i) {
    if (kGenSpecs[i].oper != i) return false;
  }
  return true;
}
static_assert(gen_specs_indexed_by_oper(), "kGenSpecs must hold one row per operator, in order");

constexpr Sf2GenSpec kOutOfRangeGenSpec = {Sf2GenSet::kNumGens, 0, kAbsolute, kReserved};

/// SF2 2.04 section 8.1.3: keynumTo*Hold/Decay leave key 60 unchanged.
constexpr int kKeynumEnvelopePivotKey = 60;

/// Default modulator: velocity -> initialFilterFc darkening (soft notes lose
/// brightness). Linearised form of the spec's -2400 cent modulator.
constexpr float kVelToFilterCents = -2400.0f;

/// Generator operators of one DAHDSR block.
struct EnvelopeOps {
  uint16_t delay, attack, hold, decay, sustain, release, keynum_to_hold, keynum_to_decay;
  bool sustain_is_centibels;
};

constexpr EnvelopeOps kVolumeEnvOps = {
    kGenDelayVolEnv,        kGenAttackVolEnv,        kGenHoldVolEnv,
    kGenDecayVolEnv,        kGenSustainVolEnv,       kGenReleaseVolEnv,
    kGenKeynumToVolEnvHold, kGenKeynumToVolEnvDecay, true};
constexpr EnvelopeOps kModEnvOps = {
    kGenDelayModEnv,        kGenAttackModEnv,        kGenHoldModEnv,
    kGenDecayModEnv,        kGenSustainModEnv,       kGenReleaseModEnv,
    kGenKeynumToModEnvHold, kGenKeynumToModEnvDecay, false};

/// Converts a DAHDSR generator block (timecents / sustain unit) to ms config at @p key.
DahdsrConfig envelope_from_gens(const Sf2GenSet& gens, const EnvelopeOps& ops, int key) noexcept {
  const int32_t key_steps = kKeynumEnvelopePivotKey - key;
  const int32_t hold_tc = gens.get(ops.hold) + gens.get(ops.keynum_to_hold) * key_steps;
  const int32_t decay_tc = gens.get(ops.decay) + gens.get(ops.keynum_to_decay) * key_steps;
  DahdsrConfig cfg;
  cfg.delay_ms =
      gens.get(ops.delay) <= -12000 ? 0.0f : 1000.0f * timecents_to_seconds(gens.get(ops.delay));
  cfg.attack_ms = 1000.0f * timecents_to_seconds(gens.get(ops.attack));
  cfg.hold_ms = hold_tc <= -12000 ? 0.0f : 1000.0f * timecents_to_seconds(hold_tc);
  cfg.decay_ms = 1000.0f * timecents_to_seconds(decay_tc);
  if (ops.sustain_is_centibels) {
    cfg.sustain = centibels_to_gain(static_cast<float>(gens.get(ops.sustain)));
  } else {
    // Modulation envelope sustain: 0.1% DECREASE units (0 = full, 1000 = none).
    cfg.sustain =
        std::clamp(1.0f - static_cast<float>(gens.get(ops.sustain)) / 1000.0f, 0.0f, 1.0f);
  }
  cfg.release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(gens.get(ops.release)));
  return cfg;
}

}  // namespace

Sf2GenSet::Sf2GenSet() noexcept {
  for (int i = 0; i < kNumGens; ++i) values_[i] = kGenSpecs[i].default_value;
}

void Sf2GenSet::apply_absolute(const Sf2Zone& zone) noexcept {
  for (const Sf2Gen& g : zone.gens) {
    if (g.oper < kNumGens) values_[g.oper] = g.amount;
  }
}

void Sf2GenSet::add_relative(const Sf2Zone& zone) noexcept {
  for (const Sf2Gen& g : zone.gens) {
    if (g.oper < kNumGens && kGenSpecs[g.oper].preset_additive) values_[g.oper] += g.amount;
  }
}

float timecents_to_seconds(int32_t timecents) noexcept {
  // Spec floor: -12000 tc is 1 ms; treat anything at/below as instantaneous-ish.
  if (timecents <= -12000) return 0.001f;
  return std::exp2(static_cast<float>(timecents) / 1200.0f);
}

const Sf2GenSpec& sf2_gen_spec(uint16_t oper) noexcept {
  return oper < Sf2GenSet::kNumGens ? kGenSpecs[oper] : kOutOfRangeGenSpec;
}

float centibels_to_gain(float centibels) noexcept {
  if (centibels <= 0.0f) return 1.0f;
  if (centibels >= 1440.0f) return 0.0f;
  return std::pow(10.0f, -centibels / 200.0f);
}

float centibel_offset_to_gain(float centibels) noexcept {
  return std::pow(10.0f, centibels / 200.0f);
}

float abs_cents_to_hz(float cents) noexcept { return 8.176f * std::exp2(cents / 1200.0f); }

float sf2_velocity_gain(Velocity16 velocity) noexcept {
  // The spec's concave 960 cB velocity->attenuation modulator reduces to
  // exactly (vel/127)^2 in linear gain.
  // f7() keeps a MIDI 2.0 velocity between the 7-bit steps; for a MIDI 1.0 velocity it is float(v).
  const float v = velocity.f7() / 127.0f;
  return v * v;
}

float sf2_cc_gain(Control32 value) noexcept {
  const float v = value.f7() / 127.0f;
  return v * v;
}

Sf2VoiceParams resolve_voice_params(const Sf2GenSet& gens, const Sf2Sample& sample, uint8_t key,
                                    Velocity16 velocity, double output_sample_rate) noexcept {
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

  // --- velocity: the generator, when set, replaces the struck velocity for every term ---
  const int32_t velocity_override = gens.get(kGenVelocity);
  const Velocity16 effective_velocity =
      velocity_override >= 0
          ? Velocity16::from7(static_cast<uint8_t>(std::min<int32_t>(velocity_override, 127)))
          : velocity;
  p.velocity_gain = sf2_velocity_gain(effective_velocity);

  // --- attenuation / pan ---
  p.attenuation_gain = centibels_to_gain(static_cast<float>(gens.get(kGenInitialAttenuation)));
  p.pan_units = std::clamp(static_cast<float>(gens.get(kGenPan)), -500.0f, 500.0f);

  // --- volume / modulation envelopes ---
  p.volume_env = envelope_from_gens(gens, kVolumeEnvOps, effective_key);
  p.mod_env = envelope_from_gens(gens, kModEnvOps, effective_key);
  p.mod_env_to_pitch = static_cast<float>(gens.get(kGenModEnvToPitch));
  p.mod_env_to_filter_fc = static_cast<float>(gens.get(kGenModEnvToFilterFc));

  // --- filter: velocity darkening (default modulator) on top of the zone Fc ---
  const float vel_offset = kVelToFilterCents * (1.0f - effective_velocity.f7() / 127.0f);
  p.filter_fc_cents = static_cast<float>(gens.get(kGenInitialFilterFc)) + vel_offset;
  // initialFilterQ is the resonance peak height in centibels: Q = 10^(cB/200).
  const float q_cb = static_cast<float>(gens.get(kGenInitialFilterQ));
  p.filter_q = std::max(constants::kButterworthQ, std::pow(10.0f, q_cb / 200.0f));

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

void Sf2Voice::start(const float* pool_data, const Sf2VoiceParams& p, double sample_rate) noexcept {
  params = p;
  reader.start(pool_data, SampleRegion{p.start, p.end, p.loop_start, p.loop_end, p.loop_mode});
  key_down = true;
  releasing = false;
  // A slot can be reused while its previous voice was still sostenuto-captured
  // (the sample ended, or All Sound Off silenced it, with CC66 still down); the
  // stale capture would make the new note ignore its own note-off.
  sostenuto = false;
  // The player installs the portamento glide after start(); a reused slot must
  // not inherit the previous note's.
  glide_cents = 0.0f;
  glide_coeff = 0.0f;
  env.configure(sample_rate, p.volume_env);
  env.note_on();
  mod_env.configure(sample_rate, p.mod_env);
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

void Sf2Voice::choke(double sample_rate) noexcept {
  // configure() recomputes stage rates and touches neither the level nor the
  // stage, so the fade starts from wherever the envelope already is and cannot
  // click. A hard kill would.
  DahdsrConfig fast = params.volume_env;
  fast.release_ms = kChokeReleaseMs;
  env.configure(sample_rate, fast);
  release();
}

float Sf2Voice::render(const Sf2ChannelMod& mod) noexcept {
  if (!active || !reader.valid()) return 0.0f;

  // Refresh the cached stereo pan gains when zone pan + CC10 changed.
  const float pan_units = params.pan_units + mod.pan_units;
  if (pan_units != cached_pan_units) {
    cached_pan_units = pan_units;
    const rt::PanGains gains = voice_pan_gains(pan_units);
    gain_left = gains.left;
    gain_right = gains.right;
  }

  // A mode 3 loop runs until the release begins, so a pedal-held note keeps looping.
  const bool looping = reader.looping(!releasing);
  if (!reader.wrap(looping)) {
    active = false;
    env.kill();
    return 0.0f;
  }
  float sample = reader.read(looping);

  // --- modulation sources ---
  const float mod_env_level = mod_env.next();
  const float mod_lfo_value = mod_lfo.next();
  const float vib_lfo_value = vib_lfo.next(mod.vib_rate_scale);

  // --- portamento: one-pole decay of the source-note offset ---
  if (glide_coeff > 0.0f) {
    glide_cents *= glide_coeff;
    if (std::fabs(glide_cents) < 0.5f) {
      glide_cents = 0.0f;
      glide_coeff = 0.0f;
    }
  }

  // --- pitch: bend + modEnv + LFOs + portamento (cents) ---
  const float pitch_cents = mod.pitch_cents + mod_env_level * params.mod_env_to_pitch +
                            mod_lfo_value * params.mod_lfo_to_pitch +
                            vib_lfo_value * (params.vib_lfo_to_pitch + mod.extra_vibrato_cents) +
                            glide_cents + per_note.cents;
  if (pitch_cents != 0.0f) {
    reader.advance(params.pitch_increment * std::exp2(static_cast<double>(pitch_cents) / 1200.0));
  } else {
    reader.advance(params.pitch_increment);
  }

  // --- filter: Fc = zone Fc + modEnv + modLFO (cents) ---
  if (!params.filter_bypass) {
    const float fc_cents = params.filter_fc_cents + mod_env_level * params.mod_env_to_filter_fc +
                           mod_lfo_value * params.mod_lfo_to_filter_fc + mod.mod_cutoff_cents +
                           vib_lfo_value * mod.lfo_cutoff_cents;
    filter.set(abs_cents_to_hz(fc_cents), params.filter_q);
    sample = filter.process(sample).lp;
  }

  // --- amplitude ---
  const float level = env.next();
  if (!env.active()) {
    active = false;
    return 0.0f;
  }
  float gain = level * params.attenuation_gain * params.velocity_gain * mod.gain;
  if (params.mod_lfo_to_volume_cb != 0.0f) {
    gain *= centibel_offset_to_gain(mod_lfo_value * params.mod_lfo_to_volume_cb);
  }
  if (mod.tremolo_depth01 != 0.0f) {
    // The LFO's peak is the part's own level and the depth is spent below it.
    gain *= 1.0f - mod.tremolo_depth01 * (1.0f - vib_lfo_value) * 0.5f;
  }
  return sample * gain;
}

}  // namespace sonare::midi::synth
