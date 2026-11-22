#pragma once

/// @file sonare_c_error_mapping.h
/// @brief Shared error mapping between the core and C ABI.

#include <sonare/sonare_c.h>

#include "util/types.h"

namespace sonare_c_detail {

/// Forward mapping used by every C-ABI facade that translates a core exception.
/// Keep this switch exhaustive and free of a default arm: adding an
/// ErrorCode enumerator must produce a compile-time diagnostic until the C
/// surface has an explicit answer for it.
inline constexpr SonareError c_error_from_error_code(sonare::ErrorCode code) noexcept {
  switch (code) {
    case sonare::ErrorCode::Ok:
      return SONARE_ERROR_UNKNOWN;
    case sonare::ErrorCode::FileNotFound:
      return SONARE_ERROR_FILE_NOT_FOUND;
    case sonare::ErrorCode::InvalidFormat:
      return SONARE_ERROR_INVALID_FORMAT;
    case sonare::ErrorCode::DecodeFailed:
      return SONARE_ERROR_DECODE_FAILED;
    case sonare::ErrorCode::InvalidParameter:
      return SONARE_ERROR_INVALID_PARAMETER;
    case sonare::ErrorCode::OutOfMemory:
      return SONARE_ERROR_OUT_OF_MEMORY;
    case sonare::ErrorCode::NotSupported:
      return SONARE_ERROR_NOT_SUPPORTED;
    case sonare::ErrorCode::InvalidState:
      return SONARE_ERROR_INVALID_STATE;
    case sonare::ErrorCode::Cancelled:
      return SONARE_ERROR_CANCELLED;
    case sonare::ErrorCode::EncodeFailed:
      return SONARE_ERROR_ENCODE_FAILED;
    case sonare::ErrorCode::AbiMismatch:
      return SONARE_ERROR_ABI_MISMATCH;
    case sonare::ErrorCode::StaleResult:
      return SONARE_ERROR_STALE_RESULT;
    case sonare::ErrorCode::Unknown:
      return SONARE_ERROR_UNKNOWN;
  }
  // A scoped enum can hold an out-of-range underlying value.
  return SONARE_ERROR_UNKNOWN;
}

/// Every core enumerator carries its C-ABI number, so a binding that reads the
/// core ordinal publishes the same code as one that goes through this mapping.
inline constexpr bool core_ordinals_match_c_abi() noexcept {
  for (const sonare::ErrorCode code :
       {sonare::ErrorCode::Ok, sonare::ErrorCode::FileNotFound, sonare::ErrorCode::InvalidFormat,
        sonare::ErrorCode::DecodeFailed, sonare::ErrorCode::InvalidParameter,
        sonare::ErrorCode::OutOfMemory, sonare::ErrorCode::NotSupported,
        sonare::ErrorCode::InvalidState, sonare::ErrorCode::Cancelled,
        sonare::ErrorCode::EncodeFailed, sonare::ErrorCode::AbiMismatch,
        sonare::ErrorCode::StaleResult, sonare::ErrorCode::Unknown}) {
    if (code == sonare::ErrorCode::Ok) continue;
    if (static_cast<int>(code) != static_cast<int>(c_error_from_error_code(code))) return false;
  }
  return true;
}
static_assert(core_ordinals_match_c_abi(), "a core ErrorCode ordinal differs from its C-ABI code");

/// Inverse of c_error_from_error_code(): lets facades that call the C ABI raise
/// the same core ErrorCode without maintaining a second switch table. Every C
/// failure code keeps its own enumerator; the default arm takes only values no
/// C call returns, as Unknown.
inline sonare::ErrorCode error_code_from_c_error(SonareError err) {
  switch (err) {
    case SONARE_ERROR_FILE_NOT_FOUND:
      return sonare::ErrorCode::FileNotFound;
    case SONARE_ERROR_INVALID_FORMAT:
      return sonare::ErrorCode::InvalidFormat;
    case SONARE_ERROR_DECODE_FAILED:
      return sonare::ErrorCode::DecodeFailed;
    case SONARE_ERROR_INVALID_PARAMETER:
      return sonare::ErrorCode::InvalidParameter;
    case SONARE_ERROR_OUT_OF_MEMORY:
      return sonare::ErrorCode::OutOfMemory;
    case SONARE_ERROR_NOT_SUPPORTED:
      return sonare::ErrorCode::NotSupported;
    case SONARE_ERROR_INVALID_STATE:
      return sonare::ErrorCode::InvalidState;
    case SONARE_ERROR_CANCELLED:
      return sonare::ErrorCode::Cancelled;
    case SONARE_ERROR_ENCODE_FAILED:
      return sonare::ErrorCode::EncodeFailed;
    case SONARE_ERROR_ABI_MISMATCH:
      return sonare::ErrorCode::AbiMismatch;
    case SONARE_ERROR_STALE_RESULT:
      return sonare::ErrorCode::StaleResult;
    case SONARE_ERROR_UNKNOWN:
      return sonare::ErrorCode::Unknown;
    case SONARE_OK:
    default:
      return sonare::ErrorCode::Unknown;
  }
}

}  // namespace sonare_c_detail
