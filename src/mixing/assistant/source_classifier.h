#pragma once

/// @file source_classifier.h
/// @brief Rule-based source classification for the mixing assistant.
///
/// @details **Offline / control thread only.** Classification itself is cheap,
///          but it reads a @ref TrackProfile that only the offline profiler can
///          produce, and it allocates while matching name hints. Never call it
///          from `process()`.
///
/// @details **No trained model, no statistical mapping, no learned parameter.**
///          The classifier is a single-layer decision table: measured features
///          go in, a class comes out, with nothing in between that fits, reduces
///          or clusters the data. Every threshold in the table is hand written
///          and readable as a table row, so a wrong label is traceable to the
///          one row that produced it.
///
/// @details The output is data, not a control sequence: a class and a
///          confidence. What to do about a classified track is the suggester's
///          decision, taken later and separately.

#include <vector>

#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief One track's classification result.
struct SourceClassification {
  /// @brief The identified class, or @ref SourceClass::Unknown.
  /// @details Unknown is a normal, frequent and *correct* output, not a
  ///          failure. A mix contains sources the table has no rule for, and a
  ///          confidently wrong suggestion is worse than no suggestion.
  SourceClass source = SourceClass::Unknown;
  /// @brief Confidence in `[0, 1]`; always 0 when @ref source is Unknown.
  /// @details Derived from how far inside its bounds the matched rule sat, so a
  ///          track that scraped past a threshold scores far below one that
  ///          matched comfortably. A track whose confidence falls under the
  ///          module's acceptance threshold is reported as Unknown rather than
  ///          as a weak label.
  float confidence = 0.0f;
};

/// @brief Classifies one already-profiled track.
/// @details An unusable profile (@ref TrackProfile::usable false) is reported as
///          Unknown with zero confidence without being examined.
///
///          @ref TrackProfile::name is consulted only as a confidence
///          adjustment: a name agreeing with the measured class raises
///          confidence, a name naming a different class lowers it. A name can
///          never select a class, so a mislabelled track is still classified by
///          what it sounds like — with less certainty attached.
/// @param profile Profile from @ref analyze_track_profile.
/// @return The identified class and its confidence.
SourceClassification classify_source(const TrackProfile& profile);

/// @brief Fills @ref TrackProfile::source and @ref TrackProfile::source_confidence
///        on every profile in place.
/// @details Each track is classified on its own; no cross-track reasoning is
///          applied here, so the result does not depend on which other tracks
///          happen to be present.
/// @param profiles Profiles to classify, modified in place.
void classify_sources(std::vector<TrackProfile>& profiles);

}  // namespace sonare::mixing::assistant
