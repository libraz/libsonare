#pragma once

/// @file sonare_c_error_mapping.h
/// @brief Shared C-ABI-to-core error mapping for non-C-ABI facades.

#include <sonare/sonare_c.h>

#include "util/types.h"

namespace sonare_c_detail {

/// Inverse of map_sonare_exception(): lets facades that call the C ABI raise
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
