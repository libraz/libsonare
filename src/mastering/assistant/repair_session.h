#pragma once

/// @file repair_session.h
/// @brief One analysis and one application of the six repair stages over any channel count.
/// @details Analysis runs the profile's defect detectors on every channel, aggregates them the way
///          the interleaved profile does and asks the assistant's stage selection
///          (@ref select_repair_stages) what to enable, so its policy is the suggester's: it never
///          recommends dereverb. Application runs a caller's stage list in the chain's fixed order
///          (declip, declick, decrackle, dehum, denoise, dereverb) whatever order the list gives.
///          Declip, declick and dehum decide over the whole channel set and report per channel,
///          decrackle runs each channel alone, and denoise and dereverb apply one linked mask and
///          report once for the set. Both entries speak JSON so every surface shares one parser;
///          a stage's settings are its catalog parameters (`repair.declip`, `repair.declick`,
///          `repair.decrackle`, `repair.dehum`, `repair.denoiseClassical`,
///          `repair.dereverbClassical`), enums by choice name or wire value.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace sonare::mastering::assistant {

/// @brief Called after each applied stage with the fraction done and `repair.<stage>`.
using RepairProgressCallback = std::function<void(float progress, const char* stage)>;

/// @brief Polled after each progress report; returning true stops the application.
using RepairCancelCallback = std::function<bool()>;

/// @brief Measures every channel's repair defects and recommends stages.
/// @param channels @p channel_count planes of @p length samples each; read only.
/// @param request_json An object, or empty for `{}`. Key: `preferStreamingSafe` (boolean,
///        default true), which makes a recommended denoise track its noise frame by frame.
/// @return `{ defects, channels, declipThresholdSafe, integratedLufs, recommended, explanation }`:
///         the aggregate and per-channel defect blocks in the profile's shape, whether declip at
///         the flat level leaves louder audio alone, the programme loudness the noise rule compares
///         against, the recommended stages in application order as `{ stage, ...settings }`
///         objects carrying every setting, and why each stage was or was not chosen.
/// @throws SonareException(InvalidParameter) for a bad channel set or request.
std::string repair_analyze_json(const float* const* channels, std::size_t channel_count,
                                std::size_t length, int sample_rate,
                                const std::string& request_json);

/// @brief Applies a stage list to every channel.
/// @param stages_json An array of `{ stage, ...settings }` objects; `stage` is one of `declip`,
///        `declick`, `decrackle`, `dehum`, `denoise`, `dereverb`. A stage named twice, an unknown
///        stage or an unknown setting is refused.
/// @param out_channels @p channel_count caller-owned planes of @p length samples; written only
///        when the call completes, so they may alias @p channels.
/// @param reports_json Receives `[{ stage, scope, reports }]` in application order: `scope` is
///        `"channel"` with one report per channel, or `"linked"` with exactly one for the set.
/// @return false when @p cancel stopped the application; nothing was written then.
/// @throws SonareException(InvalidParameter) for a bad channel set or stage list.
bool repair_apply_json(const float* const* channels, std::size_t channel_count, std::size_t length,
                       int sample_rate, const std::string& stages_json, float* const* out_channels,
                       std::string* reports_json, const RepairProgressCallback& progress = {},
                       const RepairCancelCallback& cancel = {});

/// @brief Every dotted field path @ref repair_analyze_json emits, `recommended` left opaque
///        below its `stage` key because the settings depend on the stage.
const std::vector<std::string>& repair_analysis_schema_paths();

}  // namespace sonare::mastering::assistant
