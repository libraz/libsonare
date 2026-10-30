#pragma once

/// @file engine_field_units.h
/// @brief The unit of each engine-section field, keyed by its un-indexed path.

#include <string>

#include "mastering/api/param_meta.h"

namespace sonare::midi::synth::patch_tuning_detail {

/// Looks up the unit of the engine-section field at @p member_path; false when it has no row.
bool find_unit(const std::string& member_path, mastering::api::detail::Unit* unit);

}  // namespace sonare::midi::synth::patch_tuning_detail
