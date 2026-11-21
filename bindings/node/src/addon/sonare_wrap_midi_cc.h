#ifndef SONARE_NODE_SONARE_WRAP_MIDI_CC_H_
#define SONARE_NODE_SONARE_WRAP_MIDI_CC_H_

/// @file sonare_wrap_midi_cc.h
/// @brief The one JS reader and writer of a MIDI CC binding descriptor, shared by the
///        Project-side helpers and the realtime engine so both spell it alike.

#include <napi.h>
#include <sonare/sonare_c.h>

#include <array>

#include "c_api/midi_input_domains.h"
#include "midi/cc_map.h"
#include "sonare_wrap_options.h"

namespace sonare_node {

/// @brief Binding kind tokens indexed by the C ABI ordinal, taken from the core writer.
inline std::array<const char*, 4> CcBindingKindNames() {
  std::array<const char*, 4> names{};
  for (std::size_t i = 0; i < names.size(); ++i) {
    names[i] = sonare::midi::to_string(sonare::midi::kCcBindingKinds[i]);
  }
  return names;
}

/// @brief Read a binding descriptor; throws (and returns false) naming the offending field.
inline bool CcBindingFromObject(Napi::Env env, Napi::Object object, SonareMidiCcBinding* out) {
  const Napi::Value cc_number = object.Get("ccNumber");
  const Napi::Value param_id = object.Get("paramId");
  if (!cc_number.IsNumber() || !param_id.IsNumber()) {
    Napi::TypeError::New(env, "MIDI CC binding requires numeric ccNumber and paramId")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = SonareMidiCcBinding{};
  // The 14-bit kind's narrower [0, 31] is refused by the C ABI, which sees the kind.
  try {
    out->cc_number =
        node_domain_number<uint8_t>(cc_number, "ccNumber", sonare_c_detail::kMidiDataByteDomain);
  } catch (...) {
    ThrowCurrentException(env);
    return false;
  }
  // An omitted channel matches any channel; 255, the C ABI's spelling of that, is refused.
  out->channel = static_cast<uint8_t>(
      IntInRangeOrAbsentProperty(object, "channel", 0, 15, SONARE_MIDI_CC_ANY_CHANNEL));
  out->kind = static_cast<uint8_t>(NamedSelectorProperty(object, "kind", CcBindingKindNames(), 0));
  out->cc_lsb_number = MidiByteProperty(env, object, "ccLsbNumber", 0u);
  out->selector_msb = MidiByteProperty(env, object, "selectorMsb", 0u);
  out->selector_lsb = MidiByteProperty(env, object, "selectorLsb", 0u);
  if (env.IsExceptionPending()) return false;
  out->param_id = node_narrow_uint32(env, param_id, "paramId");
  out->min_value = FloatProperty(object, "minValue", 0.0f);
  out->max_value = FloatProperty(object, "maxValue", 1.0f);
  return !env.IsExceptionPending();
}

/// @brief Write a binding descriptor in the shape @ref CcBindingFromObject reads.
inline Napi::Object CcBindingToObject(Napi::Env env, const SonareMidiCcBinding& binding) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("ccNumber", Napi::Number::New(env, binding.cc_number));
  if (binding.channel != SONARE_MIDI_CC_ANY_CHANNEL) {
    out.Set("channel", Napi::Number::New(env, binding.channel));
  }
  out.Set("kind",
          Napi::String::New(env, sonare::midi::to_string(
                                     static_cast<sonare::midi::CcBindingKind>(binding.kind))));
  out.Set("ccLsbNumber", Napi::Number::New(env, binding.cc_lsb_number));
  out.Set("selectorMsb", Napi::Number::New(env, binding.selector_msb));
  out.Set("selectorLsb", Napi::Number::New(env, binding.selector_lsb));
  out.Set("paramId", Napi::Number::New(env, binding.param_id));
  out.Set("minValue", Napi::Number::New(env, binding.min_value));
  out.Set("maxValue", Napi::Number::New(env, binding.max_value));
  return out;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_MIDI_CC_H_
