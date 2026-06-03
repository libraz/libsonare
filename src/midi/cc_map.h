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

/// A single CC <-> parameter binding. Trivially copyable POD so the table can be
/// scanned on the audio thread without allocation.
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

  /// Arm MIDI learn: the next CC seen by observe_for_learn() binds `param_id`
  /// over [min_value, max_value]. Re-arming overrides any pending learn.
  void begin_learn(uint32_t param_id, float min_value = 0.0f, float max_value = 1.0f) noexcept;
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

  // -- CONTROL thread: CC <-> automation conversion ------------------------

  /// Append a Breakpoint at `ppq` for the parameter bound to this control-change
  /// UMP. Returns false if `ump` is not a control-change or is unbound. MAY
  /// allocate (push_back). The breakpoint's value is the unit-mapped CC value.
  bool cc_to_breakpoint(const Ump& ump, double ppq, std::vector<automation::Breakpoint>* out) const;

  /// Build a control-change UMP from an automation unit value for `param_id`.
  /// The unit value is inverse-mapped to a normalized 0..1 then to a 7-bit CC
  /// (MIDI 1.0 control-change). Returns false if no binding targets `param_id`.
  /// MAY scan the table; no allocation. Control-thread (config-time) use.
  bool param_to_cc(uint32_t param_id, float unit_value, uint8_t group, Ump* out_ump) const noexcept;

 private:
  // Returns the index of an exact (cc, channel) binding, or kMaxBindings.
  size_t find_exact(uint8_t cc_number, uint8_t channel) const noexcept;

  std::array<CcBinding, kMaxBindings> bindings_{};
  size_t count_ = 0;

  bool learning_ = false;
  uint32_t learn_param_id_ = 0;
  float learn_min_ = 0.0f;
  float learn_max_ = 1.0f;
};

}  // namespace sonare::midi
