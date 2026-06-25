#pragma once

/// @file cc_map.h
/// @brief Bidirectional map between MIDI CC (controller number + channel) and
///        automation target parameter ids, plus MIDI learn.
///
/// Layering: depends on midi/ump, midi/midi_event and automation/ (Breakpoint /
/// AutomationLane). It does NOT depend on engine/ or arrangement/.
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread: bind() / unbind() / clear() mutate the binding table and
///    MAY allocate. begin_learn() arms MIDI learn; the next observed CC binds
///    the pending parameter. cc_to_breakpoint() / lane construction append to
///    std::vector<Breakpoint> and MAY allocate.
///  - AUDIO thread (optional): lookup_param() and value_to_unit() are pure
///    lookups over the fixed-capacity binding table with ZERO allocation, no
///    lock and no I/O, so a host may resolve a CC -> param id on the audio
///    thread. They never mutate the table. observe_for_learn() is control-thread
///    only (it mutates the table) and must NOT run on the audio path.
///
/// Determinism: contains no clock / random; learn binds purely from observed
/// CC identity.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "automation/automation_lane.h"
#include "midi/ump.h"

namespace sonare::midi {

/// Sentinel meaning "match a CC on any channel".
inline constexpr uint8_t kCcAnyChannel = 0xFFu;

/// Learned / bound MIDI controller identity.
enum class CcBindingKind : uint8_t {
  /// Plain 7-bit Control Change.
  kControlChange7 = 0,
  /// High-resolution 14-bit Control Change (MSB CC#0..31 plus LSB CC#32..63).
  kControlChange14 = 1,
  /// Registered Parameter Number selected by CC#101/100 and driven by Data Entry.
  kRpn = 2,
  /// Non-Registered Parameter Number selected by CC#99/98 and driven by Data Entry.
  kNrpn = 3,
};

/// A single MIDI controller <-> parameter binding. Trivially copyable POD so the
/// table can be scanned on the audio thread without allocation.
struct CcBinding {
  /// MIDI controller number (0..127).
  uint8_t cc_number = 0;
  /// MIDI channel (0..15), or kCcAnyChannel to match any channel.
  uint8_t channel = kCcAnyChannel;
  /// Automation target parameter id this CC drives.
  uint32_t param_id = 0;
  /// Output value range the normalized CC (0..1) is mapped onto. The unit value
  /// is `min_value + (cc/127) * (max_value - min_value)`.
  float min_value = 0.0f;
  float max_value = 1.0f;
  /// Controller shape. Existing callers that aggregate-initialize the first five
  /// fields continue to create plain 7-bit CC bindings.
  CcBindingKind kind = CcBindingKind::kControlChange7;
  /// LSB CC for kControlChange14 (cc_number + 32). 0 for other kinds.
  uint8_t cc_lsb_number = 0;
  /// RPN/NRPN selector MSB. 0 for ordinary CC kinds.
  uint8_t selector_msb = 0;
  /// RPN/NRPN selector LSB. 0 for ordinary CC kinds.
  uint8_t selector_lsb = 0;
};

/// Extracts the controller number from a control-change UMP (MIDI 1.0 or 2.0).
/// Returns false if `ump` is not a control-change message.
bool cc_number_of(const Ump& ump, uint8_t* out_cc) noexcept;

/// Extracts the normalized (0..1) control value from a control-change UMP.
/// MIDI 1.0 uses the 7-bit value; MIDI 2.0 uses the 32-bit value. Returns false
/// if `ump` is not a control-change message.
bool cc_normalized_value(const Ump& ump, float* out_norm) noexcept;

/// Bidirectional CC <-> automation parameter map with MIDI learn.
class CcMap {
 public:
  /// Maximum number of simultaneous CC bindings. Overflow on bind() fails
  /// (returns false), never grows; audio-thread lookups scan this fixed table.
  static constexpr size_t kMaxBindings = 256;

  // -- CONTROL thread ------------------------------------------------------

  /// Bind a CC (number + channel) to a parameter id over [min_value, max_value].
  /// If a binding with the same (cc_number, channel) exists it is replaced.
  /// Returns false if the table is full and the binding is new.
  bool bind(const CcBinding& binding);

  /// Remove the binding for (cc_number, channel). Returns true if one existed.
  bool unbind(uint8_t cc_number, uint8_t channel) noexcept;

  /// Remove all bindings and disarm learn.
  void clear() noexcept;

  size_t binding_count() const noexcept { return count_; }
  const CcBinding& binding_at(size_t index) const noexcept { return bindings_[index]; }

  /// Arm MIDI learn: a CC seen by observe_for_learn() binds `param_id` over
  /// [min_value, max_value]. Re-arming overrides any pending learn.
  ///
  /// `min_movement` is an activity threshold in 7-bit CC units (0..127). When it
  /// is 0 (the default) the FIRST observed control-change binds immediately, as
  /// before. When it is > 0, a controller is only learned once its value has
  /// MOVED by at least `min_movement` from the first value observed for that
  /// (cc_number, channel) while armed — so idle / noise traffic (a controller
  /// sitting still, or jittering by less than the threshold) does not trigger a
  /// spurious learn. The threshold is compared in 7-bit units for both MIDI 1.0
  /// (native 7-bit) and MIDI 2.0 (down-scaled) control-change values.
  void begin_learn(uint32_t param_id, float min_value = 0.0f, float max_value = 1.0f,
                   uint8_t min_movement = 0) noexcept;
  /// Disarm a pending learn without binding.
  void cancel_learn() noexcept;
  bool is_learning() const noexcept { return learning_; }

  /// CONTROL thread: feed an event to MIDI learn. If learn is armed and `ump`
  /// is a control-change, bind the observed (cc_number, channel) to the pending
  /// parameter, disarm learn and return true (the new binding is written to
  /// `out_binding` when non-null). Otherwise returns false. MAY allocate (via
  /// bind()).
  bool observe_for_learn(const Ump& ump, CcBinding* out_binding = nullptr);

  // -- AUDIO thread (lookup only; no allocation) ---------------------------

  /// Resolve the parameter id bound to (cc_number, channel). A binding with
  /// kCcAnyChannel matches any channel; an exact-channel binding takes priority
  /// over an any-channel one. Returns false if unbound. RT-safe.
  bool lookup_param(uint8_t cc_number, uint8_t channel, uint32_t* out_param) const noexcept;

  /// Map a normalized (0..1) CC value to the bound parameter's unit range.
  /// Returns false if (cc_number, channel) is unbound. RT-safe.
  bool value_to_unit(uint8_t cc_number, uint8_t channel, float norm,
                     float* out_unit) const noexcept;

  /// AUDIO thread: decode a live control-change UMP at the binding's full
  /// resolution. Unlike the cc_number-only lookup_param/value_to_unit pair, this
  /// is kind-aware: it accumulates 14-bit MSB/LSB pairs and RPN/NRPN selector +
  /// Data Entry state per channel (mirroring the learn state machine), so a
  /// high-resolution controller drives its parameter at 14-bit precision instead
  /// of the MSB-only 7 bits, and Data Entry routes to the currently-selected
  /// RPN/NRPN binding. A plain 7-bit bound CC (and any MIDI 2.0 control-change,
  /// already full-resolution) resolves immediately. Selector / LSB-only / unbound
  /// messages update state and return false. On a resolved value writes the
  /// target param id to @p out_param and the unit value to @p out_unit and
  /// returns true. RT-safe: no allocation, no lock; mutates only the per-channel
  /// live-decode state. Must be called from a single (audio) thread.
  bool observe_live_cc(const Ump& ump, uint32_t* out_param, float* out_unit) noexcept;

  /// Resets the per-channel live-decode accumulator state (14-bit MSB pending,
  /// RPN/NRPN selectors, Data Entry MSB). Does not touch bindings. Call when the
  /// live input stream is (re)started so stale partial state cannot leak across.
  void reset_live_decode() noexcept;

  // -- CONTROL thread: CC <-> automation conversion ------------------------

  /// Append a Breakpoint at `ppq` for the parameter bound to this control-change
  /// UMP. Returns false if `ump` is not a control-change or is unbound. MAY
  /// allocate (push_back). The breakpoint's value is the unit-mapped CC value.
  bool cc_to_breakpoint(const Ump& ump, double ppq, std::vector<automation::Breakpoint>* out) const;

  /// Build a control-change UMP from an automation unit value for `param_id`.
  /// The unit value is inverse-mapped to a normalized 0..1 then encoded at the
  /// binding's resolution: a kControlChange7 binding emits a 7-bit MIDI 1.0
  /// control-change; a kControlChange14 binding emits a MIDI 2.0 control-change
  /// carrying the full-resolution (14-bit, bit-scaled to 32-bit) value in a
  /// single UMP — a single 7-bit MIDI 1.0 CC cannot carry 14 bits losslessly.
  /// Returns false if no binding targets `param_id`. MAY scan the table; no
  /// allocation. Control-thread (config-time) use.
  bool param_to_cc(uint32_t param_id, float unit_value, uint8_t group, Ump* out_ump) const noexcept;

 private:
  // Returns the index of an exact (cc, channel) binding, or kMaxBindings.
  size_t find_exact(uint8_t cc_number, uint8_t channel) const noexcept;
  size_t find_exact_binding(const CcBinding& binding) const noexcept;
  bool commit_learned_binding(const CcBinding& binding, CcBinding* out_binding);

  std::array<CcBinding, kMaxBindings> bindings_{};
  size_t count_ = 0;

  bool learning_ = false;
  uint32_t learn_param_id_ = 0;
  float learn_min_ = 0.0f;
  float learn_max_ = 1.0f;

  // Activity threshold (7-bit units): a controller must move at least this far
  // from its first observed value before it is learned. 0 disables the gate.
  uint8_t learn_min_movement_ = 0;
  bool learn_baseline_valid_ = false;
  uint8_t learn_baseline_cc_ = 0;
  uint8_t learn_baseline_channel_ = 0;
  uint8_t learn_baseline_value_ = 0;

  bool pending_cc_msb_valid_ = false;
  uint8_t pending_cc_msb_ = 0;
  uint8_t pending_cc_msb_channel_ = 0;

  bool rpn_msb_valid_ = false;
  bool rpn_lsb_valid_ = false;
  uint8_t rpn_msb_ = 0;
  uint8_t rpn_lsb_ = 0;

  bool nrpn_msb_valid_ = false;
  bool nrpn_lsb_valid_ = false;
  uint8_t nrpn_msb_ = 0;
  uint8_t nrpn_lsb_ = 0;

  // Per-channel live-decode accumulator (AUDIO thread; observe_live_cc only).
  // Distinct from the control-thread learn state above.
  struct LiveChannelState {
    // 14-bit Control Change: MSB (CC 0..31) seen, awaiting its LSB (CC msb+32).
    bool cc_msb_valid = false;
    uint8_t cc_msb_number = 0;
    uint8_t cc_msb_value = 0;
    // Currently-addressed RPN/NRPN selector and which space is active.
    bool nrpn_active = false;
    uint8_t rpn_msb = 0x7Fu;
    uint8_t rpn_lsb = 0x7Fu;
    uint8_t nrpn_msb = 0x7Fu;
    uint8_t nrpn_lsb = 0x7Fu;
    // Data Entry MSB (CC 6) last value, combined with the LSB (CC 38) into the
    // 14-bit Data Entry value.
    uint8_t data_msb = 0;
  };
  std::array<LiveChannelState, 16> live_{};

  // Kind-aware binding lookup for the live-decode path: finds a binding by an
  // arbitrary predicate, preferring an exact-channel match over kCcAnyChannel.
  template <typename Pred>
  size_t find_live_binding(uint8_t channel, Pred pred) const noexcept {
    size_t any_idx = kMaxBindings;
    for (size_t i = 0; i < count_; ++i) {
      if (!pred(bindings_[i])) continue;
      if (bindings_[i].channel == channel) return i;
      if (bindings_[i].channel == kCcAnyChannel) any_idx = i;
    }
    return any_idx;
  }
};

}  // namespace sonare::midi
