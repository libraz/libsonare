#pragma once

/// @file marker_input_validation.h
/// @brief Shared applicability checks for marker-specific fields on C entry points.

#include <sonare/sonare_c_types_engine.h>

#include <string>

#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare_c_detail {

inline bool marker_kind_reads_key_signature_fields(uint8_t kind) noexcept {
  return kind == SONARE_MARKER_KIND_KEY_SIGNATURE;
}

inline void check_inactive_marker_key_signature_fields(uint8_t kind, int8_t key_fifths,
                                                       uint8_t key_minor,
                                                       const std::string& prefix) {
  if (marker_kind_reads_key_signature_fields(kind)) {
    return;
  }
  namespace check = sonare::util::check;
  const std::string kind_subject = prefix + "kind";
  const std::string fifths_subject = prefix + "key_fifths";
  const std::string minor_subject = prefix + "key_minor";
  if (key_fifths != 0) {
    check::refuse_relation({fifths_subject, kind_subject},
                           sonare::util::refusal::kMarkerKeyFieldUnlessKeySignatureRelation);
  }
  if (key_minor != 0) {
    check::refuse_relation({minor_subject, kind_subject},
                           sonare::util::refusal::kMarkerKeyFieldUnlessKeySignatureRelation);
  }
}

}  // namespace sonare_c_detail
