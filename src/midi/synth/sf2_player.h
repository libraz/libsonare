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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "midi/instrument.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/gs_master_eq.h"
#include "midi/synth/gs_system_effects.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
#include "rt/spsc_queue.h"
#include "util/constants.h"
#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_effects.h"
#endif

namespace sonare::midi::synth {

/// A realised per-part insert routing, handed to the audio thread as one
/// immutable-lifetime snapshot. `chains[part]` is the series of inserts run on
/// that part's stereo bus (a config kProcessor slot, or a GS-EFX-installed
/// single/composite chain); `part_bussed[part]` selects whether the part sums
/// through its bus (and thus its chain) or straight to the dry mix; `any_bussed`
/// short-circuits the whole bus stage when no part is routed. Built entirely on
/// the CONTROL thread and published through rt::RtPublisher, so the audio thread
/// only reads it (running the processors — which is legal through the const
/// snapshot, since a `const unique_ptr` still exposes a non-const pointee) and
/// never allocates, frees, or races the builder.
struct Sf2RealizedEfx {
  std::array<std::vector<std::unique_ptr<rt::ProcessorBase>>, 16> chains{};
  std::array<bool, 16> part_bussed{};
  bool any_bussed = false;
};

/// Per-part insert slot (the GS insertion-effect realiser): either the built-in
/// gain-compensated drive, or an arbitrary streaming ProcessorBase built by an
/// injected factory (so the SF2 player realises any insert type without
/// depending on the mastering/effects factory itself — the host wires it).
enum class Sf2InsertType : int {
  kNone = 0,
  kDrive = 1,      ///< Built-in gain-compensated tanh drive (`amount`).
  kProcessor = 2,  ///< An injected ProcessorBase built from `insert_name`.
};

struct Sf2PartInsert {
  Sf2InsertType type = Sf2InsertType::kNone;
  /// kDrive: drive amount in [0, 1] (0 = clean, 1 = heavy saturation).
  float amount = 0.0f;
  /// kProcessor: insert-factory processor name (e.g. "saturation.ampSim").
  std::string insert_name;
  /// kProcessor: JSON param object for the processor ("" / "{}" = defaults).
  std::string insert_params_json;
};

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
  /// Per-part (MIDI channel) insert slot.
  std::array<Sf2PartInsert, 16> part_inserts{};
  /// Injected insert-factory: builds a streaming ProcessorBase from a name +
  /// JSON params, for kProcessor slots. Left null (the default) means the
  /// player has no factory, so kProcessor slots stay silent no-ops — the SF2
  /// player never depends on the mastering/effects factory itself; the host
  /// (which does) wires this, typically to mastering::api::make_insert.
  std::function<std::unique_ptr<rt::ProcessorBase>(std::string_view name,
                                                   std::string_view json_params)>
      insert_factory;
  /// Offline / single-threaded hosts only: when set, process() realises any
  /// pending GS EFX change (a factory build, i.e. an allocation) inline at the
  /// top of the block, so an EFX SysEx that arrives mid-render takes effect
  /// without a separate control-thread pump. MUST stay false on the audio
  /// thread — realise_gs_efx() allocates. The live engine leaves it false and
  /// pumps EFX from the control thread instead.
  bool realize_efx_inline = false;
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

class Sf2Player final : public MidiInstrument {
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
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

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
  void gm_reset() noexcept;

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
  /// GS 40 00 05 MASTER KEY-SHIFT as its raw byte, 40 = centre
  /// (test/diagnostic).
  uint8_t master_key_shift() const noexcept { return master_.key_shift; }
  /// GS 40 1x 14 ASSIGN MODE for @p channel (test/diagnostic).
  uint8_t assign_mode(uint8_t channel) const noexcept {
    return channels_[channel & 0x0Fu].assign_mode;
  }
  /// GS 40 1x 13 MONO/POLY MODE for @p channel, 0 = Mono (test/diagnostic).
  uint8_t mono_poly(uint8_t channel) const noexcept { return channels_[channel & 0x0Fu].mono_poly; }

  /// Captured GS insertion-effect (EFX) unit state (the raw 40 03 xx wire).
  /// Exposed for the adapter layer that realises it and for diagnostics.
  const GsEfx& gs_efx() const noexcept { return efx_; }

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
  bool gs_efx_dirty() const noexcept { return gs_efx_dirty_; }

  /// CONTROL thread: (re)build the per-part inserts for the parts whose EFX
  /// switch is on, from the captured EFX type/params via the injected factory
  /// (an unmapped type or absent factory leaves the part dry). Allocates; never
  /// call from the audio thread. Clears gs_efx_dirty(). Builds a fresh
  /// Sf2RealizedEfx and publishes it; the audio thread swaps it in wait-free at
  /// the next block.
  void realize_gs_efx();

  /// CONTROL thread: parse a GS SysEx for its insertion-effect content, update
  /// the control-owned EFX mirror, and republish the realised inserts so a live
  /// engine can hear a GS EFX change without stopping. Only EFX-affecting
  /// messages (40 03 xx / part switch / GS-GM reset) do anything here; the
  /// audio-visible channel/EFX state is still delivered separately via
  /// on_event(). No-op until prepared. Overrides MidiInstrument::on_control_sysex.
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override;

 private:
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
    // Default-modulator controller state.
    uint8_t volume = 100;      // CC7
    uint8_t expression = 127;  // CC11
    uint8_t pan = 64;          // CC10
    uint8_t mod_wheel = 0;     // CC1
    uint8_t reverb_send = 0;   // CC91 (the GS layer's GS reset sets the GS power-on 40)
    uint8_t chorus_send = 0;   // CC93
    uint8_t delay_send = 0;    // CC94 (GS delay send; no SF2 generator)
    uint16_t pitch_bend = 8192;
    // RPN/NRPN state: CC101/100 select an RPN, CC99/98 select a GS NRPN; the
    // data entry CCs (6/38) route to whichever was selected last.
    ChannelParamState params;
    float bend_range_cents = 200.0f;
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
    /// GS layer: the part's NRPN / TONE MODIFY edits.
    GsPartParams gs;

    bool is_drum() const noexcept { return drum_map != kGsDrumMapNone; }
    /// Index into the per-map drum-edit slabs. A part that reached the drum
    /// bank without a GS map (GM2 CC0=120) reads map 1's.
    size_t drum_map_slot() const noexcept { return drum_map > kGsDrumMap1 ? 1u : 0u; }

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

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t source_track_id) noexcept;
  /// Data-free floor: plays the note through the GM fallback synth bank.
  void fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t source_track_id,
                        Portamento porta) noexcept;
  /// Resolves the glide a note-on on @p note inherits, consuming the channel's
  /// CC84 arming and recording @p note as the next glide source. Called exactly
  /// once per note-on, before the SoundFont / fallback split.
  Portamento take_portamento(uint8_t channel, uint8_t note) noexcept;
  /// Stops what @p channel is already sounding, in both pools: only voices on
  /// @p note, or every one of them when @p note is negative. Called once per
  /// note-on before the SoundFont / fallback split, which is what lets it skip
  /// an age gate — nothing this note-on allocates exists yet.
  void choke_part(uint8_t channel, int note) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
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
  /// Recompute the cached Sf2ChannelMod for @p channel after a CC/bend change.
  void refresh_channel_mod(uint8_t channel) noexcept;
  /// Effective SF2 bank for a channel (GS rhythm parts and GM2 CC0=120 -> 128).
  uint16_t effective_bank(uint8_t channel) const noexcept;
  /// Preset index for (bank, program) with GS-style fallbacks, or -1.
  int resolve_preset(uint16_t bank, uint8_t program) const noexcept;
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

  /// Renders one chunk (n <= kChunkFrames) of the 16-part bus graph into the
  /// internal mix scratch. In source-track mode, attributable dry voice audio
  /// is added directly to its target while destination-scoped part/effect
  /// residuals are added to target zero.
  void render_chunk(int n, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int output_offset, int num_channels) noexcept;
  void process_impl(float* const* channels, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int num_channels, int num_samples) noexcept;

  /// Internal bus-graph chunk size (matches the effect bus block).
  static constexpr int kChunkFrames = 256;

  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  /// GS master tuning, volume and pan (40 00 00-06). Unlike the effect blocks
  /// these are scalars the render loop reads directly, so they stay on the
  /// render thread in both modes, alongside the part parameters they resemble.
  GsMasterParams master_{};
  /// GS insertion-effect (EFX) unit state, captured from the 40 03 xx SysEx
  /// block. The single SC-55/88 EFX unit; parsed and stored raw here so an
  /// adapter layer can realise it. Cleared on GS/GM reset.
  GsEfx efx_{};
  /// Per-part EFX on/off switch (GS 40 4x 22): parts routed through the EFX.
  std::array<bool, 16> efx_part_enabled_{};
  /// Raised by handle_sysex() when efx_ / efx_part_enabled_ change; cleared by
  /// realise_gs_efx(). Lets the audio-safe SysEx path defer the allocating
  /// insert rebuild to the control thread.
  bool gs_efx_dirty_ = false;
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
  VoicePool<Sf2Voice> pool_;
  /// Synth-fallback voices (programs no SoundFont preset covers).
  VoicePool<NativeSynthVoice> fallback_pool_;
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
  /// Shared body resonators for the fallback voices, one per part — the same
  /// bus-level components the NativeSynth host folds in: the piano's modal
  /// soundboard + pedal-gated sympathetic string bank, and the plucked-string
  /// open-string halo (ks.sympathetic patches). Driven by the part's summed
  /// fallback dry signal; kept processing for a ring-out window after the last
  /// voice dies so the resonator tail is not truncated.
  enum class FallbackBodyKind : uint8_t { kNone, kPiano, kGuitarHalo };
  struct FallbackBodyState {
    FallbackBodyKind kind = FallbackBodyKind::kNone;
    float soundboard_mix = -1.0f;
    int64_t ringout = 0;
  };
  std::array<PianoSoundboard, 16> fallback_board_;
  std::array<PianoResonanceBank, 16> fallback_reso_;
  std::array<FallbackBodyState, 16> fallback_body_{};

  // Chunk scratch (prepared on the control thread).
  std::vector<float> mix_l_;
  std::vector<float> mix_r_;
  /// 16 parts x stereo x kChunkFrames; only used when a part insert is set.
  std::vector<float> part_bus_;
  /// Master-EQ bypass bus, stereo x kChunkFrames. A part switched out of the EQ
  /// (GS 40 4x 20) accumulates here as well as into the mix, so the EQ runs on
  /// the difference and that part's audio passes through untouched. Only used
  /// when the EQ is off flat and some part is switched out.
  std::vector<float> eq_bypass_bus_;
  /// True once the player is EFX-capable (a config insert exists or a factory is
  /// injected), so the per-part bus buffer is allocated. This only governs
  /// allocation — the actual per-part routing is decided by `part_bussed_`, so
  /// an EFX-capable player with no active insert still mixes bit-identically to
  /// a plain one (the summation order is unchanged until an insert is live).
  bool any_insert_ = false;
  /// Realised per-part routing (part_bussed / any_bussed / insert chains), built
  /// on the control thread and published to the audio thread through a wait-free
  /// RtPublisher. A part is bussed through its insert chain only when it carries
  /// a static config insert or a realised GS EFX chain; parts without one add
  /// straight to the dry mix, so injecting a factory (for run-time EFX) does not
  /// perturb an otherwise dry bounce. The audio thread acquire()s the newest
  /// snapshot at block start and reads it for the whole block; the control thread
  /// owns every snapshot's lifetime (build + free), so render allocates nothing.
  /// Held by unique_ptr because RtPublisher is non-movable (its rings pin their
  /// address) while Sf2Player stays movable — moving transfers the pointer, not
  /// the publisher, so the audio thread's held snapshot is never disturbed.
  std::unique_ptr<rt::RtPublisher<Sf2RealizedEfx>> efx_pub_ =
      std::make_unique<rt::RtPublisher<Sf2RealizedEfx>>();

  /// CONTROL thread: build a fresh realised-EFX snapshot from the current EFX
  /// mirror (efx_ / efx_part_enabled_) and the config static inserts, via the
  /// injected factory. Allocates.
  std::shared_ptr<Sf2RealizedEfx> build_realized_efx() const;
  /// CONTROL thread: apply the insertion-effect content of a GS SysEx to the EFX
  /// mirror (efx_ / efx_part_enabled_). Returns true when a full chain rebuild is
  /// required (type change, reset, part switch, or a parameter that cannot be
  /// applied in place), false when the message was handled without a rebuild
  /// (parameter-only edit resolved into the EFX parameter queue, or not an EFX
  /// message). Touches only the realise mirror and the parameter queue, never
  /// audio-side channel state.
  bool apply_efx_sysex(const uint8_t* data, size_t size) noexcept;

  /// A pending GS EFX parameter update handed from the control thread to the
  /// audio thread: apply set_parameter(@c param_id, @c value) to stage
  /// @c stage_index of part @c part's realised insert chain.
  struct EfxParamUpdate {
    uint8_t part = 0;
    uint8_t stage_index = 0;
    uint32_t param_id = 0;
    float value = 0.0f;
  };

  /// Wait-free single-producer (control thread) / single-consumer (audio thread)
  /// ring of pending EFX parameter updates. A parameter-only GS EFX edit is
  /// resolved to updates on the control thread and applied on the audio thread
  /// (serialized with process()), so a live insert processor is never mutated
  /// across threads and its DSP state (reverb/delay tails) is never rebuilt away.
  /// Held by unique_ptr so Sf2Player stays movable (std::atomic is not movable).
  class EfxParamQueue {
   public:
    /// Capacity (power of two). GS EFX parameter edits are sparse; when the ring
    /// is full a push is dropped, which is harmless because the next rebuild
    /// bakes the current parameter values in from the EFX mirror.
    static constexpr size_t kCapacity = 128;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

    /// CONTROL thread. Returns false when the ring is full (the update is
    /// dropped).
    bool push(const EfxParamUpdate& update) noexcept {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t tail = tail_.load(std::memory_order_acquire);
      if (head - tail >= kCapacity) return false;
      slots_[head & (kCapacity - 1)] = update;
      head_.store(head + 1, std::memory_order_release);
      return true;
    }

    /// AUDIO thread. Returns false when the ring is empty.
    bool pop(EfxParamUpdate& out) noexcept {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      const size_t head = head_.load(std::memory_order_acquire);
      if (head == tail) return false;
      out = slots_[tail & (kCapacity - 1)];
      tail_.store(tail + 1, std::memory_order_release);
      return true;
    }

   private:
    std::array<EfxParamUpdate, kCapacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};  // control thread (producer)
    alignas(64) std::atomic<size_t> tail_{0};  // audio thread (consumer)
  };
  std::unique_ptr<EfxParamQueue> efx_param_queue_ = std::make_unique<EfxParamQueue>();

  /// One GS system-effect / master-EQ state handed from the control thread to
  /// the audio thread. Both blocks are coefficient-only, so the audio thread
  /// applies the newest snapshot to the running units in place and the reverb
  /// and delay tails survive a live edit.
  struct GsSystemUpdate {
    GsSystemEffects fx;
    GsMasterEq eq;
    std::array<bool, 16> eq_part_bypassed;
  };
  /// Wait-free single-producer (control thread) / single-consumer (audio thread)
  /// ring of pending system states. The state is absolute rather than
  /// incremental, so a drop or an overtake costs nothing: the audio thread
  /// applies the newest entry and discards the rest. Held by unique_ptr because
  /// SpscQueue is non-movable while Sf2Player stays movable.
  std::unique_ptr<rt::SpscQueue<GsSystemUpdate>> sys_queue_ =
      std::make_unique<rt::SpscQueue<GsSystemUpdate>>();

  /// Apply the GS system-effect / master-EQ block writes @p data carries to the
  /// realise mirror (sys_fx_ / master_eq_ / eq_part_bypassed_). Returns true when
  /// the message wrote at least one of them. Touches no unit and no audio state.
  bool apply_gs_system_sysex(const uint8_t* data, size_t size) noexcept;
  /// Apply the GS part-parameter block writes (40 1x xx) @p data carries onto
  /// channel state. Every address here is a second name for a controller that
  /// already arrives on the render thread, so this writes the controller's own
  /// storage rather than a parallel copy (docs/gs.md). Returns true when the
  /// message wrote at least one part parameter.
  bool apply_gs_part_sysex(const uint8_t* data, size_t size) noexcept;
  /// Apply the GS master writes (40 00 00-06) @p data carries. Returns true when
  /// the message wrote at least one of them.
  bool apply_gs_master_sysex(const uint8_t* data, size_t size) noexcept;
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
  /// AUDIO thread: adopt the newest system state the control thread published.
  void drain_gs_system_updates() noexcept;

  /// CONTROL thread: resolve a parameter-only GS EFX edit against the live
  /// published chain (reading only the const parameter-descriptor bridge) and
  /// enqueue the resulting set_parameter tuples for the audio thread. Returns
  /// true when a full rebuild is required instead (no live chain, no automatable
  /// parameter matched, or a parameter that is not realtime-safe).
  bool enqueue_efx_param_updates();
  /// AUDIO thread: apply every pending EFX parameter update to the current
  /// published chain, serialized with process(). RT-safe (no alloc, no lock).
  void drain_efx_param_updates() noexcept;

#if defined(SONARE_MIDI_WITH_FX)
  std::unique_ptr<GsEffectBus> effects_;
#endif
};

}  // namespace sonare::midi::synth
