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
#include "midi/controller_numbers.h"
#include "midi/per_note_state.h"
#include "midi/synth/channel_mapping.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/pitch.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

using ::sonare::constants::kSemitonesPerOctave;

/// Points @p span at its per-voice slice of @p buffers; an engine with no buffer stays unattached.
template <typename Span>
void attach_slab(Span& span, std::vector<float>& buffers, size_t offset, int capacity) noexcept {
  if (!buffers.empty()) span.attach(buffers.data() + offset, capacity);
}

}  // namespace

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                                 uint32_t source_track_id, Portamento porta, uint8_t attribute_type,
                                 uint16_t attribute_data) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const uint16_t bank = effective_bank(channel);
  const GsToneMap tone_map = gs_effective_tone_map(ch.bank_msb, ch.bank_lsb);
  const bool is_drum = bank == kDrumBank;
  // Compose first: an absolute MIDI 2.0 pitch picks the key the physical model starts from.
  Sf2PerNoteVoice per_note;
  const ComposedPitch note_pitch =
      bind_note_pitch(per_note, ch, channel, note, attribute_type, attribute_data);
  // The per-note GS edits, read before the patch: PLAY NOTE NUMBER picks which
  // kit piece answers and ASSIGN GROUP which group it belongs to, and both are
  // needed before the choke below, let alone the voice.
  const GsDrumNoteParams gd = drum_note_params(ch, is_drum, note);
  // The user drum set under them: it says which stored kit and which note within
  // it this strike sounds, and the map's edits above are what edits that.
  const GsUserDrumSource* us = user_drum_source(ch, is_drum, note);
  uint8_t sound_note = gs_user_drum_sound_note(us, note);
  if ((gd.flags & GsDrumNoteParams::kPlayNote) != 0) sound_note = gd.play_note;
  // An absolute pitch overrides PLAY NOTE NUMBER / user-set substitution.
  if (note_pitch.absolute) {
    sound_note = absolute_pitch_key(note, note_pitch);
    per_note.zone_key = sound_note;
  }
  per_note.cents = per_note_cents(per_note, note_pitch);
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
  const size_t slot = static_cast<size_t>(voice_index);
  attach_slab(voice->ks, fallback_ks_buffers_, slot * 3 * fallback_ks_capacity_,
              fallback_ks_capacity_);
  attach_slab(voice->piano, fallback_piano_buffers_,
              slot * kMaxPianoStrings * fallback_piano_string_capacity_,
              fallback_piano_string_capacity_);
  // Each rank holds TWO spans (bore + jet), so the per-voice stride is
  // 2 * kMaxPipeRanks spans; without the 2, adjacent voices' slabs overlap
  // and simultaneous (legato) organ voices corrupt each other's bores.
  attach_slab(voice->pipe_organ, fallback_pipe_organ_buffers_,
              slot * 2 * kMaxPipeRanks * fallback_pipe_organ_capacity_,
              fallback_pipe_organ_capacity_);
  attach_slab(voice->bowed_string, fallback_bowed_buffers_,
              slot * kBowedSlabSpans * fallback_bowed_capacity_, fallback_bowed_capacity_);
  attach_slab(voice->vocal, fallback_vocal_buffers_, slot * fallback_vocal_capacity_,
              fallback_vocal_capacity_);
  attach_slab(voice->reed, fallback_reed_buffers_, slot * fallback_reed_capacity_,
              fallback_reed_capacity_);
  attach_slab(voice->brass, fallback_brass_buffers_, slot * fallback_brass_capacity_,
              fallback_brass_capacity_);
  attach_slab(voice->flute, fallback_flute_buffers_, slot * 2 * fallback_flute_capacity_,
              fallback_flute_capacity_);
  attach_slab(voice->plucked_string, fallback_plucked_string_buffers_,
              slot * fallback_plucked_string_capacity_, fallback_plucked_string_capacity_);
  attach_slab(voice->harpsichord, fallback_harpsichord_buffers_,
              slot * fallback_harpsichord_stride_, fallback_harpsichord_capacity_);
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
      drum_mod.pitch_ratio = std::exp2(static_cast<float>(gd.pitch_coarse) / kSemitonesPerOctave);
    }
    if ((gd.flags & GsDrumNoteParams::kLevel) != 0) {
      const float v = static_cast<float>(gd.level & 0x7Fu) / 127.0f;
      drum_mod.level_gain = v * v;  // same square law as CC7 / velocity
    }
    if ((gd.flags & GsDrumNoteParams::kPan) != 0) {
      drum_mod.pan_units = cc_pan_units(static_cast<float>(gd.pan & 0x7Fu));
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
  // The note the voice is STARTED at: PLAY NOTE NUMBER, a user set's source note and an
  // absolute pitch move it; the patch above is chosen by it, so carry the resolved note.
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
  // An absolute pitch overrides tuning tables (M2-104-UM §7.4.15.2).
  part_mod.pitch_cents = per_note.baked_gs_tuning_cents;
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
    const bool regulated = patch.pipe_organ.wind_regulated;
    if (wp.rate != rate || wp.depth != depth || wp.sag != sag || wp.regulated != regulated) {
      wp = {rate, depth, sag, regulated};
      fallback_wind_[part].prepare(sample_rate_, rate, depth, sag, regulated);
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
    } else {
      if (!body.prepared) fallback_halo_[part].prepare_guitar_sympathetic(sample_rate_);
      // The part's bank outlives any one patch, so every halo note restates its hand damping.
      fallback_halo_[part].set_hand_damping(patch.ks.halo_damp_s);
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

}  // namespace sonare::midi::synth
