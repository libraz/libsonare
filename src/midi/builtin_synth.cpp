#include "midi/builtin_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/mod_matrix.h"
#include "midi/synth/sf2_voice.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi {

namespace {

float clampf(float v, float lo, float hi) noexcept { return std::min(std::max(v, lo), hi); }

// Per-sample linear envelope increment to cross [0,1] over `ms` at `sample_rate`.
// A zero/negative time means "instantaneous" (one sample).
float stage_increment(float ms, double sample_rate) noexcept {
  const double frames = std::max(1.0, (static_cast<double>(ms) * 0.001) * sample_rate);
  return static_cast<float>(1.0 / frames);
}

double note_to_hz(uint8_t note) noexcept {
  return constants::kA4Hz * std::pow(2.0, (static_cast<double>(note) - constants::kMidiA4) /
                                              constants::kSemitonesPerOctave);
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
  return std::pow(2.0, static_cast<double>(semitones) / constants::kSemitonesPerOctave);
}

// Note On attribute #3 carries Pitch 7.9 (M2-104-UM §7.4.15).
constexpr uint8_t kAttributePitch79 = 0x03;
// Registered Per-Note Controller #3 is Pitch 7.25 (M2-104-UM §7.4.12).
constexpr uint8_t kRpncPitch725 = 3;
// Registered Controller indices in bank 0 read straight from the message.
constexpr uint8_t kRcPitchBendSensitivity = 0;
constexpr uint8_t kRcPerNoteBendSensitivity = 7;

// Adds a relative controller's two's-complement delta to @p current, saturating at the ends of
// the 32-bit range rather than wrapping.
Control32 add_saturating(Control32 current, Control32 delta) noexcept {
  const int64_t sum =
      static_cast<int64_t>(current.raw) + static_cast<int64_t>(static_cast<int32_t>(delta.raw));
  const int64_t clamped = std::min<int64_t>(std::max<int64_t>(sum, 0), int64_t{0xFFFFFFFF});
  return Control32::from_raw(static_cast<uint32_t>(clamped));
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
    : config_(clamp_synth_config(config)) {
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_controls(ch);
}

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
  channel_controls_ = {};
  mpe_.reset();
  params_ = {};
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  skipped_events_ = 0;
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_controls(ch);
  next_age_ = 1;
}

void BuiltinSynth::refresh_channel_controls(uint8_t channel) noexcept {
  ChannelControls& c = channel_controls_[channel & 0x0Fu];
  // Volume and expression multiply through the same concave (v/127)^2 curve the
  // SF2 / native voices use, so a part keeps its balance across instruments.
  c.gain = synth::sf2_cc_gain(c.volume) * synth::sf2_cc_gain(c.expression);
  // CC10 is 0..127 around a centre of 64, so the positive half spans 63 steps
  // and both 0 and 1 land hard left -- the GM mapping the other instruments use.
  const float pan_units = (c.pan.f7() - 64.0f) / 63.0f * synth::kPanUnitsFullScale;
  c.pan_gains = synth::voice_pan_gains(pan_units);
}

void BuiltinSynth::note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                           uint8_t attribute_type, uint16_t attribute_data,
                           uint32_t source_track_id) noexcept {
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
  target->source_track_id = source_track_id;
  target->phase = 0.0;
  target->base_phase_inc = note_to_hz(note) / sample_rate_;
  target->per_note.bind(channel, note);
  target->has_attribute_pitch = attribute_type == kAttributePitch79;
  target->attribute_pitch_q7_9 = target->has_attribute_pitch ? attribute_data : uint16_t{0};
  refresh_per_note_pitch(*target);
  target->velocity = clampf(velocity.f7() / 127.0f, 0.0f, 1.0f);
  target->poly_pressure = 0.0f;
  target->env = 0.0f;
  target->stage = Stage::kAttack;
  target->key_down = true;
  target->age = next_age_++;
}

void BuiltinSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  for (auto& v : voices_) {
    if (v.active && v.note == note && v.channel == channel &&
        v.source_track_id == source_track_id && v.stage != Stage::kRelease) {
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

void BuiltinSynth::refresh_channel_expression(uint8_t channel) noexcept {
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  const bool zoned = mpe_.role(ch) != MpeChannelRole::kUnassigned;
  // Inside a zone both dimensions are the zone's: the bend range is the one the
  // MCM installed rather than the fixed one above, and each carries the
  // manager's contribution (2.2.5 - 2.2.7). Outside one -- which is every
  // channel until an MCM arrives -- this is the arithmetic that was always
  // here.
  if (zoned) {
    channel_bend_semitones_[ch] = mpe_.bend_semitones(ch);
    channel_pressure_[ch] = mpe_.pressure(ch) / 127.0f;
  }
  const double ratio = bend_ratio(channel_bend_semitones_[ch]);
  for (auto& v : voices_) {
    if (v.active && (v.channel & 0x0Fu) == ch) {
      v.phase_inc = v.base_phase_inc * ratio * v.per_note_ratio;
    }
  }
  if (!zoned || mpe_.role(ch) != MpeChannelRole::kManager) return;
  // A manager's bend and pressure reach every note in its zone (2.2.6, 2.2.7),
  // so they are not done arriving on the channel they were addressed to.
  const MpeZone zone = mpe_.zone_of(ch);
  for (uint8_t member = 0; member < 16; ++member) {
    if (mpe_.role(member) != MpeChannelRole::kMember || mpe_.zone_of(member) != zone) continue;
    refresh_channel_expression(member);
  }
}

void BuiltinSynth::apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept {
  uint16_t moved = 0;
  if (!mpe_.apply_mcm(manager_channel, member_count, &moved)) return;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((moved & (uint16_t{1} << ch)) == 0) continue;
    all_sound_off(ch);
    reset_all_controllers(ch);
  }
  // Every channel, not only the ones that moved: an MCM reinstalls the whole
  // zone's bend sensitivity, so a member that kept its place still changed
  // range.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_expression(ch);
}

void BuiltinSynth::pitch_bend(uint8_t channel, Bend32 bend) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  // 14-bit scale, center 8192 -> [-1, +1] -> semitones; f14() keeps a MIDI 2.0 bend's fraction.
  const float norm = (bend.f14() - 8192.0f) / 8192.0f;
  channel_bend_semitones_[ch] = clampf(norm, -1.0f, 1.0f) * kPitchBendRangeSemitones;
  mpe_.track_bend(ch, bend);
  refresh_channel_expression(ch);
}

void BuiltinSynth::channel_pressure(uint8_t channel, Control32 pressure) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  channel_pressure_[ch] = clampf(pressure.f7() / 127.0f, 0.0f, 1.0f);
  mpe_.track_pressure(ch, pressure);
  refresh_channel_expression(ch);
}

void BuiltinSynth::poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  // Prohibited on a member channel, where pressure is the channel's and belongs
  // to the one note living on it (2.2.7).
  if (mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) return;
  const float value = clampf(pressure.f7() / 127.0f, 0.0f, 1.0f);
  for (auto& v : voices_) {
    if (v.active && v.note == note && (v.channel & 0x0Fu) == ch) {
      v.poly_pressure = value;
    }
  }
}

void BuiltinSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  for (auto& v : voices_) {
    if (v.active && v.channel == ch && v.stage != Stage::kRelease) {
      v.key_down = false;
      if (!sustain_down_[ch]) v.stage = Stage::kRelease;
    }
  }
}

void BuiltinSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  for (auto& v : voices_) {
    if (v.active && v.channel == ch) v = Voice{};
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
  // Inside a zone the same controllers are tracked by the zone model, which is
  // where the two above are read from -- leaving them there would reset the
  // channel and change nothing that is heard.
  mpe_.reset_controls(static_cast<uint16_t>(uint16_t{1} << ch));
  params_[ch].reset();
  // RP-015: expression returns to full, volume and pan are left alone.
  channel_controls_[ch].expression = Control32::from7(127);
  refresh_channel_controls(ch);
  // Per-note pitch survives: Reset All Controllers does not reach it (M2-104-UM B.2).
  for (auto& v : voices_) {
    if (v.active && (v.channel & 0x0Fu) == ch) {
      v.phase_inc = v.base_phase_inc * v.per_note_ratio;
      v.poly_pressure = 0.0f;
    }
  }
  // A manager's reset takes its contribution out of every member of its zone.
  if (mpe_.role(ch) == MpeChannelRole::kManager) refresh_channel_expression(ch);
}

void BuiltinSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return;
  }
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) {
    ++skipped_events_;  // Reserved status.
    return;
  }
  switch (ev.kind) {
    case ChannelVoiceKind::NoteOn:
      note_on(ev.channel, ev.note, ev.velocity, ev.index, ev.attribute_data, event.source_track_id);
      break;
    case ChannelVoiceKind::NoteOff:
      note_off(ev.channel, ev.note, event.source_track_id);
      break;
    case ChannelVoiceKind::PitchBend:
      pitch_bend(ev.channel, ev.bend);
      break;
    case ChannelVoiceKind::ChannelPressure:
      channel_pressure(ev.channel, ev.value);
      break;
    case ChannelVoiceKind::PolyPressure:
      poly_pressure(ev.channel, ev.note, ev.value);
      break;
    case ChannelVoiceKind::ControlChange:
      control_change(ev.channel, ev.note, ev.value);
      break;
    case ChannelVoiceKind::ProgramChange:
      break;  // One patch; programs select nothing here.
    case ChannelVoiceKind::RegisteredController:
    case ChannelVoiceKind::AssignableController:
      registered_controller(u, ev);
      break;
    case ChannelVoiceKind::RelativeRegistered:
    case ChannelVoiceKind::RelativeAssignable:
      relative_controller(ev);
      break;
    case ChannelVoiceKind::PerNotePitchBend:
      per_note_pitch_.set_per_note_bend(ev.channel, ev.note, ev.bend);
      refresh_per_note_voices(ev.channel, ev.note, false);
      break;
    case ChannelVoiceKind::RegisteredPerNote:
      if (ev.index != kRpncPitch725) {
        ++skipped_events_;
        break;
      }
      per_note_pitch_.set_pitch_7_25(ev.channel, ev.note, ev.value);
      refresh_per_note_voices(ev.channel, ev.note, false);
      break;
    case ChannelVoiceKind::AssignablePerNote:
      ++skipped_events_;
      break;
    case ChannelVoiceKind::PerNoteManagement:
      apply_per_note_management(
          per_note_pitch_, ev.channel, ev.note, (ev.flags & 0x02u) != 0, (ev.flags & 0x01u) != 0,
          voices_.begin(), voices_.end(),
          [](Voice& v) noexcept -> PerNoteBinding* { return v.active ? &v.per_note : nullptr; });
      refresh_per_note_voices(ev.channel, ev.note, false);
      break;
  }
}

void BuiltinSynth::control_change(uint8_t channel, uint8_t controller, Control32 value) noexcept {
  // Mix controllers keep the full-width value for their float laws; the switches, the channel
  // mode messages and the parameter-number machinery read the 7-bit value.
  const uint8_t value7 = value.u7();
  switch (controller) {
    case 7:  // Channel Volume.
      channel_controls_[channel & 0x0Fu].volume = value;
      refresh_channel_controls(channel);
      break;
    case 10:  // Pan.
      channel_controls_[channel & 0x0Fu].pan = value;
      refresh_channel_controls(channel);
      break;
    case 11:  // Expression.
      channel_controls_[channel & 0x0Fu].expression = value;
      refresh_channel_controls(channel);
      break;
    case 6:  // Data Entry MSB, for the two parameter numbers below.
      // RPN 00 06 is the MPE Configuration Message, which every MPE-compatible
      // device shall support (2.2.1). It is tried first because it is the one
      // that can turn the zone model on.
      if (params_[channel & 0x0Fu].selected_rpn(0, 6)) {
        apply_mcm(channel, value7);
      } else if (params_[channel & 0x0Fu].selected_rpn(0, 0)) {
        // Bend sensitivity, accepted only inside a zone: there it is the
        // zone's own and a value sent to one member reaches every member of it
        // (2.2.5), while outside one this synth keeps its fixed range.
        if (mpe_.apply_bend_sensitivity(channel, static_cast<float>(value7))) {
          for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_expression(ch);
        }
      }
      break;
    case 100:
      params_[channel & 0x0Fu].select_rpn_lsb(value7);
      break;
    case 101:
      params_[channel & 0x0Fu].select_rpn_msb(value7);
      break;
    case 64:  // Damper/sustain pedal: >=64 holds released keys.
      sustain_pedal(channel, value7 >= 64);
      break;
    case 120:  // All Sound Off — immediate silence.
      all_sound_off(channel);
      break;
    case 121:  // Reset All Controllers — damper, bend, pressure, expression.
      reset_all_controllers(channel);
      break;
    case 123:  // All Notes Off — graceful release.
    case 124:  // Omni Off / On also imply notes-off.
    case 125:
      all_notes_off(channel);
      break;
    case 126:  // Mono / Poly mode, which also imply notes-off.
    case 127:
      // Prohibited on a manager channel, where they are ignored outright, and
      // on a member channel they select the zone's mode (2.2.4.3). Outside a
      // zone they keep the channel-mode meaning they have always had here,
      // which is why the all-notes-off stays below them.
      if (mpe_.ignores(channel, MpeIgnorable::kModeMessage)) break;
      mpe_.apply_midi_mode(channel, controller == 126 ? MpeMidiMode::kMono : MpeMidiMode::kPoly);
      all_notes_off(channel);
      break;
    default:
      // Other controllers (RPN/NRPN, etc.) have no effect on this deliberately
      // minimal synth.
      break;
  }
}

void BuiltinSynth::registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept {
  const uint8_t ch = static_cast<uint8_t>(ev.channel & 0x0Fu);
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0) {
    if (ev.index == kRcPitchBendSensitivity) {
      // The semitones of RPN 0/0, the Data Entry MSB the MIDI 1.0 path reads; only a zone holds a
      // configurable range here.
      if (mpe_.apply_bend_sensitivity(ch, static_cast<float>(ev.value.u14() >> 7))) {
        for (uint8_t c = 0; c < 16; ++c) refresh_channel_expression(c);
      }
      return;
    }
    if (ev.index == kRcPerNoteBendSensitivity) {
      per_note_bend_sensitivity_[ch] = ev.value;
      refresh_per_note_voices(ch, 0, true);
      return;
    }
  }
  // Every other RC / AC takes the path its four MIDI 1.0 messages do.
  const Midi1MessageList lowered = midi2_to_midi1_messages(ump);
  for (uint8_t i = 0; i < lowered.count; ++i) {
    control_change(ch, lowered.messages[i].note_number(),
                   Control32::from7(lowered.messages[i].data2_7bit()));
  }
}

void BuiltinSynth::relative_controller(const ChannelVoiceEvent& ev) noexcept {
  const uint8_t ch = static_cast<uint8_t>(ev.channel & 0x0Fu);
  if (ev.kind == ChannelVoiceKind::RelativeRegistered && ev.bank == 0) {
    if (ev.index == kRcPitchBendSensitivity) {
      // The held range in RPN 0/0 form (semitones in the top seven bits), moved and read back as
      // the absolute message would be.
      const auto held = static_cast<uint32_t>(mpe_.bend_sensitivity(ch)) << 25;
      const Control32 moved = add_saturating(Control32::from_raw(held), ev.value);
      if (mpe_.apply_bend_sensitivity(ch, static_cast<float>(moved.u14() >> 7))) {
        for (uint8_t c = 0; c < 16; ++c) refresh_channel_expression(c);
      }
      return;
    }
    if (ev.index == kRcPerNoteBendSensitivity) {
      per_note_bend_sensitivity_[ch] = add_saturating(per_note_bend_sensitivity_[ch], ev.value);
      refresh_per_note_voices(ch, 0, true);
      return;
    }
  }
  ++skipped_events_;  // A parameter this synth does not hold.
}

void BuiltinSynth::refresh_per_note_pitch(Voice& v) noexcept {
  const uint8_t ch = static_cast<uint8_t>(v.channel & 0x0Fu);
  NotePitchRequest req;
  req.note = v.note;
  req.has_attribute_pitch = v.has_attribute_pitch;
  req.attribute_pitch_q7_9 = v.attribute_pitch_q7_9;
  req.per_note = v.per_note.pitch_inputs(per_note_pitch_);
  req.per_note_bend_sensitivity = per_note_bend_sensitivity_[ch];
  // The channel terms stay on this synth's own path below; only the per-note share is taken.
  const double semitones = compose_note_pitch(req).per_note_semitones;
  v.per_note_ratio =
      semitones == 0.0 ? 1.0 : std::pow(2.0, semitones / constants::kSemitonesPerOctave);
  v.phase_inc = v.base_phase_inc * bend_ratio(channel_bend_semitones_[ch]) * v.per_note_ratio;
}

void BuiltinSynth::refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept {
  const uint8_t ch = static_cast<uint8_t>(channel & 0x0Fu);
  for (auto& v : voices_) {
    if (v.active && (v.channel & 0x0Fu) == ch && (all_notes || v.note == note)) {
      refresh_per_note_pitch(v);
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
      osc = static_cast<float>(std::sin(constants::kTwoPiD * p));
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

  // MPE pressure boosts amplitude, so the multiplier is exactly 1.0 (no change)
  // when neither channel nor poly pressure is sent.
  const float pressure =
      synth::combined_aftertouch(channel_pressure_[v.channel & 0x0Fu], v.poly_pressure);
  const float pressure_gain = 1.0f + kPressureModDepth * pressure;
  // Channel volume x expression. Read per sample so a CC ramp is heard as it
  // arrives rather than at the next note.
  const float channel_gain = channel_controls_[v.channel & 0x0Fu].gain;
  return osc * v.env * v.velocity * pressure_gain * channel_gain;
}

void BuiltinSynth::add_frame(float* const* target, int num_channels, int sample, float left,
                             float right) const noexcept {
  if (target == nullptr) return;
  // Mono host: fold both pan legs so a centre-panned voice keeps the level it
  // would have had before the stereo split.
  if (num_channels == 1) {
    if (target[0] != nullptr) target[0][sample] += constants::kInvSqrt2 * (left + right);
    return;
  }
  if (target[0] != nullptr) target[0][sample] += left;
  if (target[1] != nullptr) target[1][sample] += right;
  // Anything past the stereo pair takes the same mono fold-down.
  for (int ch = 2; ch < num_channels; ++ch) {
    if (target[ch] != nullptr) target[ch][sample] += constants::kInvSqrt2 * (left + right);
  }
}

void BuiltinSynth::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  for (int i = 0; i < num_samples; ++i) {
    float left = 0.0f;
    float right = 0.0f;
    for (auto& v : voices_) {
      if (!v.active) continue;
      const rt::PanGains& pan = channel_controls_[v.channel & 0x0Fu].pan_gains;
      const float sample = render_voice_sample(v);
      left += sample * pan.left;
      right += sample * pan.right;
    }
    // ADD into the planar scratch (the engine zero-fills first).
    add_frame(channels, num_channels, i, left * config_.gain, right * config_.gain);
  }
}

bool BuiltinSynth::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                         size_t output_count, int num_channels,
                                         int num_samples) noexcept {
  if (!prepared_ || outputs == nullptr || output_count == 0 || num_channels <= 0 ||
      num_samples <= 0) {
    return false;
  }
  // The first (source id 0) target is the required deterministic fallback.
  if (outputs[0].source_track_id != 0 || outputs[0].channels == nullptr) return false;

  const auto output_for = [&](uint32_t source_track_id) noexcept -> float* const* {
    for (size_t index = 1; index < output_count; ++index) {
      if (outputs[index].source_track_id == source_track_id && outputs[index].channels != nullptr) {
        return outputs[index].channels;
      }
    }
    return outputs[0].channels;
  };

  for (int i = 0; i < num_samples; ++i) {
    for (auto& voice : voices_) {
      if (!voice.active) continue;
      const rt::PanGains& pan = channel_controls_[voice.channel & 0x0Fu].pan_gains;
      const float sample = render_voice_sample(voice) * config_.gain;
      add_frame(output_for(voice.source_track_id), num_channels, i, sample * pan.left,
                sample * pan.right);
    }
  }
  return true;
}

}  // namespace sonare::midi
