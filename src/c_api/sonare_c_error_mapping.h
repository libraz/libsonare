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
    case sonare::ErrorCode::NotImplemented:
      return SONARE_ERROR_NOT_SUPPORTED;
    case sonare::ErrorCode::InvalidState:
      return SONARE_ERROR_INVALID_STATE;
    case sonare::ErrorCode::Cancelled:
      return SONARE_ERROR_CANCELLED;
    case sonare::ErrorCode::EncodeFailed:
      return SONARE_ERROR_ENCODE_FAILED;
  }
  // A scoped enum can hold an out-of-range underlying value.
  return SONARE_ERROR_UNKNOWN;
}

/// Inverse of c_error_from_error_code(): lets facades that call the C ABI raise
/// the same core ErrorCode without maintaining a second switch table.
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
      return sonare::ErrorCode::NotImplemented;
    case SONARE_ERROR_INVALID_STATE:
      return sonare::ErrorCode::InvalidState;
    case SONARE_ERROR_CANCELLED:
      return sonare::ErrorCode::Cancelled;
    case SONARE_ERROR_ENCODE_FAILED:
      return sonare::ErrorCode::EncodeFailed;
    case SONARE_OK:
    case SONARE_ERROR_UNKNOWN:
    default:
      return sonare::ErrorCode::InvalidState;
  }
}

}  // namespace sonare_c_detail
