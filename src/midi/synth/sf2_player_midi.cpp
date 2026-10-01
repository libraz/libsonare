#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/channel_voice_decode.h"
#include "midi/per_note_state.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

using ::sonare::constants::kCentsPerSemitone;

/// Whether a part holding @p rx_switches receives @p u at all (GsRxSwitch).
/// Control changes answer to their own switch as well as to the master one, and
/// that pair is decided in control_change, where the controller number is known.
/// Polyphonic pressure is deliberately absent: nothing below acts on it, so
/// gating it here would dress a switch that guards nothing as one that does.
bool receives_message(uint32_t rx_switches, const Ump& u) noexcept {
  const auto on = [rx_switches](GsRxSwitch which) {
    return (rx_switches & gs_rx_switch_bit(which)) != 0;
  };
  if (u.is_note_on() || u.is_note_off()) return on(GsRxSwitch::kNoteMessage);
  switch (static_cast<UmpStatus>(u.status_nibble())) {
    case UmpStatus::kProgramChange:
      return on(GsRxSwitch::kProgramChange);
    case UmpStatus::kPitchBend:
      return on(GsRxSwitch::kPitchBend);
    case UmpStatus::kChannelPressure:
      return on(GsRxSwitch::kChannelPressure);
    default:
      return true;
  }
}

/// The receive switch @p controller answers to, or kCount for one no switch
/// names. The named controllers are the map's own list. CC5 PORTAMENTO TIME and
/// CC84 PORTAMENTO CONTROL are not on it and are left ungated: the switch is
/// named for the controller that turns portamento on, and widening it to the two
/// that shape a glide would be reading something the map does not say. Data
/// entry is absent for the opposite reason — it belongs to whichever parameter
/// number is selected, so its switch is decided at the value.
///
/// RX BANK SELECT covers both halves of the bank number, and RX BANK SELECT LSB
/// is not here at all: it does not decide whether CC32 arrives but what it is
/// worth on arrival, so it belongs where the value is stored.
GsRxSwitch rx_switch_for_controller(uint8_t controller) noexcept {
  switch (controller) {
    case 0:
    case 32:
      return GsRxSwitch::kBankSelect;
    case 1:
      return GsRxSwitch::kModulation;
    case 7:
      return GsRxSwitch::kVolume;
    case 10:
      return GsRxSwitch::kPanpot;
    case 11:
      return GsRxSwitch::kExpression;
    case 64:
      return GsRxSwitch::kHold1;
    case 65:
      return GsRxSwitch::kPortamento;
    case 66:
      return GsRxSwitch::kSostenuto;
    case 67:
      return GsRxSwitch::kSoft;
    case 98:
    case 99:
      return GsRxSwitch::kNrpn;
    case 100:
    case 101:
      return GsRxSwitch::kRpn;
    default:
      return GsRxSwitch::kCount;
  }
}

/// Longest CC5 portamento glide, matching the ceiling clamp_synth_patch puts on
/// a patch's own glide_ms.
constexpr float kPortamentoMaxMs = 5000.0f;

/// CC5 Portamento Time -> glide time in ms. GS fixes neither a unit nor a curve
/// for this controller, so the mapping is chosen monotone and zero at 0 (the
/// power-on value, i.e. no glide) with the useful few-hundred-ms range spread
/// over the lower half of the controller.
float portamento_time_ms(uint8_t value) noexcept {
  const float v = static_cast<float>(value & 0x7Fu) / 127.0f;
  return kPortamentoMaxMs * v * v;
}

// Note On attribute #3 carries Pitch 7.9 (M2-104-UM §7.4.15).
constexpr uint8_t kAttributePitch79 = 0x03;
// Registered Per-Note Controller #3 is Pitch 7.25 (M2-104-UM §7.4.12).
constexpr uint8_t kRpncPitch725 = 3;
// Registered Controller indices in bank 0 read straight from the message.
constexpr uint8_t kRcPitchBendSensitivity = 0;
constexpr uint8_t kRcFineTuning = 1;
constexpr uint8_t kRcCoarseTuning = 2;
constexpr uint8_t kRcPerNoteBendSensitivity = 7;

/// Adds a relative controller's two's-complement delta to @p current, saturating at the ends of
/// the 32-bit range rather than wrapping.
Control32 add_saturating(Control32 current, Control32 delta) noexcept {
  const int64_t sum =
      static_cast<int64_t>(current.raw) + static_cast<int64_t>(static_cast<int32_t>(delta.raw));
  const int64_t clamped = std::min<int64_t>(std::max<int64_t>(sum, 0), int64_t{0xFFFFFFFF});
  return Control32::from_raw(static_cast<uint32_t>(clamped));
}

/// The pitch offset from the sample's zone key, in cents. Exactly 0 while the voice sounds at its
/// zone key with no per-note pitch, so a MIDI 1.0 render is untouched.
float per_note_cents(const Sf2PerNoteVoice& state, const ComposedPitch& pitch) noexcept {
  const double semitones =
      static_cast<double>(static_cast<int>(state.binding.note) - static_cast<int>(state.zone_key)) +
      pitch.per_note_semitones;
  return semitones == 0.0 ? 0.0f : static_cast<float>(semitones * kCentsPerSemitone);
}

/// RPN Null (7F 7F): leaves nothing selected, so later data entry is discarded.
/// Selecting an RPN already dropped a selected NRPN — this makes the neutral
/// state explicit rather than an RPN number nothing happens to answer.
void deselect_on_rpn_null(ChannelParamState& params) noexcept {
  if (params.rpn_msb == 0x7Fu && params.rpn_lsb == 0x7Fu) params.reset();
}

}  // namespace

void Sf2Player::bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note,
                              uint8_t attribute_type, uint16_t attribute_data) const noexcept {
  state.binding.bind(channel, note);
  state.has_attribute_pitch = attribute_type == kAttributePitch79;
  state.attribute_pitch_q7_9 = state.has_attribute_pitch ? attribute_data : uint16_t{0};
  state.zone_key = note;
  state.cents = 0.0f;
}

ComposedPitch Sf2Player::compose_per_note(const Sf2PerNoteVoice& state) const noexcept {
  NotePitchRequest req;
  req.note = state.binding.note;
  req.has_attribute_pitch = state.has_attribute_pitch;
  req.attribute_pitch_q7_9 = state.attribute_pitch_q7_9;
  req.per_note = state.binding.pitch_inputs(per_note_pitch_);
  req.per_note_bend_sensitivity = per_note_bend_sensitivity_[state.binding.channel & 0x0Fu];
  // The channel terms stay on this player's own path; only the per-note share is taken.
  return compose_note_pitch(req);
}

void Sf2Player::refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept {
  state.cents = per_note_cents(state, compose_per_note(state));
}

void Sf2Player::refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const auto matches = [&](const Sf2PerNoteVoice& s) noexcept {
    return s.binding.channel == ch && (all_notes || s.binding.note == note);
  };
  for (Sf2Voice& v : pool_) {
    if (v.active && matches(v.per_note)) refresh_per_note_pitch(v.per_note);
  }
  for (size_t i = 0; i < fallback_per_note_.size(); ++i) {
    if (fallback_pool_.data()[i].active && matches(fallback_per_note_[i])) {
      refresh_per_note_pitch(fallback_per_note_[i]);
    }
  }
}

void Sf2Player::manage_per_note(uint8_t channel, uint8_t note, bool detach, bool reset) noexcept {
  // Detach runs on both pools before the row is reset, so a sounding voice of either keeps its
  // values and only later notes see the reset.
  apply_per_note_management(per_note_pitch_, channel, note, detach, false, pool_.begin(),
                            pool_.end(), [](Sf2Voice& v) noexcept -> PerNoteBinding* {
                              return v.active ? &v.per_note.binding : nullptr;
                            });
  apply_per_note_management(
      per_note_pitch_, channel, note, detach, reset, fallback_per_note_.begin(),
      fallback_per_note_.end(),
      [](Sf2PerNoteVoice& s) noexcept -> PerNoteBinding* { return &s.binding; });
  refresh_per_note_voices(channel, note, false);
}

Sf2Player::Portamento Sf2Player::take_portamento(uint8_t channel, uint8_t note) noexcept {
  ChannelState& st = channels_[channel & 0x0Fu];
  const uint8_t key = note & 0x7Fu;
  // CC84 names its own source note and outranks the previous key; CC65 glides
  // from whatever this part played last. Either way the arming is spent here.
  int source = -1;
  if (st.portamento_armed) {
    source = st.portamento_source;
    st.portamento_armed = false;
  } else if (st.portamento && st.last_note <= 127) {
    source = st.last_note;
  }
  st.last_note = key;
  Portamento porta;
  if (source < 0 || source == key) return porta;
  const float glide_ms = portamento_time_ms(st.portamento_time);
  if (glide_ms <= 0.0f || sample_rate_ <= 0.0) return porta;
  porta.cents = static_cast<float>(source - key) * kCentsPerSemitone;
  // Same one-pole sizing as the NativeSynth voice's glide: the pitch lands
  // within ~5% of the target in glide_ms.
  porta.coeff =
      static_cast<float>(std::exp(-3.0 / (static_cast<double>(glide_ms) * 0.001 * sample_rate_)));
  return porta;
}

void Sf2Player::choke_part(uint8_t channel, int note) noexcept {
  const uint8_t part = channel & 0x0Fu;
  // Both pools: a program change moves a part between the SoundFont and the
  // fallback bank, so the note still sounding can be in either one.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == part && (note < 0 || v.note == note)) v.choke(sample_rate_);
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == part && (note < 0 || v.note == note)) v.choke_fast(sample_rate_);
  }
}

void Sf2Player::choke_exclusive_group(uint8_t part, uint8_t group, uint64_t sf2_age_gate) noexcept {
  if (group == 0) return;
  // Both pools, for the reason the note-on comment has always stated: a note
  // that falls through to the modelled floor has to choke a ringing sampled
  // voice of the same group, and vice versa. Each loop walks a fixed-size pool
  // and touches no memory it does not already own, so this stays allocation-free
  // on the audio thread.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.age < sf2_age_gate && v.channel == part &&
        v.params.exclusive_class == group) {
      // Terminated rather than released: SoundFont 2.04 section 8.1.2 ends the
      // previous voice, and an instrument's own release leaves a sustaining
      // sample ringing straight through the strike that was meant to cut it.
      v.choke(sample_rate_);
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == part && v.patch != nullptr && v.exclusive_class == group) {
      // choke_fast, not choke: the latter falls back to the patch's own release,
      // which on a modelled hi-hat rings on well past the strike meant to cut it
      // and leaves the two pools terminating at visibly different speeds.
      v.choke_fast(sample_rate_);
    }
  }
}

const GsUserDrumSource* Sf2Player::user_drum_source(const ChannelState& ch, bool is_drum,
                                                    uint8_t note) const noexcept {
  if (!is_drum) return nullptr;
  const int set = ch.user_drum_set();
  if (set < 0) return nullptr;
  return &user_drum_sources_[static_cast<size_t>(set)][note & 0x7Fu];
}

GsDrumNoteParams Sf2Player::drum_note_params(const ChannelState& ch, bool is_drum,
                                             uint8_t note) const noexcept {
  if (!is_drum) return {};
  const GsDrumNoteParams& live = drum_params_[ch.drum_map_slot()][note & 0x7Fu];
  const int set = ch.user_drum_set();
  if (set < 0) return live;
  return gs_layer_drum_note_params(user_drum_params_[static_cast<size_t>(set)][note & 0x7Fu], live);
}

void Sf2Player::note_on(uint8_t channel, uint8_t note, Velocity16 velocity, uint8_t attribute_type,
                        uint16_t attribute_data, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const ChannelState& ch = channels_[channel & 0x0Fu];
  // GS KEY RANGE (40 1x 1D/1E): a key the part does not receive is not a silent
  // note. It takes no voice, chokes nothing and does not spend the armed
  // portamento, so the test precedes all three as well as both voice banks.
  if (!ch.receives_key(note)) return;
  // GS VELOCITY SENSE (40 1x 1A/1B): the part reshapes the struck velocity, so
  // this precedes both voice banks and the zone velocity ranges a preset
  // switches its layers on — a part made insensitive picks the layer the shaped
  // velocity names rather than the one the wire carried. One conversion, not one
  // per bank. Identity at the power-on 40/40, so an untouched part is bit-exact.
  velocity = gs_velocity_sense(ch.velocity_sense_depth, ch.velocity_sense_offset, velocity);
  const Portamento porta = take_portamento(channel, note);
  // Mono already stops everything the part is sounding, so it subsumes SINGLE.
  if (ch.mono_poly == kGsMonoPolyMono && !ch.is_drum()) {
    choke_part(channel, -1);
  } else if (ch.assign_mode == kGsAssignModeSingle) {
    choke_part(channel, note & 0x7F);
  }
  const uint16_t bank = effective_bank(channel);
  const bool is_drum = bank == kDrumBank;
  // The two per-note pitch offsets: a temperament indexed by the struck key,
  // and PITCH OFFSET FINE, whose Hertz become an interval only once there is a
  // note to work it out against.
  // An absolute pitch (attribute 7.9 or Pitch 7.25) overrides tuning tables, so neither is added
  // to it (M2-104-UM §7.4.15.2).
  Sf2PerNoteVoice per_note;
  bind_per_note(per_note, channel, note, attribute_type, attribute_data);
  const ComposedPitch note_pitch = compose_per_note(per_note);
  const float note_pitch_cents = note_pitch.absolute
                                     ? 0.0f
                                     : gs_scale_tuning_cents(ch.scale_tuning, note) +
                                           gs_pitch_offset_fine_cents(ch.pitch_offset_fine, note);
  const GsDrumNoteParams gd = drum_note_params(ch, is_drum, note);
  // GS RX NOTE ON (41 m8 rr / 21 d8 rr): a note the kit has switched off is not
  // sounded at all, so this precedes every choice of bank below — a note refused
  // here must not reach the model floor either.
  if ((gd.flags & GsDrumNoteParams::kRxNoteOn) != 0 && gd.rx_note_on == 0) return;
  if (config_.synth_fallback && config_.prefer_model_for_modeled_families && !is_drum &&
      gm_program_has_dedicated_model(bank, ch.program)) {
    fallback_note_on(channel, note, velocity, source_track_id, porta, attribute_type,
                     attribute_data);
    return;
  }
  // GS user drum set (21 dn rr): rhythm programs 64 and 65 play a kit the file
  // built note by note, so both the kit this strike sounds and the note within
  // it come from the set rather than from the part and the key.
  const GsUserDrumSource* us = user_drum_source(ch, is_drum, note);
  uint8_t kit_program = us != nullptr ? us->program : ch.program;
  // The selected tone map decides which kits exist, and both banks answer it
  // through the same rule rather than one each: the model floor reads the map in
  // gm_fallback_drum_kit, and here the program the preset is looked up by falls
  // back to Standard for a kit the map does not define. Applied only to the
  // lookup, so the part keeps the program it was given.
  if (is_drum &&
      gs_drum_kit_entry(kit_program, gs_effective_tone_map(ch.bank_msb, ch.bank_lsb)) == nullptr) {
    kit_program = 0;
  }
  // No SoundFont / uncovered program -> the data-free synth floor.
  const int preset_idx = soundfont_ != nullptr ? resolve_preset(bank, kit_program) : -1;
  if (preset_idx < 0) {
    if (config_.synth_fallback) {
      fallback_note_on(channel, note, velocity, source_track_id, porta, attribute_type,
                       attribute_data);
    }
    return;
  }
  const Sf2Preset& preset = soundfont_->presets()[static_cast<size_t>(preset_idx)];
  const auto& instruments = soundfont_->instruments();
  const float* pool_data = soundfont_->sample_pool().data();
  const float vel_gain = sf2_velocity_gain(velocity);

  const Sf2Zone* preset_global =
      !preset.zones.empty() && preset.zones[0].is_global() ? &preset.zones[0] : nullptr;

  // SoundFont 2.04 section 8.1.2 scopes exclusiveClass to notes that are ALREADY
  // sounding, so the layers this one note-on allocates must not choke each
  // other — a stereo hi-hat's two legs, or a layered kit piece, share one class
  // by design. Voice ages are monotonic, so every voice allocated below carries
  // an age at or above this mark and is excluded from the choke.
  const uint64_t age_before_note_on = pool_.next_age();

  // GS PLAY NOTE NUMBER (41 m1 rr): the note whose SOUND this strike plays.
  // Zone selection and the resolved params follow it; the per-note slab, the
  // choke and the voice's own note stay on the struck note. The map's edit sits
  // ON TOP of the user set's source note, which is the stored kit it edits.
  uint8_t sound_note = gs_user_drum_sound_note(us, note);
  if ((gd.flags & GsDrumNoteParams::kPlayNote) != 0) sound_note = gd.play_note;
  // The sample zone follows the integer part of an absolute pitch (§7.4.15.3), so a far
  // transposition does not stretch one recording across the keyboard.
  if (note_pitch.absolute) {
    sound_note = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::floor(static_cast<double>(note) + note_pitch.per_note_semitones)), 0,
        127));
    // A play-note substitution plays at its own root, so only a pitch-chosen zone moves the key
    // the per-note offset is measured from.
    per_note.zone_key = sound_note;
  }
  per_note.cents = per_note_cents(per_note, note_pitch);

  bool has_renderable_zone = false;
  for (const Sf2Zone& pzone : preset.zones) {
    if (pzone.is_global() || !pzone.matches(sound_note, velocity)) continue;
    if (pzone.instrument < 0 || static_cast<size_t>(pzone.instrument) >= instruments.size()) {
      continue;
    }
    const Sf2Instrument& inst = instruments[static_cast<size_t>(pzone.instrument)];
    const Sf2Zone* inst_global =
        !inst.zones.empty() && inst.zones[0].is_global() ? &inst.zones[0] : nullptr;
    for (const Sf2Zone& izone : inst.zones) {
      if (izone.is_global() || !izone.matches(sound_note, velocity)) continue;
      if (izone.sample < 0 || static_cast<size_t>(izone.sample) >= soundfont_->samples().size()) {
        continue;
      }
      const Sf2Sample& sample = soundfont_->samples()[static_cast<size_t>(izone.sample)];
      if (sample.is_rom() || !valid_sf2_sample_rate(sample.sample_rate) ||
          sample.end <= sample.start || sample.end > soundfont_->sample_pool().size()) {
        continue;
      }

      // Stack generators: defaults -> instrument global -> instrument zone
      // (absolute), then + preset global + preset zone (relative).
      Sf2GenSet gens;
      if (inst_global != nullptr) gens.apply_absolute(*inst_global);
      gens.apply_absolute(izone);
      if (preset_global != nullptr) gens.add_relative(*preset_global);
      gens.add_relative(pzone);

      Sf2VoiceParams params =
          resolve_voice_params(gens, sample, sound_note, velocity, sample_rate_);
      if (params.end <= params.start || params.end > soundfont_->sample_pool().size() ||
          !std::isfinite(params.pitch_increment) || params.pitch_increment <= 0.0) {
        continue;
      }
      has_renderable_zone = true;

      // GS layer: NRPN part edits + per-note drum-kit overrides.
      apply_gs_part_params(params, ch.gs);
      apply_gs_drum_params(params, gd);
      // Both offsets are the struck key's, not the sounding note's: a
      // temperament belongs to the keyboard, so a kit piece PLAY NOTE NUMBER
      // redirected to keeps the tuning of the key that asked for it.
      if (note_pitch_cents != 0.0f) {
        params.pitch_increment *= std::exp2(static_cast<double>(note_pitch_cents) / 1200.0);
      }
      // A TVF CUTOFF CONTROL destination engages the filter the way a TONE
      // MODIFY cutoff does, and on the part rather than on the controller: a
      // controller rises after the note-on as often as before it, and a
      // bypassed filter cannot open. Either source is enough on its own.
      if (gs_part_has_filter_destination(ch.ctrl_dest)) params.filter_bypass = false;

      // Exclusive class: choke same-group voices on this channel (hi-hats), in
      // either pool. The age gate keeps a later zone of this same note-on from
      // choking an earlier zone's voice.
      choke_exclusive_group(channel & 0x0Fu, params.exclusive_class, age_before_note_on);

      Sf2Voice* voice = pool_.allocate(channel & 0x0Fu, note, source_track_id);
      if (voice == nullptr) continue;
      voice->start(pool_data, params, sample_rate_, vel_gain);
      voice->glide_cents = porta.cents;
      voice->glide_coeff = porta.coeff;
      voice->per_note = per_note;
    }
  }
  if (!has_renderable_zone && config_.synth_fallback) {
    fallback_note_on(channel, note, velocity, source_track_id, porta, attribute_type,
                     attribute_data);
  }
}

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                                 uint32_t source_track_id, Portamento porta, uint8_t attribute_type,
                                 uint16_t attribute_data) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const uint16_t bank = effective_bank(channel);
  const GsToneMap tone_map = gs_effective_tone_map(ch.bank_msb, ch.bank_lsb);
  const bool is_drum = bank == kDrumBank;
  // The per-note GS edits, read before the patch: PLAY NOTE NUMBER picks which
  // kit piece answers and ASSIGN GROUP which group it belongs to, and both are
  // needed before the choke below, let alone the voice.
  const GsDrumNoteParams gd = drum_note_params(ch, is_drum, note);
  // The user drum set under them: it says which stored kit and which note within
  // it this strike sounds, and the map's edits above are what edits that.
  const GsUserDrumSource* us = user_drum_source(ch, is_drum, note);
  uint8_t sound_note = gs_user_drum_sound_note(us, note);
  if ((gd.flags & GsDrumNoteParams::kPlayNote) != 0) sound_note = gd.play_note;
  const NativeSynthPatch& patch =
      is_drum ? gm_fallback_drum_patch(sound_note) : gm_fallback_patch(bank, ch.program, tone_map);
  uint8_t exclusive_class = is_drum ? patch.percussion.exclusive_class : 0;
  if ((gd.flags & GsDrumNoteParams::kAssignGroup) != 0) {
    exclusive_class = gd.assign_group & 0x7Fu;
  }
  // GM kit exclusive/mute groups (hi-hats etc.): choke the ringing group voice
  // on this channel before allocating the new strike. Compared against the
  // group each voice was STARTED in, which an ASSIGN GROUP write moves away
  // from the kit piece's own. The engine a voice sounds through is not part of
  // the comparison: a sampled hi-hat belongs to the same group as a modelled
  // one and has to choke it.
  // next_age() excludes nothing: this path is reached only when the SoundFont
  // side found no renderable zone, so no voice of this note-on exists yet.
  choke_exclusive_group(channel & 0x0Fu, exclusive_class, pool_.next_age());
  NativeSynthVoice* voice = fallback_pool_.allocate(channel & 0x0Fu, note, source_track_id);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - fallback_pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!fallback_ks_buffers_.empty()) {
    voice->ks.attach(
        fallback_ks_buffers_.data() + static_cast<size_t>(voice_index) * 3 * fallback_ks_capacity_,
        fallback_ks_capacity_);
  }
  if (!fallback_piano_buffers_.empty()) {
    voice->piano.attach(fallback_piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                             kMaxPianoStrings *
                                                             fallback_piano_string_capacity_,
                        fallback_piano_string_capacity_);
  }
  if (!fallback_pipe_organ_buffers_.empty()) {
    // Each rank holds TWO spans (bore + jet), so the per-voice stride is
    // 2 * kMaxPipeRanks spans; without the 2, adjacent voices' slabs overlap
    // and simultaneous (legato) organ voices corrupt each other's bores.
    voice->pipe_organ.attach(
        fallback_pipe_organ_buffers_.data() +
            static_cast<size_t>(voice_index) * 2 * kMaxPipeRanks * fallback_pipe_organ_capacity_,
        fallback_pipe_organ_capacity_);
  }
  if (!fallback_bowed_buffers_.empty()) {
    voice->bowed_string.attach(fallback_bowed_buffers_.data() +
                                   static_cast<size_t>(voice_index) * 3 * fallback_bowed_capacity_,
                               fallback_bowed_capacity_);
  }
  if (!fallback_reed_buffers_.empty()) {
    voice->reed.attach(
        fallback_reed_buffers_.data() + static_cast<size_t>(voice_index) * fallback_reed_capacity_,
        fallback_reed_capacity_);
  }
  if (!fallback_brass_buffers_.empty()) {
    voice->brass.attach(fallback_brass_buffers_.data() +
                            static_cast<size_t>(voice_index) * fallback_brass_capacity_,
                        fallback_brass_capacity_);
  }
  if (!fallback_flute_buffers_.empty()) {
    voice->flute.attach(fallback_flute_buffers_.data() +
                            static_cast<size_t>(voice_index) * 2 * fallback_flute_capacity_,
                        fallback_flute_capacity_);
  }
  if (!fallback_plucked_string_buffers_.empty()) {
    voice->plucked_string.attach(
        fallback_plucked_string_buffers_.data() +
            static_cast<size_t>(voice_index) * fallback_plucked_string_capacity_,
        fallback_plucked_string_capacity_);
  }
  if (!fallback_harpsichord_buffers_.empty()) {
    voice->harpsichord.attach(fallback_harpsichord_buffers_.data() +
                                  static_cast<size_t>(voice_index) * fallback_harpsichord_stride_,
                              fallback_harpsichord_capacity_);
  }
  // GS drum-kit variation: the drum channel's program picks the kit (Room /
  // Power / TR-808 / ...); melodic fallback voices pass 0 (no kit).
  // A user drum set names the kit per note, so the program that picks the
  // variation is the note's source rather than the part's.
  const uint8_t kit_program = us != nullptr ? us->program : ch.program;
  const uint8_t drum_kit = is_drum ? gm_fallback_drum_kit(kit_program, tone_map) : 0;
  // GS per-note drum edits (pitch coarse / TVA level / absolute pan / the three
  // send multiplicands), mirroring apply_gs_drum_params for the model floor: a
  // parameter must not do something different because this bank answered.
  DrumVoiceMod drum_mod;
  if (is_drum) {
    if ((gd.flags & GsDrumNoteParams::kPitch) != 0 && gd.pitch_coarse != 0) {
      drum_mod.pitch_ratio = std::exp2(static_cast<float>(gd.pitch_coarse) / 12.0f);
    }
    if ((gd.flags & GsDrumNoteParams::kLevel) != 0) {
      const float v = static_cast<float>(gd.level & 0x7Fu) / 127.0f;
      drum_mod.level_gain = v * v;  // same square law as CC7 / velocity
    }
    if ((gd.flags & GsDrumNoteParams::kPan) != 0) {
      drum_mod.pan_units = (static_cast<float>(gd.pan & 0x7Fu) - 64.0f) / 63.0f * 500.0f;
    }
    if ((gd.flags & GsDrumNoteParams::kReverb) != 0) {
      drum_mod.reverb_scale = static_cast<float>(gd.reverb & 0x7Fu) / 127.0f;
    }
    if ((gd.flags & GsDrumNoteParams::kChorus) != 0) {
      drum_mod.chorus_scale = static_cast<float>(gd.chorus & 0x7Fu) / 127.0f;
    }
    if ((gd.flags & GsDrumNoteParams::kDelay) != 0) {
      drum_mod.delay_scale = static_cast<float>(gd.delay & 0x7Fu) / 127.0f;
    }
    // Resolved above, beside the choke that had to read it first.
    drum_mod.exclusive_class = static_cast<int16_t>(exclusive_class);
  }
  // The note the voice is STARTED at. Both PLAY NOTE NUMBER and a user set's
  // source note move it, and the patch above is already chosen by it, so it is
  // carried from the resolved note rather than from either of them: leaving it
  // on the struck note gives the right kit piece at the wrong pitch, which
  // renders plausibly and is not the note that was asked for.
  if (sound_note != (note & 0x7Fu)) {
    drum_mod.play_note = static_cast<int16_t>(sound_note);
  }
  // Drawbar percussion spends the channel's charge; note_off recharges it once
  // the last key is up.
  const bool organ_percussion = patch.mode == SynthEngineMode::kAdditive &&
                                patch.additive.percussion_harmonic >= 2 && ch.percussion_armed;
  if (organ_percussion) channels_[channel & 0x0Fu].percussion_armed = false;
  // GS melodic part edits (40 1x 30 TONE MODIFY and the part NRPNs), through
  // the same conversion the SoundFont bank's apply_gs_part_params uses: a
  // parameter must not do something different because this bank answered.
  GsPartMod part_mod = gs_part_mod(ch.gs);
  // SCALE TUNING and PITCH OFFSET FINE are per note where the other eight are
  // per part, so they are set on the way past rather than built with them; the
  // struck key indexes both, as it does on the SoundFont bank.
  Sf2PerNoteVoice per_note;
  bind_per_note(per_note, channel, note, attribute_type, attribute_data);
  const ComposedPitch note_pitch = compose_per_note(per_note);
  per_note.cents = per_note_cents(per_note, note_pitch);
  // An absolute pitch overrides tuning tables (M2-104-UM §7.4.15.2).
  part_mod.pitch_cents = note_pitch.absolute
                             ? 0.0f
                             : gs_scale_tuning_cents(ch.scale_tuning, note) +
                                   gs_pitch_offset_fine_cents(ch.pitch_offset_fine, note);
  // Same reason the SoundFont bank engages its filter here: the offset itself
  // arrives per sample from the controller, so what the note-on has to settle
  // is only whether there is a filter for it to reach.
  if (gs_part_has_filter_destination(ch.ctrl_dest)) part_mod.filter_edited = true;
  voice->start(patch, sample_rate_, velocity, voice_index, 0.0f, ch.una_corda, drum_kit, drum_mod,
               organ_percussion, part_mod);
  // This host passes no glide_from_hz, so start() leaves the voice's glide at
  // rest; the CC5/65/84 portamento is what drives it here.
  voice->glide_cents = porta.cents;
  voice->glide_coeff = porta.coeff;
  if (voice_index < fallback_per_note_.size()) fallback_per_note_[voice_index] = per_note;

  // Pipe-organ patches share a per-part wind chest (tremulant / wind sag).
  // Re-prepare only when the parameters change so the tremulant phase stays
  // continuous across notes.
  const uint8_t part = channel & 0x0Fu;
  if (patch.mode == SynthEngineMode::kPipeOrgan) {
    FallbackWindParams& wp = fallback_wind_params_[part];
    const float rate = patch.pipe_organ.tremulant_rate_hz;
    const float depth = patch.pipe_organ.tremulant_depth;
    const float sag = patch.pipe_organ.wind_sag;
    if (wp.rate != rate || wp.depth != depth || wp.sag != sag) {
      wp = {rate, depth, sag};
      fallback_wind_[part].prepare(sample_rate_, rate, depth, sag);
    }
  }

  // Bus-level body resonators (the components the NativeSynth host folds in):
  // the piano's modal soundboard + pedal-gated sympathetic bank, and the
  // plucked-string open-string halo. Each kind has its own body on the part and
  // is prepared once, at its first note; a different soundboard mix only
  // re-states the return level, since earlier notes may still ring through it.
  FallbackBodyKind body_kind{};
  if (fallback_body_kind(patch, &body_kind)) {
    FallbackBodyState& state = fallback_body_[part];
    FallbackBody& body = state.bodies[static_cast<size_t>(body_kind)];
    if (body_kind == FallbackBodyKind::kPiano) {
      if (!body.prepared) {
        fallback_board_[part].prepare(sample_rate_, patch.piano.soundboard);
        fallback_reso_[part].prepare(sample_rate_);
      } else if (state.soundboard_mix != patch.piano.soundboard) {
        fallback_board_[part].set_mix(patch.piano.soundboard);
      }
      state.soundboard_mix = patch.piano.soundboard;
      // The blow into the structure, which the board is struck with once rather
      // than driven by. After any prepare() above, which clears the network.
      fallback_board_[part].strike(voice->piano.case_strike());
      fallback_board_[part].strike_board(voice->piano.board_strike());
    } else if (!body.prepared) {
      fallback_halo_[part].prepare_guitar_sympathetic(sample_rate_);
    }
    body.prepared = true;
  }
}

bool Sf2Player::fallback_body_kind(const NativeSynthPatch& patch, FallbackBodyKind* kind) noexcept {
  if (patch.mode == SynthEngineMode::kPiano) {
    *kind = FallbackBodyKind::kPiano;
    return true;
  }
  if (patch.mode == SynthEngineMode::kKarplusStrong && patch.ks.sympathetic) {
    *kind = FallbackBodyKind::kGuitarHalo;
    return true;
  }
  return false;
}

void Sf2Player::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (Sf2Voice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();
      } else if (st.sustain_level < 127 && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp(static_cast<float>(127 - st.sustain_level) / 63.0f);
      }
    }
  }
  recharge_percussion(ch);
}

void Sf2Player::recharge_percussion(uint8_t ch) noexcept {
  // The keys, not the voices: a percussion charge returns when the player's
  // hands leave the manual, and a released note whose tail is still sounding
  // (or whose damper the sustain pedal is holding) has left it.
  for (const Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  for (const NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  channels_[ch].percussion_armed = true;
}

void Sf2Player::sustain_pedal(uint8_t channel, bool down) noexcept {
  sustain_cc(channel, down ? 127 : 0);
}

void Sf2Player::sustain_cc(uint8_t channel, uint8_t value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the piano strings,
    // so held-but-released fallback notes are damped rather than ringing
    // freely. Key-down and sostenuto-captured notes keep their dampers off.
    if (value < 127) {
      const float strength = static_cast<float>(127 - value) / 63.0f;
      for (NativeSynthVoice& v : fallback_pool_) {
        if (v.active && v.channel == ch && !v.key_down && !v.sostenuto && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPiano) {
          v.piano.damp(strength);
        }
      }
    }
    return;
  }
  if (!was_down) return;
  // Pedal up: the dampers fall on every held-but-released note. A
  // sostenuto-captured note stays held even when the sustain pedal lifts.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void Sf2Player::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  if (st.sostenuto_down == down) return;
  st.sostenuto_down = down;
  if (down) {
    // Capture exactly the keys held at the down edge.
    for (Sf2Voice& v : pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    return;
  }
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
}

void Sf2Player::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
  recharge_percussion(ch);
}

void Sf2Player::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch) {
      v.env.kill();
      v.active = false;
      v.releasing = false;
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
  recharge_percussion(ch);
  // All Sound Off means silence NOW, and the part's bus resonators are part of
  // its output: the piano soundboard and sympathetic bank ring for ~1.5 s and
  // the wind chest holds its tremulant/sag state, so killing the voices alone
  // would leak an audible wash past the stop. These are per-part, so clearing
  // them touches no other channel; each body's tuning is kept so the next
  // note-on does not re-prepare.
  const size_t part = ch;
  fallback_board_[part].reset();
  fallback_reso_[part].reset();
  fallback_halo_[part].reset();
  fallback_wind_[part].reset();
  for (FallbackBody& body : fallback_body_[part].bodies) body.ringout = 0;
  if (pool_.active_count() == 0 && fallback_pool_.active_count() == 0) {
    // Bus-wide (every part feeds one mix), so only once nothing is sounding.
    dc_x1_ = {};
    dc_y1_ = {};
  }
}

void Sf2Player::apply_nrpn(uint8_t channel, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const int8_t offset = static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64);
  if (st.params.nrpn_msb == 0x01) {
    // GS part parameters (relative offsets onto the SoundFont generators).
    switch (st.params.nrpn_lsb) {
      case 0x08:
        st.gs.vibrato_rate = offset;
        break;
      case 0x09:
        st.gs.vibrato_depth = offset;
        break;
      case 0x0A:
        st.gs.vibrato_delay = offset;
        break;
      case 0x20:
        st.gs.tvf_cutoff = offset;
        break;
      case 0x21:
        st.gs.tvf_resonance = offset;
        break;
      case 0x63:
        st.gs.eg_attack = offset;
        break;
      case 0x64:
        st.gs.eg_decay = offset;
        break;
      case 0x66:
        st.gs.eg_release = offset;
        break;
      default:
        break;
    }
    return;
  }
  // A melodic part has no map for the edit to land in, so the write is dropped
  // here rather than reaching a slab — the same guard as before the re-key.
  const bool is_drum = effective_bank(ch) == kDrumBank;
  if (!is_drum) return;
  // GS drum-kit NRPNs: msb selects the parameter, lsb is the drum note. The
  // edit is stored under the writing part's map, so every part on that map
  // sees it.
  GsDrumNoteParams& d = drum_params_[st.drum_map_slot()][st.params.nrpn_lsb & 0x7Fu];
  switch (st.params.nrpn_msb) {
    case 0x18:
      d.pitch_coarse = offset;
      d.flags |= GsDrumNoteParams::kPitch;
      break;
    case 0x1A:
      d.level = value;
      d.flags |= GsDrumNoteParams::kLevel;
      break;
    case 0x1C:
      d.pan = value;
      d.flags |= GsDrumNoteParams::kPan;
      break;
    case 0x1D:
      d.reverb = value;
      d.flags |= GsDrumNoteParams::kReverb;
      break;
    case 0x1E:
      d.chorus = value;
      d.flags |= GsDrumNoteParams::kChorus;
      break;
    case 0x1F:
      d.delay = value;
      d.flags |= GsDrumNoteParams::kDelay;
      break;
    default:
      break;
  }
}

void Sf2Player::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep program/bank/volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = Control32::from7(0);
  st.expression = Control32::from7(127);
  st.pitch_bend = Bend32::center();
  // Channel aftertouch is an RP-015 performance controller and is the second
  // source the controller-destination block reads, so it is cleared here rather
  // than left latched: refresh_channel_mod below scales every destination by
  // where its source sits, and a stale pressure kept driving them after CC121.
  st.channel_pressure = Control32::from_raw(0);
  st.params.reset();
  sustain_cc(ch, 0);
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  // RP-015 lists Portamento On/Off among the controllers it turns off, and a
  // pending CC84 arming goes with it; Portamento Time is a setting, not a
  // performance controller, and is left alone.
  st.portamento = false;
  st.portamento_armed = false;
  refresh_channel_mod(ch);
}

void Sf2Player::control_change(uint8_t channel, uint8_t controller, Control32 value32) noexcept {
  // Volume, expression and pan keep the full-width value for their float laws; every switch, the
  // controller record and the parameter-number machinery read the 7-bit value.
  const uint8_t value = value32.u7();
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // GS RX switches (40 1x 06, and 0B-12 for the eight named controllers): a part
  // that does not receive the message never sees the value, so this precedes
  // even the position record below — an assignable source pointed at a
  // controller the part refuses reads where that controller last was, not where
  // an unreceived message would have put it.
  //
  // 120-127 are MIDI's channel mode messages rather than controllers, and the
  // map has no switch over them: RX CONTROL CHANGE sits beside one switch per
  // named controller, all of them below 120. Leaving them through also keeps
  // CC126/127's alias onto 40 1x 13 unconditional, and keeps a panic able to
  // stop a note on a part that has stopped taking controllers.
  if (controller < 120 && !st.receives(GsRxSwitch::kControlChange)) return;
  const GsRxSwitch gated = rx_switch_for_controller(controller);
  if (gated != GsRxSwitch::kCount && !st.receives(gated)) return;
  if (controller == 6 || controller == 38) {
    // Data entry carries whichever parameter number is selected, so the switch
    // that applies is the selection's. With neither selected the value reaches
    // nothing anyway, and RPN is the harmless attribution.
    if (!st.receives(st.params.selected_nrpn() ? GsRxSwitch::kNrpn : GsRxSwitch::kRpn)) return;
  }
  // Every controller's position is recorded before the switch decides what this
  // one means, because an assignable source of the 40 2x block names a
  // controller by number and has to read where it sits whatever else it does.
  // The power-on numbers are 16 and 17, the General Purpose controllers, which
  // nothing below handles; a file is free to point a source at one that it does.
  st.cc_position[controller & 0x7Fu] = value32;
  for (const uint8_t number : st.assignable_cc) {
    if (controller == (number & 0x7Fu)) {
      refresh_channel_mod(ch);
      break;
    }
  }
  switch (controller) {
    case 0:  // Bank select MSB (GS variation bank)
      st.bank_msb = value;
      // The fallback ambience floor is keyed on (effective bank, program), so
      // a bank change moves it just like a program change does — and so is the
      // rig the bank binds.
      refresh_channel_mod(ch);
      refresh_part_rig(ch);
      break;
    case 32:  // Bank select LSB
      // RX BANK SELECT LSB off reads the byte as 00 rather than refusing it, so
      // a part whose map was moved goes back to the module's own rather than
      // staying where the last accepted message left it. The one switch in the
      // set that writes something when it is off.
      st.bank_lsb = st.receives(GsRxSwitch::kBankSelectLsb) ? value : 0;
      refresh_channel_mod(ch);
      refresh_part_rig(ch);
      break;
    case 1:
      st.mod_wheel = value32;
      refresh_channel_mod(ch);
      break;
    case 5:  // Portamento time
      st.portamento_time = value;
      break;
    case 6:  // Data entry MSB -> active RPN or GS NRPN
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_rpn(0, 1)) {
        // Master fine tuning: 14-bit, MSB is the top 7 bits.
        st.pitch_fine_tune = static_cast<uint16_t>((static_cast<uint16_t>(value) << 7) |
                                                   (st.pitch_fine_tune & 0x7Fu));
      } else if (st.params.selected_rpn(0, 2)) {
        // Master coarse tuning: MSB only, centre 0x40, +-24 semitones. A value
        // past the defined range is clamped rather than ignored — it is an
        // out-of-range value, not a malformed message, and clamping keeps the
        // parameter continuous at the boundary instead of leaving a stale one
        // no later message corrects.
        st.pitch_coarse_tune =
            static_cast<int8_t>(std::clamp(static_cast<int>(value & 0x7Fu) - 64, -24, 24));
      } else if (st.params.selected_nrpn()) {
        apply_nrpn(ch, value);
      }
      break;
    case 38:  // Data entry LSB
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents =
            100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_rpn(0, 1)) {
        st.pitch_fine_tune =
            static_cast<uint16_t>((st.pitch_fine_tune & 0x3F80u) | (value & 0x7Fu));
      }
      // Master coarse tuning has no LSB: the manual defines it as MSB only.
      break;
    case 7:
      st.volume = value32;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = value32;
      refresh_channel_mod(ch);
      break;
    case 11:
      st.expression = value32;
      refresh_channel_mod(ch);
      break;
    case 91:
      st.reverb_send = value;
      refresh_channel_mod(ch);
      break;
    case 93:
      st.chorus_send = value;
      refresh_channel_mod(ch);
      break;
    case 94:  // GS delay send (no SF2 generator; channel-level only)
      st.delay_send = value;
      refresh_channel_mod(ch);
      break;
    // TONE MODIFY 1-8 by controller. The manual annotates each of the eight as
    // one parameter reachable three ways, so these land in the storage the
    // 40 1x 30-37 block and the 01 08/09/0A/20/21/63/64/66 NRPNs already write.
    case 71:
    case 72:
    case 73:
    case 74:
    case 75:
    case 76:
    case 77:
    case 78: {
      // The eight controllers are contiguous but not in address order.
      static constexpr uint8_t kToneModifyIndex[8] = {3, 6, 4, 2, 5, 0, 1, 7};
      gs_apply_tone_modify(st.gs, kToneModifyIndex[controller - 71u], value);
      break;
    }
    case 98:  // NRPN LSB
      st.params.select_nrpn_lsb(value);
      break;
    case 99:  // NRPN MSB
      st.params.select_nrpn_msb(value);
      break;
    case 100:  // RPN LSB
      st.params.select_rpn_lsb(value);
      deselect_on_rpn_null(st.params);
      break;
    case 101:  // RPN MSB
      st.params.select_rpn_msb(value);
      deselect_on_rpn_null(st.params);
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 65:  // Portamento on/off
      st.portamento = value >= 64;
      break;
    case 66:
      sostenuto_pedal(ch, value >= 64);
      break;
    case 67:
      // Una corda: shifts the piano fallback action for notes STARTED while
      // down (the hammer strikes fewer strings); sample-playback voices keep
      // their recorded voicing.
      st.una_corda = value >= 64;
      break;
    case 84:
      // Portamento control: the next note-on on this part glides from the
      // source note this message carries, once, whatever CC65 says.
      st.portamento_source = value & 0x7Fu;
      st.portamento_armed = true;
      break;
    case 120:
      all_sound_off(ch);
      break;
    case 121:
      reset_controllers(ch);
      break;
    case 123:
    case 124:
    case 125:
      all_notes_off(ch);
      break;
    case 126:
    case 127:
      // Mono Mode On / Poly Mode On. Both are All Notes Off as well, which is
      // why they keep the case above's call. They write the one storage
      // location GS SysEx 40 1x 13 writes (docs/gs.md); CC126's data byte is a
      // voice count and any value of it still means mono.
      st.mono_poly = controller == 126 ? kGsMonoPolyMono : kGsMonoPolyPoly;
      all_notes_off(ch);
      break;
    default:
      break;
  }
}

void Sf2Player::registered_controller(uint8_t ch, const Ump& ump,
                                      const ChannelVoiceEvent& ev) noexcept {
  ChannelState& st = channels_[ch];
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0) {
    // The RPN gesture answers to the same two switches it does in MIDI 1.0 form.
    const bool received = st.receives(GsRxSwitch::kControlChange) && st.receives(GsRxSwitch::kRpn);
    if (ev.index == kRcPitchBendSensitivity) {
      if (!received) return;
      // Semitones in the top seven bits, cents below them: the RPN 0/0 data entry MSB and LSB.
      const uint16_t v14 = ev.value.u14();
      st.bend_range_cents = 100.0f * static_cast<float>(v14 >> 7) + static_cast<float>(v14 & 0x7Fu);
      refresh_channel_mod(ch);
      return;
    }
    if (ev.index == kRcPerNoteBendSensitivity) {
      if (!received) return;
      per_note_bend_sensitivity_[ch] = ev.value;
      refresh_per_note_voices(ch, 0, true);
      return;
    }
  }
  // Every other RC / AC takes the path its four MIDI 1.0 messages do, so selection, data entry
  // and the RX switches treat it the same.
  const Midi1MessageList lowered = midi2_to_midi1_messages(ump);
  for (uint8_t i = 0; i < lowered.count; ++i) {
    control_change(ch, lowered.messages[i].note_number(),
                   Control32::from7(lowered.messages[i].data2_7bit()));
  }
  return;
}

bool Sf2Player::relative_controller(uint8_t ch, const ChannelVoiceEvent& ev) noexcept {
  if (ev.kind != ChannelVoiceKind::RelativeRegistered || ev.bank != 0) return false;
  ChannelState& st = channels_[ch];
  const bool received = st.receives(GsRxSwitch::kControlChange) && st.receives(GsRxSwitch::kRpn);
  switch (ev.index) {
    case kRcPitchBendSensitivity: {
      if (!received) return true;
      // The held range in RPN 0/0 form, moved and read back as the absolute message would be.
      const int semitones = std::clamp(static_cast<int>(st.bend_range_cents / 100.0f), 0, 127);
      const int cents = std::clamp(
          static_cast<int>(st.bend_range_cents - 100.0f * static_cast<float>(semitones) + 0.5f), 0,
          127);
      const auto held = static_cast<uint32_t>((semitones << 7) | cents) << 18;
      const uint16_t v14 = add_saturating(Control32::from_raw(held), ev.value).u14();
      st.bend_range_cents = 100.0f * static_cast<float>(v14 >> 7) + static_cast<float>(v14 & 0x7Fu);
      refresh_channel_mod(ch);
      return true;
    }
    case kRcFineTuning: {
      if (!received) return true;
      const auto held = static_cast<uint32_t>(st.pitch_fine_tune) << 18;
      st.pitch_fine_tune = add_saturating(Control32::from_raw(held), ev.value).u14();
      return true;
    }
    case kRcCoarseTuning: {
      if (!received) return true;
      const auto held = static_cast<uint32_t>(st.pitch_coarse_tune + 64) << 25;
      const int moved = static_cast<int>(add_saturating(Control32::from_raw(held), ev.value).u7());
      st.pitch_coarse_tune = static_cast<int8_t>(std::clamp(moved - 64, -24, 24));
      return true;
    }
    case kRcPerNoteBendSensitivity:
      if (!received) return true;
      per_note_bend_sensitivity_[ch] = add_saturating(per_note_bend_sensitivity_[ch], ev.value);
      refresh_per_note_voices(ch, 0, true);
      return true;
    default:
      return false;
  }
}

void Sf2Player::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  // Drain direct writes first, so one queued before a same-frame event applies first.
  drain_direct_system_patch();
  drain_direct_gs_nodes();
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    // SysEx events arrive with a control-thread-resolved payload view (the UMP
    // itself only carries a handle): feed the GS layer so GS Reset / GM System
    // On / "use for rhythm part" inside an arrangement take effect.
    if (event.sysex_payload != nullptr && event.sysex_payload_size > 0) {
      // A null token (prepared before prepare()) takes the unprepared handle_sysex path.
      const PreparedSysEx* prepared = dynamic_cast<const PreparedSysEx*>(event.prepared_sysex);
      if (prepared != nullptr && prepared_owner_identity_ != nullptr &&
          prepared->owner_identity != nullptr &&
          prepared->owner_identity.get() == prepared_owner_identity_.get()) {
        apply_prepared_gs_delta(*prepared, event.sysex_payload, event.sysex_payload_size, true);
      } else {
        handle_sysex(event.sysex_payload, event.sysex_payload_size);
      }
    }
    return;
  }
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) {
    ++skipped_events_;  // Reserved status.
    return;
  }
  // GS RX CHANNEL (40 1x 02): which parts a channel message reaches. At the
  // power-on map this word carries the channel's own part and nothing else, so
  // the loop is a direct index until a file says otherwise. Several parts on one
  // channel is what real files use the parameter for — a layer — and a part set
  // to RX CHANNEL OFF is in no word at all.
  uint16_t parts = rx_parts_[u.channel() & 0x0Fu];
  if (parts == 0) return;
  bool skipped = false;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((parts & (1u << ch)) == 0) continue;
    // GS RX switches (40 1x 03-12): whether the part receives this class of
    // message at all. A dropped message leaves the part holding what the last
    // received one left, which is not the same as receiving a neutral value.
    // Polyphonic pressure has no branch below, so its switch guards nothing.
    if (!receives_message(channels_[ch].rx_switches, u)) continue;
    switch (ev.kind) {
      case ChannelVoiceKind::NoteOn:
        note_on(ch, ev.note, ev.velocity, ev.index, ev.attribute_data, event.source_track_id);
        break;
      case ChannelVoiceKind::NoteOff:
        note_off(ch, ev.note, event.source_track_id);
        break;
      case ChannelVoiceKind::ProgramChange:
        channels_[ch].program = ev.program;
        // A MIDI 2.0 program change carries the bank inside itself, so the two
        // bank-select switches decide these fields as they decide CC0 and CC32:
        // one storage location, and a second transport to it does not get to
        // arrive past a switch that closed the first.
        if (u.message_type() == UmpMessageType::kMidi2ChannelVoice && (ev.flags & 0x01u) != 0 &&
            channels_[ch].receives(GsRxSwitch::kBankSelect)) {
          channels_[ch].bank_msb = ev.bank_msb;
          channels_[ch].bank_lsb =
              channels_[ch].receives(GsRxSwitch::kBankSelectLsb) ? ev.bank_lsb : uint8_t{0};
        }
        // The fallback ambience floor is program-keyed, and so is the rig the bank
        // binds; keep both in step with the new program.
        refresh_channel_mod(ch);
        refresh_part_rig(ch);
        break;
      case ChannelVoiceKind::PitchBend:
        channels_[ch].pitch_bend = ev.bend;
        refresh_channel_mod(ch);
        break;
      case ChannelVoiceKind::ChannelPressure:
        channels_[ch].channel_pressure = ev.value;
        refresh_channel_mod(ch);
        break;
      case ChannelVoiceKind::ControlChange:
        control_change(ch, ev.note, ev.value);
        break;
      case ChannelVoiceKind::PolyPressure:
        break;  // Nothing here acts on it.
      case ChannelVoiceKind::RegisteredController:
      case ChannelVoiceKind::AssignableController:
        registered_controller(ch, u, ev);
        break;
      case ChannelVoiceKind::RelativeRegistered:
      case ChannelVoiceKind::RelativeAssignable:
        skipped |= !relative_controller(ch, ev);  // A parameter this player does not hold.
        break;
      case ChannelVoiceKind::PerNotePitchBend:
        per_note_pitch_.set_per_note_bend(ch, ev.note, ev.bend);
        refresh_per_note_voices(ch, ev.note, false);
        break;
      case ChannelVoiceKind::RegisteredPerNote:
        if (ev.index != kRpncPitch725) {
          skipped = true;
          break;
        }
        per_note_pitch_.set_pitch_7_25(ch, ev.note, ev.value);
        refresh_per_note_voices(ch, ev.note, false);
        break;
      case ChannelVoiceKind::AssignablePerNote:
        skipped = true;
        break;
      case ChannelVoiceKind::PerNoteManagement:
        manage_per_note(ch, ev.note, (ev.flags & 0x02u) != 0, (ev.flags & 0x01u) != 0);
        break;
    }
  }
  if (skipped) ++skipped_events_;
}

}  // namespace sonare::midi::synth
