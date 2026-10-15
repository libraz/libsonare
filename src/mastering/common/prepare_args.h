#pragma once

/// @file prepare_args.h
/// @brief Shared argument validation for prepare(sample_rate, max_block_size, max_channels).

#include <cstddef>
#include <string>

#include "mastering/dynamics/channel_limits.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering {

/**
 * @brief Validates the common prepare() rate and block-size arguments.
 */
inline void validate_prepare_args(double sample_rate, int max_block_size) {
  int rounded_sample_rate = 0;
  if (!numeric::checked_round_cast(sample_rate, &rounded_sample_rate) || rounded_sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must round to a positive int");
  }
  if (max_block_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_block_size must be non-negative");
  }
}

/**
 * @brief Validates prepare() arguments, throwing InvalidParameter on a bad value.
 * @param processor Class name used in the channel-capacity error message.
 */
inline void validate_prepare_args(double sample_rate, int max_block_size, int max_channels,
                                  const char* processor) {
  validate_prepare_args(sample_rate, max_block_size);
  if (max_channels < 1 || max_channels > static_cast<int>(dynamics::kRealtimePreparedChannels)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("max_channels exceeds ") + processor + " capacity");
  }
}

/**
 * @brief Refuses a process() block wider than the per-channel state prepare() allocated.
 * @details Called before any input or state is touched, so process() never grows,
 *   re-prepares or resets channel state on the audio thread.
 * @param prepared_channels Channel capacity sized by the last prepare().
 * @param processor Class name used in the error message.
 */
inline void require_prepared_channels(int num_channels, std::size_t prepared_channels,
                                      const char* processor) {
  if (num_channels < 0 || static_cast<std::size_t>(num_channels) > prepared_channels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("num_channels exceeds prepared ") + processor + " state");
  }
}

}  // namespace sonare::mastering
