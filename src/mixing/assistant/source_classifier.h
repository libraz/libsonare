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
///          The first satisfied table row is the measured class. @ref
///          TrackProfile::name then acts as follows:
///          - naming the measured class raises its confidence;
///          - stating another class redirects to it when the evidence does not
///            contradict it: a class with a row needs that row satisfied by the
///            track (it then takes that row's confidence plus the agreement
///            bonus), and a class without a row needs the measurement not to
///            have confidently placed the track in the other family — a drum
///            is never renamed keys or a voice, a guitar may be renamed keys;
///          - otherwise a name naming other classes lowers the measured class's
///            confidence, and cannot select a class whose row the track fails.
///
///          The class a name states is its head: a compound such as "Lead Vox"
///          or "Synth Lead" states its last hint word, while hint words joined
///          by anything else ("Strings and Keys") state none.
///
///          Six classes — @ref SourceClass::Keys, @ref SourceClass::Strings,
///          @ref SourceClass::Lead, @ref SourceClass::Vocal, @ref
///          SourceClass::Backing and @ref SourceClass::Fx — have no row, because
///          no combination of the measured features separates them from their
///          neighbours without a trained model. An unnamed track is never
///          reported as one of them; a named one is, at a fixed modest
///          confidence.
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
