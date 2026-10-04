#pragma once

/// @file prepare_args.h
/// @brief Shared argument validation for prepare(sample_rate, max_block_size, max_channels).

#include <string>

#include "mastering/dynamics/channel_limits.h"
#include "util/exception.h"

namespace sonare::mastering {

/**
 * @brief Validates prepare() arguments, throwing InvalidParameter on a bad value.
 * @param processor Class name used in the channel-capacity error message.
 */
inline void validate_prepare_args(double sample_rate, int max_block_size, int max_channels,
                                  const char* processor) {
  if (!(sample_rate > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("max_channels exceeds ") + processor + " capacity");
  }
}

}  // namespace sonare::mastering
