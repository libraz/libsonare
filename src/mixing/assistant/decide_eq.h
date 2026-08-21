#pragma once

/// @file decide_eq.h
/// @brief Static corrective EQ suggestions for the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe:
///          it walks every track pair, allocates the returned deltas and builds
///          their reason strings and params JSON. Never call it from
///          `process()`.
///
/// @details **One static band set per track, decided once for the whole song.**
///          The suggestion is a fixed set of peaking cuts, plus at most one
///          high-pass corner, and nothing in it follows the material over time. A
///          correction that tracks the mix moment to moment is a different tool
///          with a different failure mode — it changes the part's tone whenever
///          another part happens to enter — and the assistant deliberately does
///          not reach for one. The dynamic and multiband-dynamic equalizers the
///          library ships are available to a user who wants that; they are
///          never suggested here.
///
/// @details **Cuts are bounded.** Over-carving is the standing complaint against
///          automatic EQ, so every peaking cut is clamped to
///          @ref MixAssistantConfig::eq_max_cut_db, and a suggestion that hit
///          the ceiling says so in its @ref SceneDelta::reason rather than
///          quietly reporting the truncated figure as the decision. That
///          ceiling is the only knob this stage exposes; everything else is
///          fixed by the module so the suggestion stays predictable.
///
/// @details The realisation is an insert chain, not a channel-strip setting.
///          The scene schema has no built-in strip EQ, and widening it for one
///          decision stage would put a second EQ implementation in front of
///          every mixer surface.

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
///          - **Peaking cuts.** A band where one track's measured energy share
///            over another passes the module's interference threshold, for long
///            enough to be a standing conflict rather than a coincidence, is a
///            collision. The track that gives way is the one with the lower
///            role priority — *not* the one that happens to be quieter there.
///            Energy dominance says who is loud; it does not say who the band
///            belongs to, and carving whichever track lost the energy contest
///            is how an automatic EQ ends up thinning the lead. When two tracks
///            share a priority the band goes to whichever of them is more
///            invested in it, measured as @ref TrackProfile::band_occupancy.
///
///            Where inside the band the filter sits is a **second, narrower
///            measurement**, taken only for the band that already won and only
///            between the two tracks whose collision won it. A band runs up to
///            two octaves, so its geometric centre can sit an octave away from
///            the overlap the cut was justified by. The peak of the two tracks'
///            shared spectrum inside that span is where the filter goes, and the
///            band's centre remains the answer whenever nothing measurable is
///            there. This is a targeted second stage, not a second band grid:
///            it reads @ref TrackProfile::spectrum, keeps no state, and leaves
///            the seven-band figures every other stage compares against alone.
///
///          - **A high-pass corner**, only when
///            @ref MixAssistantConfig::enable_high_pass asks for one. Where to
///            look is set per source class, from the lowest fundamental that
///            class ordinarily produces; kick and bass have no corner at all,
///            being what the low end is made of. Whether to act is measured, not
///            assumed: the filter is proposed only when enough of the track's
///            energy sits below its corner to be worth removing and little
///            enough to read as residue rather than as the part's own material.
///            A track playing under its class's usual register therefore keeps
///            what it plays. The measured share is quoted in the
///            @ref SceneDelta::reason.
///
///          Every peaking cut a track earns lands in **one** `eq.parametric`
///          insert with contiguously numbered bands. A strip cannot hold two
///          copies of the same processor in the same slot — the second is
///          dropped on application — so a second insert would silently discard
///          a decision.
///
/// @details Tracks the profiler marked unusable, tracks that came back
///          @ref SourceClass::Unknown, and tracks whose classification
///          confidence is too low get no suggestion at all, and are also
///          ignored as the other side of a collision. Carving a track requires
///          knowing what it is for: role priority is the whole basis of the
///          decision, and applied to a guess it removes real material from a
///          part on the strength of a label nobody stands behind. An excluded
///          track produces no delta rather than a delta carrying a zero cut,
///          which would read downstream as a decision to leave it flat.
///
/// @details Degenerate input never throws. No tracks, one track, a disabled EQ
///          domain, an empty mix profile or a zero cut ceiling all yield either
///          an empty vector or the high-pass suggestions alone. A profile
///          carrying no spectrum still earns its cuts; they land at the band
///          centres, because the measurement that would have moved them off the
///          grid is the part that is missing.
///
/// @param profiles Per-track profiles, in the caller's order.
///        @ref TrackProfile::spectrum is read for the high-pass decision and for
///        each cut's centre frequency; neither reading is required for a
///        suggestion to be made.
/// @param mix Cross-track measurements; only the band dominance matrix is read.
/// @param config Assistant configuration; the cut ceiling, the strength, the EQ
///        domain switch and the high-pass switch are read.
/// @return Deltas in @ref DeltaDomain::Eq, grouped by track in input order,
///         with a track's high-pass preceding its peaking cuts.
std::vector<SceneDelta> decide_eq(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                                  const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
