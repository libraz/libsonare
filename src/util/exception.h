#pragma once

/// @file exception.h
/// @brief Exception classes for libsonare.

#include <sstream>
#include <stdexcept>
#include <string>

#include "util/types.h"

namespace sonare {

/// @brief Base exception class for libsonare errors.
class SonareException : public std::runtime_error {
 public:
  /// @brief Constructs exception with error code.
  /// @param code Error code
  explicit SonareException(ErrorCode code) : std::runtime_error(error_message(code)), code_(code) {}

  /// @brief Constructs exception with error code and custom message.
  /// @param code Error code
  /// @param message Custom error message
  SonareException(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  /// @brief Returns the error code.
  ErrorCode code() const { return code_; }

 private:
  ErrorCode code_;
};

/// @def SONARE_CHECK
/// @brief Throws SonareException if condition is false.
#define SONARE_CHECK(cond, code)   \
  do {                             \
    if (!(cond)) {                 \
      throw SonareException(code); \
    }                              \
  } while (0)

/// @def SONARE_CHECK_MSG
/// @brief Throws SonareException with custom message if condition is false.
#define SONARE_CHECK_MSG(cond, code, msg) \
  do {                                    \
    if (!(cond)) {                        \
      throw SonareException(code, msg);   \
    }                                     \
  } while (0)

/// @def SONARE_CHECK_RANGE
/// @brief Throws InvalidParameter with a detailed inclusive range message.
#define SONARE_CHECK_RANGE(name, value, min_value, max_value)                                  \
  do {                                                                                         \
    const auto sonare_check_range_value = (value);                                             \
    const auto sonare_check_range_min = (min_value);                                           \
    const auto sonare_check_range_max = (max_value);                                           \
    if (!(sonare_check_range_value >= sonare_check_range_min &&                                \
          sonare_check_range_value <= sonare_check_range_max)) {                               \
      std::ostringstream sonare_check_range_stream;                                            \
      sonare_check_range_stream << (name) << " must be in [" << sonare_check_range_min << ", " \
                                << sonare_check_range_max << "], got "                         \
                                << sonare_check_range_value;                                   \
      throw SonareException(ErrorCode::InvalidParameter, sonare_check_range_stream.str());     \
    }                                                                                          \
  } while (0)

}  // namespace sonare
