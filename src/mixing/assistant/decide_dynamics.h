#pragma once

/// @file decide_dynamics.h
/// @brief Dynamics-processing decisions for the mixing assistant.
///
/// @details **Offline / control thread only** — it allocates deltas, reason
///          strings and a JSON parameter object per insert.
///
/// @details The stage decides and returns, never instantiating a processor or
///          touching audio: each decision is a @ref mixing::api::Insert naming a
///          processor the mastering factory already knows, with parameters built
///          from the keys that processor reads. No new DSP is introduced.
///
/// @details Every parameter derives from the track's own measurements rather
///          than an absolute. A threshold is an offset from measured integrated
///          loudness, so the same part 10 dB quieter gets the same treatment;
///          timings follow the measured crest factor and sustain ratio, so a
///          spiky part and a sustained one get different envelopes.
///
/// @details Four decisions per track, in order: a compressor sized from the
///          class and dynamics profile, a class-specific tool (transient shaper,
///          or a rider and de-esser for a voice), a gate on the narrow set of
///          material where it cannot do harm, and a sidechain duck for
///          low-frequency pairs that measurably contend.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests dynamics inserts for every track that can be treated.
///
/// @details A track is treated only when it is usable, classified with enough
///          confidence to act on, and carrying a real integrated loudness. An
///          excluded track produces no delta rather than a neutral processor,
///          which would read downstream as "decided to need no dynamics" instead
///          of "never decided".
///
/// @details No strip receives the same processor name twice in one slot —
///          @ref apply_deltas drops a duplicate and reports it, turning a
///          decision into a note about a decision.
///
/// @details The sidechain pass indexes @p mix by track position, so it runs only
///          when @ref MixProfile::track_count matches @p profiles; a mismatched
///          pair describes different tracks and would key a duck off whichever
///          one landed at that index.
///
/// @details Degenerate input never throws: no tracks, all excluded, a disabled
///          domain, a wrong-sized mix profile or a non-finite configuration all
///          yield an empty vector.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param mix Cross-track measurements built from the same tracks in the same
///        order; only the low-band dominance entries are read.
/// @param config Assistant configuration; the loudness target, the suggestion
///        strength and the dynamics domain switch are read.
/// @return Deltas in @ref DeltaDomain::Dynamics, in track order, with the
///         sidechain decisions last.
std::vector<SceneDelta> decide_dynamics(const std::vector<TrackProfile>& profiles,
                                        const MixProfile& mix, const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
