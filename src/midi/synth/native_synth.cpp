#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/synth/articulation.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"
#include "util/constants.h"
#include "util/non_finite_sample.h"

namespace sonare::midi::synth {

namespace {

/// Sympathetic-string bank t60 (seconds): how long the plucked-string sound
/// halo rings. Shared by the bank tuning (prepare_custom) and the tail estimate.
constexpr float kKsSympatheticRingS = 1.5f;

/// Shared-bus residual attribution memory: how quickly a source's learned
/// share of the bus-wide residual (body/reverb tail after its dry voice
/// stops) decays toward the other live sources.
constexpr float kResidualTauSeconds = 0.5f;

}  // namespace

// ---------------------------------------------------------------------------
// NativeSynth (MidiInstrument)
// ---------------------------------------------------------------------------

NativeSynth::NativeSynth(const NativeSynthConfig& config) : config_(config) {
  config_.patch = clamp_synth_patch(config_.patch);
  // An explicit 0 is silence; only a negative or non-finite gain is not a level.
  if (config_.gain < 0.0f || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 16;
  config_.bus_drive =
      std::isfinite(config_.bus_drive) ? std::clamp(config_.bus_drive, 0.0f, 1.0f) : 0.0f;
}

void NativeSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  residual_splitter_.reset();
  piano_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  guitar_residual_splitter_.reset();
  residual_pos_ = 0;
  pool_.prepare(config_.polyphony);
  // One entry per voice bounds the notes one channel can be sounding, and this
  // is the only place the attribution scratch is sized.
  mpe_notes_.assign(pool_.size(), MpeNote{});
  mpe_note_ages_.assign(pool_.size(), 0);
  // GM mode resolves the engine per program at note-on, so any engine can be
  // selected regardless of the configured patch: every per-voice delay slab has
  // to exist up front or the waveguide engines render silence (their cores
  // return 0 while unattached). Mirrors Sf2Player::prepare(), which allocates
  // every fallback slab whenever the fallback floor is reachable.
  const bool any_engine = config_.use_gm_programs;
  // KS strings need a per-voice delay slab (the only allocation site; voices
  // attach their span at note-on).
  ks_capacity_ = ks_buffer_capacity(sample_rate_);
  guitar_halo_active_ = false;
  if (any_engine || config_.patch.mode == SynthEngineMode::kKarplusStrong) {
    ks_buffers_.assign(pool_.size() * static_cast<size_t>(ks_slab_capacity(sample_rate_)), 0.0f);
    // Sympathetic-string "sound halo": a bank tuned to the standard-tuning
    // open strings (E2 A2 D3 G3 B3 E4) plus their low harmonics — the
    // undamped strings ringing behind the played note. Gated by the same
    // sustain-pedal state as the piano board's bank (process_impl's
    // damper_open): a guitarist's hand damps the open strings unless
    // something is holding them open. Armed here for a configured
    // Karplus-Strong patch; a GM render arms it lazily at the first
    // qualifying note-on instead, since the program is not known yet.
    if (config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic) {
      guitar_halo_.prepare_guitar_sympathetic(sample_rate_);
      guitar_halo_active_ = true;
    }
  } else {
    ks_buffers_.clear();
  }
  piano_string_capacity_ = piano_string_capacity(sample_rate_);
  piano_mode_ = config_.patch.mode == SynthEngineMode::kPiano;
  if (any_engine || piano_mode_) {
    piano_buffers_.assign(pool_.size() * static_cast<size_t>(piano_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    piano_buffers_.clear();
  }
  // The modal soundboard and the pedal-gated sympathetic bank are bus-level.
  // A configured piano tunes them here; in GM mode the program that voices a
  // piano is not known until note-on, so the bank is tuned there instead (the
  // same lazy rule Sf2Player uses for its per-part bodies).
  piano_body_active_ = piano_mode_;
  piano_body_soundboard_ = -1.0f;
  if (piano_mode_) {
    resonance_.prepare(sample_rate_);
    soundboard_.prepare(sample_rate_, config_.patch.piano.soundboard);
    piano_body_soundboard_ = config_.patch.piano.soundboard;
  }
  // Pipe organ: one delay slab per voice slot (kMaxPipeRanks bore+jet span pairs,
  // so a registration's ranks all have their own self-oscillating jet pipe). The
  // only allocation site; voices attach their slab at note-on.
  pipe_organ_capacity_ = pipe_organ_buffer_capacity(sample_rate_);
  pipe_organ_mode_ = config_.patch.mode == SynthEngineMode::kPipeOrgan;
  if (any_engine || pipe_organ_mode_) {
    pipe_organ_buffers_.assign(
        pool_.size() * static_cast<size_t>(pipe_organ_slab_capacity(sample_rate_)), 0.0f);
  } else {
    pipe_organ_buffers_.clear();
  }
  // Wind chest and swell box are bus-level (shared by every sounding pipe), so
  // they stay tied to the configured patch.
  if (pipe_organ_mode_) {
    wind_.prepare(sample_rate_, config_.patch.pipe_organ.tremulant_rate_hz,
                  config_.patch.pipe_organ.tremulant_depth, config_.patch.pipe_organ.wind_sag);
    swell_depth_ = config_.patch.pipe_organ.swell;
  } else {
    swell_depth_ = 0.0f;
  }
  // Bowed string: one delay slab per voice slot (two delay-line spans, the neck
  // and bridge). The only allocation site; voices attach their slab at note-on.
  bowed_string_capacity_ = bowed_string_buffer_capacity(sample_rate_);
  bowed_string_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kBowedString;
  if (bowed_string_mode_) {
    bowed_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(bowed_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    bowed_string_buffers_.clear();
  }
  // Reed woodwind: one bore delay span per voice slot. The only allocation site;
  // voices attach their span at note-on.
  reed_capacity_ = reed_buffer_capacity(sample_rate_);
  reed_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kReed;
  if (reed_mode_) {
    reed_buffers_.assign(pool_.size() * static_cast<size_t>(reed_slab_capacity(sample_rate_)),
                         0.0f);
  } else {
    reed_buffers_.clear();
  }
  // Brass / lip reed: one bore delay span per voice slot (same as the reed).
  brass_capacity_ = brass_buffer_capacity(sample_rate_);
  brass_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kBrass;
  if (brass_mode_) {
    brass_buffers_.assign(pool_.size() * static_cast<size_t>(brass_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    brass_buffers_.clear();
  }
  // Air-jet flute: a bore span plus a jet span per voice slot.
  flute_capacity_ = flute_buffer_capacity(sample_rate_);
  flute_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kFlute;
  if (flute_mode_) {
    flute_buffers_.assign(pool_.size() * static_cast<size_t>(flute_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    flute_buffers_.clear();
  }
  // Plucked string: one string delay span per voice slot. The only allocation
  // site; voices attach their span at note-on.
  plucked_string_capacity_ = plucked_string_buffer_capacity(sample_rate_);
  plucked_string_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kPluckedString;
  if (plucked_string_mode_) {
    plucked_string_buffers_.assign(
        pool_.size() * static_cast<size_t>(plucked_string_slab_capacity(sample_rate_)), 0.0f);
  } else {
    plucked_string_buffers_.clear();
  }
  // Harpsichord: a registration slab per voice slot. The only allocation site;
  // voices attach theirs at note-on.
  harpsichord_capacity_ = harpsichord_buffer_capacity(sample_rate_);
  harpsichord_stride_ = harpsichord_slab_capacity(sample_rate_);
  harpsichord_mode_ = any_engine || config_.patch.mode == SynthEngineMode::kHarpsichord;
  if (harpsichord_mode_) {
    harpsichord_buffers_.assign(pool_.size() * static_cast<size_t>(harpsichord_stride_), 0.0f);
  } else {
    harpsichord_buffers_.clear();
  }
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  refresh_all_channel_mods();
  // GM mode (and the GM drum kit) can voice any fallback patch, so the tail has
  // to cover the slowest release in the fallback tables rather than the
  // configured patch's own release.
  const bool gm_tables =
      config_.use_gm_programs ||
      (config_.patch.mode == SynthEngineMode::kPercussion && config_.patch.percussion.gm_kit);
  // A zero-sustain (percussive) envelope never reaches Release — it ends at the
  // decay floor — so for those the decay is the stage that actually terminates
  // the voice. The GM tables already bound both stages; the configured patch
  // has to as well, or a one-shot voice is cut mid-decay at a bounce boundary.
  const DahdsrConfig& amp = config_.patch.amp_env;
  const float patch_tail_ms = amp.sustain <= DahdsrEnvelope::kSilenceLevel
                                  ? std::max(amp.release_ms, amp.decay_ms)
                                  : amp.release_ms;
  tail_samples_ = DahdsrEnvelope::release_tail_samples(
      sample_rate_, gm_tables ? gm_fallback_max_release_ms() : patch_tail_ms);
  if (guitar_halo_active_ || gm_tables) {
    // The halo bank keeps ringing after the last voice releases; fold its t60
    // into the tail so a bounce does not clip it. GM is included because
    // prepare() cannot know which program a later note-on will pick.
    tail_samples_ += static_cast<int64_t>(sample_rate_ * kKsSympatheticRingS);
  }
  if (piano_mode_ || gm_tables) {
    // Same story for the piano bus body: the soundboard and the sympathetic
    // bank ring far past the ~120 ms voice release, so a bounce would cut the
    // bloom off the last chord. GM mode is included because program 0 resolves
    // to the piano patch there and tunes the body at note-on.
    tail_samples_ += static_cast<int64_t>(sample_rate_ * kPianoBodyRingS);
  }
  // Mix-bus polish: ~8 Hz DC blocker pole and the gain-neutral drive factor.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  dc_x1_ = {};
  dc_y1_ = {};
  bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
  prepared_ = true;
}

void NativeSynth::reset() {
  pool_.reset();
  dc_x1_ = {};
  dc_y1_ = {};
  residual_splitter_.reset();
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.reset();
  residual_pos_ = 0;
  resonance_.reset();
  soundboard_.reset();
  guitar_halo_.reset();
  // A GM-mode body was tuned by a note-on, so it goes back to untuned; a
  // configured piano keeps the tuning prepare() gave it.
  piano_body_active_ = piano_mode_;
  if (!piano_mode_) piano_body_soundboard_ = -1.0f;
  // Same rule for the halo: a GM-mode arming goes back to unarmed, while a
  // configured Karplus-Strong sympathetic patch keeps the arming prepare() gave it.
  guitar_halo_active_ =
      config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic;
  wind_.reset();
  swell_lp_l_ = 0.0f;
  swell_lp_r_ = 0.0f;
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  refresh_all_channel_mods();
}

void NativeSynth::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  // Inside an MPE zone the bend range and the pressure belong to the zone: the
  // range is the one the MCM installed rather than the channel's own, and both
  // dimensions carry the manager's contribution folded in (M1-100-UM v1.1
  // sections 2.2.5 - 2.2.7). Outside one -- which is every channel until an MCM
  // arrives -- this is the arithmetic that was always here.
  const bool zoned = mpe_.role(ch) != MpeChannelRole::kUnassigned;
  mod.pitch_cents =
      zoned ? mpe_.bend_semitones(ch) * 100.0f
            : (static_cast<float>(st.pitch_bend) - 8192.0f) / 8192.0f * st.bend_range_cents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.mod_wheel01 = static_cast<float>(st.mod_wheel) / 127.0f;
  mod.extra_vibrato_cents = st.mod_depth_cents * mod.mod_wheel01;
  mod.pan_units = (static_cast<float>(st.pan) - 64.0f) / 63.0f * 500.0f;
  mod.breath01 = static_cast<float>(st.breath) / 127.0f;
  mod.aftertouch01 = static_cast<float>(zoned ? mpe_.pressure(ch) : st.pressure) / 127.0f;
  mod.expression01 = static_cast<float>(st.expression) / 127.0f;
  mod.pitch_bend01 = (static_cast<float>(st.pitch_bend) - 8192.0f) / 8192.0f;
  // The three axes that land on the channel rather than inside an engine. Each
  // is folded only when a binding has reached it, so a profile that names none
  // leaves this arithmetic untouched rather than multiplying by an identity.
  if (st.axes.has(ControllerAxis::kLoudness)) {
    mod.gain *= st.axes.values[static_cast<size_t>(ControllerAxis::kLoudness)];
  }
  if (st.axes.has(ControllerAxis::kPitchCents)) {
    mod.pitch_cents += st.axes.values[static_cast<size_t>(ControllerAxis::kPitchCents)];
  }
  if (st.axes.has(ControllerAxis::kVibratoDepth)) {
    mod.extra_vibrato_cents += st.axes.values[static_cast<size_t>(ControllerAxis::kVibratoDepth)];
  }
  refresh_mpe_note_mods(ch);
}

void NativeSynth::refresh_all_channel_mods() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
}

void NativeSynth::apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept {
  uint16_t moved = 0;
  if (!mpe_.apply_mcm(manager_channel, member_count, &moved)) return;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((moved & (uint16_t{1} << ch)) == 0) continue;
    all_sound_off(ch);
    // Which clears the zone model's tracked controllers for the channel too,
    // so a channel that left carries nothing into its conventional use and one
    // that arrived starts from the initial state 2.2.7 and 2.2.8 name.
    reset_controllers(ch);
  }
  // Every channel, not only the ones that moved: an MCM reinstalls the whole
  // zone's bend sensitivity, so a member that kept its place still changed
  // range.
  refresh_all_channel_mods();
}

namespace {

/// The deepest pitch any rank of @p patch sounds, relative to the key. A pipe
/// organ voices one key at several pitches at once, so the 16' rank runs out of
/// delay line an octave before the 8' rank does and it is the 16' that decides
/// whether the voice can be carried. 1.0 for every other engine, which sounds
/// the key and nothing below it.
float lowest_pitch_mult(const NativeSynthPatch& patch) noexcept {
  if (patch.mode != SynthEngineMode::kPipeOrgan || patch.pipe_organ.rank_count <= 0) return 1.0f;
  float lowest = 1.0f;
  const int count = std::min(patch.pipe_organ.rank_count, kMaxPipeRanks);
  for (int r = 0; r < count; ++r) {
    const float mult = patch.pipe_organ.ranks[static_cast<size_t>(r)].footage_mult;
    if (mult > 0.01f && mult < lowest) lowest = mult;
  }
  return lowest;
}

}  // namespace

NativeSynthVoice* NativeSynth::find_sounding(uint8_t ch, uint8_t note,
                                             uint32_t source_track_id) noexcept {
  // Key-down and not releasing, because a released loop cannot be carried: a
  // waveguide's release lowers its loop gain irreversibly, so a voice re-tuned
  // after note-off would arrive at the new pitch already decaying.
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && v.note == note && v.source_track_id == source_track_id &&
        v.key_down && !v.releasing) {
      return &v;
    }
  }
  return nullptr;
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                          uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  // A profile that calls velocity meaningless takes every note at full scale,
  // so the bound axes carry the dynamics on their own. 127 rather than some
  // mid value because it is the scale's identity: nothing is attenuated here
  // that a controller is not asking for.
  if (!controller_profile_.velocity_meaningful) velocity = 127;
  const uint8_t ch = channel & 0x0Fu;
  // Generic MIDI rendering resolves every channel through the shared GM/GS bank
  // rule, so a rhythm part (channel 10, a GM2 percussion bank, or a GS-assigned
  // part) reaches the drum map and a melodic channel its variation bank —
  // identically to Sf2Player. Explicit percussion patches retain their existing
  // GM-kit behavior when this mode is disabled. Resolved BEFORE the pool is
  // touched: the exclusive-group choke below reads the resolved patch, and a
  // voice allocated first would still carry the previous note's patch pointer.
  const ChannelState& st = channels_[ch];
  const NativeSynthPatch* patch = &config_.patch;
  uint8_t drum_kit = 0;
  if (config_.use_gm_programs) {
    const uint16_t bank = gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
    const GsToneMap map = gs_effective_tone_map(st.bank_msb, st.bank_lsb);
    if (bank == kDrumBank) {
      patch = &gm_fallback_drum_patch(note);
      drum_kit = gm_fallback_drum_kit(st.program, map);
    } else {
      patch = &gm_fallback_patch(bank, st.program, map);
    }
  } else if (patch->mode == SynthEngineMode::kPercussion && patch->percussion.gm_kit) {
    patch = &gm_fallback_drum_patch(note);
    drum_kit = gm_fallback_drum_kit(st.program);
  }
  // GM kit exclusive/mute groups: a new hi-hat / triangle / whistle / surdo
  // strike chokes the ringing voice in its group before the new one allocates.
  // Keyed on the RESOLVED patch's group, so it works in GM mode too.
  // Both sides are mode-independent: a kit piece voiced from the sample engine
  // carries its group in the same field, and a hi-hat that cannot choke is a
  // hi-hat with no pedal. A melodic patch leaves the field at 0 and never
  // reaches here.
  if (patch->percussion.exclusive_class != 0) {
    const uint8_t excl = patch->percussion.exclusive_class;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.channel == ch && v.patch != nullptr && v.exclusive_class == excl) {
        v.choke();
      }
    }
  }
  // Legato continuation, before a voice is allocated: on a carrying channel a
  // note-on under a held key moves that voice to the new key rather than
  // starting one, so the exciter, the delay line and both envelopes run on.
  ChannelState& live = channels_[ch];
  if (live.articulation != ArticulationMode::kPoly && live.newest_key() >= 0) {
    NativeSynthVoice* held =
        find_sounding(ch, static_cast<uint8_t>(live.newest_key()), source_track_id);
    if (held != nullptr) {
      if (live.articulation == ArticulationMode::kMonoLegato &&
          accepts_legato(patch->mode, held->note, note, lowest_pitch_mult(*patch))) {
        held->retune(note, sample_rate_);
        live.hold_key(note);
        live.last_freq_hz = synth_note_to_hz(static_cast<float>(note));
        return;
      }
      if (live.articulation == ArticulationMode::kMonoLegato) ++legato_fallbacks_;
      // Monophonic either way: the previous note stops. Fast rather than the
      // patch's own release, which on a sustaining patch runs past a second and
      // would leave the note it replaced audible under the new one.
      held->choke_fast(sample_rate_);
    }
  }
  live.hold_key(note);

  NativeSynthVoice* voice = pool_.allocate(ch, note, source_track_id);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!ks_buffers_.empty()) {
    voice->ks.attach(ks_buffers_.data() + static_cast<size_t>(voice_index) * 3 * ks_capacity_,
                     ks_capacity_);
  }
  if (!piano_buffers_.empty()) {
    voice->piano.attach(piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                    kMaxPianoStrings * piano_string_capacity_,
                        piano_string_capacity_);
  }
  if (!pipe_organ_buffers_.empty()) {
    voice->pipe_organ.attach(pipe_organ_buffers_.data() + static_cast<size_t>(voice_index) * 2 *
                                                              kMaxPipeRanks * pipe_organ_capacity_,
                             pipe_organ_capacity_);
  }
  if (!bowed_string_buffers_.empty()) {
    voice->bowed_string.attach(bowed_string_buffers_.data() +
                                   static_cast<size_t>(voice_index) * 3 * bowed_string_capacity_,
                               bowed_string_capacity_);
  }
  if (!reed_buffers_.empty()) {
    voice->reed.attach(reed_buffers_.data() + static_cast<size_t>(voice_index) * reed_capacity_,
                       reed_capacity_);
  }
  if (!brass_buffers_.empty()) {
    voice->brass.attach(brass_buffers_.data() + static_cast<size_t>(voice_index) * brass_capacity_,
                        brass_capacity_);
  }
  if (!flute_buffers_.empty()) {
    voice->flute.attach(
        flute_buffers_.data() + static_cast<size_t>(voice_index) * 2 * flute_capacity_,
        flute_capacity_);
  }
  if (!plucked_string_buffers_.empty()) {
    voice->plucked_string.attach(plucked_string_buffers_.data() +
                                     static_cast<size_t>(voice_index) * plucked_string_capacity_,
                                 plucked_string_capacity_);
  }
  voice->sampler.attach(sample_bank_);
  if (!harpsichord_buffers_.empty()) {
    voice->harpsichord.attach(
        harpsichord_buffers_.data() + static_cast<size_t>(voice_index) * harpsichord_stride_,
        harpsichord_capacity_);
  }
  // Portamento: glide from the channel's previous note when enabled.
  const float glide_from = patch->glide_ms > 0.0f ? st.last_freq_hz : 0.0f;
  // Drawbar percussion spends the channel's charge; note_off recharges it once
  // the last key is up.
  const bool organ_percussion = patch->mode == SynthEngineMode::kAdditive &&
                                patch->additive.percussion_harmonic >= 2 && st.percussion_armed;
  if (organ_percussion) channels_[ch].percussion_armed = false;
  voice->start(*patch, sample_rate_, velocity, voice_index, glide_from, st.una_corda, drum_kit,
               DrumVoiceMod{}, organ_percussion);
  // Seed the engine's excitation axes at the channel's current controllers (no
  // glide on the first sample) so a note struck mid-phrase starts at the live
  // breath / brightness rather than gliding in from the preset. An engine reads
  // only the axes it declares, and one that declares none is untouched.
  if (patch->mode == SynthEngineMode::kBowedString) {
    voice->bowed_string.set_bow_speed_scale(static_cast<float>(st.expression) / 127.0f);
  }
  {
    uint32_t present = kAxisNone;
    const ExcitationAxes base = channel_excitation(st.axes, present);
    voice->seed_excitation(base, present);
  }
  // Bus-level piano body (the direct-share attenuation, the modal soundboard
  // and the pedal-gated sympathetic bank). In GM mode the engine is resolved
  // per program, so the body is tuned at the first piano note-on rather than in
  // prepare(); re-tuned only when the resolved patch asks for a different board
  // so the bank keeps its state across notes. Allocation-free, like the lazy
  // per-part prepare on the Sf2Player fallback path.
  if (patch->mode == SynthEngineMode::kPiano) {
    if (piano_body_soundboard_ != patch->piano.soundboard) {
      piano_body_soundboard_ = patch->piano.soundboard;
      soundboard_.prepare(sample_rate_, patch->piano.soundboard);
      resonance_.prepare(sample_rate_);
    }
    piano_body_active_ = true;
    // The blow into the structure, which the board is struck with once rather
    // than driven by. After any prepare() above, which clears the network.
    soundboard_.strike(voice->piano.case_strike());
    soundboard_.strike_board(voice->piano.board_strike());
  }
  // Bus-level open-string halo, the same lazy-arming rule as the piano body
  // above but for a Karplus-Strong voice that asks for it. Guarded so an
  // already-armed bank (configured, or armed by an earlier note-on) is never
  // re-prepared, which would clear its ringing state.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      !guitar_halo_active_) {
    guitar_halo_.prepare_guitar_sympathetic(sample_rate_);
    guitar_halo_active_ = true;
  }
  channels_[ch].last_freq_hz = voice->base_freq_hz;
  // A second note on a member channel is what makes the channel's bend and
  // pressure ambiguous, so the attribution is recomputed as the set changes.
  // The new voice needs it anyway: it starts reading a mod on the next sample.
  refresh_mpe_note_mods(ch);
}

void NativeSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.release_key(note);
  // Releasing a key under a slur returns the voice to the key still held rather
  // than ending the phrase. A note-off whose key is NOT the one sounding falls
  // straight past here and past the release loop below, which is what keeps a
  // slur alive when the old key is let go late.
  if (st.articulation == ArticulationMode::kMonoLegato && st.newest_key() >= 0) {
    NativeSynthVoice* sounding = find_sounding(ch, note, source_track_id);
    if (sounding != nullptr && sounding->patch != nullptr) {
      const uint8_t back = static_cast<uint8_t>(st.newest_key());
      if (accepts_legato(sounding->patch->mode, sounding->note, back,
                         lowest_pitch_mult(*sounding->patch))) {
        sounding->retune(back, sample_rate_);
        st.last_freq_hz = synth_note_to_hz(static_cast<float>(back));
        return;
      }
      // The key still held is out of the engine's reach, so the phrase ends on
      // the release below and the held key stays silent. Counted, because
      // nothing in the sound says which of the two happened.
      ++legato_fallbacks_;
    }
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();  // pedal up: the damper falls now
      } else if (st.sustain_level < 127 && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp(static_cast<float>(127 - st.sustain_level) / 63.0f);
      }
      // else (full pedal): the string rings on freely.
    }
  }
  recharge_percussion(ch);
  // The set of notes a channel-addressed value can be attributed to just
  // changed, and a released note is not one of them.
  refresh_mpe_note_mods(ch);
}

void NativeSynth::recharge_percussion(uint8_t ch) noexcept {
  // The keys, not the voices: a percussion charge returns when the player's
  // hands leave the manual, and a released note whose tail is still sounding
  // (or whose damper the sustain pedal is holding) has left it.
  for (const NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && v.key_down) return;
  }
  channels_[ch].percussion_armed = true;
}

void NativeSynth::sustain_cc(uint8_t channel, uint8_t value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the strings, so held
    // (key-up) notes ring on at an intermediate rate; a full lift (127) leaves
    // them ringing freely. Key-down and sostenuto-captured notes keep their
    // dampers mechanically off, so they are untouched.
    if (value < 127) {
      const float strength = static_cast<float>(127 - value) / 63.0f;
      for (NativeSynthVoice& v : pool_) {
        if (v.active && v.channel == ch && !v.key_down && !v.sostenuto && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPiano) {
          v.piano.damp(strength);
        }
      }
    }
    return;
  }
  if (!was_down) return;
  for (NativeSynthVoice& v : pool_) {
    // A sostenuto-captured note stays held even when the sustain pedal lifts.
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void NativeSynth::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  // Edge-triggered: only a change of pedal position captures or releases.
  if (channels_[ch].sostenuto_down == down) return;
  channels_[ch].sostenuto_down = down;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    if (down) {
      // Capture only the notes whose keys are down at the moment of the press.
      if (v.key_down) v.sostenuto = true;
    } else if (v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !channels_[ch].sustain) v.release();
    }
  }
}

void NativeSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  channels_[ch].sustain_level = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.sostenuto = false;
      v.release();
    }
  }
  recharge_percussion(ch);
}

void NativeSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  channels_[ch].sustain_level = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
  recharge_percussion(ch);
  if (pool_.active_count() == 0) {
    // All Sound Off means silence NOW, and the instrument's bus resonators are
    // part of its output: the piano soundboard, the sympathetic bank and the
    // guitar halo ring for ~1.5 s and the swell one-pole holds a residual, so
    // killing the voices alone would leak an audible wash past the stop. They
    // are bus-level (all 16 channels feed one), so they are cleared only once
    // nothing is sounding on any channel. The DC blocker goes with them for
    // the same reason.
    resonance_.reset();
    soundboard_.reset();
    guitar_halo_.reset();
    swell_lp_l_ = 0.0f;
    swell_lp_r_ = 0.0f;
    dc_x1_ = {};
    dc_y1_ = {};
  }
}

void NativeSynth::channel_pressure(uint8_t channel, uint8_t pressure7) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].pressure = pressure7 & 0x7Fu;
  // A manager's pressure is a bias on every member of its zone, so it reaches
  // further than the channel it arrived on (2.2.7).
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    refresh_all_channel_mods();
  } else {
    refresh_channel_mod(ch);
  }
}

void NativeSynth::poly_pressure(uint8_t channel, uint8_t note, uint8_t pressure7) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // Prohibited on a member channel, where pressure is the channel's and belongs
  // to the one note living on it (2.2.7).
  if (mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) return;
  const float value = static_cast<float>(pressure7 & 0x7Fu) / 127.0f;
  // Every sounding voice on the note takes it: a layered patch is several
  // voices of one key, and pressure is a property of the key.
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch) v.poly_pressure01 = value;
  }
}

void NativeSynth::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  st.breath = 0;
  st.pressure = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.channel == ch) v.poly_pressure01 = 0.0f;
  }
  st.axes.reset();
  st.params.reset();
  // Inside a zone the same three controllers are tracked by the zone model,
  // which is where every reader of them takes their combined value -- leaving
  // them here would reset the channel and change nothing that is heard.
  mpe_.reset_controls(static_cast<uint16_t>(uint16_t{1} << ch));
  sustain_cc(ch, 0);
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    // The manager's values were a bias on every member, so each member has to
    // resolve its own again without them.
    const MpeZone zone = mpe_.zone_of(ch);
    for (uint8_t member = 0; member < 16; ++member) {
      if (mpe_.role(member) != MpeChannelRole::kMember || mpe_.zone_of(member) != zone) continue;
      for (const MpeDimension dimension : {MpeDimension::kPressure, MpeDimension::kTimbre}) {
        if (mpe_.has(member, dimension)) push_mpe_controller_axis(member, dimension);
      }
    }
    refresh_all_channel_mods();
  } else {
    refresh_channel_mod(ch);
  }
  push_excitation_control(ch);
}

void NativeSynth::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  switch (controller) {
    case 0:
      // Bank select is prohibited on a member channel in MIDI Mode 3 and
      // permitted in Mode 4 (Appendix E Table 5), where a controller gives each
      // string its own program.
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_msb = value;
      break;
    case 1:
      st.mod_wheel = value;
      refresh_channel_mod(ch);
      break;
    case 7:
      st.volume = value;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = value;
      refresh_channel_mod(ch);
      break;
    case 2:
      // The matrix source. Which axis CC2 additionally reaches, if any, is the
      // controller profile's to say and is applied before this switch runs.
      st.breath = value;
      refresh_channel_mod(ch);
      break;
    case 11:
      st.expression = value;
      refresh_channel_mod(ch);
      push_excitation_control(ch);  // expression scales bowed-string bow speed
                                    // (every other engine's loudness rides the
                                    // shared expression VCA)
      break;
    case 32:
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_lsb = value;
      break;
    case 6:
      // RPN 00 06 is the MPE Configuration Message, which every MPE-compatible
      // device shall support (2.2.1). It is tried first because it is the one
      // that can turn the zone model on.
      if (st.params.selected_rpn(0, 6)) {
        apply_mcm(ch, value);
      } else if (st.params.selected_rpn(0, 0)) {
        // Inside a zone the range is the zone's, and a value sent to one member
        // reaches every member of it (2.2.5), so the refresh is zone-wide.
        if (mpe_.apply_bend_sensitivity(ch, static_cast<float>(value))) {
          refresh_all_channel_mods();
        } else {
          st.bend_range_cents = 100.0f * static_cast<float>(value);
          refresh_channel_mod(ch);
        }
      }
      break;
    case 38:
      if (st.params.selected_rpn(0, 0)) {
        // The fractional semitone. MPE recommends senders leave it at zero and
        // permits rather than requires a receiver to answer it, so the zone
        // takes it on the same terms the channel always has.
        if (mpe_.role(ch) != MpeChannelRole::kUnassigned) {
          const float whole = std::floor(mpe_.bend_sensitivity(ch));
          mpe_.apply_bend_sensitivity(ch, whole + static_cast<float>(value) / 100.0f);
          refresh_all_channel_mods();
        } else {
          st.bend_range_cents =
              100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
          refresh_channel_mod(ch);
        }
      }
      break;
    case kMpeTimbreCc:
      // The third per-note dimension (2.2.8) reaches an axis through the
      // controller profile alone, which has already run, and the value itself
      // was tracked ahead of it -- a receiver shall go on tracking it while the
      // channel is silent so the next note starts from it. Nothing is left to
      // do here, and the case stands so the default below cannot claim it.
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 66:
      sostenuto_pedal(ch, value >= 64);
      break;
    case 67:
      st.una_corda = value >= 64;  // soft pedal (affects notes struck while held)
      break;
    case 98:
      st.params.select_nrpn_lsb(value);
      break;
    case 99:
      st.params.select_nrpn_msb(value);
      break;
    case 100:
      st.params.select_rpn_lsb(value);
      break;
    case 101:
      st.params.select_rpn_msb(value);
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
      // The two mode messages. Prohibited on a manager channel, where they are
      // ignored outright, and on a member channel they select the zone's mode
      // (2.2.4.3). Outside a zone they keep the channel-mode meaning they have
      // always had here, which is why the all-notes-off stays below them.
      if (mpe_.ignores(ch, MpeIgnorable::kModeMessage)) break;
      mpe_.apply_midi_mode(ch, controller == 126 ? MpeMidiMode::kMono : MpeMidiMode::kPoly);
      all_notes_off(ch);
      break;
    default:
      break;
  }
}

void NativeSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    return;
  }
  // Tracking comes ahead of both, because the profile reads the combined value
  // a member channel's controller carries and that value is only current once
  // the message in hand has been tracked.
  track_mpe_input(u);
  // The profile runs before the protocol dispatch so a note-on whose velocity
  // is bound to an axis starts from its own value rather than the previous
  // note's.
  apply_controller_input(u);
  if (u.is_note_on()) {
    const uint8_t vel7 =
        u.message_type() == UmpMessageType::kMidi1ChannelVoice
            ? u.data2_7bit()
            : scale_note_on_velocity_16_to_7(static_cast<uint16_t>(u.words[1] >> 16));
    note_on(u.channel(), u.note_number(), vel7, event.source_track_id);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number(), event.source_track_id);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      channels_[ch].pitch_bend =
          static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
    } else {
      channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
    }
    mpe_.track_bend(ch, channels_[ch].pitch_bend);
    // A manager's bend applies to every sounding note in its zone (2.2.6), so
    // like its pressure it reaches past the channel it arrived on.
    if (mpe_.role(ch) == MpeChannelRole::kManager) {
      refresh_all_channel_mods();
    } else {
      refresh_channel_mod(ch);
    }
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure)) {
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.note_number()
                               : scale_cc_32_to_7(u.words[1]);
    channel_pressure(u.channel(), value7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure)) {
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    poly_pressure(u.channel(), u.note_number(), value7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    control_change(u.channel(), u.note_number(), value7);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    // GS drum-kit select: in gm_kit mode the drum channel's program picks the
    // kit variation (Room/Power/808/...). Melodic patches ignore it.
    const uint8_t ch = u.channel() & 0x0Fu;
    // In MIDI Mode 3 a zone is monotimbral, so a program change reaching a
    // member channel is ignored rather than splitting the zone across two
    // patches (2.3.3). Mode 4 is the case that permits it.
    if (mpe_.ignores(ch, MpeIgnorable::kProgramChange)) return;
    if (u.message_type() == UmpMessageType::kMidi2ChannelVoice) {
      channels_[ch].program = static_cast<uint8_t>((u.words[1] >> 24) & 0x7Fu);
      // Bit 0 of word 0 is the bank-valid flag: the bank bytes carry meaning
      // only when it is set, and an unset flag leaves the channel's bank alone.
      if ((u.words[0] & 0x01u) != 0) {
        channels_[ch].bank_msb = static_cast<uint8_t>((u.words[1] >> 8) & 0x7Fu);
        channels_[ch].bank_lsb = static_cast<uint8_t>(u.words[1] & 0x7Fu);
      }
    } else {
      channels_[ch].program = u.note_number();
    }
  }
}

void NativeSynth::process(float* const* channels, int num_channels, int num_samples) {
  process_impl(channels, nullptr, 0, num_channels, num_samples);
}

bool NativeSynth::process_source_tracks(const MidiInstrumentSourceOutput* outputs,
                                        size_t output_count, int num_channels,
                                        int num_samples) noexcept {
  if (outputs == nullptr || output_count == 0 || outputs[0].source_track_id != 0 ||
      outputs[0].channels == nullptr) {
    return false;
  }
  process_impl(nullptr, outputs, output_count, num_channels, num_samples);
  return true;
}

void NativeSynth::process_impl(float* const* channels,
                               const MidiInstrumentSourceOutput* source_outputs,
                               size_t source_output_count, int num_channels,
                               int num_samples) noexcept {
  const bool source_render = source_outputs != nullptr;
  if (!prepared_ || num_channels <= 0 || num_samples <= 0 ||
      (!source_render && channels == nullptr)) {
    return;
  }
  float* left = source_render ? nullptr : channels[0];
  float* right = !source_render && num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  const auto target_for = [&](uint32_t source_track_id) noexcept -> float* const* {
    if (source_render) {
      for (size_t index = 1; index < source_output_count; ++index) {
        if (source_outputs[index].source_track_id == source_track_id &&
            source_outputs[index].channels != nullptr) {
          return source_outputs[index].channels;
        }
      }
      return source_outputs[0].channels;
    }
    return nullptr;
  };
  const auto add_output = [&](float* const* target, int sample, float l, float r) noexcept {
    if (target == nullptr) return;
    if (target[0] != nullptr) {
      target[0][sample] += num_channels == 1 ? constants::kInvSqrt2 * (l + r) : l;
    }
    if (num_channels > 1 && target[1] != nullptr) target[1][sample] += r;
    for (int ch = 2; ch < num_channels; ++ch) {
      if (target[ch] != nullptr) target[ch][sample] += constants::kInvSqrt2 * (l + r);
    }
  };

  // Sympathetic resonance is gated by the dampers being lifted on any channel
  // (sustain pedal down). Sustain state is fixed for the block (events are
  // applied before process()). Shared by the piano board and the guitar
  // halo below -- two bus-level banks, one gate rule.
  bool damper_open = false;
  if (piano_body_active_ || guitar_halo_active_) {
    for (const ChannelState& ch : channels_) {
      if (ch.sustain) {
        damper_open = true;
        break;
      }
    }
  }

  // Swell box: the expression pedal (CC11) sets the shutter. The most-closed
  // pedal across channels darkens the whole division (a bus lowpass). Expression
  // is fixed for the block, so the cutoff is computed once here. Above ~19 kHz
  // the shutter is effectively open, so the one-pole is bypassed (swell_active).
  bool swell_active = false;
  if (pipe_organ_mode_ && swell_depth_ > 0.0f) {
    uint8_t closed = 127;
    for (const ChannelState& ch : channels_) closed = std::min(closed, ch.expression);
    const float shut = (1.0f - static_cast<float>(closed) / 127.0f) * swell_depth_;
    const float fc = std::exp(std::log(20000.0f) + shut * (std::log(300.0f) - std::log(20000.0f)));
    if (fc < 19000.0f) {
      swell_active = true;
      swell_coeff_ = std::clamp(
          1.0f - std::exp(-constants::kTwoPi * fc / static_cast<float>(sample_rate_)), 0.0f, 1.0f);
    }
  }

  // Set when any sample in this call's per-sample loop below actually
  // discarded; bumped once after the loop rather than per sample, since the
  // unit is one process() call, not one sample.
  bool discarded = false;
  if (source_render) residual_pos_ = 0;
  for (int i = 0; i < num_samples; ++i) {
    float mix_l = 0.0f;
    float mix_r = 0.0f;
    // Shared wind chest: the tremulant / wind-sag modulation common to every
    // sounding pipe. Demand is the count of active pipe voices (order-
    // independent), so the sag is deterministic across bounces.
    OrganWindSupply::State wind;
    if (pipe_organ_mode_ && wind_.active()) {
      int demand = 0;
      for (const NativeSynthVoice& v : pool_) demand += v.active ? 1 : 0;
      wind = wind_.process(demand);
    }
    // Piano voices are summed apart from the rest: the body below attenuates
    // and re-radiates only them, so in GM mode — where one bus carries many
    // programs — the flute sharing the render must not be pulled through the
    // soundboard. With a single-patch configuration one of the two legs stays
    // at zero, so the sum is unchanged.
    float piano_l = 0.0f;
    float piano_r = 0.0f;
    // Guitar/harp/banjo halo drive: the same isolation as the piano leg above,
    // so drums and brass sharing a GM bus never reach a bank tuned to open
    // strings. Unlike the piano leg this one stays IN mix_l/mix_r too — the
    // halo is additive to the dry KS voice rather than replacing it.
    float guitar_l = 0.0f;
    float guitar_r = 0.0f;
    for (NativeSynthVoice& v : pool_) {
      if (!v.active) continue;
      // The channel's, except on a member channel sounding more than one note,
      // where the voice carries the part of the channel's the note was
      // attributed (M1-100-UM v1.1 section 2.2.4.1).
      const Sf2ChannelMod& mod = v.mpe_mod_active ? v.mpe_mod : channel_mods_[v.channel & 0x0Fu];
      const float s = v.render(mod, wind.pitch_ratio, wind.gain);
      const float voice_l = s * v.gain_left;
      const float voice_r = s * v.gain_right;
      if (piano_body_active_ && v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
        piano_l += voice_l;
        piano_r += voice_r;
      } else {
        mix_l += voice_l;
        mix_r += voice_r;
        if (guitar_halo_active_ && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kKarplusStrong && v.patch->ks.sympathetic) {
          guitar_l += voice_l;
          guitar_r += voice_r;
        }
      }
      if (source_render) {
        const float src_l = voice_l * config_.gain;
        const float src_r = voice_r * config_.gain;
        add_output(target_for(v.source_track_id), i, src_l, src_r);
        // The remainder follows every source; the piano body and guitar halo
        // only the voices that drive them.
        residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
        if (piano_body_active_ && v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
          piano_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
        }
        if (guitar_halo_active_ && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kKarplusStrong && v.patch->ks.sympathetic) {
          guitar_residual_splitter_.accumulate(v.source_track_id, src_l, src_r);
        }
      }
    }
    mix_l *= config_.gain;
    mix_r *= config_.gain;
    piano_l *= config_.gain;
    piano_r *= config_.gain;
    guitar_l *= config_.gain;
    guitar_r *= config_.gain;
    const float dry_l = mix_l + piano_l;
    const float dry_r = mix_r + piano_r;
    // Swell box shutter: a one-pole lowpass on the bus as the louvres close.
    if (swell_active) {
      swell_lp_l_ += swell_coeff_ * (mix_l - swell_lp_l_);
      swell_lp_r_ += swell_coeff_ * (mix_r - swell_lp_r_);
      mix_l = swell_lp_l_;
      mix_r = swell_lp_r_;
    }
    // Shared modal soundboard plus pedal-gated sympathetic resonance, both
    // driven by the summed dry piano mix. The sympathetic bank returns to the
    // centre; the board does not, because its two radiation paths differ. Runs
    // independently of the halo below them, since GM can voice a piano and a
    // guitar together.
    // What the piano body and the guitar halo add beyond their dry voices.
    float piano_res_l = 0.0f;
    float piano_res_r = 0.0f;
    float guitar_res_l = 0.0f;
    float guitar_res_r = 0.0f;
    if (piano_body_active_) {
      // Radiation split: the board returns the phase-diffused complement of
      // the direct share (plus the modal colour), so most of the note reaches
      // the mix through the board rather than as the raw string waveform.
      const float dry_mono = 0.5f * (piano_l + piano_r);
      const float body = soundboard_.process(dry_mono);
      // The board's two radiation paths differ in phase, so its return is not
      // the same signal on both legs. Zero at a zero board width.
      const float side = soundboard_.last_side();
      const float symp = resonance_.process(soundboard_.last_diffused(), damper_open);
      const float piano_out_l = kPianoDirectGain * piano_l + body + side + symp;
      const float piano_out_r = kPianoDirectGain * piano_r + body - side + symp;
      piano_res_l = piano_out_l - piano_l;
      piano_res_r = piano_out_r - piano_r;
      mix_l += piano_out_l;
      mix_r += piano_out_r;
    }
    if (guitar_halo_active_) {
      // Plucked-string sound halo: the open strings ring behind the note,
      // gated by damper_open exactly as the piano board is above -- a guitar's
      // open strings are damped unless the sustain pedal is holding them open.
      // Driven by guitar_l/guitar_r alone (the halo-eligible voices' own dry
      // mix), never by the rest of the GM bus. Skipped entirely when no
      // eligible voice has ever sounded, so every existing KS voicing with no
      // halo renders bit-identically.
      const float dry_mono = 0.5f * (guitar_l + guitar_r);
      const float symp = guitar_halo_.process(dry_mono, damper_open);
      guitar_res_l = symp;
      guitar_res_r = symp;
      mix_l += symp;
      mix_r += symp;
    }
    // Gentle gain-neutral bus saturation (glue), then the DC blocker — the
    // physical-model voices can carry a small DC component.
    if (bus_drive_gain_ > 0.0f) {
      mix_l = std::tanh(bus_drive_gain_ * mix_l) / bus_drive_gain_;
      mix_r = std::tanh(bus_drive_gain_ * mix_r) / bus_drive_gain_;
    }
    // Scrub any non-finite bus sample before it reaches the DC blocker: a single
    // NaN/Inf reaching dc_x1_/dc_y1_ would persist in the IIR state and poison
    // every subsequent sample for the whole render. Bit-identical for finite
    // input, which is not the same as safe for it -- the blocker is a feedback
    // cell whose worst-case gain is well over unity, so a large enough finite
    // sample still leaves float range. Mirrors the host-side scrub in
    // au_instrument_provider.
    discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_l);
    discarded |= resolve_non_finite(SampleDestination::kRecursiveState, mix_r);
    if (config_.dc_block) {
      const float l = mix_l - dc_x1_[0] + dc_r_ * dc_y1_[0];
      dc_x1_[0] = mix_l;
      dc_y1_[0] = l;
      mix_l = l;
      const float r = mix_r - dc_x1_[1] + dc_r_ * dc_y1_[1];
      dc_x1_[1] = mix_r;
      dc_y1_[1] = r;
      mix_r = r;
    }
    if (source_render) {
      // Shared bodies, bus drive and DC filtering are destination-scoped; their
      // residual (mix minus dry) is split in three components, each across the
      // sources that produced it. With one live source each split adds its
      // share unmultiplied, so that source's target is bit-identical to a
      // slot-0-only render; with several it is exact only up to floating-point
      // rounding. Staged in fixed chunks (prepare() has no block-length
      // argument), flushed every kResidualChunk samples and at block end.
      const size_t pos = static_cast<size_t>(residual_pos_);
      piano_residual_l_[pos] = piano_res_l;
      piano_residual_r_[pos] = piano_res_r;
      guitar_residual_l_[pos] = guitar_res_l;
      guitar_residual_r_[pos] = guitar_res_r;
      residual_l_[pos] = (mix_l - dry_l) - piano_res_l - guitar_res_l;
      residual_r_[pos] = (mix_r - dry_r) - piano_res_r - guitar_res_r;
      if (++residual_pos_ == kResidualChunk) {
        const int offset = i - kResidualChunk + 1;
        residual_splitter_.flush(source_outputs, source_output_count, residual_pos_,
                                 residual_l_.data(), residual_r_.data(), offset, add_output);
        piano_residual_splitter_.flush(source_outputs, source_output_count, residual_pos_,
                                       piano_residual_l_.data(), piano_residual_r_.data(), offset,
                                       add_output);
        guitar_residual_splitter_.flush(source_outputs, source_output_count, residual_pos_,
                                        guitar_residual_l_.data(), guitar_residual_r_.data(),
                                        offset, add_output);
        residual_pos_ = 0;
      }
    } else if (left != nullptr) {
      // Mono host: fold both pan legs so centre-panned voices keep level.
      left[i] += mono ? constants::kInvSqrt2 * (mix_l + mix_r) : mix_l;
    }
    if (right != nullptr) right[i] += mix_r;
    // Fan a mono fold-down to any additional channels.
    for (int ch = 2; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) {
        channels[ch][i] += constants::kInvSqrt2 * (mix_l + mix_r);
      }
    }
  }
  if (source_render && residual_pos_ > 0) {
    const int offset = num_samples - residual_pos_;
    residual_splitter_.flush(source_outputs, source_output_count, residual_pos_, residual_l_.data(),
                             residual_r_.data(), offset, add_output);
    piano_residual_splitter_.flush(source_outputs, source_output_count, residual_pos_,
                                   piano_residual_l_.data(), piano_residual_r_.data(), offset,
                                   add_output);
    guitar_residual_splitter_.flush(source_outputs, source_output_count, residual_pos_,
                                    guitar_residual_l_.data(), guitar_residual_r_.data(), offset,
                                    add_output);
    residual_pos_ = 0;
  }
  if (discarded) note_non_finite_discard();
}

}  // namespace sonare::midi::synth
