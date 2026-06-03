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

}  // namespace

namespace {
// A zero / non-positive (or non-finite) field means "use the default", so a
// zero-initialized config sanitizes into the full default patch and partial
// overrides keep sensible values for the fields the caller left unset. This is a
// deliberate minimal-synth convenience: an exact 0 (e.g. sustain == 0) is not
// requestable — the richer instrument bank (see backup/builtin-instrument-plan.md)
// will use an explicit "is-set" model instead.
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
  next_age_ = 1;
}

void BuiltinSynth::note_on(uint8_t channel, uint8_t note, float velocity) noexcept {
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
  target->phase_inc = note_to_hz(note) / sample_rate_;
  target->velocity = clampf(velocity, 0.0f, 1.0f);
  target->env = 0.0f;
  target->stage = Stage::kAttack;
  target->age = next_age_++;
}

void BuiltinSynth::note_off(uint8_t channel, uint8_t note) noexcept {
  for (auto& v : voices_) {
    if (v.active && v.note == note && v.channel == channel && v.stage != Stage::kRelease) {
      v.stage = Stage::kRelease;
    }
  }
}

void BuiltinSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return;
  }
  if (u.is_note_on()) {
    // MIDI 1.0 velocity is 7-bit in data2; MIDI 2.0 carries 16-bit in word[1].
    float vel = 0.0f;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      vel = static_cast<float>(u.data2_7bit()) / 127.0f;
    } else {
      vel = static_cast<float>((u.words[1] >> 16) & 0xFFFFu) / 65535.0f;
    }
    note_on(u.channel(), u.note_number(), vel);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number());
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

  return osc * v.env * v.velocity;
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
