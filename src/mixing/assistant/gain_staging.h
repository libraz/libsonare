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
/// @details A track that cannot be staged produces no delta rather than a zero
///          trim, which would read downstream as "decided to be zero" and leave
///          the balance pass summing on top of a decision nobody made. Excluded:
///          unusable, silent, too short for a gated loudness, or with no energy
///          in any band. The exclusion is silent — this returns suggestions, not
///          annotations.
///
/// @details The trim is not clamped here. @ref apply_deltas clamps the summed
///          total exactly once, and clamping a contribution too would hide how
///          far the total overshot; a suggestion outside
///          `[kMinSuggestedTrimDb, kMaxSuggestedTrimDb]` says so in its
///          @ref SceneDelta::reason instead.
///
/// @details Degenerate input never throws: no tracks, all excluded, a disabled
///          domain or a non-finite target yield an empty vector.
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
///          correct alone and their sum hot, so someone has to pull the master
///          down. Doing it once on the output leaves every track's staging and
///          every balance decision as decided.
///
/// @details The estimate is analytic, since the assistant never processes audio:
///          each strip contributes its measured true peak shifted by the gain the
///          scene gives it, summed as AMPLITUDES. That sum is a genuine upper
///          bound, where a power sum would assume material a kick and a bass are
///          not — uncorrelated.
///
/// @details Effect returns and insert make-up gain are not modelled; the
///          headroom target sits well below full scale so they have somewhere to
///          go. The result is never positive: raising a master that already fits
///          is a creative decision, and this only guarantees room.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param scene The scene after every domain's deltas have been applied; the
///        per-strip trim and fader it carries are what the estimate is built on.
/// @param config Assistant configuration; the headroom target is read.
/// @return Trim in dB to apply to the master bus, at most 0.
float decide_master_headroom_db(const std::vector<TrackProfile>& profiles, const api::Scene& scene,
                                const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
