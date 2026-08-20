#pragma once

/// @file validated.h
/// @brief Construction-time validation wrapper for public configuration structs.

#include <utility>

namespace sonare {

/// @brief A configuration value that provably passed its validation rules.
/// @details The constructor is private, so the only way to obtain a
///          @ref Validated is @ref make, which runs the `validate_config`
///          overload declared next to @p Config (found by argument-dependent
///          lookup) and lets it throw SonareException(InvalidParameter) for a
///          rejected value. Every consumer that needs a checked configuration
///          takes its value from here, so a binding surface that builds the
///          config itself inherits the checks instead of hand-copying them —
///          and a config type with no `validate_config` overload fails to
///          compile rather than passing through unchecked.
/// @tparam Config Plain configuration struct, kept an aggregate so call sites
///         can still assign fields before handing the value over.
template <typename Config>
class Validated {
 public:
  /// @brief Validates @p config and wraps it.
  /// @throws SonareException(InvalidParameter) when @p config is rejected.
  static Validated make(const Config& config) {
    validate_config(config);
    return Validated(config);
  }

  /// @brief Returns the validated configuration.
  const Config& get() const noexcept { return config_; }

  /// @brief Member access on the validated configuration.
  const Config* operator->() const noexcept { return &config_; }

 private:
  explicit Validated(const Config& config) : config_(config) {}

  Config config_;
};

}  // namespace sonare
