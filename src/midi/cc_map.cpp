#include "midi/cc_map.h"

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

bool is_control_change(const Ump& ump) noexcept {
  const UmpMessageType type = ump.message_type();
  if (type != UmpMessageType::kMidi1ChannelVoice && type != UmpMessageType::kMidi2ChannelVoice) {
    return false;
  }
  return ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange);
}

bool same_binding_identity(const CcBinding& a, const CcBinding& b) noexcept {
  if (a.kind != b.kind || a.cc_number != b.cc_number || a.channel != b.channel) {
    return false;
  }
  switch (a.kind) {
    case CcBindingKind::kControlChange7:
      return true;
    case CcBindingKind::kControlChange14:
      return a.cc_lsb_number == b.cc_lsb_number;
    case CcBindingKind::kRpn:
    case CcBindingKind::kNrpn:
      return a.selector_msb == b.selector_msb && a.selector_lsb == b.selector_lsb;
  }
  return false;
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
  if (out_norm == nullptr || !is_control_change(ump)) {
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
  for (size_t i = 0; i < count_; ++i) {
    if (bindings_[i].kind == CcBindingKind::kControlChange7 &&
        bindings_[i].cc_number == cc_number && bindings_[i].channel == channel) {
      return i;
    }
  }
  return kMaxBindings;
}

size_t CcMap::find_exact_binding(const CcBinding& binding) const noexcept {
  for (size_t i = 0; i < count_; ++i) {
    if (same_binding_identity(bindings_[i], binding)) {
      return i;
    }
  }
  return kMaxBindings;
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
  pending_cc_msb_valid_ = false;
  rpn_msb_valid_ = false;
  rpn_lsb_valid_ = false;
  nrpn_msb_valid_ = false;
  nrpn_lsb_valid_ = false;
  learn_baseline_valid_ = false;
  reset_live_decode();
}

void CcMap::begin_learn(uint32_t param_id, float min_value, float max_value,
                        uint8_t min_movement) noexcept {
  learning_ = true;
  learn_param_id_ = param_id;
  learn_min_ = min_value;
  learn_max_ = max_value;
  learn_min_movement_ = min_movement;
  learn_baseline_valid_ = false;
  pending_cc_msb_valid_ = false;
  rpn_msb_valid_ = false;
  rpn_lsb_valid_ = false;
  nrpn_msb_valid_ = false;
  nrpn_lsb_valid_ = false;
}

void CcMap::cancel_learn() noexcept {
  learning_ = false;
  pending_cc_msb_valid_ = false;
  learn_baseline_valid_ = false;
}

bool CcMap::commit_learned_binding(const CcBinding& binding, CcBinding* out_binding) {
  learning_ = false;
  pending_cc_msb_valid_ = false;
  learn_baseline_valid_ = false;
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
  uint8_t cc = 0;
  if (!cc_number_of(ump, &cc)) {
    return false;
  }
  const uint8_t value = ump.message_type() == UmpMessageType::kMidi2ChannelVoice
                            ? scale_cc_32_to_7(ump.words[1])
                            : static_cast<uint8_t>(ump.words[0] & 0x7Fu);
  const uint8_t channel = ump.channel();

  // Activity threshold: a controller must MOVE by at least learn_min_movement_
  // (7-bit units) from its first observed value before any learn logic runs, so
  // idle / noise traffic does not bind. A threshold of 0 disables the gate.
  if (learn_min_movement_ > 0) {
    if (!learn_baseline_valid_ || learn_baseline_cc_ != cc || learn_baseline_channel_ != channel) {
      learn_baseline_valid_ = true;
      learn_baseline_cc_ = cc;
      learn_baseline_channel_ = channel;
      learn_baseline_value_ = value;
      return false;
    }
    const int delta = static_cast<int>(value) - static_cast<int>(learn_baseline_value_);
    const int magnitude = delta < 0 ? -delta : delta;
    if (magnitude < static_cast<int>(learn_min_movement_)) {
      return false;
    }
  }

  if (pending_cc_msb_valid_) {
    const bool matching_lsb = cc >= 32 && cc < 64 && pending_cc_msb_channel_ == channel &&
                              cc == static_cast<uint8_t>(pending_cc_msb_ + 32u);
    CcBinding binding;
    binding.cc_number = pending_cc_msb_;
    binding.channel = pending_cc_msb_channel_;
    binding.param_id = learn_param_id_;
    binding.min_value = learn_min_;
    binding.max_value = learn_max_;
    if (matching_lsb) {
      binding.kind = CcBindingKind::kControlChange14;
      binding.cc_lsb_number = cc;
    }
    return commit_learned_binding(binding, out_binding);
  }

  if (cc == kRpnMsb) {
    rpn_msb_ = value;
    rpn_msb_valid_ = true;
    nrpn_msb_valid_ = false;
    nrpn_lsb_valid_ = false;
    return false;
  }
  if (cc == kRpnLsb) {
    rpn_lsb_ = value;
    rpn_lsb_valid_ = true;
    nrpn_msb_valid_ = false;
    nrpn_lsb_valid_ = false;
    return false;
  }
  if (cc == kNrpnMsb) {
    nrpn_msb_ = value;
    nrpn_msb_valid_ = true;
    rpn_msb_valid_ = false;
    rpn_lsb_valid_ = false;
    return false;
  }
  if (cc == kNrpnLsb) {
    nrpn_lsb_ = value;
    nrpn_lsb_valid_ = true;
    rpn_msb_valid_ = false;
    rpn_lsb_valid_ = false;
    return false;
  }

  CcBinding binding;
  binding.cc_number = cc;
  binding.channel = channel;
  binding.param_id = learn_param_id_;
  binding.min_value = learn_min_;
  binding.max_value = learn_max_;

  if ((cc == kDataEntryMsb || cc == kDataEntryLsb) && rpn_msb_valid_ && rpn_lsb_valid_) {
    binding.kind = CcBindingKind::kRpn;
    binding.selector_msb = rpn_msb_;
    binding.selector_lsb = rpn_lsb_;
    return commit_learned_binding(binding, out_binding);
  }
  if ((cc == kDataEntryMsb || cc == kDataEntryLsb) && nrpn_msb_valid_ && nrpn_lsb_valid_) {
    binding.kind = CcBindingKind::kNrpn;
    binding.selector_msb = nrpn_msb_;
    binding.selector_lsb = nrpn_lsb_;
    return commit_learned_binding(binding, out_binding);
  }

  if (cc < 32) {
    pending_cc_msb_valid_ = true;
    pending_cc_msb_ = cc;
    pending_cc_msb_channel_ = channel;
    return false;
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
    if (bindings_[i].cc_number != cc_number) {
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
    if (bindings_[i].cc_number != cc_number) {
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

void CcMap::reset_live_decode() noexcept {
  for (auto& state : live_) {
    state = LiveChannelState{};
  }
}

bool CcMap::observe_live_cc(const Ump& ump, uint32_t* out_param, float* out_unit) noexcept {
  if (out_param == nullptr || out_unit == nullptr) {
    return false;
  }
  uint8_t cc = 0;
  if (!cc_number_of(ump, &cc)) {
    return false;
  }
  const uint8_t channel = ump.channel();

  // MIDI 2.0 control-change already carries full resolution in word[1]; resolve
  // directly (14-bit MSB/LSB reassembly is a MIDI 1.0 construct).
  if (ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
    float norm = 0.0f;
    uint32_t param = 0;
    if (!cc_normalized_value(ump, &norm) || !lookup_param(cc, channel, &param) ||
        !value_to_unit(cc, channel, norm, out_unit)) {
      return false;
    }
    *out_param = param;
    return true;
  }

  const uint8_t value7 = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
  LiveChannelState& st = live_[channel & 0x0Fu];

  auto emit_from = [&](size_t idx, float norm) {
    const CcBinding& b = bindings_[idx];
    *out_param = b.param_id;
    *out_unit = b.min_value + norm * (b.max_value - b.min_value);
  };

  // RPN/NRPN selector messages address a parameter; they never emit a value.
  if (cc == kRpnMsb) {
    st.rpn_msb = value7;
    st.nrpn_active = false;
    return false;
  }
  if (cc == kRpnLsb) {
    st.rpn_lsb = value7;
    st.nrpn_active = false;
    return false;
  }
  if (cc == kNrpnMsb) {
    st.nrpn_msb = value7;
    st.nrpn_active = true;
    return false;
  }
  if (cc == kNrpnLsb) {
    st.nrpn_lsb = value7;
    st.nrpn_active = true;
    return false;
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
      return false;
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
      if (st.cc_msb_valid && static_cast<uint8_t>(st.cc_msb_number + 32u) == cc) {
        const uint16_t value14 =
            static_cast<uint16_t>((static_cast<uint16_t>(st.cc_msb_value) << 7u) | value7);
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
      st.cc_msb_valid = true;
      st.cc_msb_number = cc;
      st.cc_msb_value = value7;
      const uint16_t value14 = static_cast<uint16_t>(static_cast<uint16_t>(value7) << 7u);
      emit_from(idx, static_cast<float>(value14) / kCc14BitMax);
      return true;
    }
  }

  // Plain 7-bit Control Change.
  uint32_t param = 0;
  if (lookup_param(cc, channel, &param) &&
      value_to_unit(cc, channel, static_cast<float>(value7) / kCc7BitMax, out_unit)) {
    *out_param = param;
    return true;
  }
  return false;
}

bool CcMap::cc_to_breakpoint(const Ump& ump, double ppq,
                             std::vector<automation::Breakpoint>* out) const {
  if (out == nullptr) {
    return false;
  }
  uint8_t cc = 0;
  float norm = 0.0f;
  if (!cc_number_of(ump, &cc) || !cc_normalized_value(ump, &norm)) {
    return false;
  }
  float unit = 0.0f;
  if (!value_to_unit(cc, ump.channel(), norm, &unit)) {
    return false;
  }
  automation::Breakpoint bp;
  bp.ppq = ppq;
  bp.value = unit;
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
      // which carries the full-resolution value in a single UMP (the 14-bit
      // value is bit-scaled up to the MIDI 2.0 32-bit field). The 7-bit MIDI 1.0
      // path below is unchanged for plain kControlChange7 bindings.
      const uint16_t value14 = static_cast<uint16_t>(norm * kCc14BitMax + 0.5f);
      *out_ump =
          make_midi2_control_change(group, channel, b.cc_number, scale_bend_14_to_32(value14));
      return true;
    }
    if (b.kind == CcBindingKind::kRpn || b.kind == CcBindingKind::kNrpn) {
      return false;
    }
    const uint8_t value7 = static_cast<uint8_t>(norm * kCc7BitMax + 0.5f);
    *out_ump = make_midi1_control_change(group, channel, b.cc_number, value7);
    return true;
  }
  return false;
}

}  // namespace sonare::midi
