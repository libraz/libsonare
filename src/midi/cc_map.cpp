#include "midi/cc_map.h"

#include "midi/channel_voice_decode.h"
#include "midi/control_value.h"

namespace sonare::midi {
namespace {

constexpr float kCc7BitMax = 127.0f;
constexpr float kCc14BitMax = 16383.0f;
constexpr uint8_t kDataEntryMsb = 6;
constexpr uint8_t kDataEntryLsb = 38;
constexpr uint8_t kNrpnLsb = 98;
constexpr uint8_t kNrpnMsb = 99;
constexpr uint8_t kRpnLsb = 100;
constexpr uint8_t kRpnMsb = 101;

// A control-change of either protocol, decoded to its UMP group, controller
// number, channel and value at MIDI 2.0 width.
bool decode_control_change(const Ump& ump, uint8_t* group, uint8_t* cc, uint8_t* channel,
                           Control32* value) noexcept {
  ChannelVoiceEvent event;
  if (!decode_channel_voice(ump, &event) || event.kind != ChannelVoiceKind::ControlChange) {
    return false;
  }
  *group = event.group;
  *cc = event.note;
  *channel = event.channel;
  *value = event.value;
  return true;
}

bool is_control_change(const Ump& ump) noexcept {
  const UmpMessageType type = ump.message_type();
  if (type != UmpMessageType::kMidi1ChannelVoice && type != UmpMessageType::kMidi2ChannelVoice) {
    return false;
  }
  return ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange);
}

// True for the kinds addressed by an RPN/NRPN selector pair rather than by their
// own controller number. Their cc_number is the Data Entry controller every such
// binding shares, so it identifies nothing on its own.
bool is_selector_kind(CcBindingKind kind) noexcept {
  return kind == CcBindingKind::kRpn || kind == CcBindingKind::kNrpn;
}

// Two bindings collide when the same incoming message would address both.
//
// The kind is NOT part of the identity, because it is not part of the address: a
// 7-bit CC#7 and a 14-bit CC#7 are reached by the very same MSB message, so the
// table cannot hold both and expect a lookup to be well defined -- binding one
// over the other has to replace it. What the kind decides is WHICH field carries
// the address: a plain / 14-bit controller is addressed by (cc_number, channel),
// an RPN or NRPN by (selector, channel), which is why two NRPN bindings with
// different selectors coexist on one channel even though both name Data Entry as
// their cc_number.
bool same_binding_identity(const CcBinding& a, const CcBinding& b) noexcept {
  if (a.channel != b.channel) return false;
  if (is_selector_kind(a.kind) != is_selector_kind(b.kind)) return false;
  if (is_selector_kind(a.kind)) {
    return a.kind == b.kind && a.selector_msb == b.selector_msb && a.selector_lsb == b.selector_lsb;
  }
  return a.cc_number == b.cc_number;
}

}  // namespace

bool cc_number_of(const Ump& ump, uint8_t* out_cc) noexcept {
  if (out_cc == nullptr || !is_control_change(ump)) {
    return false;
  }
  // Both protocols carry the controller index in word[0] bits 8..14 (the same
  // field exposed by Ump::note_number()).
  *out_cc = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
  return true;
}

bool cc_normalized_value(const Ump& ump, float* out_norm) noexcept {
  if (out_norm == nullptr) {
    return false;
  }
  if (is_registered_or_assignable_controller(ump)) {
    // Registered controllers 0-31 carry structured data: truncating to 14 bits
    // reads back the sender's value whether it was zero-extended or up-scaled
    // by min-center-max (M2-115-U 4.1).
    const uint8_t index = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
    if (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kRegisteredController) &&
        index < 32u) {
      *out_norm = static_cast<float>(Control32::from_raw(ump.words[1]).u14()) / kCc14BitMax;
    } else {
      *out_norm = static_cast<float>(ump.words[1]) / static_cast<float>(0xFFFFFFFFu);
    }
    return true;
  }
  if (!is_control_change(ump)) {
    return false;
  }
  if (ump.message_type() == UmpMessageType::kMidi1ChannelVoice) {
    const uint8_t value7 = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
    *out_norm = static_cast<float>(value7) / kCc7BitMax;
  } else {
    // MIDI 2.0 control-change: full 32-bit value in word[1].
    *out_norm = static_cast<float>(ump.words[1]) / static_cast<float>(0xFFFFFFFFu);
  }
  return true;
}

size_t CcMap::find_exact(uint8_t cc_number, uint8_t channel) const noexcept {
  // The binding a control-change on this number addresses, whatever resolution
  // it was bound at. Restricting this to kControlChange7 is what left a 14-bit
  // binding unremovable: unbind() could not name it, and clear() was the only
  // way back.
  for (size_t i = 0; i < count_; ++i) {
    if (!is_selector_kind(bindings_[i].kind) && bindings_[i].cc_number == cc_number &&
        bindings_[i].channel == channel) {
      return i;
    }
  }
  return kMaxBindings;
}

size_t CcMap::find_addressed_binding(const Ump& ump) const noexcept {
  const uint8_t channel = ump.channel();
  if (is_registered_or_assignable_controller(ump)) {
    // The selector travels as (bank, index) in word[0]'s two low bytes, the same
    // pair param_to_cc writes from (selector_msb, selector_lsb).
    const CcBindingKind kind =
        ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kRegisteredController)
            ? CcBindingKind::kRpn
            : CcBindingKind::kNrpn;
    const uint8_t bank = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
    const uint8_t index = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
    return find_live_binding(channel, [&](const CcBinding& b) {
      return b.kind == kind && b.selector_msb == bank && b.selector_lsb == index;
    });
  }
  uint8_t cc = 0;
  if (!cc_number_of(ump, &cc)) {
    return kMaxBindings;
  }
  // A selector-addressed binding is deliberately unreachable from a plain
  // control-change: its cc_number is the Data Entry controller every RPN/NRPN
  // binding shares, so matching on it would hand an unrelated CC#6 the first
  // NRPN binding in the table.
  return find_live_binding(
      channel, [&](const CcBinding& b) { return !is_selector_kind(b.kind) && b.cc_number == cc; });
}

size_t CcMap::find_exact_binding(const CcBinding& binding) const noexcept {
  for (size_t i = 0; i < count_; ++i) {
    if (same_binding_identity(bindings_[i], binding)) {
      return i;
    }
  }
  return kMaxBindings;
}

bool CcMap::unbind(const CcBinding& binding) noexcept {
  const size_t idx = find_exact_binding(binding);
  if (idx == kMaxBindings) {
    return false;
  }
  bindings_[idx] = bindings_[count_ - 1];
  --count_;
  return true;
}

bool CcMap::bind(const CcBinding& binding) {
  const size_t existing = find_exact_binding(binding);
  if (existing != kMaxBindings) {
    bindings_[existing] = binding;
    return true;
  }
  if (count_ >= kMaxBindings) {
    return false;
  }
  bindings_[count_++] = binding;
  return true;
}

bool CcMap::unbind(uint8_t cc_number, uint8_t channel) noexcept {
  const size_t idx = find_exact(cc_number, channel);
  if (idx == kMaxBindings) {
    return false;
  }
  // Compact: move the last binding into the gap (order is not significant).
  bindings_[idx] = bindings_[count_ - 1];
  --count_;
  return true;
}

void CcMap::clear() noexcept {
  count_ = 0;
  learning_ = false;
  learn_states_.fill(LearnChannelState{});
  reset_live_decode();
}

void CcMap::copy_bindings_from(const CcMap& source) noexcept {
  bindings_ = source.bindings_;
  count_ = source.count_;
  live_ = source.live_;
}

void CcMap::begin_learn(uint32_t param_id, float min_value, float max_value,
                        uint8_t min_movement) noexcept {
  learning_ = true;
  learn_param_id_ = param_id;
  learn_min_ = min_value;
  learn_max_ = max_value;
  learn_min_movement_ = min_movement;
  learn_states_.fill(LearnChannelState{});
}

void CcMap::cancel_learn() noexcept {
  learning_ = false;
  learn_states_.fill(LearnChannelState{});
}

bool CcMap::commit_learned_binding(const CcBinding& binding, CcBinding* out_binding) {
  learning_ = false;
  learn_states_.fill(LearnChannelState{});
  if (!bind(binding)) {
    return false;
  }
  if (out_binding != nullptr) {
    *out_binding = binding;
  }
  return true;
}

bool CcMap::observe_for_learn(const Ump& ump, CcBinding* out_binding) {
  if (!learning_) {
    return false;
  }
  uint8_t group = 0;
  uint8_t cc = 0;
  uint8_t channel = 0;
  Control32 control{0};
  if (!decode_control_change(ump, &group, &cc, &channel, &control)) {
    return false;
  }
  const uint8_t value = control.u7();
  LearnChannelState& state = learn_states_[state_key(group, channel)];

  // A selector starts a new RPN/NRPN gesture; drop a provisional 14-bit MSB.
  const bool is_selector_cc = cc == kRpnMsb || cc == kRpnLsb || cc == kNrpnMsb || cc == kNrpnLsb;
  if (is_selector_cc) {
    state.pending_cc_msb_valid = false;
  }

  // A held low CC may be a 14-bit MSB; a matching LSB completes the pair ungated.
  if (state.pending_cc_msb_valid) {
    const bool matching_lsb =
        cc >= 32 && cc < 64 && cc == static_cast<uint8_t>(state.pending_cc_msb + 32u);
    if (matching_lsb) {
      CcBinding binding;
      binding.cc_number = state.pending_cc_msb;
      binding.channel = channel;
      binding.param_id = learn_param_id_;
      binding.min_value = learn_min_;
      binding.max_value = learn_max_;
      binding.kind = CcBindingKind::kControlChange14;
      binding.cc_lsb_number = cc;
      return commit_learned_binding(binding, out_binding);
    }

    // With no threshold, any other controller commits the held low CC as 7-bit.
    if (cc != state.pending_cc_msb) {
      if (learn_min_movement_ == 0) {
        CcBinding binding;
        binding.cc_number = state.pending_cc_msb;
        binding.channel = channel;
        binding.param_id = learn_param_id_;
        binding.min_value = learn_min_;
        binding.max_value = learn_max_;
        return commit_learned_binding(binding, out_binding);
      }

      // Keep the held low CC pending; other traffic is gated on its own baseline.
      if (!state.baseline_valid[cc]) {
        state.baseline_valid[cc] = true;
        state.baseline_values[cc] = value;
        return false;
      }
      const int delta = static_cast<int>(value) - static_cast<int>(state.baseline_values[cc]);
      const int magnitude = delta < 0 ? -delta : delta;
      if (magnitude < static_cast<int>(learn_min_movement_)) {
        return false;
      }
      CcBinding binding;
      binding.cc_number = cc;
      binding.channel = channel;
      binding.param_id = learn_param_id_;
      binding.min_value = learn_min_;
      binding.max_value = learn_max_;
      return commit_learned_binding(binding, out_binding);
    }
    if (learn_min_movement_ > 0) {
      const int delta = static_cast<int>(value) - static_cast<int>(state.pending_cc_msb_value);
      const int magnitude = delta < 0 ? -delta : delta;
      if (magnitude < static_cast<int>(learn_min_movement_)) {
        return false;
      }
    }

    CcBinding binding;
    binding.cc_number = state.pending_cc_msb;
    binding.channel = channel;
    binding.param_id = learn_param_id_;
    binding.min_value = learn_min_;
    binding.max_value = learn_max_;
    return commit_learned_binding(binding, out_binding);
  }

  if (cc == kRpnMsb) {
    state.rpn_msb = value;
    state.rpn_msb_valid = true;
    state.nrpn_msb_valid = false;
    state.nrpn_lsb_valid = false;
    return false;
  }
  if (cc == kRpnLsb) {
    state.rpn_lsb = value;
    state.rpn_lsb_valid = true;
    state.nrpn_msb_valid = false;
    state.nrpn_lsb_valid = false;
    return false;
  }
  if (cc == kNrpnMsb) {
    state.nrpn_msb = value;
    state.nrpn_msb_valid = true;
    state.rpn_msb_valid = false;
    state.rpn_lsb_valid = false;
    return false;
  }
  if (cc == kNrpnLsb) {
    state.nrpn_lsb = value;
    state.nrpn_lsb_valid = true;
    state.rpn_msb_valid = false;
    state.rpn_lsb_valid = false;
    return false;
  }

  CcBinding binding;
  binding.cc_number = cc;
  binding.channel = channel;
  binding.param_id = learn_param_id_;
  binding.min_value = learn_min_;
  binding.max_value = learn_max_;

  if ((cc == kDataEntryMsb || cc == kDataEntryLsb) && state.rpn_msb_valid && state.rpn_lsb_valid) {
    binding.kind = CcBindingKind::kRpn;
    binding.selector_msb = state.rpn_msb;
    binding.selector_lsb = state.rpn_lsb;
    return commit_learned_binding(binding, out_binding);
  }
  if ((cc == kDataEntryMsb || cc == kDataEntryLsb) && state.nrpn_msb_valid &&
      state.nrpn_lsb_valid) {
    binding.kind = CcBindingKind::kNrpn;
    binding.selector_msb = state.nrpn_msb;
    binding.selector_lsb = state.nrpn_lsb;
    return commit_learned_binding(binding, out_binding);
  }

  if (cc < 32) {
    state.pending_cc_msb_valid = true;
    state.pending_cc_msb = cc;
    state.pending_cc_msb_value = value;
    return false;
  }

  // A standalone controller must move learn_min_movement_ from its first value.
  if (learn_min_movement_ > 0) {
    if (!state.baseline_valid[cc]) {
      state.baseline_valid[cc] = true;
      state.baseline_values[cc] = value;
      return false;
    }
    const int delta = static_cast<int>(value) - static_cast<int>(state.baseline_values[cc]);
    const int magnitude = delta < 0 ? -delta : delta;
    if (magnitude < static_cast<int>(learn_min_movement_)) {
      return false;
    }
  }

  return commit_learned_binding(binding, out_binding);
}

bool CcMap::lookup_param(uint8_t cc_number, uint8_t channel, uint32_t* out_param) const noexcept {
  if (out_param == nullptr) {
    return false;
  }
  // Exact-channel binding wins over an any-channel binding.
  size_t any_idx = kMaxBindings;
  for (size_t i = 0; i < count_; ++i) {
    if (is_selector_kind(bindings_[i].kind) || bindings_[i].cc_number != cc_number) {
      continue;
    }
    if (bindings_[i].channel == channel) {
      *out_param = bindings_[i].param_id;
      return true;
    }
    if (bindings_[i].channel == kCcAnyChannel) {
      any_idx = i;
    }
  }
  if (any_idx != kMaxBindings) {
    *out_param = bindings_[any_idx].param_id;
    return true;
  }
  return false;
}

bool CcMap::value_to_unit(uint8_t cc_number, uint8_t channel, float norm,
                          float* out_unit) const noexcept {
  if (out_unit == nullptr) {
    return false;
  }
  size_t any_idx = kMaxBindings;
  for (size_t i = 0; i < count_; ++i) {
    if (is_selector_kind(bindings_[i].kind) || bindings_[i].cc_number != cc_number) {
      continue;
    }
    if (bindings_[i].channel == channel) {
      const CcBinding& b = bindings_[i];
      *out_unit = b.min_value + norm * (b.max_value - b.min_value);
      return true;
    }
    if (bindings_[i].channel == kCcAnyChannel) {
      any_idx = i;
    }
  }
  if (any_idx != kMaxBindings) {
    const CcBinding& b = bindings_[any_idx];
    *out_unit = b.min_value + norm * (b.max_value - b.min_value);
    return true;
  }
  return false;
}

void CcMap::reset_live_decode() const noexcept {
  for (auto& state : live_->channels) {
    state = LiveChannelState{};
  }
}

bool CcMap::observe_live_cc(const Ump& ump, uint32_t* out_param, float* out_unit) const noexcept {
  if (out_param == nullptr || out_unit == nullptr) {
    return false;
  }
  // Every MIDI 2.0 controller message already carries full resolution in
  // word[1] and names its own binding, so it resolves without the MIDI 1.0
  // accumulator (14-bit MSB/LSB reassembly and the selector + Data Entry
  // gesture are both MIDI 1.0 constructs). This arm covers the one-word
  // Registered / Assignable Controller forms too: they are what param_to_cc
  // emits for a selector-addressed binding, and resolving them by cc_number is
  // impossible because they carry no controller number at all.
  if (ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    const size_t idx = find_addressed_binding(ump);
    float norm = 0.0f;
    if (idx == kMaxBindings || !cc_normalized_value(ump, &norm)) {
      return false;
    }
    const CcBinding& b = bindings_[idx];
    *out_param = b.param_id;
    *out_unit = b.min_value + norm * (b.max_value - b.min_value);
    return true;
  }

  uint8_t group = 0;
  uint8_t cc = 0;
  uint8_t channel = 0;
  Control32 control{0};
  if (!decode_control_change(ump, &group, &cc, &channel, &control)) {
    return false;
  }
  const uint8_t value7 = control.u7();
  LiveChannelState& st = live_->channels[state_key(group, channel)];

  auto emit_from = [&](size_t idx, float norm) {
    const CcBinding& b = bindings_[idx];
    *out_param = b.param_id;
    *out_unit = b.min_value + norm * (b.max_value - b.min_value);
  };
  auto emit_plain = [&] {
    uint32_t param = 0;
    if (!lookup_param(cc, channel, &param) ||
        !value_to_unit(cc, channel, static_cast<float>(value7) / kCc7BitMax, out_unit)) {
      return false;
    }
    *out_param = param;
    return true;
  };

  // RPN/NRPN selector messages address a parameter; they never emit a value.
  if (cc == kRpnMsb) {
    st.rpn_msb = value7;
    st.nrpn_active = false;
    return emit_plain();
  }
  if (cc == kRpnLsb) {
    st.rpn_lsb = value7;
    st.nrpn_active = false;
    return emit_plain();
  }
  if (cc == kNrpnMsb) {
    st.nrpn_msb = value7;
    st.nrpn_active = true;
    return emit_plain();
  }
  if (cc == kNrpnLsb) {
    st.nrpn_lsb = value7;
    st.nrpn_active = true;
    return emit_plain();
  }

  // Data Entry drives the currently-selected RPN/NRPN binding at 14 bits.
  if (cc == kDataEntryMsb || cc == kDataEntryLsb) {
    const bool nrpn = st.nrpn_active;
    const uint8_t sel_msb = nrpn ? st.nrpn_msb : st.rpn_msb;
    const uint8_t sel_lsb = nrpn ? st.nrpn_lsb : st.rpn_lsb;
    const CcBindingKind want = nrpn ? CcBindingKind::kNrpn : CcBindingKind::kRpn;
    const size_t idx = find_live_binding(channel, [&](const CcBinding& b) {
      return b.kind == want && b.selector_msb == sel_msb && b.selector_lsb == sel_lsb;
    });
    if (idx == kMaxBindings) {
      return emit_plain();
    }
    uint16_t value14 = 0;
    if (cc == kDataEntryMsb) {
      st.data_msb = value7;
      value14 = static_cast<uint16_t>(static_cast<uint16_t>(value7) << 7u);
    } else {
      value14 = static_cast<uint16_t>((static_cast<uint16_t>(st.data_msb) << 7u) | value7);
    }
    emit_from(idx, static_cast<float>(value14) / kCc14BitMax);
    return true;
  }

  // 14-bit Control Change LSB (CC 32..63): combine with the pending MSB.
  if (cc >= 32 && cc < 64) {
    const size_t idx = find_live_binding(channel, [&](const CcBinding& b) {
      return b.kind == CcBindingKind::kControlChange14 && b.cc_lsb_number == cc;
    });
    if (idx != kMaxBindings) {
      const uint8_t msb_number = static_cast<uint8_t>(cc - 32u);
      if (st.cc_msb_valid[msb_number]) {
        const uint16_t value14 = static_cast<uint16_t>(
            (static_cast<uint16_t>(st.cc_msb_values[msb_number]) << 7u) | value7);
        emit_from(idx, static_cast<float>(value14) / kCc14BitMax);
        return true;
      }
      // An LSB with no matching MSB yet: hold until the MSB arrives.
      return false;
    }
    // Not a 14-bit LSB binding -> fall through to plain 7-bit handling.
  }

  // 14-bit Control Change MSB (CC 0..31): emit at MSB resolution (LSB 0 until it
  // arrives) and remember it so the following LSB completes the 14-bit value.
  if (cc < 32) {
    const size_t idx = find_live_binding(channel, [&](const CcBinding& b) {
      return b.kind == CcBindingKind::kControlChange14 && b.cc_number == cc;
    });
    if (idx != kMaxBindings) {
      st.cc_msb_valid[cc] = true;
      st.cc_msb_values[cc] = value7;
      const uint16_t value14 = static_cast<uint16_t>(static_cast<uint16_t>(value7) << 7u);
      emit_from(idx, static_cast<float>(value14) / kCc14BitMax);
      return true;
    }
  }

  // Plain 7-bit Control Change.
  return emit_plain();
}

bool CcMap::cc_to_breakpoint(const Ump& ump, double ppq,
                             std::vector<automation::Breakpoint>* out) const {
  if (out == nullptr) {
    return false;
  }
  float norm = 0.0f;
  if (!cc_normalized_value(ump, &norm)) {
    return false;
  }
  const size_t idx = find_addressed_binding(ump);
  if (idx == kMaxBindings) {
    return false;
  }
  const CcBinding& b = bindings_[idx];
  automation::Breakpoint bp;
  bp.ppq = ppq;
  bp.value = b.min_value + norm * (b.max_value - b.min_value);
  bp.curve_to_next = automation::CurveType::Linear;
  out->push_back(bp);
  return true;
}

bool CcMap::param_to_cc(uint32_t param_id, float unit_value, uint8_t group,
                        Ump* out_ump) const noexcept {
  if (out_ump == nullptr) {
    return false;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (bindings_[i].param_id != param_id) {
      continue;
    }
    const CcBinding& b = bindings_[i];
    const float span = b.max_value - b.min_value;
    float norm = span != 0.0f ? (unit_value - b.min_value) / span : 0.0f;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    const uint8_t channel = b.channel == kCcAnyChannel ? 0u : b.channel;
    if (b.kind == CcBindingKind::kControlChange14) {
      // A 14-bit (MSB+LSB) controller cannot be expressed losslessly in a single
      // 7-bit MIDI 1.0 Control Change. Emit a MIDI 2.0 Control Change instead,
      // which carries the full-resolution value in a single UMP. The 7-bit
      // MIDI 1.0 path below is unchanged for plain kControlChange7 bindings.
      const uint16_t value14 = static_cast<uint16_t>(norm * kCc14BitMax + 0.5f);
      *out_ump = make_midi2_control_change(group, channel, b.cc_number, scale_cc_14_to_32(value14));
      return true;
    }
    if (is_selector_kind(b.kind)) {
      // Same reasoning one step further: an RPN / NRPN gesture is a four-message
      // MIDI 1.0 sequence (selector MSB, selector LSB, Data Entry MSB, LSB) that
      // no single Ump can hold, but MIDI 2.0 has a one-word form carrying the
      // selector as (bank, index) plus a 32-bit value. Emitting it is what closes
      // the cc_learn -> store -> param_to_cc round trip that used to dead-end
      // here on the very bindings cc_learn is documented to produce.
      // Registered controllers 0-31 are zero-extended (M2-115-U 4.1), everything
      // else min-center-max; cc_normalized_value reads either back exactly.
      const uint16_t value14 = static_cast<uint16_t>(norm * kCc14BitMax + 0.5f);
      const uint32_t value32 =
          scale_data_entry_14_to_32(b.kind == CcBindingKind::kRpn, b.selector_lsb, value14);
      *out_ump = b.kind == CcBindingKind::kRpn
                     ? make_midi2_registered_controller(group, channel, b.selector_msb,
                                                        b.selector_lsb, value32)
                     : make_midi2_assignable_controller(group, channel, b.selector_msb,
                                                        b.selector_lsb, value32);
      return true;
    }
    const uint8_t value7 = static_cast<uint8_t>(norm * kCc7BitMax + 0.5f);
    *out_ump = make_midi1_control_change(group, channel, b.cc_number, value7);
    return true;
  }
  return false;
}

}  // namespace sonare::midi
