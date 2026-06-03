#include "midi/cc_map.h"

namespace sonare::midi {
namespace {

constexpr float kCc7BitMax = 127.0f;

bool is_control_change(const Ump& ump) noexcept {
  const UmpMessageType type = ump.message_type();
  if (type != UmpMessageType::kMidi1ChannelVoice && type != UmpMessageType::kMidi2ChannelVoice) {
    return false;
  }
  return ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange);
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
    if (bindings_[i].cc_number == cc_number && bindings_[i].channel == channel) {
      return i;
    }
  }
  return kMaxBindings;
}

bool CcMap::bind(const CcBinding& binding) {
  const size_t existing = find_exact(binding.cc_number, binding.channel);
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
}

void CcMap::begin_learn(uint32_t param_id, float min_value, float max_value) noexcept {
  learning_ = true;
  learn_param_id_ = param_id;
  learn_min_ = min_value;
  learn_max_ = max_value;
}

void CcMap::cancel_learn() noexcept { learning_ = false; }

bool CcMap::observe_for_learn(const Ump& ump, CcBinding* out_binding) {
  if (!learning_) {
    return false;
  }
  uint8_t cc = 0;
  if (!cc_number_of(ump, &cc)) {
    return false;
  }
  CcBinding binding;
  binding.cc_number = cc;
  binding.channel = ump.channel();
  binding.param_id = learn_param_id_;
  binding.min_value = learn_min_;
  binding.max_value = learn_max_;
  learning_ = false;
  if (!bind(binding)) {
    return false;
  }
  if (out_binding != nullptr) {
    *out_binding = binding;
  }
  return true;
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
    const uint8_t value7 = static_cast<uint8_t>(norm * kCc7BitMax + 0.5f);
    const uint8_t channel = b.channel == kCcAnyChannel ? 0u : b.channel;
    *out_ump = make_midi1_control_change(group, channel, b.cc_number, value7);
    return true;
  }
  return false;
}

}  // namespace sonare::midi
