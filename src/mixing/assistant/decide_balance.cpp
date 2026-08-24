/// @file decide_balance.cpp
/// @brief Source-class relative level balance for the mixing assistant.

#include "mixing/assistant/decide_balance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "util/number_format.h"

namespace sonare::mixing::assistant {
namespace {

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// invert or exaggerate the offset rather than weaken it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;

// The classifier refuses to guess rather than returning a low-confidence label,
// so a confidence this far below certain means the decision table matched only
// weakly. A class-relative offset built on a weak match is as likely to push the
// wrong part forward as the right one, so the track keeps its staged level and
// gets no delta at all.
constexpr float kMinSourceConfidence = 0.5f;
// A fully certain classification, at which the table's offset is applied in
// full. Between the floor and here the offset scales with the confidence, so a
// marginal call moves the fader less than a clear one.
constexpr float kFullConfidence = 1.0f;

// One decimal is the resolution a mixer acts on; more digits make the
// explanation read like a measurement rather than a suggestion.
constexpr int kReasonDecimals = 1;
// Two decimals for a [0, 1] confidence, which has no natural unit to round to.
constexpr int kConfidenceDecimals = 2;
// Half of the printed resolution: below this the confidence-scaled offset and
// the table's own offset would print identically, so saying the value was
// scaled down would show the reader two identical numbers.
constexpr float kReasonNoticeableDb = 0.05f;

/// @brief One class's level offset relative to the staged reference.
struct ClassOffset {
  SourceClass source;
  float offset_db;
};

/// @brief A complete set of class-relative offsets.
struct BalanceTable {
  const char* name;
  std::array<ClassOffset, static_cast<std::size_t>(kSourceClassCount)> offsets;
};

// ---------------------------------------------------------------------------
// These numbers are studio convention, not physics.
// ---------------------------------------------------------------------------
// Every value below is a customary starting position from recording practice —
// where a part usually sits relative to the others once each one has been
// staged to the same absolute level — and none of them is derived, measured or
// otherwise defensible as a constant. They are deliberately small: a starting
// balance a mixer will move, not a finished one. Rows may be retuned freely, so
// nothing downstream may depend on an exact value; only the ordering between
// classes carries meaning.
//
// The general table is voiced for popular music, where a lead vocal is the most
// forward element and the kick and bass anchor the bottom.
constexpr BalanceTable kGeneralTable{
    "general",
    {{
        // Never emitted: an unclassified track gets no offset at all. Present so
        // the table covers the enum and a new class cannot be forgotten.
        {SourceClass::Unknown, 0.0f},
        // Anchors the bottom end with the bass; sits just above the reference so
        // the pulse stays audible under a dense arrangement.
        {SourceClass::Kick, 1.0f},
        // The backbeat sits with the kick but slightly under it, so the two do
        // not fight for the same forward position.
        {SourceClass::Snare, 0.5f},
        // Continuous, narrow-band and the first thing to become fatiguing, so it
        // is placed well behind the kit's accents.
        {SourceClass::HiHat, -3.5f},
        // Heard in fills rather than continuously, so it sits back until it
        // plays and does not need to hold a forward position.
        {SourceClass::Tom, -1.5f},
        // Broadband and long-decaying: level with the backbeat it masks
        // everything above the mids, so it sits furthest back in the kit.
        {SourceClass::Cymbal, -4.5f},
        // Shares the foundation with the kick and is placed with it.
        {SourceClass::Bass, 1.0f},
        // Usually a rhythm bed rather than the focus; a guitar that is the focus
        // classifies as a lead instead.
        {SourceClass::Guitar, -1.5f},
        // A comping layer that fills space between the other parts.
        {SourceClass::Keys, -2.5f},
        // Sustained and wide, the archetypal bed: it reads as present from well
        // behind the reference and crowds the mids from in front of it.
        {SourceClass::Strings, -3.0f},
        // The focus instrument, in front of every bed part but under the voice.
        {SourceClass::Lead, 1.5f},
        // The most forward element in popular music, which is what the rest of
        // this table is arranged around. A survey of professional practice found
        // the lead vocal sitting 3-8 LU above the other elements (Pestana and
        // Reiss, "Intelligent Audio Production Strategies Informed by Best
        // Practices", AES 53rd International Conference on Semantic Audio,
        // 2014). This clears the loudest of those elements, the lead instrument,
        // by 3.5 dB — inside the range and at its conservative end, because the
        // assistant proposes a starting balance rather than a finished one.
        {SourceClass::Vocal, 5.0f},
        // Supports the lead voice and must not compete with it, so it sits far
        // enough back to be felt rather than followed.
        {SourceClass::Backing, -4.0f},
        // A colour layer over the kit, placed with the hi-hat for the same
        // reason: it is continuous and it accumulates.
        {SourceClass::Percussion, -3.5f},
        // A garnish — loud enough to notice, quiet enough not to read as a part.
        {SourceClass::Fx, -5.0f},
    }},
};

// Verifies the table is indexed by SourceClass, so adding a class without a row
// is a build failure rather than a silent zero offset.
constexpr bool covers_every_class(const BalanceTable& table) {
  for (std::size_t index = 0; index < table.offsets.size(); ++index) {
    if (static_cast<std::size_t>(table.offsets[index].source) != index) return false;
  }
  return true;
}
static_assert(covers_every_class(kGeneralTable),
              "the general balance table must hold one row per SourceClass, in enum order");

// Index order must match kDefaultBalanceTableIndex.
constexpr std::array<BalanceTable, 1> kBalanceTables{{kGeneralTable}};
static_assert(kDefaultBalanceTableIndex < kBalanceTables.size(),
              "the default balance table index must name a table that exists");

/// @brief One genre label and the table that voices it.
struct GenreTableEntry {
  const char* genre;
  std::size_t table_index;
};

// Genre labels resolve here before falling back to the default table. Only the
// general table exists so far, so the map is empty and every genre reaches the
// fallback; adding a table means appending its labels here, and decide_balance
// itself does not change.
constexpr std::array<GenreTableEntry, 0> kGenreTableMap{};

std::string format_signed_db(float value) {
  // Negative zero would print as "-0.0" and read as a real downward move.
  if (value == 0.0f) value = 0.0f;
  // The decimal point is a point wherever the host runs. A reason string is read
  // by a person and parsed by nobody, but a comma in the middle of a number
  // reads as a second number.
  return sonare::util::format_fixed(static_cast<double>(value), kReasonDecimals,
                                    /*force_sign=*/true);
}

std::string format_confidence(float value) {
  return sonare::util::format_fixed(static_cast<double>(value), kConfidenceDecimals);
}

float offset_for(const BalanceTable& table, SourceClass source) noexcept {
  const auto index = static_cast<std::size_t>(source);
  if (index >= table.offsets.size()) return 0.0f;
  return table.offsets[index].offset_db;
}

// Returns the genre every track is balanced against, or an empty string when no
// track offers one.
//
// The whole call is balanced against one label rather than each track against
// its own. A relative level table only means anything when every part is read
// from the same one, and a single stem is a poor genre witness anyway — a solo
// hi-hat track carries none of the material the label describes. The modal
// label across the usable tracks is the most robust reading available without
// asking the caller, and choosing one representative track would only move the
// guess to which track is representative. Ties keep the label seen first, so
// the result does not depend on the order two equally supported labels happen
// to be tallied in.
std::string dominant_genre(const std::vector<TrackProfile>& profiles) {
  std::vector<std::pair<std::string, int>> tally;
  for (const TrackProfile& profile : profiles) {
    if (!profile.usable) continue;
    if (profile.base.genre_candidates.empty()) continue;
    const std::string& label = profile.base.genre_candidates.front().name;
    if (label.empty()) continue;

    bool counted = false;
    for (std::pair<std::string, int>& entry : tally) {
      if (entry.first == label) {
        ++entry.second;
        counted = true;
        break;
      }
    }
    if (!counted) tally.emplace_back(label, 1);
  }

  const std::pair<std::string, int>* best = nullptr;
  for (const std::pair<std::string, int>& entry : tally) {
    if (best == nullptr || entry.second > best->second) best = &entry;
  }
  return best == nullptr ? std::string() : best->first;
}

// Returns why the track is not balanced, or nullptr when it is.
//
// The text names which check fired but is deliberately not emitted: excluding a
// track means emitting no delta at all, and this stage returns suggestions
// rather than annotations.
//
// Level is not re-tested here the way gain staging tests it. This decision reads
// only the source class, and a silent or unmeasurable track cannot carry a
// confident classification in the first place.
const char* balance_exclusion(const TrackProfile& profile) {
  if (!profile.usable) {
    return "the profiler marked the track unusable";
  }
  if (profile.source == SourceClass::Unknown) {
    return "the track was never classified, so no class offset applies to it";
  }
  if (!std::isfinite(profile.source_confidence) ||
      profile.source_confidence < kMinSourceConfidence) {
    return "the classification is too weak to place the track against the other parts";
  }
  return nullptr;
}

}  // namespace

std::size_t balance_table_count() noexcept { return kBalanceTables.size(); }

std::size_t balance_table_index_for_genre(const std::string& genre) noexcept {
  for (const GenreTableEntry& entry : kGenreTableMap) {
    if (genre == entry.genre) return entry.table_index;
  }
  return kDefaultBalanceTableIndex;
}

std::vector<SceneDelta> decide_balance(const std::vector<TrackProfile>& profiles,
                                       const MixAssistantConfig& config) {
  std::vector<SceneDelta> deltas;
  if (!config.enable_balance) return deltas;

  const std::size_t table_index =
      std::min(balance_table_index_for_genre(dominant_genre(profiles)), kBalanceTables.size() - 1);
  const BalanceTable& table = kBalanceTables[table_index];

  const float strength =
      std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength);

  deltas.reserve(profiles.size());
  for (const TrackProfile& profile : profiles) {
    if (balance_exclusion(profile) != nullptr) continue;

    const float confidence =
        std::clamp(profile.source_confidence, kMinSourceConfidence, kFullConfidence);
    // The whole decision: the class's customary position, weakened by how sure
    // the classifier is of the class and by how strong a suggestion was asked
    // for. Nothing here reads the track's level — the absolute target is gain
    // staging's decision and sums in through a separate field.
    const float table_offset_db = offset_for(table, profile.source) * strength;
    const float offset_db = table_offset_db * confidence;

    SceneDelta delta;
    delta.domain = DeltaDomain::Gain;
    delta.strip_id = profile.strip_id;
    delta.fader_db = offset_db;
    delta.reason = "balanced " + profile.strip_id + " at " + format_signed_db(offset_db) +
                   " dB relative to its staged level as a " +
                   source_class_to_string(profile.source) + " part";
    if (std::fabs(table_offset_db - offset_db) >= kReasonNoticeableDb) {
      delta.reason += ", scaled down from " + format_signed_db(table_offset_db) + " dB by a " +
                      format_confidence(confidence) + " classification confidence";
    }
    deltas.push_back(std::move(delta));
  }

  return deltas;
}

}  // namespace sonare::mixing::assistant
