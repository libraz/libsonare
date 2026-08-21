#pragma once

/// @file gain_staging.h
/// @brief Static input-trim staging for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe:
///          it allocates the returned deltas and builds their reason strings.
///          Never call it from `process()`.
///
/// @details The decision is a normalisation towards a stated absolute level,
///          the same operation broadcast and streaming delivery has performed
///          for years. The target is @ref MixAssistantConfig::target_track_lufs
///          and nothing else; it deliberately does not follow an average taken
///          over the tracks that happen to be loaded, which would couple every
///          track's staging to its neighbours and move all of them whenever one
///          is added or muted.
///
/// @details One track gets one static trim, never a time-varying gain. Riding
///          level over time is a different decision with a different tool.
///
/// @details The target is uniform across source classes: this stage never reads
///          @ref TrackProfile::source. A class-relative offset (a kick sitting
///          above a pad) is the balance pass's decision, and the two sum through
///          the additive @ref SceneDelta::input_trim_db field.

#include <vector>

#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests a static input trim per track so every usable track lands on
///        the configured absolute loudness target.
///
/// @details A track that cannot be staged produces no delta at all rather than
///          a delta carrying a zero trim. A zero would read downstream as "this
///          track was decided to be zero", which is the opposite of "this track
///          was never decided", and the balance pass would then be summing on
///          top of a decision nobody made. Excluded are tracks the profiler
///          marked unusable, silent tracks, tracks too short for a gated
///          integrated loudness, and tracks with no energy in any analysis
///          band. The exclusion is silent: this function returns suggestions,
///          not annotations.
///
/// @details The suggested trim is not clamped here. @ref apply_deltas clamps
///          the summed total exactly once, and clamping a contribution as well
///          would hide how far the total overshot. When a suggestion falls
///          outside `[kMinSuggestedTrimDb, kMaxSuggestedTrimDb]` the delta's
///          @ref SceneDelta::reason says so, because a trim that is quietly
///          truncated looks like staging that simply did not work.
///
/// @details Degenerate input never throws. No tracks, all-excluded tracks, a
///          disabled gain domain or a non-finite target all yield an empty
///          vector.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param config Assistant configuration; the target, the strength and the gain
///        domain switch are read.
/// @return One delta per staged track, in input order.
std::vector<SceneDelta> decide_gain_staging(const std::vector<TrackProfile>& profiles,
                                            const MixAssistantConfig& config);

/// @brief Master-bus trim that keeps the summed mix inside its headroom target.
///
/// @details Staging every track to the same absolute target makes each one
///          correct on its own and the sum of them hot: N tracks at the target
///          add up well above it. Someone has to pull the master down, and doing
///          it once on the output is what a console operator does — it leaves
///          every track's staging and every balance decision exactly as decided.
///
/// @details The estimate is analytic, because the assistant does not process
///          audio and therefore cannot measure the render. Each strip's peak
///          contribution is its measured true peak shifted by the gain the scene
///          gives it, and those contributions are added as amplitudes rather
///          than powers: the sum of true peaks is a genuine upper bound on the
///          true peak of the sum, whereas a power sum is only correct for
///          uncorrelated material and a kick and a bass are not uncorrelated.
///
/// @details Two contributions are not modelled: effect returns, whose level
///          depends on a reverb's own gain, and insert make-up gain. The
///          headroom target absorbs them — it is set well below full scale
///          precisely so the unmodelled part has somewhere to go.
///
/// @details The result is never positive. Raising a master that already fits is
///          a creative decision, and this stage only guarantees the mix has room.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param scene The scene after every domain's deltas have been applied; the
///        per-strip trim and fader it carries are what the estimate is built on.
/// @param config Assistant configuration; the headroom target is read.
/// @return Trim in dB to apply to the master bus, at most 0.
float decide_master_headroom_db(const std::vector<TrackProfile>& profiles, const api::Scene& scene,
                                const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
