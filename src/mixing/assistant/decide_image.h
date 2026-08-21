#pragma once

/// @file decide_image.h
/// @brief Stereo placement, time/polarity alignment and mono-fold decisions for
///        the mixing assistant.
///
/// @details **Offline / control thread only.** Nothing here is realtime-safe: it
///          allocates the returned deltas, builds their reason strings and
///          serialises an insert's parameters to JSON. Never call it from
///          `process()`.
///
/// @details **Position comes from the source class, never from the spectrum.**
///          Which side of the image a part belongs on is a recording-practice
///          convention — a kick, a bass, a snare and the lead voice sit at the
///          centre, everything else is spread around them — and that convention
///          is what this stage encodes. A spectral measurement can say a band is
///          crowded; it cannot say whether a guitar belongs left or right, and a
///          stage that mapped frequency content onto pan position would move a
///          part every time its tone changed.
///
/// @details @ref ImageOccupancy is therefore a **secondary** input. It adjusts
///          how far out the members of a class are spread when there is more
///          than one of them, and it never selects a side and never places a
///          lone track. The magnitude of a placement can change with the mix's
///          crowding; its sign cannot.
///
/// @details Placement, polarity and delay are separated by what they correct.
///          Placement is an aesthetic decision scaled by
///          @ref MixAssistantConfig::suggestion_strength, so a strength of 0
///          leaves every track at the centre. Polarity inversion, alignment
///          delay and the low-end mono fold are physical corrections for a
///          measured cancellation, so they are emitted at full value whatever
///          the strength: a cancellation is not half-wrong at half strength.
///
/// @details The @ref DeltaDomain::Image domain applies last, so its
///          last-writer-wins fields have the final say over the earlier domains'
///          spatial suggestions.

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
///             order. A centred class is placed at 0 explicitly rather than left
///             alone, because this domain applies last and an earlier domain's
///             pan would otherwise survive as a decision nobody made here.
///             Members of a spreadable class alternate left, right, left, …
///             stepping inwards every pair, ordered by their position in
///             @p profiles and nothing else.
///          2. **Polarity.** A pair the alignment pass found polarity-opposed
///             inverts the **target**, never the reference:
///             @ref PairAlignment::reference_index is always the lower of the
///             two, so inverting the target keeps the whole matrix consistent
///             and never flips one track twice.
///          3. **Delay.** A non-zero @ref PairAlignment::lag_samples delays
///             whichever side arrives first, following the sign convention
///             documented on that field. A pair already in time gets no delta at
///             all rather than a delta carrying zero samples.
///          4. **Mono fold.** A track @ref analyze_mono_risks flagged gets its
///             width pulled in, and a track whose low end specifically is wide
///             additionally gets a `stereo.monoMaker` insert at a fixed,
///             class-independent crossover.
///
/// @details **One delay and one polarity decision per track.** A track appears
///          in as many pairs as it has neighbours, and these are last-writer-wins
///          fields, so emitting a delta per pair would silently keep whichever
///          pair happened to be enumerated last. The pair with the largest
///          `|correlation|` wins instead — the strongest evidence decides — and
///          ties keep the earlier entry in @ref MixProfile::alignment, which is
///          itself enumerated in a fixed order.
///
/// @details **Alignment does not depend on classification.** A cancellation
///          between two tracks is measured, not inferred from what they are, so
///          a usable track still gets its polarity and delay suggestions when
///          the classifier returned @ref SourceClass::Unknown or a
///          low-confidence label. Placement is the opposite: it reads nothing
///          but the class, so an unclassified or barely-classified track is left
///          where it is.
///
/// @details Tracks with @ref TrackProfile::usable false are never touched at
///          all, in any of the four decisions.
///
/// @details Degenerate input never throws. No tracks, one track, a disabled
///          image domain, an empty crowding vector, or a pair whose indices fall
///          outside @p profiles all yield an empty vector or simply skip the
///          affected decision.
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
