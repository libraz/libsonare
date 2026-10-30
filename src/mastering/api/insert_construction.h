#pragma once

/// @file insert_construction.h
/// @brief Internal: the single entry that builds an insert from a parameter map,
///        shared by the insert factory and the parameter catalog.

#include <memory>
#include <string>

#include "mastering/api/processor_params.h"
#include "rt/processor_base.h"
#include "util/json.h"

namespace sonare::mastering::api::detail {

/// @brief Builds the insert @p name from @p params, or nullptr for an unknown name.
/// @param json_root Admitted JSON document for keys that carry nested values, or nullptr.
/// @param sample_rate Rate the build is specialised for; 0 when rate-less.
std::unique_ptr<sonare::rt::ProcessorBase> build_insert(
    const std::string& name, const ParamMap& params,
    const sonare::util::json::Value* json_root = nullptr, double sample_rate = 0.0);

}  // namespace sonare::mastering::api::detail
