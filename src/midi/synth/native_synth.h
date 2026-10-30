#pragma once

/// @file native_synth.h
/// @brief NativeSynth — the patch-driven synthesis engine: a patch POD tagged
///        with a SynthEngineMode (declared below, one enumerator per engine),
///        the unison subtractive voice
///        (PolyBLEP oscillators -> TPT SVF -> exponential DAHDSR VCA) with
///        the per-mode cores embedded beside it, and a 16-channel
///        MidiInstrument host around the shared voice pool.
///
/// Two consumers share the voice:
///   - NativeSynth (this file): a standalone patch-driven instrument.
///   - Sf2Player: per-note synth fallback when no SoundFont covers a program
///     (the data-free floor — every GM program stays audible with zero data).
///
/// RT contract (MidiInstrument): prepare() runs on the control thread and is
/// the only allocating call; on_event()/process() are allocation-free,
/// lock-free and IO-free. Voices reference their patch by pointer — patches
/// must outlive the voice (config member / static fallback tables).
///
/// Determinism: no RNG, no wall clock. Unison detune jitter, oscillator
/// start phases, drift-LFO variation and every engine's noise stream derive
/// from the counter-based (voice_index, note, age) hash — or from the note
/// alone under SynthRetrigger::kNote — so identical event streams bounce
/// bit-identically within one build.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "midi/channel_voice_decode.h"
#include "midi/control_value.h"
#include "midi/controller_profile.h"
#include "midi/instrument.h"
#include "midi/mpe.h"
#include "midi/per_note_state.h"
#include "midi/source_residual.h"
#include "midi/synth/additive_voice.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/envelope.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/fm_voice.h"
#include "midi/synth/free_reed_voice.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/modal_voice.h"
#include "midi/synth/native_synth_patch.h"
#include "midi/synth/native_synth_voice.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/part_fx_stage.h"
#include "midi/synth/patch_clamp.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/render_path_record.h"
#include "midi/synth/sample_bank.h"
#include "midi/synth/sample_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/vocal_voice.h"
#include "midi/synth/voice_pool.h"
#include "util/constants.h"
#include "util/numeric_validation.h"

namespace sonare::midi::synth {

namespace native_synth_detail {

/// The engine excitation slot @p axis fills, or kAxisNone for a channel-level axis.
inline uint32_t excitation_axis_mask(ControllerAxis axis) noexcept {
  switch (axis) {
    case ControllerAxis::kExcitation:
      return kAxisForce;
    case ControllerAxis::kPosition:
      return kAxisPosition;
    case ControllerAxis::kBrightness:
      return kAxisBrightness;
    case ControllerAxis::kMorph:
      return kAxisMorph;
    case ControllerAxis::kNone:
    case ControllerAxis::kLoudness:
    case ControllerAxis::kPitchCents:
    case ControllerAxis::kVibratoDepth:
      return kAxisNone;
  }
  return kAxisNone;
}

/// The value @p axes holds in @p axis's slot; 0 for a channel-level axis.
inline float excitation_value(const ExcitationAxes& axes, ControllerAxis axis) noexcept {
  switch (axis) {
    case ControllerAxis::kExcitation:
      return axes.force;
    case ControllerAxis::kPosition:
      return axes.position;
    case ControllerAxis::kBrightness:
      return axes.brightness;
    case ControllerAxis::kMorph:
      return axes.morph;
    case ControllerAxis::kNone:
    case ControllerAxis::kLoudness:
    case ControllerAxis::kPitchCents:
    case ControllerAxis::kVibratoDepth:
      return 0.0f;
  }
  return 0.0f;
}

}  // namespace native_synth_detail

struct NativeSynthConfig {
  NativeSynthPatch patch;
  /// Master output gain applied to the summed voices (linear).
  float gain = 0.5f;
  /// Voice pool size (clamped to [1, kMaxSynthVoices]).
  int polyphony = 16;
  /// Gentle gain-neutral tanh on the mix bus in [0,1] (0 = clean) — glues a
  /// stack of voices together.
  float bus_drive = 0.0f;
  /// DC blocker on the mix bus (the physical-model voices can carry a small
  /// DC component).
  bool dc_block = true;
  /// Resolve each melodic channel from its GM bank/program state and route
  /// channel 10 through the GM drum-kit map. This is intended for generic MIDI
  /// file playback; false keeps a deliberately selected patch fixed.
  bool use_gm_programs = false;
  /// Builds a streaming processor from a name and JSON params, for part rigs
  /// and GS insertion units. Null (the default) leaves the synth with no part
  /// buses, so every part renders dry; the host that links the effects wires it.
  GsEfxStageFactory insert_factory;
  /// Bind the bank's default rig to a part with no entry of its own, for the
  /// program it plays (docs/voicing.md). Reached only under use_gm_programs.
  bool bank_rig_binding = true;
  /// Offline hosts only: realise a pending rig or GS EFX change at the top of
  /// the block, and read GS EFX SysEx from the event stream. Realising
  /// allocates, so a live host leaves this false and pushes EFX SysEx through
  /// on_control_sysex instead.
  bool realize_efx_inline = false;
};

/// Continuously automatable NativeSynth parameters, addressed through
/// MidiInstrument::parameter_id_for_key / apply_parameter. The ordinals are the
/// persisted automation-target ids, so they are append-only: never renumber an
/// existing entry.
///
/// Every id reaches sounding voices from the next process() block. A voice
/// reads some patch fields every sample; the state it derives from the others
/// at note-on is re-derived by NativeSynthVoice::refresh_live() at the top of
/// the block after the write, so a held note lands where a note struck at the
/// new value would. Voices of a GM program or kit, and choked voices, are not
/// refreshed.
///
/// The set is the patch's own continuous fields, less the structural ones a
/// voice pool or a DSP topology depends on; anything continuous the patch grows
/// belongs here in the same change, or the two disagree in silence.
enum class NativeSynthParamId : unsigned int {
  kGain = 0,
  kBusDrive = 1,
  kCutoffHz = 2,
  kResonanceQ = 3,
  kDrive = 4,
  kKeyTrack = 5,
  kEnvToCutoffCents = 6,
  kVelToCutoffCents = 7,
  kAmpAttackMs = 8,
  kAmpDecayMs = 9,
  kAmpSustain = 10,
  kAmpReleaseMs = 11,
  kFilterAttackMs = 12,
  kFilterDecayMs = 13,
  kFilterSustain = 14,
  kFilterReleaseMs = 15,
  kLfoRateHz = 16,
  kLfoToPitchCents = 17,
  kLfo2RateHz = 18,
  kGlideMs = 19,
  kBodyMix = 20,
  kStereoSpread = 21,
  kDetuneCents = 22,
  kDriftCents = 23,
  kPitchOffsetCents = 24,
  kHpCutoffHz = 25,
  kSampleHoldHz = 26,
  kBitDepth = 27,
  kDriftRateHz = 28,
  kAmpDelayMs = 29,
  kAmpHoldMs = 30,
  kFilterDelayMs = 31,
  kFilterHoldMs = 32,
};

/// Bit for @p id in a refresh_live() mask.
inline constexpr uint64_t native_synth_param_bit(NativeSynthParamId id) noexcept {
  return uint64_t{1} << static_cast<unsigned int>(id);
}

/// JSON-key name for @p id (the same string a SynthPatch object would use), or
/// nullptr for an unknown id.
const char* native_synth_param_name(NativeSynthParamId id) noexcept;
/// Number of automatable parameters, for enumeration by a host UI.
size_t native_synth_param_count() noexcept;
/// JSON-key name at @p index in [0, native_synth_param_count()), or nullptr.
const char* native_synth_param_name_at(size_t index) noexcept;
/// The facade key and the [lo, hi] range @p id is clamped to and reports from
/// describe_parameter. Returns false for an unknown id; a null out-pointer is skipped.
bool native_synth_param_range(NativeSynthParamId id, const char** key, float* lo,
                              float* hi) noexcept;

/// The "gm" controller preset: CC2 on the force axis, CC74 on the second one,
/// note-on velocity meaningful. What the synth answered before a profile layer
/// existed, so a host that sets none keeps its sound.
ControllerProfile default_controller_profile() noexcept;

/// Standalone patch-driven MidiInstrument: all 16 channels play the same
/// patch with BuiltinSynth-compatible channel semantics plus the default-
/// modulator CCs (CC1 vibrato, CC7/CC11 gain, CC10 pan, pitch bend, CC64
/// sustain, CC120/121/123 channel modes).
///
/// With an insert factory, a part with a rig or a GS insertion-effect route
/// renders through PartFxStage: its voices sum into the part's bus, which the
/// rig runs on in place. The GS EFX block and part assignment are read only
/// under use_gm_programs; the part edits (TONE MODIFY, Rx NRPN) and the resets
/// that clear them apply in both modes (docs/gs.md).
class NativeSynth final : public MidiInstrument, private PartFxHost {
 public:
  explicit NativeSynth(const NativeSynthConfig& config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override { return true; }
  void reset() override;
  /// The rig chains are not latency-compensated, but their tails are counted.
  int tail_samples() const noexcept override {
    const int base = static_cast<int>(
        std::clamp<int64_t>(tail_samples_->load(std::memory_order_relaxed), 0,
                            static_cast<int64_t>(std::numeric_limits<int>::max())));
    return numeric::saturating_add(base, std::max(part_fx_.tail_samples(), 0));
  }
  /// A received GS envelope-time edit raises the tail (raise_tail); in GM mode and on a kit
  /// preset the first note-on narrows it to the patches heard (observe_tail_patch).
  bool tail_follows_events() const noexcept override { return true; }
  /// CONTROL thread, offline tail probe: publish a pending inline EFX change
  /// without processing audio, so the host reads the chain that replayed events
  /// actually selected.
  bool materialize_tail_probe() override;
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;
  /// CONTROL thread: a host-pushed GS EFX block write or reset, realised here
  /// and handed to the audio thread wait-free. The live path's only EFX writer:
  /// a SysEx scheduled inside a clip reaches on_event, which ignores it unless
  /// realize_efx_inline is set.
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override;
  /// CONTROL thread: install @p rig for part @p part, or for every part no entry
  /// names when @p part is kPartRigAllParts, and rebuild the chains when
  /// prepared. Applies whether or not use_gm_programs is set; kBank binds
  /// nothing without it.
  bool set_part_rig(uint8_t part, const PartRig& rig) noexcept override;
  /// CONTROL thread: the processor names of the chain last published for
  /// @p part, in signal order (test/diagnostic).
  std::vector<std::string> part_rig_stage_names(uint8_t part) const;
  /// AUDIO thread: the block's first device frame, which a render-path
  /// recorder stamps its records with. Ignored with no recorder attached.
  void set_transport(const transport::TransportState& state) noexcept override;
  /// CONTROL thread, offline renders only: record the realised signal path into
  /// @p recorder until it is detached with nullptr (diagnostic). Refused, and
  /// nothing attached, unless the synth realises its EFX inline.
  bool set_render_path_recorder(RenderPathRecorder* recorder) noexcept;
  int parameter_id_for_key(const std::string& key) const noexcept override;
  bool apply_parameter(unsigned int param_id, float value) noexcept override;
  bool describe_parameter(unsigned int param_id,
                          automation::ParameterDescription* out) const override;

  /// CONTROL thread: the bank the kSample engine reads; nullptr leaves that
  /// engine silent. Sounding voices keep the bank they started on, so swap it
  /// while nothing is playing.
  ///
  /// Two forms. The borrowed one is for a caller that already outlives the
  /// instrument; the owning one takes a share, which is what a host binding
  /// wants — a caller that drops its own handle mid-render then frees nothing a
  /// voice is reading.
  void set_sample_bank(const SampleBank* bank) noexcept {
    owned_sample_bank_.reset();
    sample_bank_ = bank;
    raise_tail();
  }
  void set_sample_bank(std::shared_ptr<const SampleBank> bank) noexcept {
    owned_sample_bank_ = std::move(bank);
    sample_bank_ = owned_sample_bank_.get();
    raise_tail();
  }
  const SampleBank* sample_bank() const noexcept { return sample_bank_; }

  const NativeSynthPatch& patch() const noexcept { return config_.patch; }
  /// Instrument master gain (test/diagnostic; the automated kGain target).
  float gain() const noexcept { return config_.gain; }

  /// Currently sounding voices (test/diagnostic).
  int active_voice_count() const noexcept { return pool_.active_count(); }

  /// The device spelling the synth reads its expression axes through. Replacing
  /// it clears every channel's axis values: the new profile's bindings say
  /// nothing about what the old ones had reached, and carrying them over would
  /// leave an axis held at a value no binding can now move. Always accepted —
  /// every engine here reads at least one axis through it.
  bool set_controller_profile(const ControllerProfile& profile) noexcept override;
  const ControllerProfile* controller_profile() const noexcept override {
    return &controller_profile_;
  }

  /// Always accepted — every engine here has an articulation, including the
  /// ones that decline to be carried: those answer a slur by counting a
  /// fallback and playing the note.
  bool set_articulation(uint8_t channel, ArticulationMode mode) noexcept override {
    channels_[channel & 0x0Fu].articulation = mode;
    return true;
  }
  bool articulation(uint8_t channel, ArticulationMode* out) const noexcept override {
    if (out == nullptr) return false;
    *out = channels_[channel & 0x0Fu].articulation;
    return true;
  }

  /// Both a declining engine and a pitch below the engine's delay line land
  /// here.
  /// Safe to read from the control thread while process() runs.
  bool legato_fallback_count(uint64_t* out) const noexcept override {
    if (out == nullptr) return false;
    *out = legato_fallbacks_->load(std::memory_order_relaxed);
    return true;
  }

  /// Channel-voice messages received but not acted on: reserved statuses, per-note controllers
  /// other than pitch, and relative controllers on a parameter this synth does not hold. Cleared
  /// by reset().
  uint64_t skipped_event_count() const noexcept { return skipped_events_; }

 private:
  struct ChannelState {
    bool sustain = false;                           // CC64 >= 64 (dampers lifted)
    Control32 sustain_level = Control32::from7(0);  // CC64 (half-pedal damper position)
    /// CC66 pedal position. Sostenuto captures exactly the keys held at the
    /// DOWN EDGE, so the capture sweep must be edge-triggered: a pedal that
    /// keeps sending values >= 64 would otherwise capture notes struck after
    /// the press, which is the opposite of what the pedal means.
    bool sostenuto_down = false;
    bool una_corda = false;  // CC67 soft pedal
    /// Drawbar-organ percussion: charged, and spent by the next note-on that
    /// takes it. Recharges only when the channel has no key held, which is what
    /// makes percussion sound on the first note of a phrase and not on the ones
    /// played under it. Per channel rather than per voice (a voice cannot see
    /// its neighbours) and rather than per patch (a patch outlives the phrase).
    bool percussion_armed = true;
    /// Rhythm part: resolves through the drum map instead of the melodic
    /// programs. Channel 10 by default (GM), matching Sf2Player.
    bool drums = false;
    /// Controller values at MIDI 2.0 width: a float law reads f7(), a switch u7().
    Control32 volume = Control32::from7(100);      // CC7
    Control32 expression = Control32::from7(127);  // CC11
    Control32 pan = Control32::from7(64);          // CC10
    Control32 mod_wheel = Control32::from7(0);     // CC1
    /// CC2 and channel aftertouch as the channel last sent them, unscaled by
    /// any engine's own axis. The per-engine breath fields below carry a 255
    /// "untouched" sentinel because they override a preset value; these two are
    /// controller state and start at zero, which is what a channel that has
    /// sent nothing means.
    Control32 breath = Control32::from7(0);
    Control32 pressure = Control32::from7(0);
    uint8_t program = 0;  // last program change (or GM melodic program)
    uint8_t bank_msb = 0;
    uint8_t bank_lsb = 0;
    Bend32 pitch_bend = Bend32::center();
    /// Every controller as last sent, which is what an EFX CONTROL SOURCE reads.
    std::array<Control32, 128> cc_position = gs_default_cc_positions_wide();
    /// Expression axes as the channel's ControllerProfile has resolved them.
    /// Which controller reaches which axis is the profile's to say, and which
    /// axis an engine reads is engine_axis_capability()'s; nothing between them
    /// names a CC number. An axis no binding has reached is absent rather than
    /// zero, so the patch's own voicing, the channel's own pitch and a unit gain
    /// all stand until a controller actually arrives.
    ControllerAxisState axes;
    /// The last writer for profile axes that can be reached by MPE pressure,
    /// timbre, or bend.  A zero bit means an ordinary controller is the last
    /// writer, so a manager reset can clear only stale MPE-derived axes without
    /// erasing an unrelated CC mapping on the same axis.
    uint32_t mpe_pressure_axes = kAxisNone;
    uint32_t mpe_timbre_axes = kAxisNone;
    uint32_t mpe_bend_axes = kAxisNone;
    MpeDimension last_mpe_controller = MpeDimension::kPressure;
    ChannelParamState params;
    /// Persistent GS part edits (TONE MODIFY 1-8). They survive controller,
    /// program and bank changes; GS Reset, GM System On and reset() clear them.
    GsPartParams gs;
    /// 40 1x 0A RX NRPN; GM System On turns it off and GS Reset back on.
    bool rx_nrpn = true;
    float bend_range_cents = 200.0f;
    /// MODULATION LFO1 PITCH DEPTH (40 2x 04), the depth CC1 reaches at full.
    float mod_depth_cents = gs_mod_depth_cents(kGsModDepthDefault);
    /// Previous note's frequency (glide source; 0 = none yet).
    float last_freq_hz = 0.0f;
    /// What a second note-on does while the first is still held.
    ArticulationMode articulation = ArticulationMode::kPoly;
    /// Keys held on this channel, oldest first. Ordered rather than a bitmap
    /// because last-note priority is a question about the order they were
    /// pressed in, which a set cannot answer. The Note On attribute stays with
    /// its key so a legato return restores that key's absolute pitch.
    struct HeldKey {
      uint8_t note = 0;
      uint8_t attribute_type = 0;
      uint16_t attribute_data = 0;
      /// The source track that pressed the key; a key and the voice it sounds have one owner.
      uint32_t source_track_id = 0;
    };
    /// Ten fingers plus margin; a press past that is not recorded rather than
    /// displacing an older key, so the note still sounds and only the
    /// return-to priority loses it.
    std::array<HeldKey, 16> held_keys{};
    uint8_t held_count = 0;

    /// Records @p note as held by @p source. A repeat moves it to the top, which
    /// is what a re-press means for last-note priority.
    void hold_key(uint8_t note, uint8_t attribute_type, uint16_t attribute_data,
                  uint32_t source) noexcept {
      release_key(note, source);
      if (held_count < held_keys.size()) {
        held_keys[held_count++] = {note, attribute_type, attribute_data, source};
      }
    }
    /// Removes @p note held by @p source from the stack; a key that is not there is a no-op.
    void release_key(uint8_t note, uint32_t source) noexcept {
      for (uint8_t i = 0; i < held_count; ++i) {
        if (held_keys[i].note != note || held_keys[i].source_track_id != source) continue;
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < held_count; ++j) {
          held_keys[j - 1] = held_keys[j];
        }
        --held_count;
        return;
      }
    }
    /// The most recently pressed held key and its Note On attribute, or null. With @p owned
    /// the search is limited to keys @p source pressed.
    const HeldKey* newest_held_key(bool owned = false, uint32_t source = 0) const noexcept {
      for (uint8_t i = held_count; i > 0; --i) {
        if (!owned || held_keys[i - 1].source_track_id == source) return &held_keys[i - 1];
      }
      return nullptr;
    }
  };

  /// The voice a note-off or a legato continuation on @p ch means: sounding on
  /// @p note, key still down, not yet releasing. One matching rule in one
  /// place, because a legato continuation moves `note` to the new key and every
  /// reader of it has to agree on that.
  NativeSynthVoice* find_sounding(uint8_t ch, uint8_t note, uint32_t source_track_id) noexcept;
  void note_on(uint8_t channel, uint8_t note, Velocity16 velocity, uint8_t attribute_type,
               uint16_t attribute_data, uint32_t source_track_id,
               const ExcitationAxes& velocity_excitation = {},
               uint32_t velocity_excitation_mask = kAxisNone) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  /// Records a note-on velocity's engine axes on @p voice as their last writer.
  void record_velocity_axes(NativeSynthVoice& voice, const ExcitationAxes& velocity,
                            uint32_t mask) noexcept;
  /// The patch tail at the parts' current GS EG time edits plus the fixed bus
  /// tails, without allocation.
  int64_t recompute_tail() const noexcept;
  /// Raises the published tail to recompute_tail() and never lowers it: a voice
  /// struck under an edit since withdrawn may still be ringing.
  void raise_tail() noexcept;
  /// GM mode and kit presets: adds a note-on's resolved patch (with its drum @p kit, or
  /// kNoTailKit) to the set bounding the tail, and republishes it; the first note lowers the
  /// every-program bound.
  void observe_tail_patch(const NativeSynthPatch& patch, uint8_t kit, uint8_t note) noexcept;
  /// The slowest GS EG time multipliers any part holds; 1 where none lengthens.
  EnvelopeTimeScales gs_tail_scales() const noexcept;
  /// GS part SysEx (40 1x xx) and the system resets, on the channel state.
  void apply_part_sysex(const uint8_t* data, size_t size) noexcept;
  /// Whether the controller profile routes CC @p controller to an axis.
  bool profile_binds_cc(uint8_t controller) const noexcept;
  void process_impl(float* const* channels, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int num_channels, int num_samples) noexcept;
  /// PartFxHost: where an EFX CONTROL source sits on @p part.
  float part_controller_position(int part, uint8_t source) const noexcept override;
  /// PartFxHost: the part pan a mono rig prefix is restored to.
  float part_pan_units(int part) const noexcept override {
    return channel_mods_[static_cast<size_t>(part & 0x0F)].pan_units;
  }
  /// Publishes the bank rig @p channel's program binds; offline marks the stage
  /// for a rebuild at the next block, live leaves it to the control thread.
  void refresh_part_rig(uint8_t channel) noexcept;
  /// Offline render thread: the EFX content of a GS SysEx, into the mirror.
  void apply_efx_sysex(const uint8_t* data, size_t size) noexcept;
  /// Offline render thread: rebuild and publish the stage. Allocates.
  void realize_part_fx();
  /// Offline render thread: hand the recorder the path this block realises.
  /// Allocates; reached only with a recorder attached.
  void record_render_path();
  void control_change(uint8_t channel, uint8_t controller, Control32 control) noexcept;
  void channel_pressure(uint8_t channel, Control32 pressure) noexcept;
  void poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept;
  /// Damper pedal as the zone model scopes it: a manager's pedal reaches its whole zone.
  void sustain_cc(uint8_t channel, Control32 value) noexcept;
  /// The damper on exactly one channel.
  void sustain_channel(uint8_t channel, Control32 value) noexcept;
  /// RPN 0/0's Data Entry MSB (whole semitones) and LSB (cents), which a MIDI 2.0 Registered
  /// Controller 0/0 delivers together.
  void bend_range_msb(uint8_t channel, uint8_t semitones) noexcept;
  void bend_range_lsb(uint8_t channel, uint8_t cents) noexcept;
  /// A MIDI 2.0 Registered / Assignable Controller: 0/0 is read straight from the message, every
  /// other one takes the path its four MIDI 1.0 messages do.
  void registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept;
  /// A MIDI 2.0 Relative Registered / Assignable Controller on a parameter this synth holds (RC
  /// 0/0 and 0/7), added and saturated. False for any other, which the caller counts as skipped.
  bool relative_controller(const ChannelVoiceEvent& ev) noexcept;
  /// Binds @p state to (channel, note) at note-on with the Note On attribute, the engine starting
  /// on @p zone_key.
  void bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note, uint8_t attribute_type,
                     uint16_t attribute_data, int16_t zone_key) const noexcept;
  ComposedPitch compose_per_note(const Sf2PerNoteVoice& state) const noexcept;
  void refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept;
  /// Re-evaluates every sounding voice on (channel, note), or on the whole channel when
  /// @p all_notes is set (a sensitivity change reaches every key of it).
  void refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept;
  /// Moves a legato-carried voice's per-note binding to the key it now sounds.
  void carry_per_note(const NativeSynthVoice& voice, uint8_t from_note, uint8_t to_note,
                      uint8_t attribute_type, uint16_t attribute_data) noexcept;
  /// What the one legato rule decided.
  enum class LegatoOutcome : uint8_t {
    kNone,      ///< nothing held to continue from; the caller plays or releases normally
    kCarried,   ///< the sounding voice now sounds the target key
    kReplaced,  ///< the previous voice was stopped (note-on) or declined (release)
  };
  struct LegatoResult {
    LegatoOutcome outcome = LegatoOutcome::kNone;
    NativeSynthVoice* voice = nullptr;  ///< the carried voice, for kCarried
  };
  /// The single legato rule, for both directions. At note-on (@p release false) the target is
  /// the incoming key and the voice is the one sounding the channel's newest held key; at a
  /// release (@p release true, the released key already removed) the target is the newest key
  /// the same source still holds and the voice is the one the released key sounded. Reach is
  /// judged on the composed sounding keys, and the voice's owner follows the key it now sounds.
  LegatoResult legato_continue(uint8_t ch, bool release, uint8_t note, uint8_t attribute_type,
                               uint16_t attribute_data, uint32_t source_track_id,
                               const NativeSynthPatch* patch) noexcept;
  /// Sostenuto pedal with the same zone scope as sustain_cc().
  void sostenuto_pedal(uint8_t channel, bool down) noexcept;
  /// The sostenuto on exactly one channel.
  void sostenuto_channel(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  /// Recharges the channel's drawbar-organ percussion if no key is still held.
  void recharge_percussion(uint8_t channel) noexcept;
  /// The channel's expression axes as an engine axis set; @p present comes back
  /// naming the axes a controller has actually reached.
  static ExcitationAxes channel_excitation(const ControllerAxisState& axes,
                                           uint32_t& present) noexcept;
  /// Resolves @p ump through the controller profile and applies whatever axis
  /// values it produced. Runs before the protocol dispatch, so a note-on whose
  /// velocity is bound to an axis starts from its own value.
  ///
  /// Inside a zone this is also where the manager's fold happens, because the
  /// third dimension reaches an axis through the profile rather than through
  /// this class: a member's message is resolved carrying the combined value,
  /// and a manager's is resolved once per member of its zone as well as on the
  /// channel it arrived on.
  void apply_controller_input(const Ump& ump) noexcept;
  /// Resolves one message through the profile and applies its axis values.
  void apply_resolved_input(const Ump& ump) noexcept;
  /// Resolves note-on velocity, retaining engine-owned axes for the newly
  /// allocated voice while preserving channel-level velocity bindings.
  ExcitationAxes apply_note_on_controller_input(const Ump& ump, uint32_t* out_mask) noexcept;
  /// The per-note dimension @p ump carries, or false for a message that is not
  /// one of the three.
  static bool mpe_dimension_of(const Ump& ump, MpeDimension* out) noexcept;
  /// Fills mpe_notes_ with the channel's sounding notes, oldest first and one
  /// entry per note, and returns how many. Deduplicated because a layered patch
  /// is several voices of one key while the tracking rule names notes.
  size_t gather_mpe_notes(uint8_t channel) noexcept;
  /// The note a value addressed to the whole channel belongs to, or
  /// kControllerAnyNote when it belongs to every sounding note -- which is the
  /// answer outside a zone, on a manager channel whose values reach the whole
  /// zone, under kAllNotes, and whenever at most one note is sounding.
  uint8_t mpe_attributed_note(uint8_t channel, MpeDimension dimension) noexcept;
  /// Recomputes the per-voice mods of @p channel from its channel mod, so the
  /// bend and pressure a member channel carries reach the note they were
  /// attributed to and no other.
  void refresh_mpe_note_mods(uint8_t channel) noexcept;
  /// Captures the MPE dimensions at NoteOff while leaving ordinary channel
  /// controls live on a sustained/releasing voice.
  void freeze_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept;
  /// Profile axes @p dimension (bend, pressure or timbre) can write: the set
  /// a member voice freezes at its NoteOff.
  uint32_t mpe_profile_axis_mask(MpeDimension dimension) const noexcept;
  /// Rebuilds one frozen voice from current ordinary channel state and its
  /// captured MPE dimensions.
  void refresh_frozen_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept;
  /// Resolves @p channel's combined value through the profile, and every member
  /// of the zone as well when @p channel is the manager, whose value is a bias
  /// on each of them.
  void push_mpe_controller_axis(uint8_t channel, MpeDimension dimension) noexcept;
  /// Applies one combined MPE value to a channel's future-note template without
  /// pushing an attributed engine axis to currently sounding voices.
  void apply_mpe_channel_axes(uint8_t channel, MpeDimension dimension, Control32 combined) noexcept;
  /// Resolves one combined MPE value to one held voice. The member raw value is
  /// captured only for a member message; manager updates preserve the voice's
  /// captured member source and combine the current manager raw value with it.
  void apply_mpe_voice_axes(NativeSynthVoice& voice, uint8_t channel, MpeDimension dimension,
                            Control32 combined, bool update_member_raw, Control32 member_raw,
                            bool member_present) noexcept;
  /// Tracks the two combining dimensions ahead of the profile, so a value
  /// reaching an axis carries the manager's fold (2.2.7, 2.2.8).
  void track_mpe_input(const Ump& ump) noexcept;
  /// Pushes the channel's live excitation axes (and CC11 bow speed) to its
  /// sounding voices.
  void push_excitation_control(uint8_t channel, uint32_t changed_mask, bool mpe_dimension = false,
                               MpeDimension dimension = MpeDimension::kBend) noexcept;
  void restore_excitation_control(uint8_t channel, bool mpe_dimension = false) noexcept;
  void update_frozen_mpe_axis(uint8_t channel, ControllerAxis axis, float value) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  void refresh_channel_mod(uint8_t channel) noexcept;
  void refresh_all_channel_mods() noexcept;
  /// Refreshes every channel a zone-wide value arriving on @p channel reaches.
  void refresh_channel_mods_in_scope(uint8_t channel) noexcept;
  /// Accepts an MPE Configuration Message and does what accepting one obliges:
  /// every channel that entered or left the zone loses its sounding notes and
  /// its controllers, so a sender that re-zones mid-performance cannot leave a
  /// note hanging on a channel with no owner (M1-100-UM v1.1 section 2.2.3).
  void apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept;

  NativeSynthConfig config_{};
  ControllerProfile controller_profile_ = default_controller_profile();
  /// Empty until an MCM arrives, and while it is empty every channel reads as
  /// unassigned and nothing below behaves differently than it did before this
  /// member existed.
  MpeState mpe_{};
  /// Scratch for the note attribution and the allocation ages that order it,
  /// sized in prepare() to the polyphony, which bounds how many notes one
  /// channel can be sounding. Never resized on the audio thread.
  std::vector<MpeNote> mpe_notes_;
  std::vector<uint64_t> mpe_note_ages_;
  /// Counted on the audio thread, read by the host; atomic and held by pointer so the instrument
  /// stays movable.
  std::unique_ptr<std::atomic<uint64_t>> legato_fallbacks_ =
      std::make_unique<std::atomic<uint64_t>>(0);
  uint64_t skipped_events_ = 0;
  /// MIDI 2.0 per-note pitch: the rows per key (surviving note-off and Reset All Controllers),
  /// RC 0/7 per channel, and each pool voice's binding, indexed like the pool and sized in
  /// prepare().
  PerNotePitchTable per_note_pitch_{};
  std::array<Control32, 16> per_note_bend_sensitivity_{};
  std::vector<Sf2PerNoteVoice> per_note_;
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  /// Written on the audio thread by a live raise and read by the host, so
  /// atomic; held by pointer so the instrument stays movable.
  std::unique_ptr<std::atomic<int64_t>> tail_samples_ = std::make_unique<std::atomic<int64_t>>(0);
  /// GM mode: the patches note-ons resolved since prepare()/reset(), which bound the tail in
  /// place of every program's. Audio-thread state; a full set falls back to that bound.
  struct TailPatch {
    const NativeSynthPatch* patch;
    uint8_t kit;
    uint8_t note;
  };
  static constexpr uint8_t kNoTailKit = 0xFFu;
  static constexpr size_t kMaxTailPatches = 128;
  std::array<TailPatch, kMaxTailPatches> tail_patches_{};
  size_t tail_patch_count_ = 0;
  bool tail_patches_full_ = false;
  /// Ids written by apply_parameter whose voice state awaits refresh_live (bit =
  /// NativeSynthParamId). Audio-thread state under config_.patch's contract.
  uint64_t live_dirty_ = 0;
  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  /// Mix-bus polish state (per stereo leg): DC blocker + drive constants.
  /// Channels that have started a note since the shared piano / guitar / swell buses were last
  /// cleared; those buses carry what any of them excited, so a channel's All Sound Off clears
  /// them only when no other channel is in the set.
  uint16_t bus_fed_channels_ = 0;
  std::array<float, 2> dc_x1_{};
  std::array<float, 2> dc_y1_{};
  float dc_r_ = 0.999f;
  float bus_drive_gain_ = 0.0f;
  /// Shared-bus residual (mix minus dry), split per component so each lands on
  /// the sources that produced it. prepare() has no block-length argument, so
  /// each is staged per bus chunk and flushed at its end (process_impl).
  SourceResidualSplitter residual_splitter_;         // remainder: swell, bus drive, DC block
  SourceResidualSplitter piano_residual_splitter_;   // piano body + sympathetic
  SourceResidualSplitter guitar_residual_splitter_;  // guitar/KS sound halo
  std::array<float, kResidualChunk> residual_l_{};
  std::array<float, kResidualChunk> residual_r_{};
  std::array<float, kResidualChunk> piano_residual_l_{};
  std::array<float, kResidualChunk> piano_residual_r_{};
  std::array<float, kResidualChunk> guitar_residual_l_{};
  std::array<float, kResidualChunk> guitar_residual_r_{};
  /// A bussed part's chain output, and a unit's, follow only the voices that
  /// fed that bus.
  std::array<SourceResidualSplitter, 16> part_bus_splitters_;
  /// Per-part piano bodies used whenever a factory is present. Keeping one
  /// bank per part lets a bussed return enter its rig before it can reach any
  /// other part, and lets an assignment change move the existing tail without
  /// migrating or mixing state.
  std::array<SourceResidualSplitter, 16> part_piano_splitters_;
  /// Per-part organ swell residuals use the same source attribution boundary
  /// as their raw organ voices. A global splitter would let an unrelated part
  /// absorb a channel's CC11 change.
  std::array<SourceResidualSplitter, 16> part_organ_splitters_;
  /// The same source boundary for a factory-backed per-part guitar halo.
  std::array<SourceResidualSplitter, 16> part_guitar_splitters_;
  std::array<SourceResidualSplitter, kGsEfxUnitCount> unit_splitters_;
  static_assert(kResidualChunk == kPartFxChunkFrames,
                "the residual staging is flushed once per bus chunk");
  /// The voice sums of one chunk, taken before the bus outputs are folded in
  /// and the post stages run. Pre-gain, like the per-sample sums they hold.
  std::array<float, kPartFxChunkFrames> chunk_mix_l_{};
  std::array<float, kPartFxChunkFrames> chunk_mix_r_{};
  std::array<float, kPartFxChunkFrames> chunk_piano_l_{};
  std::array<float, kPartFxChunkFrames> chunk_piano_r_{};
  std::array<float, kPartFxChunkFrames> chunk_guitar_l_{};
  std::array<float, kPartFxChunkFrames> chunk_guitar_r_{};
  /// Raw piano legs for each part when the factory-backed per-part bodies are
  /// active. The entries are replaced with that part's body residual after it
  /// has been fed, so no second scratch slab is needed.
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_piano_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_piano_r_{};
  /// Raw organ legs for each part. They are replaced with the pre-rig swell
  /// residual after the part has been filtered.
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_organ_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_organ_r_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_guitar_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_guitar_r_{};
  /// Part buses, rig chains and GS insertion units.
  PartFxStage part_fx_;
  /// Attached for one offline render only.
  RenderPathRecorder* path_recorder_ = nullptr;
  /// Open-string halo per part, for a bussed part's sympathetic voices: its
  /// return rides the part's bus and therefore the part's rig, as Sf2Player's
  /// does. Armed at the first qualifying note-on on the part.
  std::array<PianoResonanceBank, 16> part_halo_;
  std::array<bool, 16> part_halo_armed_{};
  struct PianoPartState {
    bool prepared = false;
    float soundboard_mix = -1.0f;
  };
  /// Factory-backed piano bodies are per-part for the same reason as the GS
  /// fallback bodies: a return must follow the part's current EFX assignment.
  /// These banks are allocated in prepare() only when an insert factory is
  /// wired, keeping the common no-EFX NativeSynth object small on the stack.
  std::vector<PianoSoundboard> part_soundboards_;
  std::vector<PianoResonanceBank> part_resonance_;
  std::array<PianoPartState, 16> part_piano_state_{};
  VoicePool<NativeSynthVoice> pool_;
  /// Host sample bank for the kSample engine. The raw pointer is what the audio
  /// thread reads; the share below is held only when the caller handed one over.
  const SampleBank* sample_bank_ = nullptr;
  std::shared_ptr<const SampleBank> owned_sample_bank_;
  /// KS delay slab: one ks_slab_capacity() (three ks_buffer_capacity() spans —
  /// the primary string, the second-polarization line, and the octave-up 4'
  /// companion line) per voice slot, allocated in prepare() only when the patch
  /// is a Karplus-Strong instrument.
  std::vector<float> ks_buffers_;
  int ks_capacity_ = 0;
  /// Piano delay slab: kMaxPianoStrings string spans per voice slot,
  /// allocated in prepare() only when the patch is a piano.
  std::vector<float> piano_buffers_;
  int piano_string_capacity_ = 0;
  /// Shared sympathetic resonance bank, piano patches only: driven pedal-gated
  /// (the sustain-pedal sound halo), tuned in prepare() for a configured piano
  /// or lazily at the first GM piano note-on.
  PianoResonanceBank resonance_;
  /// Shared open-string sound halo for Karplus-Strong patches that opt in
  /// (patch.ks.sympathetic): a second bank rather than reusing the one above,
  /// since GM mode can voice a piano and a guitar together and each needs its
  /// own tuning and its own ring state. Armed in prepare() from this synth's
  /// own configured patch, or lazily at the first qualifying GM note-on
  /// (guitar_halo_active_); Sf2Player arms its own copy per part instead.
  PianoResonanceBank guitar_halo_;
  /// Shared modal soundboard body (piano patches only).
  PianoSoundboard soundboard_;
  /// The configured patch is a piano, so the bus body is tuned in prepare().
  bool piano_mode_ = false;
  /// A piano patch is currently voiced, so the render loop routes its voices
  /// through the bus body (direct-share attenuation, modal soundboard,
  /// pedal-gated sympathetic bank). Decided per note-on rather than from the
  /// construction-time patch, because GM mode resolves the engine per program:
  /// program 0 reaches the piano patch there too, and gating on the configured
  /// mode would render it as a bare string. Mirrors Sf2Player's per-part
  /// fallback_body_, bus-scoped because NativeSynth has one mix bus.
  bool piano_body_active_ = false;
  /// Soundboard mix the bus body is currently tuned for (-1 = never tuned), so
  /// a note-on re-prepares only when the resolved patch asks for a different
  /// board — the bank keeps its state across notes otherwise.
  float piano_body_soundboard_ = -1.0f;
  /// The halo bank above is armed and should be driven this render: true for
  /// a configured Karplus-Strong sympathetic patch from prepare(), or set
  /// lazily at the first qualifying GM note-on (mirrors piano_body_active_).
  /// False leaves every existing KS voicing on its original render path.
  bool guitar_halo_active_ = false;
  /// Pipe-organ delay slab: one kMaxPipeRanks-pipe slab per voice slot,
  /// allocated in prepare() only when the patch is a pipe organ.
  std::vector<float> pipe_organ_buffers_;
  int pipe_organ_capacity_ = 0;  // per-rank span
  bool pipe_organ_mode_ = false;
  /// Bowed-string delay slab: two delay-line spans (neck + bridge) per voice
  /// slot, allocated in prepare() only when the patch is a bowed string.
  std::vector<float> bowed_string_buffers_;
  int bowed_string_capacity_ = 0;  // per-line span
  bool bowed_string_mode_ = false;
  /// Vocal section history: one span per voice slot, allocated in prepare()
  /// only when the patch is a vocal.
  std::vector<float> vocal_buffers_;
  int vocal_capacity_ = 0;  // history span
  /// Reed-woodwind bore slab: one bore span per voice slot, allocated in
  /// prepare() only when the patch is a reed woodwind.
  std::vector<float> reed_buffers_;
  int reed_capacity_ = 0;  // bore span
  bool reed_mode_ = false;
  /// Brass bore slab: one bore span per voice slot, allocated in prepare() only
  /// when the patch is a brass / lip reed.
  std::vector<float> brass_buffers_;
  int brass_capacity_ = 0;  // bore span
  bool brass_mode_ = false;
  /// Flute delay slab: a bore span plus a jet span per voice slot, allocated in
  /// prepare() only when the patch is an air-jet flute.
  std::vector<float> flute_buffers_;
  int flute_capacity_ = 0;  // per-span (bore / jet) capacity
  bool flute_mode_ = false;
  /// Plucked-string delay slab: one string span per voice slot, allocated in
  /// prepare() only when the patch is a plucked string.
  std::vector<float> plucked_string_buffers_;
  int plucked_string_capacity_ = 0;  // per-span capacity
  bool plucked_string_mode_ = false;
  /// Harpsichord registration slab: the two 8' choirs, the 4' choir and the
  /// behind-the-bridge segment per voice slot, allocated in prepare() only when
  /// the patch is a harpsichord. The spans are not all the same length, so the
  /// per-voice stride is carried separately from the speaking-string span.
  std::vector<float> harpsichord_buffers_;
  int harpsichord_capacity_ = 0;  // speaking-string span
  int harpsichord_stride_ = 0;    // whole registration slab, per voice slot
  bool harpsichord_mode_ = false;
  /// Per-part organ wind supply parameters (tremulant / wind sag), armed at the
  /// note-on that resolves a pipe-organ patch on that part.
  struct OrganPartParams {
    float tremulant_rate_hz = -1.0f;
    float tremulant_depth = -1.0f;
    float wind_sag = -1.0f;
    bool wind_regulated = false;
    bool armed = false;
  };
  /// One wind chest and swell box per MIDI part. GM resolves the pipe patch at
  /// note-on, so construction-time patch mode cannot own these states.
  std::array<OrganWindSupply, 16> part_wind_;
  std::array<OrganPartParams, 16> organ_part_params_{};
  std::array<float, 16> part_swell_depth_{};
  std::array<float, 16> part_swell_coeff_{};
  std::array<float, 16> part_swell_lp_l_{};
  std::array<float, 16> part_swell_lp_r_{};
};

}  // namespace sonare::midi::synth
