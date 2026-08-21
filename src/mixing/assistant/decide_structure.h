#pragma once

/// @file decide_structure.h
/// @brief Bus, VCA and effect-send topology for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe:
///          it allocates the returned deltas and builds their reason strings and
///          params JSON. Never call it from `process()`.
///
/// @details **This stage runs first and every other domain assumes its output.**
///          Buses, sends and connections exist before anything routes to them,
///          so the topology has to be settled before a level, an EQ curve or a
///          pan position means anything. A mix with no master bus renders
///          silence, so the master bus is emitted unconditionally.
///
/// @details **The bus layout is a plain map from @ref SourceClass, nothing
///          more.** No masking figure, no spectral overlap and no cross-track
///          measurement takes part in the decision: which subgroup a track
///          belongs to is a question about what the part *is*, and re-deciding
///          it from how the parts happen to interfere would move a guitar out of
///          the guitar bus because a keyboard was loud that day. @ref MixProfile
///          is therefore accepted for signature symmetry with the other domains
///          and deliberately not read.
///
/// @details **Realisation follows the `vocalReverbSend` scene preset.** An
///          effect bus is an aux bus, a return strip carrying the effect insert,
///          a bus-to-return connection, a return-to-master connection, and one
///          send per contributing strip. The effect processor lives on the
///          return strip rather than on the bus so the send level alone controls
///          how much of the part is treated.
///
/// @details **Effect buses need the optional FX suite.** The reverb and delay
///          processors are compiled only when the library is built with FX
///          support, and a scene naming an insert the factory cannot build fails
///          to load rather than degrading. Without FX the stage emits the
///          subgroup topology and no effect bus at all.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests the mix's bus topology, VCA grouping and effect sends.
///
/// @details Four decisions, in this order:
///
///          - **The master bus.** Always emitted, because every other edge ends
///            there.
///
///          - **Subgroup buses.** A subgroup is created only where at least
///            @ref kMinTracksPerSubgroup tracks map to it. One track behind a
///            subgroup fader is a second fader for the same part, which adds a
///            place for gain to hide and buys nothing.
///
///          - **VCA groups.** One per subgroup that was created, at unity, so a
///            section can be ridden without moving the summing path the inserts
///            and sends hang off.
///
///          - **Effect buses.** A reverb and a stereo delay, each created only
///            when at least one track is classified into a class the send table
///            actually feeds.
///
///          Every strip ends with a path to master: a track that joined a
///          subgroup routes through it, and every other track — including one
///          the profiler marked unusable and one that came back
///          @ref SourceClass::Unknown — is connected straight to master. An
///          unrouted strip is silent, and silently dropping a part the
///          classifier could not read is the one failure a mixer would not think
///          to look for.
///
/// @details Generated bus, VCA and return-strip identifiers are checked against
///          the input strip ids and against each other. The mixer builds strips
///          and buses as nodes in one graph namespace, so a bus sharing an id
///          with a track is a load failure rather than a cosmetic clash; a
///          colliding identifier takes an ascending numeric suffix.
///
/// @details @ref MixAssistantConfig::suggestion_strength scales the effect send
///          levels; at zero no effect bus is proposed at all, since a bus nobody
///          sends to is dead weight. The subgroup topology does not scale with
///          it — routing is not a level.
///
/// @details Degenerate input never throws. No tracks, all-unusable tracks or a
///          disabled structure domain yield either an empty vector or the master
///          bus alone.
///
/// @param profiles Per-track profiles, in the caller's order.
/// @param mix Cross-track measurements. Accepted for symmetry with the other
///        decision domains and not read; see the file-level note.
/// @param config Assistant configuration; the strength and the structure domain
///        switch are read.
/// @return Deltas in @ref DeltaDomain::Structure, in a fixed order: master bus,
///         subgroups in bus-table order, unbussed strips in input order, then
///         the effect buses.
std::vector<SceneDelta> decide_structure(const std::vector<TrackProfile>& profiles,
                                         const MixProfile& mix, const MixAssistantConfig& config);

/// @brief Fewest tracks that justify a subgroup bus.
/// @details A subgroup exists to move several parts together. Below this the
///          group fader duplicates the one fader the part already has.
inline constexpr int kMinTracksPerSubgroup = 2;

}  // namespace sonare::mixing::assistant
