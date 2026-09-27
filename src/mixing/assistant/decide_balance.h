#pragma once

/// @file decide_balance.h
/// @brief Source-class relative level balance for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe:
///          it allocates the returned deltas and builds their reason strings.
///          Never call it from `process()`.
///
/// @details This stage sits *on top of* @ref decide_gain_staging rather than
///          beside it. Staging pulls every usable track onto one absolute
///          loudness target, which by itself produces a mix where a hi-hat is as
///          loud as the lead vocal. The balance pass adds the class-relative
///          offset that a mixer would dial in afterwards: forward elements sit
///          above the staged reference, bed elements below it.
///
/// @details The two decisions stay in separate fields. Staging owns
///          @ref SceneDelta::input_trim_db and this stage owns
///          @ref SceneDelta::fader_db, which mirrors console practice — level is
///          staged at the input, balance is ridden on the fader — and keeps
///          either decision readable in the resulting scene. Both fields are
///          additive, so neither erases the other, and @ref apply_deltas clamps
///          each summed total exactly once.
///
/// @details The offsets come from one general table, read the same way for
///          every track.

#include <cstddef>
#include <vector>

#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests a class-relative fader offset per track.
///
/// @details A track is skipped, rather than given a zero offset, when it is
///          unusable, when it was never classified, or when the classification
///          is too weak to act on. An offset applied to a misread class moves
///          the wrong fader, which is worse than leaving the track where staging
///          put it; a zero would additionally read downstream as a decision that
///          the track belongs at the reference.
///
/// @details The offset is not clamped here. @ref apply_deltas clamps the summed
///          fader once, and clamping a contribution as well would hide how far
///          the total overshot.
///
/// @details Degenerate input never throws. No tracks, all-excluded tracks or a
///          disabled balance domain all yield an empty vector.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param config Assistant configuration; the strength and the balance domain
///        switch are read.
/// @return One delta per balanced track, in input order.
std::vector<SceneDelta> decide_balance(const std::vector<TrackProfile>& profiles,
                                       const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
