#pragma once

/// @file decide_image.h
/// @brief Stereo placement, time/polarity alignment and mono-fold decisions for
///        the mixing assistant.
///
/// @details **Offline / control thread only** — it allocates deltas, reason
///          strings and insert parameter JSON.
///
/// @details **Position comes from the source class, never from the spectrum.**
///          A measurement can say a band is crowded; it cannot say whether a
///          guitar belongs left or right, and mapping frequency content onto pan
///          would move a part every time its tone changed. @ref ImageOccupancy is
///          therefore secondary: it widens or narrows the spread of a class with
///          several members, but never selects a side and never places a lone
///          track. Crowding changes a placement's magnitude, not its sign.
///
/// @details Placement is aesthetic and scales with
///          @ref MixAssistantConfig::suggestion_strength, so strength 0 leaves
///          every track centred. Polarity, alignment delay and the low-end mono
///          fold are physical corrections for a measured cancellation and are
///          emitted at full value whatever the strength.
///
/// @details @ref DeltaDomain::Image applies last, so its last-writer-wins fields
///          override the earlier domains' spatial suggestions.

#include <vector>

#include "mixing/assistant/mix_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Suggests pan, width, polarity and alignment delay for every track.
///
/// @details Four independent decisions, emitted in a fixed order so the same
///          input always yields the same vector:
///          1. **Placement.** One pan delta per classified track, in input
///             order. A centred class is placed at 0 explicitly, since this
///             domain applies last and an earlier domain's pan would otherwise
///             survive as a decision nobody made here. A spreadable class
///             alternates left, right, left, … stepping inwards every pair.
///          2. **Polarity.** A polarity-opposed pair inverts the **target**,
///             never the reference — @ref PairAlignment::reference_index is
///             always the lower of the two, so no track is flipped twice.
///          3. **Delay.** A non-zero @ref PairAlignment::lag_samples delays
///             whichever side arrives first. A pair already in time gets no
///             delta rather than one carrying zero samples.
///          4. **Mono fold.** A track @ref analyze_mono_risks flagged has its
///             width pulled in, and a wide low end additionally gets a
///             `stereo.monoMaker` insert at a fixed crossover.
///
/// @details **One delay and one polarity decision per track**, since a track
///          appears in as many pairs as it has neighbours and these are
///          last-writer-wins fields. The largest `|correlation|` wins; ties keep
///          the earlier entry in @ref MixProfile::alignment.
///
/// @details **Alignment does not depend on classification** — a cancellation is
///          measured, so @ref SourceClass::Unknown and low-confidence tracks
///          still get polarity and delay. Placement reads nothing but the class,
///          so those same tracks are left where they are. Tracks with
///          @ref TrackProfile::usable false are never touched at all.
///
/// @details Degenerate input never throws: no tracks, one track, a disabled
///          domain, an empty crowding vector or an out-of-range pair yield an
///          empty vector or skip the affected decision.
///
/// @param profiles Per-track profiles, in the caller's order. That order is the
///        spread order, so it must be stable across calls for the suggestion to
///        be reproducible.
/// @param mix Cross-track measurements. The alignment matrix, the image
///        occupancy and the mono risks are read.
/// @param config Assistant configuration; the strength and the image domain
///        switch are read.
/// @return Deltas in the fixed order above. Every entry carries
///         @ref DeltaDomain::Image.
std::vector<SceneDelta> decide_image(const std::vector<TrackProfile>& profiles,
                                     const MixProfile& mix, const MixAssistantConfig& config);

}  // namespace sonare::mixing::assistant
