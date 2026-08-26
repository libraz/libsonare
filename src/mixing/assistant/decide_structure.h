#pragma once

/// @file decide_structure.h
/// @brief Bus, VCA and effect-send topology for the mixing assistant.
///
/// @details **Offline / control thread only** — it allocates deltas, reason
///          strings and params JSON.
///
/// @details **This stage runs first and every other domain assumes its output**,
///          since a level, an EQ curve or a pan position means nothing until
///          there is somewhere to route it. The master bus is emitted
///          unconditionally, because a mix without one renders silence.
///
/// @details **The bus layout is a plain map from @ref SourceClass.** Which
///          subgroup a track belongs to is a question about what the part is, and
///          re-deciding it from interference would move a guitar out of the
///          guitar bus because a keyboard was loud that day. @ref MixProfile is
///          accepted for signature symmetry and deliberately not read.
///
/// @details **Realisation follows the `vocalReverbSend` scene preset**: an aux
///          bus, a return strip carrying the effect insert, two connections and
///          one send per contributing strip. The processor sits on the return so
///          the send level alone controls how much of the part is treated. This
///          needs the optional FX suite — without it the stage emits the subgroup
///          topology and no effect bus, since a scene naming an insert the
///          factory cannot build fails to load rather than degrading.

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
///          - **The master bus**, always, because every other edge ends there.
///          - **Subgroup buses**, only where at least @ref kMinTracksPerSubgroup
///            tracks map to one — a single track behind a subgroup fader is just
///            a second fader for the same part.
///          - **VCA groups**, one per created subgroup at unity, so a section can
///            be ridden without moving the summing path.
///          - **Effect buses** — a reverb and a stereo delay — each only when a
///            track is classified into a class the send table feeds.
///
///          Every strip ends with a path to master: unusable and
///          @ref SourceClass::Unknown tracks connect straight there rather than
///          being left unrouted, since a silently dropped part is the one failure
///          a mixer would not think to look for.
///
/// @details Generated bus, VCA and return-strip ids are checked against the input
///          strip ids and each other — strips and buses share one graph
///          namespace, so a collision is a load failure rather than a cosmetic
///          clash — and a colliding id takes an ascending numeric suffix.
///
/// @details @ref MixAssistantConfig::suggestion_strength scales the send levels,
///          and at zero no effect bus is proposed at all. The subgroup topology
///          does not scale with it: routing is not a level.
///
/// @details Degenerate input never throws — no tracks, all-unusable tracks or a
///          disabled domain yield an empty vector or the master bus alone.
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
