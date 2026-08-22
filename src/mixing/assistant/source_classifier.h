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
///          For every class the decision table has a row for, @ref
///          TrackProfile::name is consulted only as a confidence adjustment: a
///          name agreeing with the measured class raises confidence, a name
///          naming a different class lowers it. A name cannot select one of
///          those classes, so a mislabelled track is still classified by what it
///          sounds like — with less certainty attached.
///
///          Four classes — @ref SourceClass::Keys, @ref SourceClass::Strings,
///          @ref SourceClass::Backing and @ref SourceClass::Fx — have no row,
///          because no combination of the measured features separates them from
///          their neighbours without a trained model. Those, and only those, are
///          supplied by the name, at a fixed modest confidence, and only when
///          the table produced no answer of its own. That is not the name
///          overriding a measurement: there is no measurement to override.
/// @param profile Profile from @ref analyze_track_profile.
/// @return The identified class and its confidence.
SourceClassification classify_source(const TrackProfile& profile);

/// @brief Confidence a perfectly-matching, unnamed track earns for @p source.
/// @details The per-class prior a rule row carries, before the match score
///          scales it and before any name adjustment. Exposed because a
///          downstream stage that wants "the classifier is nearly certain about
///          this class" cannot express it as one absolute number: confidence is
///          scaled by a prior that differs per class, so an absolute threshold
///          asks a different amount of certainty of each one — and a threshold
///          above a class's prior silently removes that class from whatever the
///          stage was gating.
/// @return The row's base confidence, or 0 for a class with no row.
float source_base_confidence(SourceClass source) noexcept;

/// @brief Fills @ref TrackProfile::source and @ref TrackProfile::source_confidence
///        on every profile in place.
/// @details Each track is classified on its own; no cross-track reasoning is
///          applied here, so the result does not depend on which other tracks
///          happen to be present.
/// @param profiles Profiles to classify, modified in place.
void classify_sources(std::vector<TrackProfile>& profiles);

}  // namespace sonare::mixing::assistant
