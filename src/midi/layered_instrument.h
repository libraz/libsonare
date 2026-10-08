#pragma once

/// @file layered_instrument.h
/// @brief One MidiInstrument built from several, each covering a key/velocity
///        rectangle and carrying its own transpose, level and balance.
///
/// This is the multi-tone patch: a note reaches every layer whose rectangle
/// covers it, and the layers' outputs sum. The layers need not be the same kind
/// of instrument, so a sampled tone can sit under a modelled one.
///
/// A note-off carries a different velocity from the note-on that opened it, so
/// the layer set is recorded at note-on and replayed at note-off rather than
/// recomputed — recomputing strands a note in a velocity-split layer. The
/// bounded ledger also carries the destination, source track and wire group,
/// because a shared instrument can have the same source key open more than
/// once. Note-addressed controllers are routed to every layer whose source key
/// range contains the addressed key, then receive that layer's transpose.
///
/// Latency: children must agree to the Q8 fraction, and prepare() throws when they do not. Summing
/// outputs of unequal latency would smear the attack, and a layered patch has
/// no use for the delay network that would fix it.
///
/// RT contract: prepare() is the only allocation site (the per-child mix
/// scratch); on_event() and process() are allocation- and lock-free.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "midi/instrument.h"
#include "rt/overflow_counter.h"

namespace sonare::midi {

/// Placement of one layer inside a layered patch.
struct InstrumentLayerSpec {
  uint8_t key_lo = 0;
  uint8_t key_hi = 127;
  uint8_t vel_lo = 1;
  uint8_t vel_hi = 127;
  /// Semitones added to the played note before the layer sees it.
  int transpose = 0;
  /// Linear gain on this layer's output.
  float level = 1.0f;
  /// Stereo balance in [-1, 1]; unity at centre, so a layer's own image is
  /// left alone.
  float pan = 0.0f;
};

/// Maximum layers in one patch (each note-ledger entry holds a bit per layer).
inline constexpr size_t kMaxInstrumentLayers = 32;

/// Parameter-id stride per layer: a child's own id is added to
/// layer_index * this, so the ids stay stable and need no lookup table.
inline constexpr int kLayerParamStride = 1000;

/// A patch made of several instruments playing together.
class LayeredInstrument final : public MidiInstrument {
 public:
  /// CONTROL thread, before prepare(). Takes ownership. Returns false when the
  /// patch is full or @p instrument is null.
  bool add_layer(std::unique_ptr<MidiInstrument> instrument, const InstrumentLayerSpec& spec);

  size_t layer_count() const noexcept { return layers_.size(); }
  /// Borrowed; valid while this instrument is.
  MidiInstrument* layer_at(size_t index) noexcept;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override;
  void reset() override;
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;
  void set_transport(const transport::TransportState& state) noexcept override;
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override;
  bool prepare_sysex(const uint8_t* data, size_t size,
                     std::shared_ptr<const PreparedMidiSysEx>& out) override;
  void on_prepared_sysex_accepted(const uint8_t* data, size_t size,
                                  const PreparedMidiSysEx* prepared) noexcept override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  int tail_samples() const noexcept override;
  int parameter_id_for_key(const std::string& key) const noexcept override;
  bool apply_parameter(unsigned int param_id, float value) noexcept override;
  bool describe_parameter(unsigned int param_id,
                          automation::ParameterDescription* out) const override;

  /// ANY thread: note-ons dropped because the note ledger was full.
  uint32_t ledger_overflow_count() const noexcept { return ledger_overflow_count_.load(); }
  void reset_telemetry() noexcept { ledger_overflow_count_.reset(); }

 private:
  struct Layer {
    std::unique_ptr<MidiInstrument> instrument;
    InstrumentLayerSpec spec;
    float gain_left = 1.0f;
    float gain_right = 1.0f;
  };

  /// Stable identity shared by this instrument and every composite operation
  /// it prepares. Unlike `this`, it survives the implicit move of the owning
  /// LayeredInstrument while remaining cheap to inspect on the audio thread.
  struct PreparedLayeredSysExIdentity {};

  /// Composite operation for one SysEx event. The identity/domain pair
  /// prevents a token prepared for another layered patch (or an old layer
  /// layout) from crossing the audio/control boundary; every child lease stays
  /// alive until the composite operation is retired.
  struct PreparedLayeredSysEx final : PreparedMidiSysEx {
    std::shared_ptr<const PreparedLayeredSysExIdentity> identity;
    uint64_t domain = 0;
    std::vector<std::shared_ptr<const PreparedMidiSysEx>> children;
  };

  /// A bounded prepared-time ledger entry for one source key identity. A
  /// retrigger with the same identity merges its layer owners, so one exact
  /// note-off releases every layer opened by that key without broadcasting an
  /// unmatched note-off to arbitrary children.
  struct ActiveNote {
    bool active = false;
    uint8_t group = 0;
    uint8_t channel = 0;
    uint8_t source_note = 0;
    uint32_t source_track_id = 0;
    uint32_t destination_id = 0;
    uint32_t owners = 0;
  };

  /// Drop counter that stays with the instance that counted: a move starts the
  /// destination at zero, so the patch itself remains movable.
  struct LedgerOverflowCounter : rt::OverflowCounter {
    LedgerOverflowCounter() = default;
    LedgerOverflowCounter(LedgerOverflowCounter&&) noexcept {}
    LedgerOverflowCounter& operator=(LedgerOverflowCounter&&) noexcept { return *this; }
  };

  static constexpr size_t kNoteLedgerCapacity = 256u;
  static constexpr size_t kNoNoteLedgerEntry = static_cast<size_t>(-1);

  bool covers(const Layer& layer, uint8_t note, uint8_t velocity) const noexcept;
  /// Retunes the note field and any absolute per-note pitch carried by @p ump.
  /// Returns false when a layer's transpose would move an absolute pitch out
  /// of its MIDI range, in which case the layer must receive no event.
  static bool retune(const Ump& ump, uint8_t note, int transpose, Ump* out) noexcept;
  bool send_note(size_t layer_index, const MidiEvent& event, uint8_t note, int transpose,
                 uint32_t destination_id, const PreparedLayeredSysEx* prepared) noexcept;
  size_t find_note_ledger_entry(uint32_t destination_id, const MidiEvent& event) const noexcept;
  void retire_note_ledger_channel(uint32_t destination_id, uint8_t group, uint8_t channel) noexcept;
  const PreparedLayeredSysEx* valid_prepared_sysex(
      const PreparedMidiSysEx* prepared) const noexcept;
  void invalidate_prepared_domain() noexcept {
    ++prepared_domain_;
    if (prepared_domain_ == 0) ++prepared_domain_;
  }
  /// Adds one layer's rendered legs to @p target at the layer's balance, with
  /// the mono fold-down on every channel past two.
  static void add_layer_output(float* const* target, int num_channels, int legs, const float* left,
                               const float* right, const Layer& layer, int num_samples) noexcept;

  /// @brief Every layer's discard count added together, for the block delta in
  ///        process(). RT-safe: relaxed atomic loads only.
  /// @details Each layer is a full MidiInstrument with its own live counter,
  ///   reachable from outside only through this instrument, so a discard inside
  ///   one is observable nowhere unless process() records it. The sum answers
  ///   "did any layer move", never published as a count of its own -- a layer
  ///   can be driven several times per this instrument's block. Mirrors
  ///   ChannelStrip / BusProcessor.
  uint64_t layer_discard_sum() const noexcept {
    uint64_t total = 0;
    for (const Layer& layer : layers_) total += layer.instrument->non_finite_discard_count();
    return total;
  }

  std::vector<Layer> layers_;
  std::vector<ActiveNote> note_ledger_;
  LedgerOverflowCounter ledger_overflow_count_;
  /// Per-child render scratch: max_block_size frames for each of two legs.
  std::vector<float> scratch_;
  /// Per-layer source-track render scratch, reused across layers (zero-cleared
  /// per layer, per call): kMaxResidualSources slots x two legs x
  /// max_block_size_ frames. Sized to midi::kMaxResidualSources rather than
  /// the engine's own MidiInstrumentSourceOutput bound, which this header
  /// cannot see without depending on src/engine.
  std::vector<float> source_scratch_;
  int max_block_size_ = 0;
  bool prepared_ = false;
  uint64_t prepared_domain_ = 1;
  std::shared_ptr<const PreparedLayeredSysExIdentity> prepared_identity_;
};

}  // namespace sonare::midi
