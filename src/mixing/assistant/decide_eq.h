#pragma once

/// @file decide_eq.h
/// @brief Static corrective EQ suggestions for the mixing assistant.
///
/// @details **Offline / control thread only** — it walks every track pair and
///          allocates deltas, reason strings and params JSON.
///
/// @details **One static band set per track, decided once for the whole song**:
///          fixed peaking cuts plus at most one high-pass corner, never anything
///          that follows the material over time. A correction that tracks the mix
///          changes a part's tone whenever another part enters; the library's
///          dynamic equalizers serve a user who wants that, but are never
///          suggested here.
///
/// @details Every cut is clamped to @ref MixAssistantConfig::eq_max_cut_db —
///          the only knob this stage exposes — and a suggestion that hit the
///          ceiling says so in its @ref SceneDelta::reason. The realisation is an
///          insert chain because the scene schema has no strip EQ, and widening
///          it here would put a second EQ implementation in front of every mixer
///          surface.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests corrective EQ inserts that unpick band collisions and tidy
///        the low end.
///
/// @details Two decisions, both static:
///
///          - **Peaking cuts**, where a band's energy dominance is a standing
///            conflict rather than a coincidence. The track that gives way is the
///            lower role priority, *not* the quieter one — energy says who is
///            loud, not who the band belongs to, and carving the loser of the
///            energy contest is how an automatic EQ thins the lead. Equal
///            priorities go to the higher @ref TrackProfile::band_occupancy.
///            A collision alone does not justify a cut: the band must be
///            essential to the part being made room for and non-essential to the
///            part giving way, both quoted in the @ref SceneDelta::reason.
///            Neither-clearly produces no cut, which is a real answer.
///            The filter's frequency inside the band is a second, narrower
///            measurement over the two tracks' shared spectrum — a band runs up
///            to two octaves, so its centre can sit an octave off the overlap —
///            falling back to the band centre when nothing measurable is there.
///
///          - **A high-pass corner**, only under
///            @ref MixAssistantConfig::enable_high_pass. The corner is per source
///            class, from the lowest fundamental that class produces; kick and
///            bass get none. It is proposed only when the energy below the corner
///            is large enough to be worth removing and small enough to read as
///            residue, so a track playing under its class's register keeps what
///            it plays.
///
///          A track's cuts all land in **one** `eq.parametric` insert: a strip
///          drops a second copy of the same processor in a slot, so a second
///          insert would silently discard a decision.
///
/// @details Unusable, @ref SourceClass::Unknown and low-confidence tracks get no
///          suggestion and are ignored as the other side of a collision — role
///          priority is the whole basis of the decision, and applied to a guess
///          it carves real material on the strength of a label nobody stands
///          behind. They produce no delta rather than a zero cut, which would
///          read downstream as a decision to leave the track flat.
///
/// @details Degenerate input never throws — no tracks, one track, a disabled EQ
///          domain, an empty mix profile or a zero ceiling yield an empty vector
///          or the high-pass suggestions alone.
///
/// @details **A profile carrying no spectrum degrades two ways**: its cuts still
///          appear at the band centres, but its high-pass does not appear at all,
///          because the unmeasured share below the corner reads as zero and falls
///          under the floor. A hand-assembled @ref TrackProfile therefore gets
///          coarser cuts and no high-pass whatever the switch says.
///
/// @param profiles Per-track profiles, in the caller's order.
///        @ref TrackProfile::spectrum is read for the high-pass decision and for
///        each cut's centre frequency. The cuts survive its absence; the
///        high-pass does not.
/// @param mix Cross-track measurements; only the band dominance matrix is read.
/// @param config Assistant configuration; the cut ceiling, the strength, the EQ
///        domain switch and the high-pass switch are read.
/// @return Deltas in @ref DeltaDomain::Eq, grouped by track in input order,
///         with a track's high-pass preceding its peaking cuts.
std::vector<SceneDelta> decide_eq(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                                  const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
