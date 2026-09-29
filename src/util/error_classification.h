#pragma once

/// @file error_classification.h
/// @brief Classifies a std::exception the way the C-ABI catch chain does.

#include <stdexcept>

namespace sonare {

/// @brief The numeric SonareError code (include/sonare/sonare_c_types_enums.h)
///        the C-ABI catch chain (SONARE_C_CATCH, src/c_api/sonare_c_internal.h)
///        assigns a caught std::exception, by RTTI class: std::bad_alloc -> 5
///        (OutOfMemory), std::invalid_argument -> 4 (InvalidParameter),
///        std::logic_error -> 7 (InvalidState), any other std::exception -> 99
///        (Unknown). The values are written out rather than referencing the
///        public C header, which core code sits below rather than above.
///
///        Call only for an exception that is NOT a sonare::SonareException --
///        that type carries its own code via SonareException::code() and is
///        not reclassified here. This is the single source of truth the WASM
///        exception-introspection switch and the Node SONARE_NODE_CATCH /
///        SONARE_NODE_CATCH_VOID macros both call, so a class added to one
///        catch chain cannot silently stay unmirrored on the other.
inline int error_code_for_std_exception(const std::exception& e) {
  if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) return 5;         // OutOfMemory
  if (dynamic_cast<const std::invalid_argument*>(&e) != nullptr) return 4;  // InvalidParameter
  if (dynamic_cast<const std::logic_error*>(&e) != nullptr) return 7;       // InvalidState
  return 99;                                                                // Unknown
}

}  // namespace sonare
