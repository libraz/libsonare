#pragma once

/// @file aliasing_control.h
/// @brief Shared nonlinear antialiasing mode selection.

namespace sonare::rt {

enum class AliasingControl {
  None,
  Adaa1,
  Adaa2,
  Oversample4x,
};

}  // namespace sonare::rt
