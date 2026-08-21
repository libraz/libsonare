/// @file source_classifier.cpp
/// @brief Single-layer decision table mapping measured features to a source class.

#include "mixing/assistant/source_classifier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace sonare::mixing::assistant {
namespace {

constexpr float kUnbounded = std::numeric_limits<float>::infinity();

// Returned by a condition or a rule that does not apply. Every real score is in
// [0, 1], so a negative value cannot be confused with a weak match.
constexpr float kNoMatch = -1.0f;

// Band indices into TrackProfile::band_occupancy, in the order of kBands.
// Named so a share expression reads as a frequency range rather than an offset.
constexpr std::size_t kSubBand = 0;      // 20-60 Hz
constexpr std::size_t kLowBand = 1;      // 60-250 Hz
constexpr std::size_t kLowMidBand = 2;   // 250-500 Hz
constexpr std::size_t kMidBand = 3;      // 500-2000 Hz
constexpr std::size_t kHighMidBand = 4;  // 2-6 kHz
constexpr std::size_t kHighBand = 5;     // 6-12 kHz
constexpr std::size_t kAirBand = 6;      // 12 kHz-Nyquist

static_assert(kBandCount == 7, "the band index constants above enumerate the whole band grid");

// Representative frequency of each band: the geometric mean of its edges, which
// is the centre a log-frequency weighting sees. The air band is open ended to
// Nyquist, so it takes a nominal 16 kHz -- the geometric centre of 12-22.05 kHz
// and 12-24 kHz alike, which keeps the derived centroid stable across the two
// common sample rates instead of moving with them.
constexpr std::array<float, kBandCount> kBandCenterHz = {35.0f,   122.0f,  354.0f,  1000.0f,
                                                         3464.0f, 8485.0f, 16000.0f};

/// @brief Feature vocabulary the decision table is written in.
/// @details Everything here is read straight off the profile or is a sum of
///          adjacent band shares. There is no fitting, projection or clustering
///          step between the measurement and the table: a rule row compares the
///          measured numbers themselves.
///
///          The raw spectral centroid is deliberately absent. It is a
///          linear-frequency weighted mean, so one quiet cymbal wash drags it
///          across octaves and its value moves with the sample rate; the band
///          share centroid below is weighted in log frequency over the fixed
///          band grid and stays comparable between tracks.
enum class Feature {
  BandCentroidHz,
  RolloffHz,
  Flatness,
  SubLowShare,
  MidShare,
  HighMidShare,
  HighAirShare,
  SustainRatio,
  AttackDensity,
  CrestFactorDb,
  Count,
};

constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::Count);

// Sentinel for an unfilled condition slot in a rule row.
constexpr Feature kUnusedSlot = Feature::Count;

/// @brief How distance from a bound is measured for a feature.
enum class Scale {
  /// Distance in the feature's own units.
  Linear,
  /// Distance in octaves, so 100 Hz sits as far from 50 Hz as 8 kHz does from
  /// 4 kHz. A frequency bound measured linearly would make every low-register
  /// rule look like it barely matched.
  Octave,
};

struct FeatureSpec {
  Scale scale = Scale::Linear;
  /// @brief Distance past a bound that counts as fully inside the rule.
  /// @details This is what turns a satisfied condition into a graded score: at
  ///          the bound the condition contributes 0, at `tolerance` past it 1.
  float tolerance = 1.0f;
};

// Tolerances are "how much headroom a track needs before the rule is clearly
// right rather than marginally right". Half an octave for frequencies (a
// musically obvious distance), 0.15 for the [0, 1] shares and ratios (a seventh
// of their full range), one onset per second for attack density, and 3 dB for
// crest factor (a doubling of peak-to-RMS power).
constexpr std::array<FeatureSpec, kFeatureCount> kFeatureSpecs = {{
    {Scale::Octave, 0.5f},   // BandCentroidHz
    {Scale::Octave, 0.5f},   // RolloffHz
    {Scale::Linear, 0.15f},  // Flatness
    {Scale::Linear, 0.15f},  // SubLowShare
    {Scale::Linear, 0.15f},  // MidShare
    {Scale::Linear, 0.15f},  // HighMidShare
    {Scale::Linear, 0.15f},  // HighAirShare
    {Scale::Linear, 0.15f},  // SustainRatio
    {Scale::Linear, 1.0f},   // AttackDensity
    {Scale::Linear, 3.0f},   // CrestFactorDb
}};

/// @brief One feature bound inside a rule row.
struct Condition {
  Feature feature = kUnusedSlot;
  float low = -kUnbounded;
  float high = kUnbounded;
};

constexpr Condition between(Feature feature, float low, float high) { return {feature, low, high}; }
constexpr Condition at_least(Feature feature, float low) { return {feature, low, kUnbounded}; }
constexpr Condition at_most(Feature feature, float high) { return {feature, -kUnbounded, high}; }

// Five bounds is enough for every rule below and keeps a row on one screen. A
// class needing more evidence than that is not separable by a table and belongs
// in Unknown.
constexpr std::size_t kMaxConditions = 5;

/// @brief One row of the decision table.
struct Rule {
  SourceClass source = SourceClass::Unknown;
  /// @brief Confidence a perfectly-inside match earns, before name hints.
  /// @details Set per row by how definitional the signature is: a low, short,
  ///          loud transient is a kick and very little else, while the bounds
  ///          that catch a guitar also catch several other plucked mid-register
  ///          parts, so its row can never claim as much.
  float base_confidence = 0.0f;
  std::array<Condition, kMaxConditions> conditions{};
};

// The table. Rows are evaluated top to bottom and the first match wins.
//
// Grouped by band centroid -- low register, mid register, high register -- so
// the coarse split (kick/bass vs vocal vs cymbal) happens once at the top of
// each row and the remaining bounds only separate neighbours inside one group.
// Every row carries its band-centroid bound first; the static_assert below
// enforces it, because a row without one would silently reach across groups.
//
// Adding a class is adding one row. There is no nesting to unpick and no branch
// order to re-derive, which is the whole reason the rules are data.
constexpr std::array<Rule, 10> kRules = {{
    // --- Low register -------------------------------------------------------
    // Kick: everything under 130 Hz, gone almost as soon as it arrives, with the
    // peak-to-RMS ratio of a repeated hit rather than of a held note.
    {SourceClass::Kick,
     0.85f,
     {{between(Feature::BandCentroidHz, 20.0f, 130.0f), at_least(Feature::SubLowShare, 0.65f),
       at_most(Feature::SustainRatio, 0.35f), at_least(Feature::CrestFactorDb, 8.0f)}}},
    // Bass: the same register as the kick, separated by the one thing that
    // actually differs -- the note is held, and it is a note, so the spectrum is
    // tonal rather than noisy.
    {SourceClass::Bass,
     0.80f,
     {{between(Feature::BandCentroidHz, 40.0f, 250.0f), at_least(Feature::SubLowShare, 0.55f),
       at_least(Feature::SustainRatio, 0.40f), at_most(Feature::Flatness, 0.20f)}}},
    // Tom: a drum pitched above the kick and below the snare's noise. The lack
    // of high-band content is what keeps a snare out of this row.
    {SourceClass::Tom,
     0.60f,
     {{between(Feature::BandCentroidHz, 90.0f, 400.0f), at_most(Feature::SustainRatio, 0.45f),
       at_most(Feature::HighAirShare, 0.12f), at_most(Feature::AttackDensity, 4.0f),
       at_least(Feature::CrestFactorDb, 8.0f)}}},

    // --- Mid register -------------------------------------------------------
    // Snare: a mid-register transient that always carries noise above its body.
    // That high-band share is the whole difference from the tom row above.
    {SourceClass::Snare,
     0.75f,
     {{between(Feature::BandCentroidHz, 300.0f, 3000.0f), at_most(Feature::SustainRatio, 0.35f),
       at_least(Feature::AttackDensity, 0.8f), at_least(Feature::HighAirShare, 0.06f),
       at_least(Feature::CrestFactorDb, 8.0f)}}},
    // Percussion: dense short hits like the snare, but tonal (shaker, conga,
    // woodblock) and without the snare's noise tail, so the high-band bound is
    // the complement of the row above.
    {SourceClass::Percussion,
     0.55f,
     {{between(Feature::BandCentroidHz, 300.0f, 4000.0f), at_most(Feature::SustainRatio, 0.30f),
       at_least(Feature::AttackDensity, 2.0f), at_most(Feature::HighAirShare, 0.06f),
       at_most(Feature::Flatness, 0.30f)}}},
    // Vocal: sustained, tonal, concentrated in 250-2000 Hz and rolling off well
    // before the cymbal region. The rolloff bound is what separates it from the
    // lead row: a voice runs out of energy where a bright lead line keeps going.
    {SourceClass::Vocal,
     0.70f,
     {{between(Feature::BandCentroidHz, 250.0f, 1400.0f), at_least(Feature::SustainRatio, 0.40f),
       at_most(Feature::Flatness, 0.22f), at_least(Feature::MidShare, 0.40f),
       between(Feature::RolloffHz, 1000.0f, 8000.0f)}}},
    // Guitar: mid register and tonal like a vocal, but plucked -- partly decayed
    // rather than held, with the pick attack showing as high-mid share. Placed
    // after the vocal row because a held vocal note satisfies neither bound of
    // its sustain range.
    {SourceClass::Guitar,
     0.55f,
     {{between(Feature::BandCentroidHz, 250.0f, 2500.0f),
       between(Feature::SustainRatio, 0.15f, 0.45f), at_most(Feature::Flatness, 0.30f),
       between(Feature::HighMidShare, 0.08f, 0.50f), at_least(Feature::CrestFactorDb, 9.0f)}}},
    // Lead: a sustained tonal line sitting a register above a voice, with its
    // energy in the presence band. The centroid bound starts where the vocal
    // row's ends, so the two cannot both claim a track outright.
    {SourceClass::Lead,
     0.60f,
     {{between(Feature::BandCentroidHz, 1200.0f, 5000.0f), at_least(Feature::SustainRatio, 0.40f),
       at_most(Feature::Flatness, 0.30f), at_least(Feature::HighMidShare, 0.25f)}}},

    // --- High register ------------------------------------------------------
    // Hi-hat: bright noise, short, and repeated often. Density plus the short
    // decay is the entire difference from a cymbal -- the two share a register,
    // so both rows take the same wide centroid range and separate in time.
    {SourceClass::HiHat,
     0.80f,
     {{between(Feature::BandCentroidHz, 5000.0f, 20000.0f), at_most(Feature::SustainRatio, 0.30f),
       at_least(Feature::AttackDensity, 2.0f), at_least(Feature::Flatness, 0.30f),
       at_least(Feature::HighAirShare, 0.50f)}}},
    // Cymbal: the same bright noise left to ring. Note the gap against the row
    // above -- a sustain ratio between 0.30 and 0.35 matches neither, which is
    // the intended answer for something that is genuinely between the two.
    {SourceClass::Cymbal,
     0.70f,
     {{between(Feature::BandCentroidHz, 4000.0f, 20000.0f), at_least(Feature::SustainRatio, 0.35f),
       at_most(Feature::AttackDensity, 2.0f), at_least(Feature::Flatness, 0.30f),
       at_least(Feature::HighAirShare, 0.50f)}}},
}};

// Classes with no row -- Keys, Strings, Backing, Fx -- are never produced. No
// combination of the measured features separates a piano from a plucked guitar,
// or a backing stack from a lead vocal, without the kind of trained model this
// module refuses to carry. They stay in the taxonomy because a caller may set
// them by hand, and the table declines to guess at them.

// A track whose confidence lands below this is reported as Unknown. It sits
// under every row's base confidence, so no rule is dead on arrival, but far
// enough above zero that a rule matched on the edge of its bounds is discarded:
// a wrong suggestion costs an engineer more time than a missing one.
constexpr float kMinConfidence = 0.35f;

// How much of a rule's score comes from its weakest satisfied condition rather
// than from the average of all of them. Weighted towards the weakest because a
// rule is only as good as the bound it barely cleared, but not entirely, so one
// tight-yet-satisfied bound cannot erase an otherwise convincing match.
constexpr float kWeakestLinkWeight = 0.6f;

// Name-hint adjustments. The penalty is the larger of the two on purpose: a
// matching name is weak evidence, since tracks are routinely left on a default
// or generic name, while a name naming a different class means the engineer's
// label and the measurement disagree and either one of them may be the wrong
// one. Applied before the acceptance threshold, so a strongly contradicted
// match can fall through to Unknown.
constexpr float kNameAgreementBonus = 0.12f;
constexpr float kNameConflictPenalty = -0.25f;

// Longest hint list any class carries.
constexpr std::size_t kMaxHintWords = 6;

/// @brief Substrings that suggest a class when they appear in a track name.
struct NameHint {
  SourceClass source = SourceClass::Unknown;
  /// @brief Lowercase substrings; unused slots are null.
  std::array<const char*, kMaxHintWords> words{};
};

// Matching is a lowercase substring test and nothing more -- no regular
// expressions, no separator parsing, no language detection. "Kick In",
// "KICK_01" and "kick" all have to read the same, and that is the entire
// requirement. Classes with no rule row still appear here: they cannot be
// selected, but they can contradict a measured class, which is worth knowing.
constexpr std::array<NameHint, 11> kNameHints = {{
    {SourceClass::Kick, {{"kick", "bassdrum", "bass drum"}}},
    {SourceClass::Snare, {{"snare", "rimshot"}}},
    {SourceClass::HiHat, {{"hihat", "hi-hat", "hi hat", "hat", "hh"}}},
    {SourceClass::Tom, {{"tom", "floortom"}}},
    {SourceClass::Cymbal, {{"cymbal", "crash", "ride", "china", "splash"}}},
    {SourceClass::Bass, {{"bass", "sub"}}},
    {SourceClass::Guitar, {{"guitar", "gtr"}}},
    {SourceClass::Keys, {{"piano", "keys", "rhodes", "organ", "synth"}}},
    {SourceClass::Strings, {{"strings", "violin", "cello", "viola"}}},
    {SourceClass::Lead, {{"lead", "solo"}}},
    {SourceClass::Vocal, {{"vocal", "vox", "voice"}}},
}};

constexpr bool every_rule_is_anchored_on_band_centroid() {
  for (const Rule& rule : kRules) {
    if (rule.conditions[0].feature != Feature::BandCentroidHz) return false;
  }
  return true;
}

constexpr bool every_rule_can_clear_the_threshold() {
  for (const Rule& rule : kRules) {
    if (rule.base_confidence <= kMinConfidence || rule.base_confidence > 1.0f) return false;
  }
  return true;
}

constexpr bool every_tolerance_is_positive() {
  for (const FeatureSpec& spec : kFeatureSpecs) {
    if (!(spec.tolerance > 0.0f)) return false;
  }
  return true;
}

static_assert(every_rule_is_anchored_on_band_centroid(),
              "each rule must open with its band-centroid range so the coarse register split "
              "stays explicit");
static_assert(every_rule_can_clear_the_threshold(),
              "a row whose base confidence cannot exceed the acceptance threshold can never "
              "produce a label");
static_assert(every_tolerance_is_positive(), "a zero tolerance cannot grade a condition");

/// @brief Log-frequency centroid of the band occupancy, in Hz.
/// @details Weighted in log frequency, so the result is the geometric centre of
///          where the energy sits. Returns 0 for a silent occupancy, which fails
///          every rule's opening bound and therefore yields Unknown.
float band_centroid_hz(const std::array<float, kBandCount>& occupancy) {
  float weight = 0.0f;
  float log_sum = 0.0f;
  for (std::size_t band = 0; band < static_cast<std::size_t>(kBandCount); ++band) {
    const float share = occupancy[band];
    if (!(share > 0.0f)) continue;
    weight += share;
    log_sum += share * std::log(kBandCenterHz[band]);
  }
  if (!(weight > 0.0f)) return 0.0f;
  return std::exp(log_sum / weight);
}

std::array<float, kFeatureCount> extract_features(const TrackProfile& profile) {
  const auto& occupancy = profile.band_occupancy;
  const auto& spectral = profile.base.spectral;
  const auto& dynamics = profile.base.dynamics;

  std::array<float, kFeatureCount> features{};
  features[static_cast<std::size_t>(Feature::BandCentroidHz)] = band_centroid_hz(occupancy);
  features[static_cast<std::size_t>(Feature::RolloffHz)] = spectral.rolloff_hz;
  features[static_cast<std::size_t>(Feature::Flatness)] = spectral.flatness;
  features[static_cast<std::size_t>(Feature::SubLowShare)] =
      occupancy[kSubBand] + occupancy[kLowBand];
  features[static_cast<std::size_t>(Feature::MidShare)] =
      occupancy[kLowMidBand] + occupancy[kMidBand];
  features[static_cast<std::size_t>(Feature::HighMidShare)] = occupancy[kHighMidBand];
  features[static_cast<std::size_t>(Feature::HighAirShare)] =
      occupancy[kHighBand] + occupancy[kAirBand];
  features[static_cast<std::size_t>(Feature::SustainRatio)] = dynamics.sustain_ratio;
  features[static_cast<std::size_t>(Feature::AttackDensity)] = dynamics.attack_density;
  features[static_cast<std::size_t>(Feature::CrestFactorDb)] =
      profile.base.loudness.crest_factor_db;
  return features;
}

/// @brief Grades one condition against a measured value.
/// @return 1 for comfortably inside, tapering to 0 at the bound, or @ref kNoMatch
///         when the value is outside.
float condition_score(const Condition& condition, float value) {
  const FeatureSpec spec = kFeatureSpecs[static_cast<std::size_t>(condition.feature)];
  float distance = kUnbounded;

  if (spec.scale == Scale::Octave) {
    // A non-positive frequency has no octave distance from anything.
    if (!(value > 0.0f)) return kNoMatch;
    if (std::isfinite(condition.low) && condition.low > 0.0f) {
      distance = std::min(distance, std::log2(value / condition.low));
    }
    if (std::isfinite(condition.high) && condition.high > 0.0f) {
      distance = std::min(distance, std::log2(condition.high / value));
    }
  } else {
    if (std::isfinite(condition.low)) distance = std::min(distance, value - condition.low);
    if (std::isfinite(condition.high)) distance = std::min(distance, condition.high - value);
  }

  if (distance < 0.0f) return kNoMatch;
  // Bounded on neither side: nothing to grade, so it neither helps nor hurts.
  if (!std::isfinite(distance)) return 1.0f;
  return std::min(distance / spec.tolerance, 1.0f);
}

/// @brief Grades a whole rule row.
/// @return The margin score in `[0, 1]`, or @ref kNoMatch when any condition fails.
float rule_score(const Rule& rule, const std::array<float, kFeatureCount>& features) {
  float weakest = 1.0f;
  float total = 0.0f;
  int count = 0;

  for (const Condition& condition : rule.conditions) {
    if (condition.feature == kUnusedSlot) continue;
    const float score =
        condition_score(condition, features[static_cast<std::size_t>(condition.feature)]);
    if (score < 0.0f) return kNoMatch;
    weakest = std::min(weakest, score);
    total += score;
    ++count;
  }

  // A row with no conditions would claim every track; refuse it rather than
  // letting an editing slip become a silent catch-all.
  if (count == 0) return kNoMatch;

  const float mean = total / static_cast<float>(count);
  return kWeakestLinkWeight * weakest + (1.0f - kWeakestLinkWeight) * mean;
}

std::string to_lower_ascii(const std::string& text) {
  std::string lowered = text;
  for (char& character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lowered;
}

/// @brief Confidence adjustment implied by the track name.
/// @details Agreement wins outright: a name containing its own class's word is
///          not treated as contradicted just because it also mentions another
///          class, which is what makes "bass gtr" on a bass track a confirmation
///          rather than a conflict.
/// @param name Track name, in whatever case the caller supplied.
/// @param source The class the measurements chose.
/// @return A bonus, a penalty, or zero when the name says nothing.
float name_hint_adjustment(const std::string& name, SourceClass source) {
  if (name.empty()) return 0.0f;
  const std::string lowered = to_lower_ascii(name);

  bool names_another_class = false;
  for (const NameHint& hint : kNameHints) {
    for (const char* word : hint.words) {
      if (word == nullptr) break;
      if (lowered.find(word) == std::string::npos) continue;
      if (hint.source == source) return kNameAgreementBonus;
      names_another_class = true;
    }
  }
  return names_another_class ? kNameConflictPenalty : 0.0f;
}

}  // namespace

SourceClassification classify_source(const TrackProfile& profile) {
  SourceClassification classification;
  if (!profile.usable) return classification;

  const std::array<float, kFeatureCount> features = extract_features(profile);
  // A non-finite feature makes every comparison below false rather than failing
  // one, which would read as a match. Refuse the whole track instead.
  for (float value : features) {
    if (!std::isfinite(value)) return classification;
  }

  for (const Rule& rule : kRules) {
    const float score = rule_score(rule, features);
    if (score < 0.0f) continue;

    const float confidence = std::clamp(
        rule.base_confidence * score + name_hint_adjustment(profile.name, rule.source), 0.0f, 1.0f);
    // The first matching row is the answer even when it is not confident enough
    // to give. Falling through would hand the track to a weaker, broader row
    // that the specific one already ruled on.
    if (confidence < kMinConfidence) return classification;

    classification.source = rule.source;
    classification.confidence = confidence;
    return classification;
  }
  return classification;
}

void classify_sources(std::vector<TrackProfile>& profiles) {
  for (TrackProfile& profile : profiles) {
    const SourceClassification classification = classify_source(profile);
    profile.source = classification.source;
    profile.source_confidence = classification.confidence;
  }
}

}  // namespace sonare::mixing::assistant
