#pragma once

/// @file scene_delta_internal.h
/// @brief Internal SceneDelta ordering helpers.

#include <vector>

#include "mixing/assistant/scene_delta.h"

namespace sonare::mixing::assistant::detail {

/// @brief Returns pointers to @p deltas in stable application order.
/// @details Domains are ordered by their enum value while preserving the input
///          order within each domain.
std::vector<const SceneDelta*> ordered_deltas(const std::vector<SceneDelta>& deltas);

}  // namespace sonare::mixing::assistant::detail
