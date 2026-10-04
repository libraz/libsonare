#pragma once

/// @file sf2_player.h
/// @brief Multitimbral (16-part) SoundFont 2 player implementing
///        MidiInstrument: (bank, program) -> preset resolution, layered
///        preset/instrument zone matching, the shared voice pool with
///        deterministic stealing, BuiltinSynth-compatible channel-mode CC
///        semantics (CC64 sustain, CC120/121/123), and the GS effect bus
///        (reverb / chorus / delay send-returns + per-part insert slot).
///
/// One Sf2Player instance receives all 16 MIDI channels (GS multitimbral
/// convention); channel 10 (index 9), and any GM2 channel selected with
/// CC0=120, resolve percussion via bank 128.
/// Programs no SoundFont preset covers — including the no-SoundFont case —
/// fall back to the NativeSynth GM bank (the data-free floor), so an
/// arrangement never bounces silent because of missing data.
/// Internally process() runs a 16-part bus graph in fixed-size chunks:
/// voices accumulate into their part bus (insert processing) and into the
/// system effect send buses (CC91/93/94 + zone send generators); the wet
/// returns are summed with the dry mix. The effect bodies reuse the existing
/// effects/ suite and only exist when the FX library is built
/// (SONARE_MIDI_WITH_FX); otherwise the player renders dry.
///
/// RT contract (MidiInstrument): set_soundfont() and prepare() run on the
/// CONTROL thread and are the only allocating calls; on_event()/process()
/// are allocation-free, lock-free and IO-free. The Sf2File is shared
/// read-only with the audio thread.
///
/// Determinism: no RNG, no wall clock; voice stealing, effects and rendering
/// are bit-identical for identical event streams within one build.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "midi/channel_voice_decode.h"
#include "midi/control_value.h"
#include "midi/instrument.h"
#include "midi/per_note_state.h"
#include "midi/source_residual.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/gs_efx_graph.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/gs_master_eq.h"
#include "midi/synth/gs_system_effects.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/part_fx_stage.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"
#include "rt/processor_base.h"
#include "rt/seqlock_cell.h"
#include "util/constants.h"
#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_effects.h"
#endif

namespace sonare::midi::synth {

/// GS-style preset lookup on a parsed SoundFont: exact (bank, program) first,
/// then the GS fallbacks (unknown variation bank -> capital tone bank 0; drum
/// bank 128 -> standard kit program 0). Returns the preset index or -1. This is
/// the resolution rule Sf2Player uses for note-on, exposed so hosts can report
/// which programs a SoundFont covers (the bounce manifest) without a player.
int resolve_gs_preset(const Sf2File& soundfont, uint16_t bank, uint8_t program) noexcept;

struct Sf2PlayerConfig {
  /// Master output gain applied to the summed voices (linear).
  float gain = 0.5f;
  /// Voice pool size. GS playback layers zones across 16 parts + drums, so the
  /// default is far above BuiltinSynth's 16 (clamped to [1, kMaxSynthVoices]).
  int polyphony = 48;
  /// Data-free floor: notes whose program no SoundFont preset covers (or with
  /// no SoundFont loaded at all) play through the NativeSynth GM fallback
  /// bank instead of dropping silent.
  bool synth_fallback = true;
  /// When true, melodic GM programs backed by a dedicated physical model use
  /// that model even if the loaded SoundFont contains a matching preset. The
  /// default keeps the established SoundFont-first behavior; drums always
  /// keep their SoundFont kit routing.
  bool prefer_model_for_modeled_families = false;
  /// Per-part rig entries, as set_part_rig() would install them. kBank, the
  /// default, installs no entry, so the part takes whatever ranks below.
  std::array<PartRig, 16> part_rigs{};
  /// Injected insert-factory: builds a streaming ProcessorBase from a name +
  /// JSON params, for rig chains and GS insertion units. Left null (the
  /// default) means the player has no part buses and every part renders dry —
  /// the SF2 player never depends on the mastering/effects factory itself; the
  /// host (which does) wires this, typically to mastering::api::make_insert.
  std::function<std::unique_ptr<rt::ProcessorBase>(std::string_view name,
                                                   std::string_view json_params)>
      insert_factory;
  /// Bind the bank's per-program default rig on a part that carries no insert of
  /// its own (docs/voicing.md). The voice is the instrument — an electric
  /// guitar's string and its pickup — and the amplifier it is never heard
  /// without is this stage, so a file that selects program 30 and sends no
  /// insertion effect still comes out amplified while the direct signal stays
  /// available. Bound only where the note plays the model floor: a SoundFont's
  /// electric guitar is a recording of an amplified one, and a second amplifier
  /// on top of it would be the bake this separation removes. Needs
  /// `insert_factory` — a host that wires none gets the instrument alone.
  bool bank_rig_binding = true;
  /// Offline / single-threaded hosts only: when set, process() realises any
  /// pending GS EFX change (a factory build, i.e. an allocation) inline at the
  /// top of the block, so an EFX SysEx that arrives mid-render takes effect
  /// without a separate control-thread pump. MUST stay false on the audio
  /// thread — realise_gs_efx() allocates. The live engine leaves it false and
  /// pumps EFX from the control thread instead.
  bool realize_efx_inline = false;
  /// Which layer realises the GS insertion units, for every unit at once. The
  /// modern chain is the default; classic runs each type's generated graph.
  GsEfxRealization gs_efx_realization = GsEfxRealization::kModern;
  /// ~8 Hz first-order DC blocker on the summed mix bus. What motivates it is
  /// the synth-fallback floor, which renders the same physical-model voices as
  /// the NativeSynth host: a sustained wind or reed part leaves a DC offset on
  /// the bus that eats headroom and skews the peak level a downstream mastering
  /// chain measures. It runs on the summed bus unconditionally, though, so a
  /// render of nothing but sampled presets passes through it too — the bus is
  /// the thing being kept DC-free, not one class of voice on it, which is the
  /// same scope NativeSynthConfig::dc_block has. Same default, same pole.
  bool dc_block = true;
#if defined(SONARE_MIDI_WITH_FX)
  /// System effect units (reverb / chorus / delay send-returns).
  GsEffectsConfig effects;
#endif
};

class Sf2Player final : public MidiInstrument, private PartFxHost {
 public:
  explicit Sf2Player(const Sf2PlayerConfig& config = {});
  ~Sf2Player() override;
  Sf2Player(Sf2Player&&) = default;
  Sf2Player& operator=(Sf2Player&&) = default;

  /// CONTROL thread: attach a parsed SoundFont. May be called before or after
  /// prepare(), but never concurrently with the audio thread.
  void set_soundfont(std::shared_ptr<const Sf2File> soundfont);

  const Sf2File* soundfont() const noexcept { return soundfont_.get(); }

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override { return true; }
  void reset() override;
  int tail_samples() const noexcept override {
    return static_cast<int>(tail_samples_) + part_fx_.tail_samples();
  }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;
  /// CONTROL thread: prepare an immutable SysEx operation for a scheduled
  /// event. The returned token carries only fixed plans; its DSP nodes stay
  /// owned by this player and are selected by the audio thread at the event's
  /// frame. Before prepare() it succeeds with a null token, which the event
  /// path treats as an unprepared SysEx; the engine re-prepares at prepare().
  bool prepare_sysex(const uint8_t* data, size_t size,
                     std::shared_ptr<const PreparedMidiSysEx>& out) override;

  /// Channel-voice messages received but not acted on: reserved statuses, per-note controllers
  /// other than pitch, and relative controllers on a parameter this player does not hold. Cleared
  /// by reset().
  uint64_t skipped_event_count() const noexcept { return skipped_events_; }

  /// Feeds a SysEx payload (with or without F0/F7 framing) to the GS layer:
  /// GM System On, GS Reset and "use for rhythm part" are recognised. Hosts
  /// that own the SysEx store call this when a SysEx event is due. Safe on
  /// the audio thread (allocation-free).
  ///
  /// Returns true when an apply layer took the write, which is narrower than
  /// "recognised": an address the table carries at ACCEPT or IGNORE is decoded
  /// and deliberately dropped, so it returns false without being unknown
  /// (docs/gs.md). Nothing in the tree branches on this — on_event discards it.
  bool handle_sysex(const uint8_t* data, size_t size) noexcept;

  /// GS Reset semantics (also reachable via handle_sysex): GS power-on
  /// defaults — programs/banks cleared, channel 10 drums, CC91 = 40,
  /// NRPN part edits and drum-note overrides cleared.
  void gs_reset() noexcept;
  /// GM System On semantics: the same power-on defaults as gs_reset(). GM
  /// Level 1 specifies no effect controls, but real GM-mode hardware keeps its
  /// power-on reverb level, so the sends land on the GS defaults rather than at
  /// zero and a plain GM file still renders in the default room.
  ///
  /// @p level is which System On arrived, and it changes the receive switches
  /// the reset leaves behind rather than anything else (docs/gs.md).
  void gm_reset(GmLevel level) noexcept;

  /// Currently sounding voices, SF2 + synth fallback (test/diagnostic).
  int active_voice_count() const noexcept {
    return pool_.active_count() + fallback_pool_.active_count();
  }

  /// RPN 00 01 Master Fine Tuning for @p channel as its raw 14-bit value
  /// (8192 = centre, full scale +-100 cents); the same storage the GS SysEx
  /// PITCH FINE TUNE parameter writes (test/diagnostic).
  uint16_t pitch_fine_tune(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].pitch_fine_tune;
  }
  /// RPN 00 02 Master Coarse Tuning for @p channel in semitones, 0 = centre
  /// (test/diagnostic).
  int pitch_coarse_tune(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].pitch_coarse_tune;
  }
  /// The combined master tuning offset in cents the render applies to
  /// @p channel (test/diagnostic).
  float master_tune_cents(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].tune_cents();
  }
  /// GS 40 1x 16 PITCH KEY SHIFT for @p channel as its raw byte, 40 = centre
  /// (test/diagnostic).
  uint8_t pitch_key_shift(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].pitch_key_shift;
  }
  /// GS 40 1x 17-18 PITCH OFFSET FINE for @p channel as the byte its two
  /// nibbles make, 80 = centre (test/diagnostic).
  uint8_t pitch_offset_fine(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].pitch_offset_fine;
  }
  /// GS 40 00 05 MASTER KEY-SHIFT as its raw byte, 40 = centre
  /// (test/diagnostic).
  uint8_t master_key_shift() const noexcept { return master_.key_shift; }
  /// GS 40 1x 14 ASSIGN MODE for @p channel (test/diagnostic).
  uint8_t assign_mode(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].assign_mode;
  }
  /// GS 40 1x 13 MONO/POLY MODE for @p channel, 0 = Mono (test/diagnostic).
  uint8_t mono_poly(uint8_t channel) const noexcept { return channels_[channel & 0x0Fu].mono_poly; }
  /// GS receive switches for @p channel, one bit per GsRxSwitch.
  /// Exposed because a switch over a message class libsonare does not receive is
  /// held rather than discarded, and a held byte owes a way to read it back.
  uint32_t rx_switches(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].rx_switches;
  }

  /// Captured GS insertion-effect (EFX) unit state (the raw block wire). The
  /// default is the spec unit at 40 03 xx; 1-15 are the extension's at 40 3u xx.
  /// Exposed for the adapter layer that realises it and for diagnostics.
  const GsEfx& gs_efx(size_t unit = 0) const noexcept {
    return part_fx_.efx()[std::min(unit, kGsEfxUnitCount - 1)];
  }
  /// GS 40 4x 22 PART EFX ASSIGN for @p channel, as the raw byte (test/diagnostic).
  uint8_t gs_efx_assign(uint8_t channel) const noexcept {
    return part_fx_.part_assign()[channel & 0x0Fu];
  }

  /// Captured GS system-effect block (the raw 40 01 30-5A wire) and master EQ
  /// block (40 02 00-03), at the GS power-on defaults until a file writes them.
  /// This is the realise mirror, so it reads back what arrived rather than what
  /// the units are currently running (test/diagnostic).
  const GsSystemEffects& gs_system_effects() const noexcept { return sys_fx_; }
  const GsMasterEq& gs_master_eq() const noexcept { return master_eq_; }

  /// Whether @p channel is routed through the master EQ (GS 40 4x 20). Powers
  /// on ON for every part (test/diagnostic).
  bool gs_part_eq_enabled(uint8_t channel) const noexcept {
    return !eq_part_bypassed_[channel & 0x0Fu];
  }

  /// True when the captured EFX unit or a part's EFX on/off switch changed
  /// since the last realise_gs_efx(). handle_sysex() (audio-thread safe) only
  /// stores the wire and raises this flag; the host polls it on the CONTROL
  /// thread and calls realise_gs_efx() to (re)build the inserts.
  bool gs_efx_dirty() const noexcept { return part_fx_.dirty(); }

  /// CONTROL thread: (re)build the per-part inserts for the parts whose EFX
  /// switch is on, from the captured EFX type/params via the injected factory
  /// (an unmapped type or absent factory leaves the part dry). Allocates; never
  /// call from the audio thread. Clears gs_efx_dirty(). Builds a fresh
  /// PartFxSnapshot and publishes it; the audio thread swaps it in wait-free at
  /// the next block.
  void realize_gs_efx();

  /// CONTROL thread: switch every insertion unit to @p realization. A change
  /// rebuilds all units, so their tails are cut. Allocates. On a running
  /// player the audio thread drops its prepared nodes at its next boundary
  /// through the direct queue; scheduled GS state already applied survives,
  /// and tokens prepared before the switch are dropped.
  void set_gs_efx_realization(GsEfxRealization realization);
  GsEfxRealization gs_efx_realization() const noexcept { return part_fx_.realization(); }

  /// CONTROL thread: realise and edit the units over @p rows instead of the
  /// generated binding tables, as gs_efx_insert_chain's own overload does;
  /// nullptr returns to the generated tables. @p rows must outlive the player.
  /// Rebuilds the published units when prepared and restarts the prepared
  /// runtime under the thread contract of set_gs_efx_realization (test seam).
  void set_gs_efx_rows(const GsEfxRowView* rows);

  /// How many times the insertion units have been built and published; an edit
  /// applied in place leaves it where it was (test/diagnostic).
  uint32_t gs_efx_generation() const noexcept { return part_fx_.generation(); }

  /// The byte EFX CONTROL @p control (0 = CONTROL 1) last wrote into its slot
  /// in the adopted snapshot, or -1 where that control drives nothing. Written
  /// by the audio thread, so read it between process() calls (test/diagnostic).
  int gs_efx_control_byte(size_t control) const noexcept;

  /// CONTROL thread: parse a GS SysEx for its insertion-effect content, update
  /// the control-owned EFX mirror, and republish the realised inserts so a live
  /// engine can hear a GS EFX change without stopping. Only EFX-affecting
  /// messages (40 03 xx / part switch / GS-GM reset) do anything here; the
  /// audio-visible channel/EFX state is still delivered separately via
  /// on_event(). No-op until prepared. Overrides MidiInstrument::on_control_sysex.
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override;
  /// CONTROL thread: a prepared live SysEx has already been made immutable and
  /// accepted by the engine. Its audio event owns the only application path;
  /// deliberately do not apply it to the control mirror here.
  void on_prepared_sysex_accepted(const uint8_t* data, size_t size,
                                  const PreparedMidiSysEx* prepared) noexcept override;

  /// CONTROL thread: the live controller snapshot the render path consumes, per
  /// channel. Breath (CC2) and expression (CC11) as normalized [0,1] floats.
  float channel_breath01(uint8_t channel) const noexcept {
    return channel_mods_[channel & 0x0Fu].breath01;
  }
  float channel_expression01(uint8_t channel) const noexcept {
    return channel_mods_[channel & 0x0Fu].expression01;
  }
  /// Where controller @p controller presently sits on @p channel, as a MIDI 1.0
  /// 7-bit value, whatever that controller means on this part.
  uint8_t controller_position(uint8_t channel, uint8_t controller) const noexcept {
    return channels_[channel & 0x0Fu].cc_position[controller & 0x7Fu].u7();
  }

  /// CONTROL thread: install @p rig for part slot @p part, or for every part no
  /// entry names when @p part is kPartRigAllParts, and rebuild the chains when
  /// prepared. A part keeps its rig whatever channel RX CHANNEL points it at.
  bool set_part_rig(uint8_t part, const PartRig& rig) noexcept override;
  /// CONTROL thread: the processor names of the chain last published for
  /// @p part, in signal order (test/diagnostic).
  std::vector<std::string> part_rig_stage_names(uint8_t part) const;

 private:
  /// gs_default_cc_positions() widened to the controller record's width.
  static std::array<Control32, 128> default_cc_positions() noexcept {
    std::array<Control32, 128> out{};
    const std::array<uint8_t, 128> seven = gs_default_cc_positions();
    for (size_t i = 0; i < out.size(); ++i) out[i] = Control32::from7(seven[i]);
    return out;
  }

  struct ChannelState {
    uint8_t program = 0;
    uint8_t bank_msb = 0;  // CC0; GS variation bank select
    uint8_t bank_lsb = 0;  // CC32
    bool sustain = false;
    /// Raw CC64 value for half-pedal: a partially raised damper (1..126) rests
    /// on the strings of the piano fallback voices instead of freeing them.
    uint8_t sustain_level = 0;
    bool sostenuto_down = false;  // CC66 pedal state (edge-triggered capture)
    bool una_corda = false;       // CC67; softens piano fallback voices at start
    /// Drawbar-organ percussion: charged, and spent by the next note-on that
    /// takes it. Recharges only when the channel has no key held, which is what
    /// makes percussion sound on the first note of a phrase and not on the ones
    /// played under it. The GM fallback bank reaches its organs through here,
    /// so the bit has to exist on this host as well as on NativeSynth's.
    bool percussion_armed = true;
    /// Where every controller presently sits, by number. Separate from the
    /// fields below because those are what a controller MEANS and this is only
    /// where it is: an assignable source names a controller by number and has
    /// to read its position whatever else that number does, including a number
    /// nothing else on this part interprets. Incoming messages are recorded at
    /// the top of control_change, and reset_controllers mirrors resettable
    /// performance/source defaults here before refreshing the live snapshot.
    /// The controllers that power on off zero are seeded from
    /// gs_default_cc_positions(), and the fields below take their defaults from
    /// it rather than restating them.
    std::array<Control32, 128> cc_position = default_cc_positions();
    // Default-modulator controller state.
    // Held at MIDI 2.0 width; a MIDI 1.0 value widens exactly and reads back as float(v).
    Control32 volume = Control32::from7(gs_default_cc_positions()[7]);       // CC7
    Control32 expression = Control32::from7(gs_default_cc_positions()[11]);  // CC11
    Control32 pan = Control32::from7(gs_default_cc_positions()[10]);         // CC10
    Control32 mod_wheel = Control32::from7(0);                               // CC1
    uint8_t reverb_send = 0;  // CC91 (the GS layer's GS reset sets the GS power-on 40)
    uint8_t chorus_send = 0;  // CC93
    uint8_t delay_send = 0;   // CC94 (GS delay send; no SF2 generator)
    Bend32 pitch_bend = Bend32::center();
    // RPN/NRPN state: CC101/100 select an RPN, CC99/98 select a GS NRPN; the
    // data entry CCs (6/38) route to whichever was selected last.
    ChannelParamState params;
    float bend_range_cents = 200.0f;
    /// The controller-destination block (40 2x xx), one entry per source. What
    /// each source is worth is its own value here scaled by where the source
    /// presently sits; refresh_channel_mod sums them into the voice snapshot.
    std::array<GsDestinationSet, kGsCtrlSourceCount> ctrl_dest = gs_default_destinations();
    /// Channel aftertouch, the second source the block's positions come from.
    /// The modulation wheel's is mod_wheel above; the other four sources have
    /// no controller yet and stay at rest.
    Control32 channel_pressure = Control32::from_raw(0);
    /// CC1 / CC2 CONTROLLER NUMBER (40 1x 1F/20): which MIDI controller drives
    /// each assignable source of the controller-destination block.
    std::array<uint8_t, 2> assignable_cc{{0x10, 0x11}};
    // --- portamento (CC5 time / CC65 switch / CC84 control) ---
    /// CC5, mapped to a glide time by portamento_time_ms(). GS power-on is 0,
    /// which is no glide however the note-on was armed.
    uint8_t portamento_time = 0;
    bool portamento = false;  // CC65 >= 64; GS power-on is off
    /// CC84 source note plus its one-shot arming. The manual defines Portamento
    /// Control as gliding the NEXT note-on from the source note it carries, so
    /// it fires once and does so with CC65 off as well as on.
    uint8_t portamento_source = 0;
    bool portamento_armed = false;
    /// Last key started on this part (the CC65 glide source); 128 = none yet.
    uint8_t last_note = 128;
    // --- master tuning ---
    /// RPN 00 01 Master Fine Tuning as its raw 14-bit value, 8192 = centre,
    /// full scale = +-100 cents. This is the SAME parameter as GS SysEx
    /// 40 1x 2A-2B PITCH FINE TUNE (one storage location per gs.md), so the
    /// SysEx phase writes this field rather than adding a second copy.
    uint16_t pitch_fine_tune = 8192;
    /// RPN 00 02 Master Coarse Tuning in semitones, clamped to +-24. Its value
    /// range coincides with GS SysEx 40 1x 16 PITCH KEY SHIFT, but the manual
    /// does not state they are one parameter, so this field is its own.
    int8_t pitch_coarse_tune = 0;
    /// GS SysEx 40 1x 16 PITCH KEY SHIFT as its raw 28-58 byte. It is not the
    /// same parameter as the coarse tuning above however exactly their ranges
    /// coincide, so it adds rather than overwriting; and unlike every other
    /// tuning field it does not reach a rhythm part (docs/gs.md).
    uint8_t pitch_key_shift = 0x40;
    /// GS SysEx 40 1x 17-18 PITCH OFFSET FINE as the one byte its two nibbles
    /// make, 80 = centre. Unlike every other tuning field it is a frequency
    /// rather than an interval, so it is converted per note rather than folded
    /// into the part's constant offset (docs/gs.md).
    uint8_t pitch_offset_fine = 0x80;
    /// GS SysEx 40 1x 14 ASSIGN MODE. Only 0 (SINGLE) branches: 1 and 2 differ
    /// on the hardware in how many stale duplicates of a note it keeps before
    /// stealing, which is a voice budget rather than a behaviour (docs/gs.md).
    uint8_t assign_mode = 1;
    /// GS SysEx 40 1x 13 MONO/POLY MODE, 0 = Mono. The same storage location as
    /// CC126 / CC127 (docs/gs.md), and like key shift it does not reach a
    /// rhythm part.
    uint8_t mono_poly = 1;
    /// GS 40 1x 15 USE FOR RHYTHM PART: 0 melodic, 1/2 drum map 1/2. A rhythm
    /// part resolves bank 128, and the map is what its per-note drum edits are
    /// keyed by, so two parts on one map share them (docs/gs.md). This carries
    /// the VALUE's numbering; the m nibble of the 41 mn rr drum setup address
    /// is zero-based (0 = MAP1), so it is this field minus one.
    uint8_t drum_map = kGsDrumMapNone;
    /// GS 40 1x 1D/1E KEY RANGE: the lowest and highest key the part receives.
    /// Held as the written bytes and tested at the note-on, where a key outside
    /// them is refused before anything else the note-on would do.
    uint8_t key_range_low = 0x00;
    uint8_t key_range_high = 0x7F;
    /// GS 40 1x 1A/1B VELOCITY SENSE DEPTH and OFFSET, both centred on 40. Held
    /// as the written bytes and applied to the struck velocity at the note-on,
    /// ahead of both voice banks and of the zone velocity ranges, because the
    /// part reshapes the velocity rather than what any one voice does with it.
    uint8_t velocity_sense_depth = 0x40;
    uint8_t velocity_sense_offset = 0x40;
    /// GS 40 1x 40-4B SCALE TUNING, one byte per pitch class from C.
    GsScaleTuning scale_tuning = kGsScaleTuningEqual;
    /// GS 40 1x 02 RX CHANNEL: the MIDI channel this part listens to, or 16 for
    /// a part that listens to none. Each part powers on to its own channel, so
    /// the reset writes it rather than a member default.
    uint8_t rx_channel = 0;
    /// GS 40 1x 03-12 and 23-24: one bit per GsRxSwitch, set when the part
    /// receives that class of message. A cleared bit drops the message on
    /// arrival, so the value it would have written stays as the last received
    /// one left it.
    uint32_t rx_switches = kGsRxAllOn;
    /// GS layer: the part's NRPN / TONE MODIFY edits.
    GsPartParams gs;

    bool is_drum() const noexcept { return drum_map != kGsDrumMapNone; }
    /// Whether the part receives @p which at all.
    bool receives(GsRxSwitch which) const noexcept {
      return (rx_switches & gs_rx_switch_bit(which)) != 0;
    }
    /// Whether the part receives @p note at all (40 1x 1D/1E). A range whose low
    /// is above its high receives nothing, which is what the two bytes say.
    bool receives_key(uint8_t note) const noexcept {
      const uint8_t key = note & 0x7Fu;
      return key >= key_range_low && key <= key_range_high;
    }
    /// Index into the per-map drum-edit slabs. A part that reached the drum
    /// bank without a GS map (GM2 CC0=120) reads map 1's.
    size_t drum_map_slot() const noexcept { return drum_map > kGsDrumMap1 ? 1u : 0u; }
    /// The user drum set this part's program selects, or -1. Derived rather than
    /// stored, so a program change carries it with no second write to keep in
    /// step. The caller gates it on the note's own rhythm test: whether a part
    /// reached the drum bank is note_on's question, not the GS map field's.
    int user_drum_set() const noexcept { return gs_user_drum_set(program); }

    /// Combined master tuning as a pitch offset in cents (0 at the defaults).
    /// PITCH KEY SHIFT is deliberately not here: it is the one pitch offset a
    /// rhythm part does not take, and this is read unconditionally.
    float tune_cents() const noexcept {
      return (static_cast<float>(pitch_fine_tune) - 8192.0f) * (100.0f / 8192.0f) +
             static_cast<float>(pitch_coarse_tune) * ::sonare::constants::kCentsPerSemitone;
    }
  };

  /// The pitch glide a note-on inherits from its part's portamento state.
  struct Portamento {
    float cents = 0.0f;  ///< Start offset from the glide source (0 = no glide).
    float coeff = 0.0f;  ///< Per-sample one-pole decay (0 = no glide).
  };

  void note_on(uint8_t channel, uint8_t note, Velocity16 velocity, uint8_t attribute_type,
               uint16_t attribute_data, uint32_t source_track_id) noexcept;
  /// Data-free floor: plays the note through the GM fallback synth bank.
  void fallback_note_on(uint8_t channel, uint8_t note, Velocity16 velocity,
                        uint32_t source_track_id, Portamento porta, uint8_t attribute_type,
                        uint16_t attribute_data) noexcept;
  /// Resolves the glide a note-on on @p note inherits, consuming the channel's
  /// CC84 arming and recording @p note as the next glide source. Called exactly
  /// once per note-on, before the SoundFont / fallback split.
  Portamento take_portamento(uint8_t channel, uint8_t note) noexcept;
  /// Stops what @p channel is already sounding, in both pools: only voices on
  /// @p note, or every one of them when @p note is negative. Called once per
  /// note-on before the SoundFont / fallback split, which is what lets it skip
  /// an age gate — nothing this note-on allocates exists yet.
  void choke_part(uint8_t channel, int note) noexcept;
  /// Chokes every voice already sounding @p group on @p part, in BOTH pools.
  /// @details The engine a voice sounds through is not part of the comparison:
  ///   a sampled hi-hat belongs to the same exclusive/assign group as a modelled
  ///   one and has to choke it, which is what a per-pool loop could not do.
  ///   @p sf2_age_gate excludes voices this same note-on already allocated —
  ///   the SoundFont path allocates one per matching zone, so a later zone's
  ///   choke must not kill an earlier zone's voice. A caller that has allocated
  ///   nothing yet passes the pool's current next_age(), which excludes nothing.
  ///   The two pools keep the group in differently named fields
  ///   (Sf2Voice::params.exclusive_class, NativeSynthVoice::exclusive_class),
  ///   so the comparison is written twice rather than through one accessor.
  void choke_exclusive_group(uint8_t part, uint8_t group, uint64_t sf2_age_gate) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  void control_change(uint8_t channel, uint8_t controller, Control32 value) noexcept;
  /// MIDI 2.0 Registered / Assignable Controllers, absolute and relative, on part @p ch. RC 0/0
  /// and 0/7 are read from the message; every other one takes the MIDI 1.0 parameter-number path.
  void registered_controller(uint8_t ch, const Ump& ump, const ChannelVoiceEvent& ev) noexcept;
  /// Returns false when the message names a parameter this player does not hold.
  bool relative_controller(uint8_t ch, const ChannelVoiceEvent& ev) noexcept;
  /// Attaches @p state to the row of (@p channel, @p note) for a voice about to start, so values
  /// already on the key apply, and captures the Note On attribute.
  void bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note, uint8_t attribute_type,
                     uint16_t attribute_data) const noexcept;
  /// The per-note share of @p state's pitch (§7.4.15).
  ComposedPitch compose_per_note(const Sf2PerNoteVoice& state) const noexcept;
  /// Recomputes the pitch offset @p state carries from its binding and attribute.
  void refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept;
  /// Re-evaluates every sounding voice on (channel, note), or on the whole channel when
  /// @p all_notes is set (a sensitivity change reaches every key of it).
  void refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept;
  /// Per-Note Management for one key across both voice pools.
  void manage_per_note(uint8_t channel, uint8_t note, bool detach, bool reset) noexcept;
  /// CC64 with half-pedal semantics: 0 releases held notes, 127 holds them
  /// freely, 1..126 rests the partially raised damper on ringing piano
  /// fallback voices (piano.damp).
  void sustain_cc(uint8_t channel, uint8_t value) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  /// CC66: captures the keys held at the down edge; they ring past note-off
  /// until the pedal lifts.
  void sostenuto_pedal(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  /// Recharges the channel's drawbar-organ percussion if no key is still held.
  void recharge_percussion(uint8_t channel) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  /// Routes a data-entry value (CC6 MSB) to the active GS NRPN.
  void apply_nrpn(uint8_t channel, uint8_t value) noexcept;
  /// Shared GM/GS power-on state (programs, drums on 10, edits cleared).
  void reset_all_state(uint8_t reverb_send_default, uint8_t chorus_send_default) noexcept;
  /// Clear control-owned GS mirrors and pending publications at a lifecycle
  /// boundary. The caller must have quiesced the audio thread.
  void clear_control_owned_gs_state();
  /// Drop the audio-side prepared-event selection at a lifecycle boundary.
  /// Public reset keeps the preparation domain so already compiled clips stay
  /// valid; prepare/reconfiguration changes the domain separately.
  void clear_prepared_audio_state() noexcept;
  /// Seed the prepared overlay from the quiescent CONTROL mirrors at a
  /// preparation/configuration boundary. Once delivery starts, the overlay is
  /// changed only by relative audio-owned SysEx deltas.
  void seed_prepared_from_control_state() noexcept;
  /// Copy the published host/default routing into the prepared runtime after
  /// the first prepared event. This is a fixed-size audio-thread operation.
  void sync_prepared_base() noexcept;
  /// AUDIO thread: derive the effective routing from the host projection and
  /// the GS overlay. A part routed into a unit loses the bank's default rig,
  /// as the published snapshot does, so its DI reaches the unit.
  void rebuild_prepared_routing() noexcept;
  /// AUDIO thread: apply the two GS EFX CONTROL fan-outs to the selected
  /// prepared unit after its raw state has been adopted.
  void apply_prepared_efx_controls() noexcept;
  /// AUDIO thread: the units the overlay runs in place of the snapshot's.
  PartFxUnitOverrides prepared_unit_overrides() const noexcept;
  /// The lowest part the overlay routes into @p unit, or -1.
  int prepared_control_part(size_t unit) const noexcept;
  /// PartFxHost: where an EFX CONTROL source sits on @p part.
  float part_controller_position(int part, uint8_t source) const noexcept override;
  /// PartFxHost: the part pan a mono rig prefix is restored to.
  float part_pan_units(int part) const noexcept override {
    return channel_mods_[static_cast<size_t>(part & 0x0F)].pan_units;
  }
  /// Recompute the cached Sf2ChannelMod for @p channel after a CC/bend change.
  void refresh_channel_mod(uint8_t channel) noexcept;
  /// Rebuilds rx_parts_ from the parts' rx_channel. Called after any write to
  /// one, so the dispatch table cannot fall out of step with the parts.
  void refresh_rx_channels() noexcept;
  /// Effective SF2 bank for a channel (GS rhythm parts and GM2 CC0=120 -> 128).
  uint16_t effective_bank(uint8_t channel) const noexcept;
  /// The user drum set entry a strike on @p note reads, or nullptr when the part
  /// plays a preset kit. @p is_drum is the caller's own rhythm test — whether
  /// the part resolved the drum bank, which a GM2 CC0=120 part does without a GS
  /// map field.
  const GsUserDrumSource* user_drum_source(const ChannelState& ch, bool is_drum,
                                           uint8_t note) const noexcept;
  /// The per-note drum edits a strike on @p note reads: the user drum set's
  /// stored ones under the map's live ones. The one place either bank asks, so a
  /// note cannot be edited differently depending on which answered it.
  GsDrumNoteParams drum_note_params(const ChannelState& ch, bool is_drum,
                                    uint8_t note) const noexcept;
  /// Preset index for (bank, program) with GS-style fallbacks, or -1.
  int resolve_preset(uint16_t bank, uint8_t program) const noexcept;
  /// AUDIO thread: recompute which default rig @p channel's current program binds
  /// and publish it for the builder. Called from the program / bank / rhythm-part
  /// writes only — never per note and never per controller, since it resolves a
  /// preset. Also called for every part from the control thread at prepare(),
  /// set_soundfont() and reset, where the audio thread is quiescent.
  void refresh_part_rig(uint8_t channel) noexcept;
  /// Recompute tail_samples_ from the SoundFont release scan, the synth
  /// fallback bank and the effect units (requires prepared_).
  void recompute_tail() noexcept;

  Sf2PlayerConfig config_{};
  std::shared_ptr<const Sf2File> soundfont_;
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  int64_t tail_samples_ = 0;
  /// Longest release timecents found in the soundfont (set_soundfont scan).
  int32_t max_release_timecents_ = -12000;
  /// Mix-bus DC blocker state (config_.dc_block): pole and per-leg histories.
  float dc_r_ = 0.0f;
  std::array<float, 2> dc_x1_{};
  std::array<float, 2> dc_y1_{};
  /// Kinds of fallback body resonator a part can hold, one of each (see
  /// fallback_body_ below).
  enum class FallbackBodyKind : uint8_t { kPiano, kGuitarHalo };
  static constexpr size_t kFallbackBodyKinds = 2;

  /// Shared-bus residual, split per component so each lands on the sources
  /// that produced it, one chunk (kChunkFrames) at a time:
  ///  - part_bus_splitters_[part]: a bussed part's whole post-insert bus
  ///    output (when it feeds no unit), weighted by that part's voices.
  ///  - unit_splitters_[unit]: an insertion unit's post-chain output, weighted
  ///    by every voice routed into it.
  ///  - send_residual_splitter_: the reverb/chorus/delay return, weighted by
  ///    each voice's send energy (a routed voice's through its unit's send).
  ///  - body_residual_splitters_[part * kFallbackBodyKinds + kind]: a
  ///    non-bussed part's board or halo return, weighted by the voices feeding
  ///    that body.
  ///  - residual_splitter_: the remainder (master EQ, DC block), weighted by
  ///    every voice's dry energy.
  SourceResidualSplitter residual_splitter_;
  std::array<SourceResidualSplitter, 16> part_bus_splitters_;
  std::array<SourceResidualSplitter, kGsEfxUnitCount> unit_splitters_;
  SourceResidualSplitter send_residual_splitter_;
  std::array<SourceResidualSplitter, 16 * kFallbackBodyKinds> body_residual_splitters_;
  /// Per-body staging for body_residual_splitters_, 16 x kFallbackBodyKinds x
  /// (L, R) x kChunkFrames.
  std::vector<float> body_residual_;

  /// Renders one chunk (n <= kChunkFrames) of the 16-part bus graph into the
  /// internal mix scratch. In source-track mode, attributable dry voice audio
  /// is added directly to its target while destination-scoped part/effect
  /// residuals are added to target zero.
  /// @return Whether any voice discarded a non-finite recursive-state sample
  ///         this chunk. process_impl() accumulates this across every chunk of
  ///         one call rather than bumping here, since one call renders several.
  bool render_chunk(int n, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int output_offset, int num_channels) noexcept;
  void process_impl(float* const* channels, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int num_channels, int num_samples) noexcept;
  /// @brief Every owned processor's discard count added together -- the master
  ///        EQ and the realised EFX chains (per-part inserts and per-unit
  ///        chains) -- for the process_impl() call delta. RT-safe: relaxed
  ///        atomic loads only.
  /// @details Reachable from outside only as a count, so a discard inside one
  ///   is observable nowhere unless the player records it. The sum answers "did
  ///   any of them move", which is the question process_impl()'s own per-call
  ///   count asks; it is never published as a count of its own, and summing is
  ///   safe only because of that -- a chain may run once per render_chunk() and
  ///   one process_impl() call spans several. Mirrors ChannelStrip / BusProcessor.
  uint64_t member_discard_sum() const noexcept;

  /// Internal bus-graph chunk size (matches the effect bus block).
  static constexpr int kChunkFrames = kPartFxChunkFrames;

  std::array<ChannelState, 16> channels_{};
  /// Which parts each incoming MIDI channel reaches, one bit per part
  /// (GS RX CHANNEL, 40 1x 02). Derived from the parts' own rx_channel by
  /// refresh_rx_channels() so the dispatch reads one word instead of walking
  /// sixteen parts per event; at the power-on map every word has exactly one
  /// bit and it is the channel's own part.
  std::array<uint16_t, 16> rx_parts_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  /// GS master tuning, volume and pan (40 00 00-06). Unlike the effect blocks
  /// these are scalars the render loop reads directly, so they stay on the
  /// render thread in both modes, alongside the part parameters they resemble.
  GsMasterParams master_{};
  /// GS system-effect block (40 01 30-5A), master EQ (40 02 00-03) and per-part
  /// EQ switch (40 4x 20) as they arrived on the wire. Same thread ownership as
  /// the EFX mirror above: the render thread offline, the control thread live.
  /// Every member default-constructs to its GS power-on value, so a reset is a
  /// default-construct — which is why the switch is stored bypassed-side-up.
  GsSystemEffects sys_fx_{};
  GsMasterEq master_eq_{};
  std::array<bool, 16> eq_part_bypassed_{};
  /// Raised when the mirror above changes on the offline path; process() applies
  /// it to the units at the top of the next block.
  bool gs_system_dirty_ = false;
  /// AUDIO thread: the master EQ stage and the parts switched out of it.
  GsMasterEqFilter eq_;
  std::array<bool, 16> eq_bypassed_{};
  /// GS drum-kit per-note overrides (NRPN 18/1A/1C/1D/1E), per drum map.
  std::array<std::array<GsDrumNoteParams, 128>, kGsDrumMapCount> drum_params_{};
  /// Where each note of each user drum set takes its sound from (21 dA/dB/dC
  /// rr). Per SET, not per part or per map: a set is a stored kit, and every
  /// rhythm part that selects it with program 64/65 plays the same one.
  std::array<std::array<GsUserDrumSource, 128>, kGsUserDrumSetCount> user_drum_sources_{};
  /// The per-note edits stored IN each user drum set (21 d1-d9 rr), which the
  /// map's own edits at 41 mn rr are layered over.
  std::array<std::array<GsDrumNoteParams, 128>, kGsUserDrumSetCount> user_drum_params_{};
  VoicePool<Sf2Voice> pool_;
  /// Synth-fallback voices (programs no SoundFont preset covers).
  VoicePool<NativeSynthVoice> fallback_pool_;
  /// Per-note pitch state of each fallback voice, by slot (the voice type is shared with
  /// NativeSynth and carries none); sized in prepare().
  std::vector<Sf2PerNoteVoice> fallback_per_note_;
  /// Per-note pitch rows (per key, surviving note-off) and RC 0/7 per part.
  PerNotePitchTable per_note_pitch_{};
  std::array<Control32, 16> per_note_bend_sensitivity_{};
  uint64_t skipped_events_ = 0;
  /// KS delay slab for the fallback voices (plucked GM programs), one
  /// ks_slab_capacity() (three ks_buffer_capacity() spans — the primary string,
  /// the second-polarization line, and the octave-up 4' companion line) per
  /// slot; allocated in prepare() when the synth fallback is enabled.
  std::vector<float> fallback_ks_buffers_;
  int fallback_ks_capacity_ = 0;
  /// Piano delay slab for the fallback voices (the acoustic-piano GM
  /// programs), kMaxPianoStrings spans per slot; allocated in prepare()
  /// when the synth fallback is enabled.
  std::vector<float> fallback_piano_buffers_;
  int fallback_piano_string_capacity_ = 0;
  /// Pipe-organ delay slab for the fallback voices (the Church Organ GM
  /// program), one pipe_organ_buffer_capacity() span per slot; allocated in
  /// prepare() when the synth fallback is enabled.
  std::vector<float> fallback_pipe_organ_buffers_;
  int fallback_pipe_organ_capacity_ = 0;
  /// Physical waveguide delay slabs for the fallback voices — the bowed-string
  /// (GM 40-43), reed (GM 64-71), brass (GM 56-60) and air-jet flute (GM 72-79)
  /// families. Each slot gets its engine's *_slab_capacity() span (bowed = 3
  /// lines, flute = 2, reed/brass = 1); allocated in prepare() when the synth
  /// fallback is enabled, attached at note-on like the other waveguide voices.
  std::vector<float> fallback_bowed_buffers_;
  int fallback_bowed_capacity_ = 0;
  std::vector<float> fallback_reed_buffers_;
  int fallback_reed_capacity_ = 0;
  std::vector<float> fallback_brass_buffers_;
  int fallback_brass_capacity_ = 0;
  std::vector<float> fallback_flute_buffers_;
  int fallback_flute_capacity_ = 0;
  std::vector<float> fallback_plucked_string_buffers_;
  std::vector<float> fallback_harpsichord_buffers_;
  int fallback_plucked_string_capacity_ = 0;
  int fallback_harpsichord_capacity_ = 0;  // speaking-string span
  int fallback_harpsichord_stride_ = 0;    // whole registration slab, per voice slot
  /// Shared organ wind (tremulant / wind sag) for the fallback voices, one
  /// chest per part: the NativeSynth host feeds its voices from a wind supply,
  /// and the fallback path must do the same or the pipe-organ patches' trem /
  /// sag parameters are silently ignored. Prepared lazily at the first organ
  /// note-on of a part (re-prepared only when the patch parameters change, so
  /// the LFO phase stays continuous across notes).
  struct FallbackWindParams {
    float rate = -1.0f;
    float depth = -1.0f;
    float sag = -1.0f;
  };
  std::array<OrganWindSupply, 16> fallback_wind_;
  std::array<FallbackWindParams, 16> fallback_wind_params_{};
  /// Shared body resonators for the fallback voices, one of each kind per part
  /// — the same bus-level components the NativeSynth host folds in: the piano's
  /// modal soundboard + pedal-gated sympathetic string bank, and the
  /// plucked-string open-string halo (ks.sympathetic patches). Each voice feeds
  /// the body its own patch was struck with, so a program change on the part
  /// never re-prepares or silences a body earlier notes still ring through.
  /// A body is prepared at the first note-on of its kind and kept processing
  /// for a ring-out window after its last voice dies.
  /// The body @p patch strikes; false when it has none.
  static bool fallback_body_kind(const NativeSynthPatch& patch, FallbackBodyKind* kind) noexcept;
  struct FallbackBody {
    bool prepared = false;
    int64_t ringout = 0;
  };
  struct FallbackBodyState {
    std::array<FallbackBody, kFallbackBodyKinds> bodies{};
    float soundboard_mix = -1.0f;
  };
  std::array<PianoSoundboard, 16> fallback_board_;
  /// Piano sympathetic string bank, behind the board.
  std::array<PianoResonanceBank, 16> fallback_reso_;
  /// Plucked-string open-string halo.
  std::array<PianoResonanceBank, 16> fallback_halo_;
  std::array<FallbackBodyState, 16> fallback_body_{};

  // Chunk scratch (prepared on the control thread).
  std::vector<float> mix_l_;
  std::vector<float> mix_r_;
  /// Master-EQ bypass bus, stereo x kChunkFrames. A part switched out of the EQ
  /// (GS 40 4x 20) accumulates here as well as into the mix, so the EQ runs on
  /// the difference and that part's audio passes through untouched. Only used
  /// when the EQ is off flat and some part is switched out.
  std::vector<float> eq_bypass_bus_;
  /// Part buses, rig chains and GS insertion units. Built on the control thread
  /// and published to the audio thread; parts it does not buss add straight to
  /// the dry mix, so injecting a factory does not perturb an otherwise dry bounce.
  PartFxStage part_fx_;

  /// Direct live GS/EFX writes are relative operations. CONTROL owns the
  /// accepted payload and its prepared lease; AUDIO drains nodes in publication
  /// order and mutates one audio-owned prepared overlay.
  struct PreparedSysEx;
  static constexpr size_t kDirectGsMaxBytes = 512;
  struct DirectGsNode {
    std::atomic<DirectGsNode*> next{nullptr};
    uint64_t seq = 0;
    uint16_t size = 0;
    std::array<uint8_t, kDirectGsMaxBytes> bytes{};
    const PreparedSysEx* prepared_raw = nullptr;
    std::shared_ptr<const PreparedMidiSysEx> prepared_owner;
    bool legacy_full_snapshot = false;
    /// Non-zero: a prepared-runtime restart into this preparation domain.
    uint64_t restart_domain = 0;
  };
  struct DirectGsQueue {
    DirectGsNode stub{};
    DirectGsNode* control_tail = &stub;
    DirectGsNode* audio_head = &stub;
    std::atomic<DirectGsNode*> published_tail{&stub};
    std::atomic<uint64_t> consumed_seq{0};
    std::atomic<uint64_t> retired_seq{0};
    uint64_t next_seq = 1;
    std::deque<std::unique_ptr<DirectGsNode>> owned;
  };
  std::unique_ptr<DirectGsQueue> direct_queue_ = std::make_unique<DirectGsQueue>();

  /// CONTROL-owned compatibility state for an unsupported custom EFX graph.
  /// The normal prepared API remains structural and history-independent; this
  /// marker is consulted only by on_control_sysex before publishing a direct
  /// parameter delta, so that a legacy full-snapshot graph is never replaced by
  /// a null prepared node.
  std::array<bool, kGsEfxUnitCount> direct_legacy_efx_fallback_{};

  bool append_direct_gs_node(const uint8_t* data, size_t size,
                             std::shared_ptr<const PreparedMidiSysEx> prepared,
                             bool legacy_full_snapshot) noexcept;
  void sweep_direct_gs_nodes() noexcept;
  void clear_direct_gs_queue() noexcept;
  void drain_direct_gs_nodes() noexcept;
  void adopt_legacy_direct_snapshot() noexcept;
  /// CONTROL thread: own an unpublished restart node before a rebuild, so the
  /// commit below cannot fail; nullptr while unprepared. May throw.
  DirectGsNode* reserve_restart_node();
  /// CONTROL thread: invalidate prepared tokens after the published units were
  /// rebuilt under a new realisation or row set, and publish @p reserved behind
  /// every direct write already queued.
  void restart_prepared_runtime(DirectGsNode* reserved) noexcept;
  /// AUDIO thread: drop every prepared node and adopt @p domain. The scheduled
  /// raw GS state is kept; a unit whose rebuilt snapshot has the same type
  /// takes it, any other renders empty.
  void restart_prepared_audio_runtime(uint64_t domain) noexcept;

  struct DirectSystemField {
    uint64_t seq = 0;
    uint8_t value = 0;
  };
  static constexpr size_t kDirectSystemFieldCount = kGsSystemEffectFieldCount + 4 + 16;
  struct DirectSystemPatch {
    uint64_t publish_seq = 0;
    uint64_t last_reset_seq = 0;
    std::array<DirectSystemField, kDirectSystemFieldCount> fields{};
  };
  using DirectSystemPatchCell = rt::SeqlockCell<DirectSystemPatch>;
  std::unique_ptr<DirectSystemPatchCell> direct_system_patch_ =
      std::make_unique<DirectSystemPatchCell>();
  std::unique_ptr<DirectSystemPatchCell::Reader> direct_system_patch_reader_;
  DirectSystemPatch direct_system_patch_control_{};
  GsSystemEffects direct_system_audio_fx_{};
  GsMasterEq direct_system_audio_eq_{};
  std::array<bool, 16> direct_system_audio_bypassed_{};
  uint64_t last_system_direct_seq_ = 0;

  /// Apply the GS system-effect / master-EQ block writes @p data carries to the
  /// realise mirror (sys_fx_ / master_eq_ / eq_part_bypassed_). Returns true when
  /// the message wrote at least one of them. Touches no unit and no audio state.
  bool apply_gs_system_sysex(const uint8_t* data, size_t size) noexcept;
  bool apply_gs_system_sysex_to(GsSystemEffects& fx, GsMasterEq& eq, std::array<bool, 16>& eq_part,
                                const uint8_t* data, size_t size) noexcept;
  /// Apply the GS part-parameter block writes (40 1x xx) @p data carries onto
  /// channel state. Every address here is a second name for a controller that
  /// already arrives on the render thread, so this writes the controller's own
  /// storage rather than a parallel copy (docs/gs.md). Returns true when the
  /// message wrote at least one part parameter.
  bool apply_gs_part_sysex(const uint8_t* data, size_t size) noexcept;
  /// Apply the GS master writes (40 00 00-06) @p data carries. Returns true when
  /// the message wrote at least one of them.
  bool apply_gs_master_sysex(const uint8_t* data, size_t size) noexcept;
  /// Apply the GS drum setup writes (41 mn rr) @p data carries onto the per-map
  /// drum-note slabs. Each address is a second name for a drum NRPN the render
  /// thread already writes, so this shares that storage (docs/gs.md). Returns
  /// true when the message wrote at least one drum note.
  bool apply_gs_drum_sysex(const uint8_t* data, size_t size) noexcept;
  bool apply_gs_user_drum_sysex(const uint8_t* data, size_t size) noexcept;
  /// The two output-leg gains: the host's own gain times GS MASTER VOLUME and
  /// MASTER PAN. Every path that leaves the player passes through these, and
  /// both are exactly config_.gain at the GS power-on values.
  void output_gains(float* left, float* right) const noexcept {
    float pan_l = 1.0f;
    float pan_r = 1.0f;
    gs_master_pan_gains(master_.pan, &pan_l, &pan_r);
    const float gain = config_.gain * gs_master_volume_gain(master_.volume);
    *left = gain * pan_l;
    *right = gain * pan_r;
  }
  /// Re-aim the effect bus and the master EQ at @p fx / @p eq / @p eq_part.
  /// Coefficients only: allocation-free, and the effect tails survive.
  void apply_gs_system_state(const GsSystemEffects& fx, const GsMasterEq& eq,
                             const std::array<bool, 16>& eq_part) noexcept;
  /// CONTROL thread: publish fields written by one direct message. A reset
  /// stamps only its sequence, leaving older field writes behind it.
  void publish_direct_system_patch(bool reset, const uint8_t* data = nullptr,
                                   size_t size = 0) noexcept;
  /// AUDIO thread: adopt the latest fixed-field mailbox at a boundary.
  void drain_direct_system_patch() noexcept;

  static constexpr size_t kMaxPreparedDestinations = 64;
  static constexpr size_t kMaxPreparedEnables = 32;
  static constexpr size_t kMaxPreparedCandidates = 7;

  /// One modern (row, stage, realtime parameter) destination frozen in a
  /// prepared node. The row is copied because custom row views are a control
  /// seam; its scalar data is stable for the node's preparation domain.
  struct PreparedEfxParamDest {
    GsEfxBindingRow row{};
    uint8_t stage_index = 0xFF;
    uint32_t param_id = 0;
  };

  /// One enable term and the stage indices it names. At audio time the raw
  /// 20-byte state is read and every term is ANDed in the original table order.
  struct PreparedEfxEnablePlan {
    GsEfxEnable rule{};
    std::array<uint8_t, 4> stage_indices{};
  };

  /// A processor graph prepared once on the control thread. The audio pin
  /// keeps a selected node alive while a scheduled event can still point at
  /// it; control-side collection only removes unpinned entries.
  struct PreparedEfxNode {
    uint64_t domain = 0;
    uint8_t unit = 0;
    uint16_t type = 0;
    Sf2EfxUnitRt unit_rt{};
    std::atomic<uint32_t> audio_pins{0};
    std::array<PreparedEfxParamDest, kMaxPreparedDestinations> param_dests{};
    uint8_t param_dest_count = 0;
    std::array<PreparedEfxEnablePlan, kMaxPreparedEnables> enable_plans{};
    uint8_t enable_plan_count = 0;
    std::array<Sf2EfxControlRt, 2> controls{};
    std::array<bool, 32> default_enabled{};
    uint8_t stage_count = 0;
  };

  struct PreparedEfxCandidate {
    uint8_t unit = 0;
    uint16_t type = 0;
    PreparedEfxNode* node = nullptr;
    /// CONTROL-owned lease. The audio thread uses only `node`; no shared
    /// pointer operation is performed while dispatching the event.
    std::shared_ptr<PreparedEfxNode> lease;
    GsEfx target{};
  };

  /// Heap-stable owner identity for prepared tokens. Sf2Player is movable, so
  /// token ownership cannot be tied to the object's address.
  struct PreparedOwnerIdentity final {};

  /// Opaque token carried by MidiEvent. It contains no mutable audio state.
  struct PreparedSysEx final : PreparedMidiSysEx {
    std::shared_ptr<const PreparedOwnerIdentity> owner_identity;
    uint64_t domain = 0;
    bool efx_block = false;
    bool full_reapply = false;
    uint8_t unit = 0;
    std::array<PreparedEfxCandidate, kMaxPreparedCandidates> candidates{};
    uint8_t candidate_count = 0;
  };

  std::shared_ptr<PreparedEfxNode> find_or_build_prepared_node(size_t unit, uint16_t type);
  void activate_prepared_node(size_t unit, PreparedEfxNode* node) noexcept;
  void apply_prepared_gs_delta(const PreparedSysEx& token, const uint8_t* data, size_t size,
                               bool apply_performance) noexcept;
  void apply_prepared_candidate(const PreparedSysEx& token,
                                const PreparedEfxCandidate& candidate) noexcept;
  void apply_prepared_node_plan(PreparedEfxNode& node, const GsEfx& target,
                                bool preserve_enable_fade) noexcept;
  void release_prepared_nodes() noexcept;

  /// CONTROL-owned cache of (unit, type, preparation-domain) nodes. Entries
  /// are only created for candidates named by a prepared message, rather than
  /// for the entire 16x65 catalogue.
  std::vector<std::shared_ptr<PreparedEfxNode>> prepared_nodes_;
  uint64_t prepared_domain_ = 1;
  /// AUDIO-owned copy of prepared_domain_, moved only at a quiescent boundary
  /// or by a queued restart.
  uint64_t prepared_audio_domain_ = 1;
  std::shared_ptr<const PreparedOwnerIdentity> prepared_owner_identity_ =
      std::make_shared<PreparedOwnerIdentity>();

  /// AUDIO-owned prepared EFX runtime. Host/static insert chains remain in the
  /// published snapshot; these fields only overlay GS unit selection/routing.
  std::array<PreparedEfxNode*, kGsEfxUnitCount> prepared_active_nodes_{};
  std::array<bool, kGsEfxUnitCount> prepared_unit_overridden_{};
  std::array<GsEfx, kGsEfxUnitCount> prepared_efx_{};
  std::array<uint8_t, 16> prepared_assign_{};
  GsSystemEffects prepared_sys_fx_{};
  GsMasterEq prepared_master_eq_{};
  std::array<bool, 16> prepared_eq_part_bypassed_{};
  std::array<uint8_t, 16> prepared_part_unit_{};
  std::array<bool, 16> prepared_part_bussed_{};
  std::array<uint8_t, 16> prepared_mono_prefix_{};
  std::array<bool, kGsEfxUnitCount> prepared_unit_fed_{};
  std::array<bool, 16> prepared_host_part_bussed_{};
  std::array<bool, 16> prepared_host_default_bank_rig_{};
  std::array<uint8_t, 16> prepared_host_mono_prefix_{};
  bool prepared_host_any_bussed_ = false;
  bool prepared_any_unit_ = false;
  bool prepared_any_bussed_ = false;
  bool prepared_runtime_active_ = false;
  bool prepared_base_synced_ = false;
  Sf2EfxUnitRt prepared_empty_unit_{};

#if defined(SONARE_MIDI_WITH_FX)
  std::unique_ptr<GsEffectBus> effects_;
#endif
};

}  // namespace sonare::midi::synth
