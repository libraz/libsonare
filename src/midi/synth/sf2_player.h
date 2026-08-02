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
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"
#include "rt/processor_base.h"
#include "rt/rt_publisher.h"
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

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t source_track_id) noexcept;
  /// Data-free floor: plays the note through the GM fallback synth bank.
  void fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity,
                        uint32_t source_track_id) noexcept;
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
  int fallback_plucked_string_capacity_ = 0;
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
