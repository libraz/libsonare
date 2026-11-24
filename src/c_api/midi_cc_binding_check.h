#pragma once

/// @file midi_cc_binding_check.h
/// @brief The MIDI CC binding check shared by C entries and the WASM engine reader.

#include <sonare/sonare_c.h>

#include <array>
#include <string>

#include "c_api/midi_input_domains.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// Refuses fields a selected kind does not read, preserving both caller-visible subjects.
template <typename Kind>
inline void check_midi_cc_binding_kind_fields(Kind kind, Kind control_change_14, Kind rpn,
                                              Kind nrpn, uint8_t cc_lsb_number,
                                              uint8_t selector_msb, uint8_t selector_lsb,
                                              const std::string& cc_lsb_subject,
                                              const std::string& selector_msb_subject,
                                              const std::string& selector_lsb_subject,
                                              const std::string& kind_subject) {
  namespace check = sonare::util::check;
  if (kind != control_change_14 && cc_lsb_number != 0) {
    check::refuse_relation({cc_lsb_subject, kind_subject},
                           sonare::util::refusal::kCcLsbNumberUnlessControlChange14Relation);
  }
  if (kind != rpn && kind != nrpn) {
    if (selector_msb != 0) {
      check::refuse_relation({selector_msb_subject, kind_subject},
                             sonare::util::refusal::kCcSelectorUnlessRpnOrNrpnRelation);
    }
    if (selector_lsb != 0) {
      check::refuse_relation({selector_lsb_subject, kind_subject},
                             sonare::util::refusal::kCcSelectorUnlessRpnOrNrpnRelation);
    }
  }
}

/// Refuses a binding row no CC map can hold, naming each field under @p path.
inline void check_midi_cc_binding(const SonareMidiCcBinding& binding, const std::string& path) {
  namespace check = sonare::util::check;
  const auto subject = [&](const char* field) { return path + "." + field; };
  check::integer_within(binding.cc_number, kMidiDataByteDomain, subject("cc_number").c_str());
  if (binding.channel != SONARE_MIDI_CC_ANY_CHANNEL &&
      !check::in_domain(binding.channel, kMidiChannelDomain)) {
    check::refuse(subject("channel").c_str(), check::domain_predicate(kMidiChannelDomain) + " or " +
                                                  std::to_string(SONARE_MIDI_CC_ANY_CHANNEL));
  }
  check::one_of(binding.kind,
                std::array{SONARE_MIDI_CC_CONTROL_CHANGE_7, SONARE_MIDI_CC_CONTROL_CHANGE_14,
                           SONARE_MIDI_CC_RPN, SONARE_MIDI_CC_NRPN},
                subject("kind").c_str());
  check::one_of(binding.consume, std::array<int, 2>{0, 1}, subject("consume").c_str());
  if (binding.reserved != 0) check::refuse(subject("reserved").c_str(), "must be 0");
  check::integer_within(binding.param_id, kMidiTargetParamIdDomain, subject("param_id").c_str());
  check::finite_float(binding.min_value, subject("min_value").c_str());
  check::finite_float(binding.max_value, subject("max_value").c_str());
  if (binding.max_value < binding.min_value) {
    check::refuse_relation({subject("max_value"), subject("min_value")},
                           "{0} must be at least {1}");
  }
  if (binding.kind == SONARE_MIDI_CC_CONTROL_CHANGE_14) {
    check::integer_within(binding.cc_number, kMidiControl14NumberDomain,
                          subject("cc_number").c_str());
    if (binding.cc_lsb_number != binding.cc_number + 32u) {
      check::refuse_relation({subject("cc_lsb_number"), subject("cc_number")},
                             "{0} must be {1} + 32");
    }
  }
  check_midi_cc_binding_kind_fields(
      binding.kind, static_cast<uint8_t>(SONARE_MIDI_CC_CONTROL_CHANGE_14),
      static_cast<uint8_t>(SONARE_MIDI_CC_RPN), static_cast<uint8_t>(SONARE_MIDI_CC_NRPN),
      binding.cc_lsb_number, binding.selector_msb, binding.selector_lsb, subject("cc_lsb_number"),
      subject("selector_msb"), subject("selector_lsb"), subject("kind"));
  if (binding.kind == SONARE_MIDI_CC_RPN || binding.kind == SONARE_MIDI_CC_NRPN) {
    check::integer_within(binding.selector_msb, kMidiDataByteDomain,
                          subject("selector_msb").c_str());
    check::integer_within(binding.selector_lsb, kMidiDataByteDomain,
                          subject("selector_lsb").c_str());
  }
}

}  // namespace sonare_c_detail
