#pragma once

/// @file aliasing_control.h
/// @brief Shared nonlinear antialiasing mode selection.

namespace sonare::mastering::common {

enum class AliasingControl {
  None,
  Adaa1,
  Oversample4x,
};

}  // namespace sonare::mastering::common
