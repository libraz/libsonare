#pragma once

/// @file decide_dynamics.h
/// @brief Dynamics-processing decisions for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe: it
///          allocates the returned deltas, builds their reason strings and
///          serialises a JSON parameter object per insert. Never call it from
///          `process()`.
///
/// @details The stage decides and returns; it never instantiates a processor and
///          never touches audio. Each decision arrives as a
///          @ref mixing::api::Insert carrying a processor name the mastering
///          insert factory already knows and a parameter object built from the
///          keys that processor actually reads. No new DSP is introduced.
///
/// @details Every parameter is derived from the track's own measurements rather
///          than written down as an absolute. A threshold is an offset from the
///          measured integrated loudness, so the same part rendered 10 dB
///          quieter gets the same treatment; a timing follows the measured crest
///          factor and sustain ratio, so a spiky part and a sustained one are not
///          handed the same envelope.
///
/// @details Four kinds of decision are made, in this order per track:
///          a compressor sized from the source class and the dynamics profile,
///          a class-specific tool (a transient shaper for a percussive part, a
///          level rider and a de-esser for a voice), a gate on the narrow set of
///          material where a gate cannot do harm, and finally a sidechain duck
///          for the low-frequency pairs that measurably contend for the same
///          band.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests dynamics inserts for every track that can be treated.
///
/// @details A track is treated only when the profiler marked it usable, the
///          classifier resolved it to something other than
///          @ref SourceClass::Unknown with enough confidence to act on, and its
///          integrated loudness is a real level. An excluded track produces no
///          delta at all rather than a delta carrying a neutral processor: an
///          inert insert would read downstream as "this track was decided to
///          need no dynamics", which is the opposite of "this track was never
///          decided".
///
/// @details No strip ever receives the same processor name twice in the same
///          slot. @ref apply_deltas drops a duplicate insert and reports it, so
///          emitting one would turn a decision into a note about a decision.
///
/// @details The sidechain pass reads @p mix by track index, so it runs only when
///          @ref MixProfile::track_count matches @p profiles. A mismatched pair
///          describes a different set of tracks, and indexing one with the
///          other's positions would key a duck off whichever track happened to
///          land at that index.
///
/// @details Degenerate input never throws. No tracks, all-excluded tracks, a
///          disabled dynamics domain, a mix profile of the wrong size or a
///          non-finite configuration all yield an empty vector.
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
