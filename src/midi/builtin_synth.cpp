#include "midi/builtin_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/ump.h"

namespace sonare::midi {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

float clampf(float v, float lo, float hi) noexcept { return std::min(std::max(v, lo), hi); }

// Per-sample linear envelope increment to cross [0,1] over `ms` at `sample_rate`.
// A zero/negative time means "instantaneous" (one sample).
float stage_increment(float ms, double sample_rate) noexcept {
  const double frames = std::max(1.0, (static_cast<double>(ms) * 0.001) * sample_rate);
  return static_cast<float>(1.0 / frames);
}

double note_to_hz(uint8_t note) noexcept {
  return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0);
}

// Fixed MPE pitch-bend range (semitones for a full-scale bend in either
// direction). A configurable range would require RPN 0, which this minimal
// fallback deliberately does not parse.
constexpr float kPitchBendRangeSemitones = 2.0f;

// Amplitude boost at full pressure. A multiplier of 1 + depth*pressure keeps
// pressure == 0 exactly unity (so non-MPE playback is bit-identical).
constexpr float kPressureModDepth = 1.0f;

// Multiplier on the base phase increment for a bend expressed in semitones.
double bend_ratio(float semitones) noexcept {
  if (semitones == 0.0f) return 1.0;
  return std::pow(2.0, static_cast<double>(semitones) / 12.0);
}

}  // namespace

namespace {
// A zero / non-positive (or non-finite) field means "use the default", so a
// zero-initialized config sanitizes into the full default patch and partial
// overrides keep sensible values for the fields the caller left unset. This is a
// deliberate minimal-synth convenience: an exact 0 (e.g. sustain == 0) is not
// requestable — the richer instrument bank (planned separately) will use an
// explicit "is-set" model instead.
float positive_or_default(float v, float fallback, float hi) noexcept {
  if (!std::isfinite(v) || v <= 0.0f) return fallback;
  return clampf(v, 0.0f, hi);
}
}  // namespace

BuiltinSynthConfig clamp_synth_config(const BuiltinSynthConfig& cfg) noexcept {
  BuiltinSynthConfig c = cfg;
  switch (cfg.waveform) {
    case SynthWaveform::kSine:
    case SynthWaveform::kSaw:
    case SynthWaveform::kSquare:
    case SynthWaveform::kTriangle:
      break;
    default:
      c.waveform = SynthWaveform::kSine;
      break;
  }
  c.gain = positive_or_default(cfg.gain, 0.2f, 4.0f);
  c.attack_ms = positive_or_default(cfg.attack_ms, 5.0f, 20000.0f);
  c.decay_ms = positive_or_default(cfg.decay_ms, 60.0f, 20000.0f);
  c.sustain = positive_or_default(cfg.sustain, 0.7f, 1.0f);
  c.release_ms = positive_or_default(cfg.release_ms, 120.0f, 20000.0f);
  c.polyphony = cfg.polyphony > 0 ? std::min(cfg.polyphony, kMaxSynthVoices) : 16;
  return c;
}

int64_t synth_tail_samples(const BuiltinSynthConfig& cfg, double sample_rate) noexcept {
  if (!(sample_rate > 0.0)) return 0;
  const BuiltinSynthConfig c = clamp_synth_config(cfg);
  return static_cast<int64_t>(std::ceil((static_cast<double>(c.release_ms) * 0.001) * sample_rate));
}

BuiltinSynth::BuiltinSynth(const BuiltinSynthConfig& config) noexcept
    : config_(clamp_synth_config(config)) {}

void BuiltinSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  attack_inc_ = stage_increment(config_.attack_ms, sample_rate_);
  decay_inc_ = stage_increment(config_.decay_ms, sample_rate_);
  release_inc_ = stage_increment(config_.release_ms, sample_rate_);
  tail_samples_ = synth_tail_samples(config_, sample_rate_);
  voices_.assign(static_cast<size_t>(config_.polyphony), Voice{});
  next_age_ = 1;
  prepared_ = true;
}

void BuiltinSynth::reset() {
  for (auto& v : voices_) v = Voice{};
  sustain_down_ = {};
  channel_bend_semitones_ = {};
  channel_pressure_ = {};
  next_age_ = 1;
}

void BuiltinSynth::note_on(uint8_t channel, uint8_t note, float velocity) noexcept {
  if (!prepared_) return;
  if (voices_.empty()) return;
  // Pick a free voice; else steal the oldest (smallest age) voice deterministically.
  Voice* target = nullptr;
  for (auto& v : voices_) {
    if (!v.active) {
      target = &v;
      break;
    }
  }
  if (target == nullptr) {
    target = &voices_[0];
    for (auto& v : voices_) {
      if (v.age < target->age) target = &v;
    }
  }
  target->active = true;
  target->note = note;
  target->channel = channel;
  target->phase = 0.0;
  target->base_phase_inc = note_to_hz(note) / sample_rate_;
  target->phase_inc = target->base_phase_inc * bend_ratio(channel_bend_semitones_[channel & 0x0Fu]);
  target->velocity = clampf(velocity, 0.0f, 1.0f);
  target->poly_pressure = 0.0f;
  target->env = 0.0f;
  target->stage = Stage::kAttack;
  target->key_down = true;
  target->age = next_age_++;
}

void BuiltinSynth::note_off(uint8_t channel, uint8_t note) noexcept {
  if (!prepared_) return;
  for (auto& v : voices_) {
    if (v.active && v.note == note && v.channel == channel && v.stage != Stage::kRelease) {
      v.key_down = false;
      if (!sustain_down_[channel & 0x0Fu]) {
        v.stage = Stage::kRelease;
      }
    }
  }
}

void BuiltinSynth::sustain_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  if (sustain_down_[ch] == down) return;
  sustain_down_[ch] = down;
  if (down) return;
  for (auto& v : voices_) {
    if (v.active && v.channel == ch && !v.key_down && v.stage != Stage::kRelease) {
      v.stage = Stage::kRelease;
    }
  }
}

void BuiltinSynth::pitch_bend(uint8_t channel, uint16_t bend14) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  // 14-bit unsigned, center 8192 -> [-1, +1] -> semitones.
  const float norm = (static_cast<float>(bend14) - 8192.0f) / 8192.0f;
  const float semitones = clampf(norm, -1.0f, 1.0f) * kPitchBendRangeSemitones;
  channel_bend_semitones_[ch] = semitones;
  const double ratio = bend_ratio(semitones);
  for (auto& v : voices_) {
    if (v.active && (v.channel & 0x0Fu) == ch) {
      v.phase_inc = v.base_phase_inc * ratio;
    }
  }
}

void BuiltinSynth::channel_pressure(uint8_t channel, uint8_t pressure7) noexcept {
  if (!prepared_) return;
  channel_pressure_[channel & 0x0Fu] = clampf(static_cast<float>(pressure7) / 127.0f, 0.0f, 1.0f);
}

void BuiltinSynth::poly_pressure(uint8_t channel, uint8_t note, uint8_t pressure7) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  const float value = clampf(static_cast<float>(pressure7) / 127.0f, 0.0f, 1.0f);
  for (auto& v : voices_) {
    if (v.active && v.note == note && (v.channel & 0x0Fu) == ch) {
      v.poly_pressure = value;
    }
  }
}

void BuiltinSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  sustain_down_[channel & 0x0Fu] = false;
  for (auto& v : voices_) {
    if (v.active && v.channel == channel && v.stage != Stage::kRelease) {
      v.key_down = false;
      v.stage = Stage::kRelease;
    }
  }
}

void BuiltinSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  sustain_down_[channel & 0x0Fu] = false;
  for (auto& v : voices_) {
    if (v.active && v.channel == channel) v = Voice{};
  }
}

void BuiltinSynth::reset_all_controllers(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  // Lift the damper and release any keys it was holding.
  sustain_pedal(ch, false);
  // Recenter pitch bend and restore the unbent pitch of every active voice.
  channel_bend_semitones_[ch] = 0.0f;
  channel_pressure_[ch] = 0.0f;
  for (auto& v : voices_) {
    if (v.active && (v.channel & 0x0Fu) == ch) {
      v.phase_inc = v.base_phase_inc;
      v.poly_pressure = 0.0f;
    }
  }
}

void BuiltinSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return;
  }
  const bool is_midi1 = u.message_type() == UmpMessageType::kMidi1ChannelVoice;
  if (u.is_note_on()) {
    // MIDI 1.0 velocity is 7-bit in data2; MIDI 2.0 carries 16-bit in word[1].
    float vel = 0.0f;
    if (is_midi1) {
      vel = static_cast<float>(u.data2_7bit()) / 127.0f;
    } else {
      vel = static_cast<float>((u.words[1] >> 16) & 0xFFFFu) / 65535.0f;
    }
    note_on(u.channel(), u.note_number(), vel);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number());
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    // MIDI 1.0 splits the 14-bit value across data1 (LSB) / data2 (MSB); MIDI 2.0
    // carries a 32-bit value in word[1] that scales down to the same 14-bit form.
    const uint16_t bend14 =
        is_midi1
            ? static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number())
            : scale_bend_32_to_14(u.words[1]);
    pitch_bend(u.channel(), bend14);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure)) {
    // MIDI 1.0 carries the 7-bit pressure in data1 (the note-number slot); MIDI
    // 2.0 in word[1].
    const uint8_t pressure7 = is_midi1 ? u.note_number() : scale_cc_32_to_7(u.words[1]);
    channel_pressure(u.channel(), pressure7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure)) {
    const uint8_t pressure7 = is_midi1 ? u.data2_7bit() : scale_cc_32_to_7(u.words[1]);
    poly_pressure(u.channel(), u.note_number(), pressure7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    // Channel-mode messages. The controller index rides word[0] bits 8..14 for
    // both protocols (same slot as a note number).
    const uint8_t controller = u.note_number();
    const uint8_t channel = u.channel();
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    switch (controller) {
      case 64:  // Damper/sustain pedal: >=64 holds released keys.
        sustain_pedal(channel, value7 >= 64);
        break;
      case 120:  // All Sound Off — immediate silence.
        all_sound_off(channel);
        break;
      case 121:  // Reset All Controllers — lift damper, recenter bend, clear pressure.
        reset_all_controllers(channel);
        break;
      case 123:  // All Notes Off — graceful release.
      case 124:  // Omni Off / On and Mono/Poly mode changes also imply notes-off.
      case 125:
      case 126:
      case 127:
        all_notes_off(channel);
        break;
      default:
        // Other controllers (RPN/NRPN, etc.) have no effect on this deliberately
        // minimal synth.
        break;
    }
  }
}

float BuiltinSynth::render_voice_sample(Voice& v) noexcept {
  // Advance envelope.
  switch (v.stage) {
    case Stage::kAttack:
      v.env += attack_inc_;
      if (v.env >= 1.0f) {
        v.env = 1.0f;
        v.stage = Stage::kDecay;
      }
      break;
    case Stage::kDecay:
      v.env -= decay_inc_;
      if (v.env <= config_.sustain) {
        v.env = config_.sustain;
        v.stage = Stage::kSustain;
      }
      break;
    case Stage::kSustain:
      v.env = config_.sustain;
      break;
    case Stage::kRelease:
      v.env -= release_inc_;
      if (v.env <= 0.0f) {
        v.env = 0.0f;
        v.active = false;
        v.stage = Stage::kIdle;
        return 0.0f;
      }
      break;
    case Stage::kIdle:
      return 0.0f;
  }

  // Oscillator (naive; minimal synth intentionally does not band-limit).
  const double p = v.phase;
  float osc = 0.0f;
  switch (config_.waveform) {
    case SynthWaveform::kSine:
      osc = static_cast<float>(std::sin(kTwoPi * p));
      break;
    case SynthWaveform::kSaw:
      osc = static_cast<float>(2.0 * p - 1.0);
      break;
    case SynthWaveform::kSquare:
      osc = p < 0.5 ? 1.0f : -1.0f;
      break;
    case SynthWaveform::kTriangle:
      osc = static_cast<float>(4.0 * std::abs(p - 0.5) - 1.0);
      break;
  }
  v.phase += v.phase_inc;
  if (v.phase >= 1.0) v.phase -= std::floor(v.phase);

  // MPE pressure boosts amplitude. Channel and poly pressure combine, clamped to
  // unity, so the multiplier is exactly 1.0 (no change) when neither is sent.
  const float pressure = clampf(channel_pressure_[v.channel & 0x0Fu] + v.poly_pressure, 0.0f, 1.0f);
  const float pressure_gain = 1.0f + kPressureModDepth * pressure;
  return osc * v.env * v.velocity * pressure_gain;
}

void BuiltinSynth::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  for (int i = 0; i < num_samples; ++i) {
    float mix = 0.0f;
    for (auto& v : voices_) {
      if (v.active) mix += render_voice_sample(v);
    }
    mix *= config_.gain;
    // ADD into the planar scratch (engine zero-fills first); fan mono to all channels.
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) channels[ch][i] += mix;
    }
  }
}

}  // namespace sonare::midi
