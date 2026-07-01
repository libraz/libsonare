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
/// convention); channel 10 (index 9) resolves percussion via bank 128.
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
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "midi/instrument.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"
#include "rt/processor_base.h"
#if defined(SONARE_MIDI_WITH_FX)
#include "midi/synth/gs_effects.h"
#endif

namespace sonare::midi::synth {

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
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

  /// Feeds a SysEx payload (with or without F0/F7 framing) to the GS layer:
  /// GM System On, GS Reset and "use for rhythm part" are recognised. Hosts
  /// that own the SysEx store call this when a SysEx event is due. Safe on
  /// the audio thread (allocation-free); returns true if recognised.
  bool handle_sysex(const uint8_t* data, size_t size) noexcept;

  /// GS Reset semantics (also reachable via handle_sysex): GS power-on
  /// defaults — programs/banks cleared, channel 10 drums, CC91 = 40,
  /// NRPN part edits and drum-note overrides cleared.
  void gs_reset() noexcept;
  /// GM System On semantics: like GS Reset but with effect sends at zero
  /// (GM Level 1 mandates no effects).
  void gm_reset() noexcept;

  /// Currently sounding voices, SF2 + synth fallback (test/diagnostic).
  int active_voice_count() const noexcept {
    return pool_.active_count() + fallback_pool_.active_count();
  }

  /// Captured GS insertion-effect (EFX) unit state (the raw 40 03 xx wire).
  /// Exposed for the adapter layer that realises it and for diagnostics.
  const GsEfx& gs_efx() const noexcept { return efx_; }

  /// True when the captured EFX unit or a part's EFX on/off switch changed
  /// since the last realise_gs_efx(). handle_sysex() (audio-thread safe) only
  /// stores the wire and raises this flag; the host polls it on the CONTROL
  /// thread and calls realise_gs_efx() to (re)build the inserts.
  bool gs_efx_dirty() const noexcept { return gs_efx_dirty_; }

  /// CONTROL thread: (re)build the per-part inserts for the parts whose EFX
  /// switch is on, from the captured EFX type/params via the injected factory
  /// (an unmapped type or absent factory leaves the part dry). Allocates; never
  /// call from the audio thread. Clears gs_efx_dirty().
  void realize_gs_efx();

 private:
  struct ChannelState {
    uint8_t program = 0;
    uint8_t bank_msb = 0;  // CC0; GS variation bank select
    uint8_t bank_lsb = 0;  // CC32
    bool sustain = false;
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
    /// GS layer: rhythm-part flag (drums resolve bank 128) and NRPN edits.
    bool drums = false;
    GsPartParams gs;
  };

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
  /// Data-free floor: plays the note through the GM fallback synth bank.
  void fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
  void note_off(uint8_t channel, uint8_t note) noexcept;
  void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
  void sustain_pedal(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  /// Routes a data-entry value (CC6 MSB) to the active GS NRPN.
  void apply_nrpn(uint8_t channel, uint8_t value) noexcept;
  /// Shared GM/GS power-on state (programs, drums on 10, edits cleared).
  void reset_all_state(uint8_t reverb_send_default) noexcept;
  /// Recompute the cached Sf2ChannelMod for @p channel after a CC/bend change.
  void refresh_channel_mod(uint8_t channel) noexcept;
  /// Effective SF2 bank for a channel (drum channel -> 128).
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

  /// Renders one chunk (n <= kChunkFrames) of the 16-part bus graph into the
  /// internal mix scratch.
  void render_chunk(int n) noexcept;

  /// Internal bus-graph chunk size (matches the effect bus block).
  static constexpr int kChunkFrames = 256;

  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
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
  /// GS drum-kit per-note overrides (NRPN 18/1A/1C/1D/1E), per channel.
  std::array<std::array<GsDrumNoteParams, 128>, 16> drum_params_{};
  VoicePool<Sf2Voice> pool_;
  /// Synth-fallback voices (programs no SoundFont preset covers).
  VoicePool<NativeSynthVoice> fallback_pool_;
  /// KS delay slab for the fallback voices (plucked GM programs), one
  /// ks_slab_capacity() (two ks_buffer_capacity() spans — the primary string
  /// plus the second-polarization line) per slot; allocated in prepare() when
  /// the synth fallback is enabled.
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

  // Chunk scratch (prepared on the control thread).
  std::vector<float> mix_l_;
  std::vector<float> mix_r_;
  /// 16 parts x stereo x kChunkFrames; only used when a part insert is set.
  std::vector<float> part_bus_;
  bool any_insert_ = false;
  /// Per-part insert chain, run in series on the part bus. A config kProcessor
  /// slot is a one-stage chain; a GS EFX unit realises a one-stage chain for a
  /// single effect or a multi-stage chain for a composite type (e.g. GTR Multi
  /// = Cmp-OD-EQ-CF). Built by the injected factory on the control thread
  /// (prepare / realise_gs_efx); an empty chain is inert. Iterating it in
  /// render is allocation-free.
  std::array<std::vector<std::unique_ptr<rt::ProcessorBase>>, 16> part_chains_{};

#if defined(SONARE_MIDI_WITH_FX)
  std::unique_ptr<GsEffectBus> effects_;
#endif
};

}  // namespace sonare::midi::synth
