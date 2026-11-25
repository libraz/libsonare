#ifndef SONARE_NODE_SONARE_WRAP_MIDI_CC_H_
#define SONARE_NODE_SONARE_WRAP_MIDI_CC_H_

/// @file sonare_wrap_midi_cc.h
/// @brief The one JS reader and writer of a MIDI CC binding descriptor, shared by the
///        Project-side helpers and the realtime engine so both spell it alike.

#include <napi.h>
#include <sonare/sonare_c.h>

#include <array>
#include <string>

#include "c_api/midi_input_domains.h"
#include "midi/cc_map.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

/// @brief Binding kind tokens indexed by the C ABI ordinal, taken from the core writer.
inline std::array<const char*, 4> CcBindingKindNames() {
  std::array<const char*, 4> names{};
  for (std::size_t i = 0; i < names.size(); ++i) {
    names[i] = sonare::midi::to_string(sonare::midi::kCcBindingKinds[i]);
  }
  return names;
}

/// @brief Check every binding field's type before any one of its domains is read.
inline bool CcBindingShapeFromValue(Napi::Env env, const Napi::Value& value,
                                    const std::string& at) {
  if (!CheckNodeInputShape(env, value, at.empty() ? "binding" : at, NodeInputShape::kObject)) {
    return false;
  }
  return PreflightNodeObjectShapes(env, value.As<Napi::Object>(),
                                   {{"ccNumber", NodeInputShape::kInteger, false,
                                     node_domain_type_text(sonare_c_detail::kMidiDataByteDomain)},
                                    {"channel", NodeInputShape::kInteger, true},
                                    {"kind", NodeInputShape::kString, true},
                                    {"ccLsbNumber", NodeInputShape::kInteger, true},
                                    {"selectorMsb", NodeInputShape::kInteger, true},
                                    {"selectorLsb", NodeInputShape::kInteger, true},
                                    {"paramId", NodeInputShape::kInteger, false},
                                    {"minValue", NodeInputShape::kNumber, true},
                                    {"maxValue", NodeInputShape::kNumber, true},
                                    {"consume", NodeInputShape::kBoolean, true}},
                                   at);
}

/// @brief Check every row's shape before any binding's domain is read.
inline bool CcBindingListShapesFromArray(Napi::Env env, const Napi::Array& array,
                                         const std::string& name = "bindings") {
  const uint32_t count = node_list_length(array, name);
  for (uint32_t i = 0; i < count; ++i) {
    const std::string at = name + "[" + std::to_string(i) + "]";
    const Napi::Value value = array.Get(i);
    if (!CcBindingShapeFromValue(env, value, at)) return false;
  }
  return true;
}

/// @brief Read a binding descriptor after its full shape has been admitted.
/// @param at The binding's place in a list (`bindings[2]`), prefixed to every field name, or
///   empty for a binding passed on its own.
inline bool CcBindingFromObject(Napi::Env env, Napi::Object object, SonareMidiCcBinding* out,
                                const std::string& at) {
  if (!CcBindingShapeFromValue(env, object, at)) return false;
  const Napi::Value cc_number = object.Get("ccNumber");
  const Napi::Value param_id = object.Get("paramId");
  *out = SonareMidiCcBinding{};
  // The 14-bit kind's narrower [0, 31] is refused by the C ABI, which sees the kind.
  try {
    out->cc_number =
        node_domain_number<uint8_t>(cc_number, node_element_field(at, "ccNumber").c_str(),
                                    sonare_c_detail::kMidiDataByteDomain);
  } catch (...) {
    ThrowCurrentException(env);
    return false;
  }
  // An omitted channel matches any channel; 255, the C ABI's spelling of that, is refused.
  out->channel = static_cast<uint8_t>(
      IntInRangeOrAbsentProperty(object, "channel", 0, 15, SONARE_MIDI_CC_ANY_CHANNEL, at));
  out->kind =
      static_cast<uint8_t>(NamedSelectorProperty(object, "kind", CcBindingKindNames(), 0, at));
  const auto& byte_domain = sonare_c_detail::kMidiDataByteDomain;
  if (!OptionalDomainProperty(env, object, "ccLsbNumber", node_element_field(at, "ccLsbNumber"),
                              byte_domain, &out->cc_lsb_number) ||
      !OptionalDomainProperty(env, object, "selectorMsb", node_element_field(at, "selectorMsb"),
                              byte_domain, &out->selector_msb) ||
      !OptionalDomainProperty(env, object, "selectorLsb", node_element_field(at, "selectorLsb"),
                              byte_domain, &out->selector_lsb)) {
    return false;
  }
  out->param_id = node_narrow_uint32(env, param_id, node_element_field(at, "paramId").c_str());
  out->min_value = FloatProperty(object, "minValue", 0.0f, at);
  out->max_value = FloatProperty(object, "maxValue", 1.0f, at);
  const bool consume = BoolProperty(object, "consume", false, at);
  out->consume = consume ? 1 : 0;
  return !env.IsExceptionPending();
}

/// @brief Write a binding descriptor in the shape @ref CcBindingFromObject reads.
inline Napi::Object CcBindingToObject(Napi::Env env, const SonareMidiCcBinding& binding) {
  const auto kind_names = CcBindingKindNames();
  if (binding.kind >= kind_names.size()) {
    throw MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                          "unknown MIDI CC binding kind ordinal: " + std::to_string(binding.kind));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("ccNumber", Napi::Number::New(env, binding.cc_number));
  if (binding.channel != SONARE_MIDI_CC_ANY_CHANNEL) {
    out.Set("channel", Napi::Number::New(env, binding.channel));
  }
  out.Set("kind", Napi::String::New(env, kind_names[binding.kind]));
  out.Set("ccLsbNumber", Napi::Number::New(env, binding.cc_lsb_number));
  out.Set("selectorMsb", Napi::Number::New(env, binding.selector_msb));
  out.Set("selectorLsb", Napi::Number::New(env, binding.selector_lsb));
  out.Set("paramId", Napi::Number::New(env, binding.param_id));
  out.Set("minValue", Napi::Number::New(env, binding.min_value));
  out.Set("maxValue", Napi::Number::New(env, binding.max_value));
  out.Set("consume", Napi::Boolean::New(env, binding.consume != 0));
  return out;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_MIDI_CC_H_
