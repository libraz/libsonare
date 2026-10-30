#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/controller_numbers.h"
#include "midi/synth/articulation.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/mix_dc_blocker.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"
#include "util/constants.h"
#include "util/non_finite_sample.h"
#include "util/numeric_validation.h"

namespace sonare::midi::synth {

using sonare::constants::kDefaultDawSampleRate;

namespace {

/// Sympathetic-string bank t60 (seconds): how long the plucked-string sound
/// halo rings. Shared by the bank tuning (prepare_custom) and the tail estimate.
constexpr float kKsSympatheticRingS = PianoResonanceBank::kGuitarHaloRingT60S;

/// Shared-bus residual attribution memory: how quickly a source's learned
/// share of the bus-wide residual (body/reverb tail after its dry voice
/// stops) decays toward the other live sources.
constexpr float kResidualTauSeconds = 0.5f;

/// Note On attribute #3 carries Pitch 7.9 (M2-104-UM §7.4.15).
constexpr uint8_t kAttributePitch79 = 0x03;
/// Registered Per-Note Controller #3 is Pitch 7.25 (M2-104-UM §7.4.12).
constexpr uint8_t kRpncPitch725 = 3;

using native_synth_detail::excitation_axis_mask;
using native_synth_detail::excitation_value;

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
  // The stage owns the factory and the bank-rig switch from here on. The GS
  // units take the modern realisation; no field selects another.
  PartFxStageConfig fx;
  fx.insert_factory = std::move(config_.insert_factory);
  fx.bank_rig_binding = config_.bank_rig_binding;
  part_fx_ = PartFxStage(std::move(fx));
}

int64_t NativeSynth::recompute_tail() const noexcept {
  const EnvelopeTimeScales scales = gs_tail_scales();
  const auto with_dc_tail = [this](int64_t tail) noexcept {
    if (!config_.dc_block) return tail;
    return numeric::saturating_add<int64_t>(tail, mix_dc_blocker_tail_samples(dc_r_));
  };
  // A kit patch resolves each note against the drum table and a program change only selects
  // kits, so neither the melodic programs nor the piano and guitar halos are reachable.
  const bool kit_preset = !config_.use_gm_programs &&
                          config_.patch.mode == SynthEngineMode::kPercussion &&
                          config_.patch.percussion.gm_kit;
  const bool heard_set_usable = tail_patch_count_ > 0 && !tail_patches_full_;
  if (kit_preset && !heard_set_usable) {
    return with_dc_tail(
        gm_drum_kit_max_tail_samples(sample_rate_, scales.attack, scales.decay, scales.release));
  }
  // Once a note has resolved its patch, only the patches heard so far can still be ringing.
  if ((config_.use_gm_programs || kit_preset) && heard_set_usable) {
    int64_t tail = 0;
    for (size_t i = 0; i < tail_patch_count_; ++i) {
      const TailPatch& heard = tail_patches_[i];
      NativeSynthPatch voiced = *heard.patch;
      if (heard.kit != kNoTailKit) {
        apply_gs_drum_kit(voiced.percussion, voiced.amp_env, heard.kit, heard.note);
      }
      tail = std::max(tail, native_patch_tail_samples(voiced, sample_rate_, scales, sample_bank_));
    }
    const bool part_halo =
        std::any_of(part_halo_armed_.begin(), part_halo_armed_.end(), [](bool a) { return a; });
    if (guitar_halo_active_ || part_halo) {
      tail = numeric::saturating_add(
          tail, numeric::ceil_sample_count(sample_rate_ * kKsSympatheticRingS));
    }
    if (piano_body_active_) {
      tail =
          numeric::saturating_add(tail, numeric::ceil_sample_count(sample_rate_ * kPianoBodyRingS));
    }
    return with_dc_tail(tail);
  }
  // GM mode resolves the patch per program at note-on, so the fallback tables bound it.
  const bool gm_tables = config_.use_gm_programs;
  int64_t tail =
      gm_tables
          ? gm_fallback_max_tail_samples(sample_rate_, scales.attack, scales.decay, scales.release)
          : native_patch_tail_samples(config_.patch, sample_rate_, scales, sample_bank_);
  if (guitar_halo_active_ || gm_tables) {
    // The halo rings past the last voice; GM counts it since prepare() cannot know the program.
    tail = numeric::saturating_add(tail,
                                   numeric::ceil_sample_count(sample_rate_ * kKsSympatheticRingS));
  }
  if (piano_mode_ || gm_tables) {
    // The piano body rings past the voice release; in GM mode program 0 is the piano.
    tail =
        numeric::saturating_add(tail, numeric::ceil_sample_count(sample_rate_ * kPianoBodyRingS));
  }
  return with_dc_tail(tail);
}

void NativeSynth::raise_tail() noexcept {
  if (!prepared_) return;
  const int64_t tail = recompute_tail();
  int64_t current = tail_samples_->load(std::memory_order_relaxed);
  while (tail > current &&
         !tail_samples_->compare_exchange_weak(current, tail, std::memory_order_relaxed)) {
  }
}

void NativeSynth::observe_tail_patch(const NativeSynthPatch& patch, uint8_t kit,
                                     uint8_t note) noexcept {
  if (tail_patches_full_) return;
  for (size_t i = 0; i < tail_patch_count_; ++i) {
    const TailPatch& heard = tail_patches_[i];
    // Only a kit variation reads the note, so a melodic patch is one entry for every key.
    if (heard.patch == &patch && heard.kit == kit && (kit == kNoTailKit || heard.note == note)) {
      return;
    }
  }
  const bool first = tail_patch_count_ == 0;
  if (tail_patch_count_ == kMaxTailPatches) {
    tail_patches_full_ = true;
  } else {
    tail_patches_[tail_patch_count_++] = {&patch, kit, note};
  }
  // Nothing rang before the first note, so lowering the every-program bound strands no voice.
  if (first) {
    tail_samples_->store(recompute_tail(), std::memory_order_relaxed);
  } else {
    raise_tail();
  }
}

bool NativeSynth::materialize_tail_probe() {
  if (!prepared_ || !config_.realize_efx_inline || !part_fx_.enabled()) return true;
  if (part_fx_.dirty()) realize_part_fx();
  part_fx_.settle_quiescent(*this);
  return true;
}

EnvelopeTimeScales NativeSynth::gs_tail_scales() const noexcept {
  return gs_slowest_eg_time_scales(
      channels_, [](const ChannelState& st) -> const GsPartParams& { return st.gs; });
}

void NativeSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  residual_splitter_.reset();
  piano_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  guitar_residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : unit_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : part_piano_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : part_organ_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : part_guitar_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  part_fx_.clear_mirror();
  part_fx_.prepare(sample_rate_);
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  part_soundboards_.clear();
  part_resonance_.clear();
  if (part_fx_.enabled()) {
    part_soundboards_.resize(16);
    part_resonance_.resize(16);
  }
  for (PianoSoundboard& board : part_soundboards_) board.reset();
  for (PianoResonanceBank& bank : part_resonance_) bank.reset();
  for (PianoPartState& state : part_piano_state_) state = {};
  pool_.prepare(config_.polyphony);
  // One entry per voice bounds the notes one channel can be sounding, and this
  // is the only place the attribution scratch is sized.
  mpe_notes_.assign(pool_.size(), MpeNote{});
  mpe_note_ages_.assign(pool_.size(), 0);
  per_note_.assign(pool_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
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
  // Organ wind and swell arm per part at note-on, since GM programs resolve after prepare().
  for (OrganWindSupply& wind : part_wind_) wind.reset();
  for (OrganPartParams& params : organ_part_params_) params = {};
  part_swell_depth_.fill(0.0f);
  part_swell_coeff_.fill(1.0f);
  part_swell_lp_l_.fill(0.0f);
  part_swell_lp_r_.fill(0.0f);
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
  // Vocal: one section-history span per voice slot. The only allocation site;
  // voices attach their span at note-on.
  vocal_capacity_ = vocal_slab_capacity(sample_rate_);
  if (any_engine || config_.patch.mode == SynthEngineMode::kVocal) {
    vocal_buffers_.assign(pool_.size() * static_cast<size_t>(vocal_capacity_), 0.0f);
  } else {
    vocal_buffers_.clear();
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
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  bus_fed_channels_ = 0;
  refresh_all_channel_mods();
  tail_patch_count_ = 0;
  tail_patches_full_ = false;
  // Mix-bus polish: ~8 Hz DC blocker pole and the gain-neutral drive factor.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  tail_samples_->store(recompute_tail(), std::memory_order_relaxed);
  dc_x1_ = {};
  dc_y1_ = {};
  bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
  live_dirty_ = 0;
  prepared_ = true;
  // The rig entries and the bank rigs the parts' power-on programs bind, so a
  // bussed part routes from the first block.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  part_fx_.publish();
  part_fx_.settle_quiescent(*this);
  part_fx_.clear_dirty();
}

void NativeSynth::reset() {
  pool_.reset();
  live_dirty_ = 0;
  dc_x1_ = {};
  dc_y1_ = {};
  residual_splitter_.reset();
  piano_residual_splitter_.reset();
  guitar_residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) s.reset();
  for (SourceResidualSplitter& s : unit_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_piano_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_organ_splitters_) s.reset();
  for (SourceResidualSplitter& s : part_guitar_splitters_) s.reset();
  part_fx_.clear_mirror();
  resonance_.reset();
  soundboard_.reset();
  guitar_halo_.reset();
  for (PianoResonanceBank& halo : part_halo_) halo.reset();
  part_halo_armed_.fill(false);
  for (PianoSoundboard& board : part_soundboards_) board.reset();
  for (PianoResonanceBank& bank : part_resonance_) bank.reset();
  for (PianoPartState& state : part_piano_state_) state = {};
  // A GM-mode body was tuned by a note-on, so it goes back to untuned; a
  // configured piano keeps the tuning prepare() gave it.
  piano_body_active_ = piano_mode_;
  if (!piano_mode_) piano_body_soundboard_ = -1.0f;
  // Same rule for the halo: a GM-mode arming goes back to unarmed, while a
  // configured Karplus-Strong sympathetic patch keeps the arming prepare() gave it.
  guitar_halo_active_ =
      config_.patch.mode == SynthEngineMode::kKarplusStrong && config_.patch.ks.sympathetic;
  for (OrganWindSupply& wind : part_wind_) wind.reset();
  for (OrganPartParams& params : organ_part_params_) params = {};
  part_swell_depth_.fill(0.0f);
  part_swell_coeff_.fill(1.0f);
  part_swell_lp_l_.fill(0.0f);
  part_swell_lp_r_.fill(0.0f);
  channels_ = {};
  // GM power-on: channel 10 is the rhythm part (no SysEx needed). MPE mode is
  // off until an MCM turns it on, which is what a power-on default of "no zone
  // configured" means (2.2.1); the refresh below therefore takes the ordinary
  // per-channel path.
  channels_[kDrumChannelIndex].drums = true;
  mpe_.reset();
  bus_fed_channels_ = 0;
  skipped_events_ = 0;
  per_note_.assign(per_note_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  refresh_all_channel_mods();
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  // A fresh snapshot rebuilds the chains, which is their reset.
  if (prepared_) {
    part_fx_.publish();
    part_fx_.settle_quiescent(*this);
  }
  part_fx_.clear_dirty();
  tail_patch_count_ = 0;
  tail_patches_full_ = false;
  if (prepared_) tail_samples_->store(recompute_tail(), std::memory_order_relaxed);
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
  mod.pitch_cents = zoned ? mpe_.bend_semitones(ch) * 100.0f
                          : (st.pitch_bend.f14() - static_cast<float>(kBend14Center)) /
                                static_cast<float>(kBend14Center) * st.bend_range_cents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.mod_wheel01 = st.mod_wheel.f7() / 127.0f;
  mod.extra_vibrato_cents = st.mod_depth_cents * mod.mod_wheel01;
  mod.pan_units = (st.pan.f7() - 64.0f) / 63.0f * 500.0f;
  mod.breath01 = st.breath.f7() / 127.0f;
  mod.aftertouch01 = (zoned ? mpe_.pressure(ch) : st.pressure.f7()) / 127.0f;
  mod.expression01 = st.expression.f7() / 127.0f;
  mod.pitch_bend01 =
      (st.pitch_bend.f14() - static_cast<float>(kBend14Center)) / static_cast<float>(kBend14Center);
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

void NativeSynth::refresh_channel_mods_in_scope(uint8_t channel) noexcept {
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) refresh_channel_mod(ch);
  }
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

/// The deepest pitch any SOUNDING rank of @p patch plays, relative to the key. A pipe organ voices
/// one key at several pitches at once, so its lowest audible rank runs out of delay line first
/// and decides whether the voice can be carried; a muted rank decides nothing, and an
/// upperwork-only registration reaches above 1. 1.0 for every other engine and for the implicit
/// single rank, which sounds the key and nothing below it.
float lowest_pitch_mult(const NativeSynthPatch& patch) noexcept {
  if (patch.mode != SynthEngineMode::kPipeOrgan || patch.pipe_organ.rank_count <= 0) return 1.0f;
  float lowest = 0.0f;
  const int count = std::min(patch.pipe_organ.rank_count, kMaxPipeRanks);
  for (int r = 0; r < count; ++r) {
    const PipeOrganRank& rank = patch.pipe_organ.ranks[static_cast<size_t>(r)];
    if (!rank.sounding()) continue;
    // A footage the voice cannot use sounds at the key, which is how the voice reads it too.
    const float mult = rank.footage_mult > 0.01f ? rank.footage_mult : 1.0f;
    if (lowest == 0.0f || mult < lowest) lowest = mult;
  }
  return lowest > 0.0f ? lowest : 1.0f;
}

/// The integer key an engine is started on for @p note under @p pitch: the note itself, or the
/// floor of an absolute pitch, which is also what picks a drum piece or a sample zone.
uint8_t voiced_key(uint8_t note, const ComposedPitch& pitch) noexcept {
  if (!pitch.absolute) return note;
  return static_cast<uint8_t>(std::clamp(
      static_cast<int>(std::floor(static_cast<double>(note) + pitch.per_note_semitones)), 0, 127));
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

NativeSynth::LegatoResult NativeSynth::legato_continue(uint8_t ch, bool release, uint8_t note,
                                                       uint8_t attribute_type,
                                                       uint16_t attribute_data,
                                                       uint32_t source_track_id,
                                                       const NativeSynthPatch* patch) noexcept {
  ChannelState& st = channels_[ch];
  const bool legato = st.articulation == ArticulationMode::kMonoLegato;
  NativeSynthVoice* voice = nullptr;
  uint8_t to_note = note;
  uint8_t to_type = attribute_type;
  uint16_t to_data = attribute_data;
  if (release) {
    if (!legato) return {};
    const ChannelState::HeldKey* back = st.newest_held_key(true, source_track_id);
    if (back == nullptr) return {};
    voice = find_sounding(ch, note, source_track_id);
    if (voice == nullptr || voice->patch == nullptr) return {};
    to_note = back->note;
    to_type = back->attribute_type;
    to_data = back->attribute_data;
  } else {
    if (st.articulation == ArticulationMode::kPoly) return {};
    // The channel is monophonic across sources: the voice to continue from is whoever sounds
    // the newest held key.
    const ChannelState::HeldKey* held = st.newest_held_key();
    if (held == nullptr) return {};
    voice = find_sounding(ch, held->note, held->source_track_id);
    if (voice == nullptr) return {};
  }

  // Reach is a question about the sounding pitch, not the binding key the pitch was composed from.
  Sf2PerNoteVoice target;
  bind_per_note(target, ch, to_note, to_type, to_data, to_note);
  const ComposedPitch to_pitch = compose_per_note(target);
  const size_t index = static_cast<size_t>(voice - pool_.data());
  const bool carry =
      legato && voice->patch != nullptr && voice->source_track_id == source_track_id &&
      (release || voice->patch == patch) &&
      accepts_legato(voice->patch->mode,
                     voiced_key(voice->note, compose_per_note(per_note_[index])),
                     voiced_key(to_note, to_pitch), lowest_pitch_mult(*voice->patch));
  if (!carry) {
    // Counted, because nothing in the sound says whether the phrase ended or was declined.
    if (legato) legato_fallbacks_->fetch_add(1, std::memory_order_relaxed);
    if (!release) {
      // Monophonic either way: the previous note stops. Fast rather than the patch's own release,
      // which on a sustaining patch runs past a second and would leave the note it replaced
      // audible under the new one.
      freeze_mpe_voice(*voice, ch);
      voice->choke_fast(sample_rate_);
    }
    return {LegatoOutcome::kReplaced, nullptr};
  }
  const float cents_before = per_note_[index].cents;
  carry_per_note(*voice, voice->note, to_note, to_type, to_data);
  voice->retune(to_note, voiced_key(to_note, to_pitch), per_note_[index].cents - cents_before,
                sample_rate_);
  st.last_freq_hz = voice->voiced_freq_hz();
  return {LegatoOutcome::kCarried, voice};
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                          uint8_t attribute_type, uint16_t attribute_data, uint32_t source_track_id,
                          const ExcitationAxes& velocity_excitation,
                          uint32_t velocity_excitation_mask) noexcept {
  if (!prepared_) return;
  // A profile that calls velocity meaningless takes every note at full scale,
  // so the bound axes carry the dynamics on their own. 127 rather than some
  // mid value because it is the scale's identity: nothing is attenuated here
  // that a controller is not asking for.
  if (!controller_profile_.velocity_meaningful) velocity = Velocity16::from7(127);
  const uint8_t ch = channel & 0x0Fu;
  // Generic MIDI rendering resolves every channel through the shared GM/GS bank
  // rule, so a rhythm part (channel 10, a GM2 percussion bank, or a GS-assigned
  // part) reaches the drum map and a melodic channel its variation bank —
  // identically to Sf2Player. Explicit percussion patches retain their existing
  // GM-kit behavior when this mode is disabled. Resolved BEFORE the pool is
  // touched: the exclusive-group choke below reads the resolved patch, and a
  // voice allocated first would still carry the previous note's patch pointer.
  const ChannelState& st = channels_[ch];
  // Compose the per-note pitch first: an absolute pitch picks the drum piece at the target key.
  Sf2PerNoteVoice per_note;
  bind_per_note(per_note, ch, note, attribute_type, attribute_data, note);
  const ComposedPitch note_pitch = compose_per_note(per_note);
  const uint8_t drum_lookup_note = voiced_key(note, note_pitch);
  const NativeSynthPatch* patch = &config_.patch;
  uint8_t drum_kit = 0;
  if (config_.use_gm_programs) {
    const uint16_t bank = gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
    const GsToneMap map = gs_effective_tone_map(st.bank_msb, st.bank_lsb);
    if (bank == kDrumBank) {
      patch = &gm_fallback_drum_patch(drum_lookup_note);
      drum_kit = gm_fallback_drum_kit(st.program, map);
      observe_tail_patch(*patch, drum_kit, drum_lookup_note);
    } else {
      patch = &gm_fallback_patch(bank, st.program, map);
      observe_tail_patch(*patch, kNoTailKit, note);
    }
  } else if (patch->mode == SynthEngineMode::kPercussion && patch->percussion.gm_kit) {
    patch = &gm_fallback_drum_patch(drum_lookup_note);
    drum_kit = gm_fallback_drum_kit(st.program);
    observe_tail_patch(*patch, drum_kit, drum_lookup_note);
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
        if (v.key_down) freeze_mpe_voice(v, ch);
        v.choke();
      }
    }
  }
  // Legato continuation, before a voice is allocated: on a carrying channel a
  // note-on under a held key moves that voice to the new key rather than
  // starting one, so the exciter, the delay line and both envelopes run on.
  ChannelState& live = channels_[ch];
  const LegatoResult legato =
      legato_continue(ch, false, note, attribute_type, attribute_data, source_track_id, patch);
  if (legato.outcome == LegatoOutcome::kCarried) {
    if (velocity_excitation_mask != kAxisNone) {
      record_velocity_axes(*legato.voice, velocity_excitation, velocity_excitation_mask);
      legato.voice->push_excitation(velocity_excitation, velocity_excitation_mask);
    }
    live.hold_key(note, attribute_type, attribute_data, source_track_id);
    return;
  }
  live.hold_key(note, attribute_type, attribute_data, source_track_id);

  NativeSynthVoice* voice = pool_.allocate(ch, note, source_track_id);
  if (voice == nullptr) return;
  const auto bus_tails = [&] {
    return std::array<bool, 3>{piano_body_active_, guitar_halo_active_, part_halo_armed_[ch]};
  };
  const std::array<bool, 3> bus_tails_before = bus_tails();
  bus_fed_channels_ |= static_cast<uint16_t>(uint16_t{1} << ch);
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
    voice->bowed_string.attach(bowed_string_buffers_.data() + static_cast<size_t>(voice_index) *
                                                                  kBowedSlabSpans *
                                                                  bowed_string_capacity_,
                               bowed_string_capacity_);
  }
  if (!vocal_buffers_.empty()) {
    voice->vocal.attach(vocal_buffers_.data() + static_cast<size_t>(voice_index) * vocal_capacity_,
                        vocal_capacity_);
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
  // Per-note pitch (M2-104-UM §7.4.15). An absolute pitch starts the engine on the key of its
  // integer part, so a sample zone and a delay line are chosen for the pitch that sounds; the
  // struck note stays the voice's own, which is what a note-off matches.
  DrumVoiceMod voice_mod{};
  if (note_pitch.absolute) {
    per_note.zone_key = voiced_key(note, note_pitch);
    voice_mod.play_note = per_note.zone_key;
  }
  per_note.cents = per_note_cents(per_note, note_pitch);
  const GsPartMod part_mod = gs_part_mod(st.gs);
  voice->start(*patch, sample_rate_, velocity, voice_index, glide_from, st.una_corda, drum_kit,
               voice_mod, organ_percussion, part_mod);
  voice->piano_case_strike_pending = 0.0f;
  voice->piano_board_strike_pending = 0.0f;
  voice->mpe_mod_active = false;
  voice->mpe_release_frozen = false;
  voice->mpe_frozen_axis_mask = kAxisNone;
  voice->mpe_frozen_axis_present = kAxisNone;
  voice->mpe_frozen_source_mask = kAxisNone;
  voice->mpe_frozen_axis_values = {};
  voice->mpe_frozen_member_bend_cents = 0.0f;
  voice->mpe_frozen_aftertouch01 = 0.0f;
  voice->live_excitation_axes.reset();
  voice->live_mpe_pressure_axes = kAxisNone;
  voice->live_mpe_timbre_axes = kAxisNone;
  voice->live_mpe_bend_axes = kAxisNone;
  voice->mpe_member_pressure = Control32::from_raw(0);
  voice->mpe_member_timbre = Control32::from_raw(0);
  voice->mpe_member_pressure_present = false;
  voice->mpe_member_timbre_present = false;
  per_note_[voice_index] = per_note;
  // Seed the engine's excitation axes at the channel's current controllers (no
  // glide on the first sample) so a note struck mid-phrase starts at the live
  // breath / brightness rather than gliding in from the preset. An engine reads
  // only the axes it declares, and one that declares none is untouched.
  {
    uint32_t present = kAxisNone;
    ExcitationAxes base = channel_excitation(st.axes, present);
    for (size_t i = 0; i < kControllerAxisCount; ++i) {
      const ControllerAxis axis = static_cast<ControllerAxis>(i);
      if (excitation_axis_mask(axis) == kAxisNone || !st.axes.has(axis)) continue;
      voice->live_excitation_axes.set(axis, st.axes.values[i]);
    }
    for (size_t i = 0; i < kControllerAxisCount; ++i) {
      const ControllerAxis axis = static_cast<ControllerAxis>(i);
      if (excitation_axis_mask(axis) == kAxisNone || !st.axes.has(axis)) continue;
      const uint32_t axis_bit = 1u << static_cast<uint32_t>(i);
      if ((st.mpe_pressure_axes & axis_bit) != 0u) {
        voice->live_mpe_pressure_axes |= axis_bit;
      }
      if ((st.mpe_timbre_axes & axis_bit) != 0u) {
        voice->live_mpe_timbre_axes |= axis_bit;
      }
      if ((st.mpe_bend_axes & axis_bit) != 0u) {
        voice->live_mpe_bend_axes |= axis_bit;
      }
    }
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      voice->mpe_member_pressure_present =
          mpe_.own_control(ch, MpeDimension::kPressure, &voice->mpe_member_pressure);
      voice->mpe_member_timbre_present =
          mpe_.own_control(ch, MpeDimension::kTimbre, &voice->mpe_member_timbre);
    }
    if (velocity_excitation_mask != kAxisNone) {
      if ((velocity_excitation_mask & kAxisForce) != 0u) {
        base.force = velocity_excitation.force;
      }
      if ((velocity_excitation_mask & kAxisPosition) != 0u) {
        base.position = velocity_excitation.position;
      }
      if ((velocity_excitation_mask & kAxisBrightness) != 0u) {
        base.brightness = velocity_excitation.brightness;
      }
      if ((velocity_excitation_mask & kAxisMorph) != 0u) {
        base.morph = velocity_excitation.morph;
      }
      present |= velocity_excitation_mask;
      record_velocity_axes(*voice, velocity_excitation, velocity_excitation_mask);
    }
    voice->seed_excitation(base, present);
    if (!controller_profile_.velocity_meaningful) voice->hold_onset();
  }
  // Bus-level piano body (the direct-share attenuation, the modal soundboard
  // and the pedal-gated sympathetic bank). In GM mode the engine is resolved
  // per program, so the body is tuned at the first piano note-on rather than in
  // prepare(); a later program asking for a different board mix only re-states
  // the return level, since other notes may still be ringing through the bank.
  // Allocation-free, like the lazy per-part prepare on the Sf2Player fallback path.
  if (patch->mode == SynthEngineMode::kPiano) {
    if (part_fx_.enabled()) {
      PianoPartState& state = part_piano_state_[ch];
      if (!state.prepared) {
        part_soundboards_[ch].prepare(sample_rate_, patch->piano.soundboard);
        part_resonance_[ch].prepare(sample_rate_);
        state.prepared = true;
      } else if (state.soundboard_mix != patch->piano.soundboard) {
        part_soundboards_[ch].set_mix(patch->piano.soundboard);
      }
      state.soundboard_mix = patch->piano.soundboard;
      // The current routing snapshot may still be dirty. Queue the strike for
      // the first process() sample, where the part-owned board is known to be
      // the destination regardless of whether the part is bussed today.
      voice->piano_case_strike_pending = voice->piano.case_strike();
      voice->piano_board_strike_pending = voice->piano.board_strike();
    } else {
      if (piano_body_soundboard_ < 0.0f) {
        soundboard_.prepare(sample_rate_, patch->piano.soundboard);
        resonance_.prepare(sample_rate_);
      } else if (piano_body_soundboard_ != patch->piano.soundboard) {
        soundboard_.set_mix(patch->piano.soundboard);
      }
      piano_body_soundboard_ = patch->piano.soundboard;
      // The blow into the structure, which the board is struck with once rather
      // than driven by. After any prepare() above, which clears the network.
      soundboard_.strike(voice->piano.case_strike());
      soundboard_.strike_board(voice->piano.board_strike());
    }
    piano_body_active_ = true;
  }
  // GM resolution can select a pipe organ even when the configured patch is
  // subtractive. Prepare wind and swell from the resolved patch on this part;
  // non-organ voices receive unity wind in process_impl().
  if (patch->mode == SynthEngineMode::kPipeOrgan) {
    OrganPartParams& params = organ_part_params_[ch];
    const float trem_rate = patch->pipe_organ.tremulant_rate_hz;
    const float trem_depth = patch->pipe_organ.tremulant_depth;
    const float sag = patch->pipe_organ.wind_sag;
    const bool regulated = patch->pipe_organ.wind_regulated;
    if (!params.armed || params.tremulant_rate_hz != trem_rate ||
        params.tremulant_depth != trem_depth || params.wind_sag != sag ||
        params.wind_regulated != regulated) {
      part_wind_[ch].prepare(sample_rate_, trem_rate, trem_depth, sag, regulated);
      params.tremulant_rate_hz = trem_rate;
      params.tremulant_depth = trem_depth;
      params.wind_sag = sag;
      params.wind_regulated = regulated;
      params.armed = true;
    }
    part_swell_depth_[ch] = patch->pipe_organ.swell;
  }
  // Bus-level open-string halo, the same lazy-arming rule as the piano body
  // above but for a Karplus-Strong voice that asks for it. Guarded so an
  // already-armed bank (configured, or armed by an earlier note-on) is never
  // re-prepared, which would clear its ringing state.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      !part_fx_.enabled() && !guitar_halo_active_) {
    guitar_halo_.prepare_guitar_sympathetic(sample_rate_);
    guitar_halo_active_ = true;
  }
  // The part's own halo too, which the render drives in place of the shared
  // one whenever the part is bussed.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic &&
      part_fx_.enabled() && !part_halo_armed_[ch]) {
    part_halo_[ch].prepare_guitar_sympathetic(sample_rate_);
    part_halo_armed_[ch] = true;
  }
  // A bank outlives any one patch, so every halo note restates how a hand damps it.
  if (patch->mode == SynthEngineMode::kKarplusStrong && patch->ks.sympathetic) {
    if (part_fx_.enabled()) {
      part_halo_[ch].set_hand_damping(patch->ks.halo_damp_s);
    } else {
      guitar_halo_.set_hand_damping(patch->ks.halo_damp_s);
    }
  }
  // A body or halo armed by this note rings past it, which the heard-patch tail counts.
  if (config_.use_gm_programs && bus_tails_before != bus_tails()) raise_tail();
  channels_[ch].last_freq_hz = voice->base_freq_hz;
  // A second note on a member channel is what makes the channel's bend and
  // pressure ambiguous, so the attribution is recomputed as the set changes.
  // The new voice needs it anyway: it starts reading a mod on the next sample.
  refresh_mpe_note_mods(ch);
}

void NativeSynth::record_velocity_axes(NativeSynthVoice& voice, const ExcitationAxes& velocity,
                                       uint32_t mask) noexcept {
  for (size_t i = 0; i < kControllerAxisCount; ++i) {
    const ControllerAxis axis = static_cast<ControllerAxis>(i);
    if ((mask & excitation_axis_mask(axis)) == 0u) continue;
    const uint32_t axis_bit = 1u << static_cast<uint32_t>(i);
    voice.live_excitation_axes.set(axis, excitation_value(velocity, axis));
    voice.live_mpe_pressure_axes &= ~axis_bit;
    voice.live_mpe_timbre_axes &= ~axis_bit;
    voice.live_mpe_bend_axes &= ~axis_bit;
  }
}

void NativeSynth::note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.release_key(note, source_track_id);
  // Releasing a key under a slur returns the voice to the key still held rather
  // than ending the phrase. A note-off whose key is NOT the one sounding falls
  // straight past here and past the release loop below, which is what keeps a
  // slur alive when the old key is let go late.
  if (legato_continue(ch, true, note, 0, 0, source_track_id, nullptr).outcome ==
      LegatoOutcome::kCarried) {
    return;
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.source_track_id == source_track_id &&
        v.key_down) {
      freeze_mpe_voice(v, ch);
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();  // pedal up: the damper falls now
      } else if (st.sustain_level.f7() < 127.0f && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp((127.0f - st.sustain_level.f7()) / 63.0f);
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

void NativeSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    if (event.sysex_payload == nullptr || event.sysex_payload_size == 0) return;
    // Part state is the render thread's on both paths, as Sf2Player's is.
    apply_part_sysex(event.sysex_payload, event.sysex_payload_size);
    // Offline the stream carries the file's GS EFX; live the host pushes it
    // through on_control_sysex, so a SysEx scheduled in a clip is not applied.
    if (config_.realize_efx_inline && config_.use_gm_programs && part_fx_.enabled()) {
      apply_efx_sysex(event.sysex_payload, event.sysex_payload_size);
    }
    return;
  }
  // Tracking comes ahead of both, because the profile reads the combined value
  // a member channel's controller carries and that value is only current once
  // the message in hand has been tracked.
  track_mpe_input(u);
  ChannelVoiceEvent ev;
  if (!decode_channel_voice(u, &ev)) {
    ++skipped_events_;  // Reserved status.
    return;
  }
  const uint8_t ch = ev.channel & 0x0Fu;
  // A message the zone ignores must not reach a profile axis either.
  if (ev.kind == ChannelVoiceKind::PolyPressure &&
      mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) {
    return;
  }
  if (ev.kind == ChannelVoiceKind::ControlChange &&
      (((ev.note == 0 || ev.note == 32) && mpe_.ignores(ch, MpeIgnorable::kBankSelect)) ||
       ((ev.note == 126 || ev.note == 127) && mpe_.ignores(ch, MpeIgnorable::kModeMessage)))) {
    return;
  }
  // The profile runs before the protocol dispatch so a note-on whose velocity
  // is bound to an axis starts from its own value rather than the previous
  // note's.
  ExcitationAxes velocity_excitation{};
  uint32_t velocity_excitation_mask = kAxisNone;
  if (ev.kind == ChannelVoiceKind::NoteOn) {
    velocity_excitation = apply_note_on_controller_input(u, &velocity_excitation_mask);
  } else {
    apply_controller_input(u);
  }
  switch (ev.kind) {
    case ChannelVoiceKind::NoteOn:
      note_on(ch, ev.note, ev.velocity, ev.index, ev.attribute_data, event.source_track_id,
              velocity_excitation, velocity_excitation_mask);
      break;
    case ChannelVoiceKind::NoteOff:
      note_off(ch, ev.note, event.source_track_id);
      break;
    case ChannelVoiceKind::PitchBend:
      channels_[ch].pitch_bend = ev.bend;
      mpe_.track_bend(ch, ev.bend);
      // A manager's bend applies to every sounding note in its zone (2.2.6), so
      // like its pressure it reaches past the channel it arrived on.
      refresh_channel_mods_in_scope(ch);
      break;
    case ChannelVoiceKind::ChannelPressure:
      channel_pressure(ch, ev.value);
      break;
    case ChannelVoiceKind::PolyPressure:
      poly_pressure(ch, ev.note, ev.value);
      break;
    case ChannelVoiceKind::ControlChange:
      control_change(ch, ev.note, ev.value);
      break;
    case ChannelVoiceKind::RegisteredController:
    case ChannelVoiceKind::AssignableController:
      registered_controller(u, ev);
      break;
    case ChannelVoiceKind::ProgramChange:
      // GS drum-kit select: in gm_kit mode the drum channel's program picks the
      // kit variation (Room/Power/808/...). Melodic patches ignore it.
      // In MIDI Mode 3 a zone is monotimbral, so a program change reaching a
      // member channel is ignored rather than splitting the zone across two
      // patches (2.3.3). Mode 4 is the case that permits it.
      if (mpe_.ignores(ch, MpeIgnorable::kProgramChange)) break;
      channels_[ch].program = ev.program;
      // The MIDI 2.0 bank-valid flag: the bank bytes carry meaning only when it
      // is set, and an unset flag leaves the channel's bank alone.
      if ((ev.flags & 0x01u) != 0) {
        channels_[ch].bank_msb = ev.bank_msb;
        channels_[ch].bank_lsb = ev.bank_lsb;
      }
      refresh_part_rig(ch);
      break;
    case ChannelVoiceKind::RelativeRegistered:
    case ChannelVoiceKind::RelativeAssignable:
      if (!relative_controller(ev)) ++skipped_events_;  // A parameter this synth does not hold.
      break;
    case ChannelVoiceKind::PerNotePitchBend:
      per_note_pitch_.set_per_note_bend(ch, ev.note, ev.bend);
      refresh_per_note_voices(ch, ev.note, false);
      break;
    case ChannelVoiceKind::RegisteredPerNote:
      if (ev.index != kRpncPitch725) {
        ++skipped_events_;
        break;
      }
      per_note_pitch_.set_pitch_7_25(ch, ev.note, ev.value);
      refresh_per_note_voices(ch, ev.note, false);
      break;
    case ChannelVoiceKind::AssignablePerNote:
      ++skipped_events_;
      break;
    case ChannelVoiceKind::PerNoteManagement:
      apply_per_note_management(
          per_note_pitch_, ch, ev.note, (ev.flags & 0x02u) != 0, (ev.flags & 0x01u) != 0,
          pool_.begin(), pool_.end(), [this](NativeSynthVoice& v) noexcept -> PerNoteBinding* {
            return v.active ? &per_note_[static_cast<size_t>(&v - pool_.data())].binding : nullptr;
          });
      refresh_per_note_voices(ch, ev.note, false);
      break;
  }
}

bool NativeSynth::profile_binds_cc(uint8_t controller) const noexcept {
  for (size_t i = 0; i < controller_profile_.binding_count(); ++i) {
    const ControllerBinding& binding = controller_profile_.binding_at(i);
    if (binding.input == ControllerInput::kControlChange && binding.index == controller) {
      return true;
    }
  }
  return false;
}

void NativeSynth::apply_part_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  if (gs_sysex_resets(msg.kind)) {
    // GM System On closes NRPN reception, the NRPNs being Roland's; GS Reset reopens it.
    const bool gm = msg.kind != GsSysExKind::kGsReset;
    for (uint8_t ch = 0; ch < 16; ++ch) {
      // A reset silences the pre-reset voices and returns the part to its power-on state --
      // program, bank variation, volume, pan, expression and the rest of the controllers -- in
      // place, the CC#32 tone map (40 4x 00) included. What the host configured (articulation)
      // is kept.
      all_sound_off(ch);
      ChannelState& st = channels_[ch];
      const ArticulationMode articulation = st.articulation;
      st = ChannelState{};
      st.articulation = articulation;
      st.rx_nrpn = !gm;
      st.drums = ch == kDrumChannelIndex;
    }
    refresh_all_channel_mods();
    // Program and bank returned to power-on, so every part's rig binding is re-derived.
    for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
    return;
  }
  uint16_t rig_dirty = 0;
  bool eg_moved = false;
  gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    const bool rhythm_write = w.param == GsParam::kUseForRhythmPart;
    // USE FOR RHYTHM PART is the one GS row whose out-of-range value reads as
    // map 1 (docs/gs.md). NativeSynth stores only the rhythm/melodic choice,
    // so every nonzero byte, including >2, means rhythm here.
    if (entry == nullptr || (!rhythm_write && !gs_value_in_range(*entry, w.value))) return false;
    ChannelState& st = channels_[w.part & 0x0Fu];
    if (rhythm_write) {
      const bool drums = w.value != 0;
      if (st.drums != drums) {
        st.drums = drums;
        rig_dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
      }
    } else if (w.param == GsParam::kPartToneModify) {
      const GsPartParams before = st.gs;
      gs_apply_tone_modify(st.gs, w.index, w.value);
      eg_moved |= gs_eg_times_differ(before, st.gs);
    } else if (w.param == GsParam::kPartRxNrpn) {
      st.rx_nrpn = w.value != 0;
    }
    return true;
  });
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((rig_dirty & (uint16_t{1} << ch)) != 0) refresh_part_rig(ch);
  }
  if (eg_moved) raise_tail();
}

void NativeSynth::bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note,
                                uint8_t attribute_type, uint16_t attribute_data,
                                int16_t zone_key) const noexcept {
  state.binding.bind(channel, note);
  state.has_attribute_pitch = attribute_type == kAttributePitch79;
  state.attribute_pitch_q7_9 = state.has_attribute_pitch ? attribute_data : uint16_t{0};
  state.zone_key = zone_key;
  state.cents = 0.0f;
}

ComposedPitch NativeSynth::compose_per_note(const Sf2PerNoteVoice& state) const noexcept {
  NotePitchRequest req;
  req.note = state.binding.note;
  req.has_attribute_pitch = state.has_attribute_pitch;
  req.attribute_pitch_q7_9 = state.attribute_pitch_q7_9;
  req.per_note = state.binding.pitch_inputs(per_note_pitch_);
  req.per_note_bend_sensitivity = per_note_bend_sensitivity_[state.binding.channel & 0x0Fu];
  // The channel terms stay on the channel mod; only the per-note share is taken.
  return compose_note_pitch(req);
}

void NativeSynth::refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept {
  state.cents = per_note_cents(state, compose_per_note(state));
}

void NativeSynth::refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  for (size_t i = 0; i < per_note_.size(); ++i) {
    Sf2PerNoteVoice& state = per_note_[i];
    if (pool_.data()[i].active && state.binding.channel == ch &&
        (all_notes || state.binding.note == note)) {
      refresh_per_note_pitch(state);
    }
  }
}

void NativeSynth::carry_per_note(const NativeSynthVoice& voice, uint8_t from_note, uint8_t to_note,
                                 uint8_t attribute_type, uint16_t attribute_data) noexcept {
  Sf2PerNoteVoice& state = per_note_[static_cast<size_t>(&voice - pool_.data())];
  // retune() moves the engine by the interval between the keys, so the key it nominally sounds
  // moves by the same interval from the one it was started on.
  const int16_t zone_key = static_cast<int16_t>(
      static_cast<int>(state.zone_key) + static_cast<int>(to_note) - static_cast<int>(from_note));
  bind_per_note(state, voice.channel, to_note, attribute_type, attribute_data, zone_key);
  refresh_per_note_pitch(state);
}

}  // namespace sonare::midi::synth
