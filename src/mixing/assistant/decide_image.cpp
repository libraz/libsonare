/// @file decide_image.cpp
/// @brief Stereo placement, alignment and mono-fold decisions.

#include "mixing/assistant/decide_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "util/json.h"
#include "util/number_format.h"

namespace sonare::mixing::assistant {
namespace {

// One row of the class placement table.
struct PlacementRule {
  SourceClass source;
  // True when the class is pinned to the centre and never spread.
  bool centered;
  // Largest |pan| the outermost member of this class may take, before the
  // suggestion strength and the crowding adjustment are applied. Ignored when
  // `centered` is true.
  float spread_extent;
};

// Placement extents, written as the few coarse steps a mixer actually works in
// rather than on a continuous scale nothing here could justify.
//
// The two widest steps are set against a survey of professional practice, which
// tested "hard panning should be avoided" and found it false, and reports wide
// placements accounting for roughly a third of all panning decisions (Pestana
// and Reiss, "Intelligent Audio Production Strategies Informed by Best
// Practices", AES 53rd International Conference on Semantic Audio, 2014). The
// support and inner-image steps below them are unchanged by that finding: it
// says wide placements are common, not that every part belongs wide.
constexpr float kCentreExtent = 0.0f;
// Effects and colour parts sit at the edge; nothing in the arrangement depends
// on them being present in both speakers.
constexpr float kEdgeExtent = 0.9f;
// The main spreadable parts, and where a double-tracked pair lands: far enough
// out to be a wide placement rather than a hedge towards the centre, and short
// of the edge so the part is still present in the opposite speaker.
constexpr float kWideExtent = 0.8f;
// Parts that support the centre rather than frame it.
constexpr float kMediumExtent = 0.55f;
// Parts that belong inside another group's image — a hi-hat sits just off the
// snare, not out at the edge of the mix.
constexpr float kNarrowExtent = 0.3f;

// Where each source class sits in the image, and how far out it may go.
//
// This table is recording practice, not a derivation. The kick, the bass, the
// snare and the lead voice have been kept at the centre for as long as stereo
// mixing has existed: they carry the most energy and the most information, and
// both have to survive a mono fold, a single-subwoofer PA and a listener
// sitting well off-axis. Everything else is spread around them to open space
// for that centre. No part of the decision is read off the material — a
// spectral measurement can say a band is crowded, but it cannot say which side
// of the image a guitar belongs on, and a rule that mapped frequency content
// onto pan position would move a part every time its tone changed.
//
// SourceClass::Unknown deliberately has no row. An unclassified track has no
// convention to apply, so it is left where the caller had it.
constexpr std::array<PlacementRule, static_cast<std::size_t>(kSourceClassCount) - 1>
    kPlacementRules = {{
        {SourceClass::Kick, true, kCentreExtent},
        {SourceClass::Snare, true, kCentreExtent},
        {SourceClass::HiHat, false, kNarrowExtent},
        {SourceClass::Tom, false, kMediumExtent},
        {SourceClass::Cymbal, false, kMediumExtent},
        {SourceClass::Bass, true, kCentreExtent},
        {SourceClass::Guitar, false, kWideExtent},
        {SourceClass::Keys, false, kMediumExtent},
        {SourceClass::Strings, false, kMediumExtent},
        {SourceClass::Lead, true, kCentreExtent},
        {SourceClass::Vocal, true, kCentreExtent},
        {SourceClass::Backing, false, kMediumExtent},
        {SourceClass::Percussion, false, kWideExtent},
        {SourceClass::Fx, false, kEdgeExtent},
    }};

// Catches a source class added to the enum without a placement rule, which
// would otherwise silently leave every track of the new class unplaced.
constexpr bool rules_cover_every_class() {
  for (int value = 1; value < kSourceClassCount; ++value) {
    bool found = false;
    for (const PlacementRule& rule : kPlacementRules) {
      if (static_cast<int>(rule.source) == value) found = true;
    }
    if (!found) return false;
  }
  return true;
}
static_assert(rules_cover_every_class(),
              "every SourceClass except Unknown needs a row in kPlacementRules");

// Fraction of its class extent a lone member is given. With no partner on the
// other side, a full-extent placement tilts the whole mix, so a single track of
// a spreadable class only steps part of the way out.
constexpr float kLoneMemberFraction = 0.5f;

// How much further out a track is spread when the band it mostly occupies is
// crowded. Deliberately small: this adjusts an amount the class rule already
// decided, and a large factor would let the measurement take the decision over.
constexpr float kCrowdedSpreadBoost = 1.25f;

// A suggestion stops just short of a full hard pan. Not because hard panning is
// a mistake — the survey cited above tested that belief and found it false — but
// because a part at exactly ±1 is absent from one speaker, and choosing to make
// a part disappear for half the room is a decision the mixer takes rather than
// one an automatic starting point takes on their behalf.
constexpr float kMaxAbsPan = 0.9f;

// Below this the classifier is closer to guessing than to deciding, and a wrong
// class puts a part on the wrong side of the image — worse than leaving it
// where the caller had it. Placement is the only decision here that reads the
// class at all, so this gate applies to nothing else.
constexpr float kMinPlacementConfidence = 0.5f;

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// exaggerate or mirror a placement rather than weaken it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;

// Width a track flagged for mono collapse is pulled in to at full strength. It
// keeps most of its image: the width is usually deliberate, and the complaint
// is that it cancels, not that it exists.
constexpr float kNarrowedWidth = 0.7f;
constexpr float kNeutralWidth = 1.0f;

// Conventional crossover for folding a low end to mono. Below roughly this
// point stereo information survives neither a vinyl cut nor a PA with a single
// subwoofer, and the ear localises very little there in any case. It is a
// property of low-frequency reproduction rather than of what is playing, so it
// is the same figure for every source class.
constexpr float kMonoMakerCrossoverHz = 120.0f;
// Fully mono below the crossover. The insert is a correction for a measured
// collapse, not a taste control, so it is applied at full amount.
constexpr float kMonoMakerAmount = 1.0f;

constexpr const char* kMonoMakerProcessor = "stereo.monoMaker";

// Percentages and frequencies read as figures a mixer acts on, not as
// measurements, so the reason strings carry no decimals.
constexpr int kReasonDecimals = 0;

std::string format_rounded(float value) {
  // The decimal point is a point wherever the host runs. A reason string is read
  // by a person and parsed by nobody, but a comma in the middle of a number
  // reads as a second number.
  return sonare::util::format_fixed(static_cast<double>(value), kReasonDecimals);
}

std::string format_percent(float fraction) { return format_rounded(fraction * 100.0f); }

// Only ever called with a non-zero pan; a centred track says "centre" in its
// own reason instead.
std::string describe_pan(float pan) {
  const char* side = pan < 0.0f ? "left" : "right";
  return format_percent(std::fabs(pan)) + "% " + side;
}

const PlacementRule* placement_rule(SourceClass source) {
  for (const PlacementRule& rule : kPlacementRules) {
    if (rule.source == source) return &rule;
  }
  return nullptr;
}

// Placement reads the class and nothing else, so a track without a trustworthy
// class is left alone. Polarity and delay deliberately do not consult this:
// a cancellation is measured between two signals, whatever they turn out to be.
bool is_placeable(const TrackProfile& profile) {
  return profile.usable && placement_rule(profile.source) != nullptr &&
         profile.source_confidence >= kMinPlacementConfidence;
}

// Offset of the `ordinal`-th member of a class, as a fraction of the class
// extent. Members alternate left, right, left, ... and step inwards every pair,
// so a class of two is symmetric about the centre and every further pair sits
// inside the previous one. An odd member count cannot balance exactly; the
// unpaired track lands nearest the centre, where the imbalance costs least.
//
// The ordinal is the track's position among its class in the caller's
// `profiles` order and comes from nowhere else, so the same tracks in the same
// order always produce the same image.
float spread_fraction(int ordinal, int member_count) {
  if (member_count <= 1) {
    // The side is fixed rather than chosen: with one member there is no
    // symmetry to preserve, and picking a side from the material would be the
    // frequency-to-position mapping this module exists to avoid.
    return -kLoneMemberFraction;
  }
  const int pair = ordinal / 2;
  const float magnitude = 1.0f / static_cast<float>(pair + 1);
  return (ordinal % 2 == 0) ? -magnitude : magnitude;
}

// Band the track mostly occupies, when the mix is crowded there; -1 otherwise.
// This is the only place crowding enters a placement, and it can only change
// how far a track is spread, never which way.
int crowded_dominant_band(const TrackProfile& profile, const MixProfile& mix) {
  int dominant = -1;
  float best_share = 0.0f;
  for (int band = 0; band < kBandCount; ++band) {
    const float share = profile.band_occupancy[static_cast<std::size_t>(band)];
    if (!std::isfinite(share) || share <= best_share) continue;
    best_share = share;
    dominant = band;
  }
  if (dominant < 0) return -1;
  // The crowding vector is sized by the occupancy analysis; a hand-built or
  // partially filled MixProfile can leave it short or empty.
  if (static_cast<std::size_t>(dominant) >= mix.image.crowded.size()) return -1;
  return mix.image.crowded[static_cast<std::size_t>(dominant)] ? dominant : -1;
}

// The single delay and the single polarity decision kept for one track while
// its pairs are scanned. Both fields are last-writer-wins in the scene, so a
// delta per pair would leave whichever pair was enumerated last in charge; the
// strongest evidence decides instead.
struct AlignmentChoice {
  bool has_delay = false;
  int delay_samples = 0;
  float delay_evidence = 0.0f;
  std::string delay_partner;
  bool invert = false;
  float invert_evidence = 0.0f;
  std::string invert_partner;
};

// The pair with the largest |correlation| wins a track's delay and its polarity
// independently. Ties keep the entry already held, so the earlier pair in
// MixProfile::alignment decides and the outcome never depends on how a
// comparison happens to round.
std::vector<AlignmentChoice> collect_alignment(const std::vector<TrackProfile>& profiles,
                                               const MixProfile& mix) {
  std::vector<AlignmentChoice> choices(profiles.size());
  for (const PairAlignment& pair : mix.alignment) {
    // An unrelated pair is two tracks that happen to be playing at once.
    // Aligning them does nothing useful and can do harm.
    if (!pair.related) continue;
    if (pair.reference_index < 0 || pair.target_index < 0) continue;
    const std::size_t reference = static_cast<std::size_t>(pair.reference_index);
    const std::size_t target = static_cast<std::size_t>(pair.target_index);
    if (reference >= profiles.size() || target >= profiles.size()) continue;
    if (reference == target) continue;
    // Aligning against a track the profiler rejected corrects nothing: the
    // correlation it came from describes silence or an unmeasurable fragment.
    if (!profiles[reference].usable || !profiles[target].usable) continue;

    // A polarity-opposed pair peaks at a negative correlation, so the strength
    // of the evidence is the magnitude either way.
    const float evidence = std::fabs(pair.correlation);

    if (pair.polarity_opposed) {
      // The target is inverted, never the reference. reference_index is always
      // the lower of the two, so one side of every pair is fixed in advance and
      // no track can be flipped twice by two different pairs agreeing.
      AlignmentChoice& choice = choices[target];
      if (!choice.invert || evidence > choice.invert_evidence) {
        choice.invert = true;
        choice.invert_evidence = evidence;
        choice.invert_partner = profiles[reference].strip_id;
      }
    }

    // A pair already in time gets no delta at all. A delta carrying zero
    // samples would read as a decision to align something that never needed it,
    // and would add a line to the explanation saying nothing.
    if (pair.lag_samples != 0) {
      // lag_samples is the lag that aligns the target onto the reference:
      // positive means the target arrives later, so the reference is the side
      // that has to wait; negative is the mirror case. channel_delay_samples
      // cannot be negative, which is exactly why the side has to be resolved
      // here rather than passed through as a signed number.
      const bool delay_reference = pair.lag_samples > 0;
      const std::size_t delayed = delay_reference ? reference : target;
      const std::size_t partner = delay_reference ? target : reference;
      AlignmentChoice& choice = choices[delayed];
      if (!choice.has_delay || evidence > choice.delay_evidence) {
        choice.has_delay = true;
        choice.delay_samples = std::abs(pair.lag_samples);
        choice.delay_evidence = evidence;
        choice.delay_partner = profiles[partner].strip_id;
      }
    }
  }
  return choices;
}

void append_placement(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                      float strength, std::vector<SceneDelta>& deltas) {
  constexpr std::size_t kClassSlots = static_cast<std::size_t>(kSourceClassCount);

  // Two passes: the member count of a class decides the ladder every member of
  // it is placed on, so it has to be known before the first placement is made.
  std::array<int, kClassSlots> member_count{};
  for (const TrackProfile& profile : profiles) {
    if (!is_placeable(profile)) continue;
    ++member_count[static_cast<std::size_t>(profile.source)];
  }

  std::array<int, kClassSlots> next_ordinal{};
  for (const TrackProfile& profile : profiles) {
    if (!is_placeable(profile)) continue;
    const std::size_t slot = static_cast<std::size_t>(profile.source);
    const int ordinal = next_ordinal[slot]++;
    const PlacementRule* rule = placement_rule(profile.source);

    SceneDelta delta;
    delta.domain = DeltaDomain::Image;
    delta.strip_id = profile.strip_id;

    if (rule->centered) {
      // Centred explicitly rather than left alone: this domain applies last, so
      // an earlier domain's pan would otherwise survive as a placement nobody
      // decided here.
      delta.pan = 0.0f;
      delta.reason = "centred " + profile.strip_id + " because a " +
                     source_class_to_string(profile.source) + " belongs at the centre of the image";
      deltas.push_back(std::move(delta));
      continue;
    }

    const int crowded_band = crowded_dominant_band(profile, mix);
    const float boost = crowded_band >= 0 ? kCrowdedSpreadBoost : 1.0f;
    const float fraction = spread_fraction(ordinal, member_count[slot]);
    const float pan =
        std::clamp(fraction * rule->spread_extent * boost * strength, -kMaxAbsPan, kMaxAbsPan);

    delta.pan = pan;
    if (pan == 0.0f) {
      delta.reason = "left " + profile.strip_id +
                     " at the centre because the suggestion strength allows no "
                     "spread";
    } else {
      delta.reason = "panned " + profile.strip_id + " to " + describe_pan(pan) +
                     " to open space around the centred parts";
      if (crowded_band >= 0) {
        delta.reason += ", spread further out because the " +
                        std::string(kBandNames[static_cast<std::size_t>(crowded_band)]) +
                        " band is crowded";
      }
    }
    deltas.push_back(std::move(delta));
  }
}

void append_alignment(const std::vector<TrackProfile>& profiles,
                      const std::vector<AlignmentChoice>& choices,
                      std::vector<SceneDelta>& deltas) {
  for (std::size_t index = 0; index < choices.size(); ++index) {
    const AlignmentChoice& choice = choices[index];
    const std::string& strip_id = profiles[index].strip_id;

    if (choice.invert) {
      SceneDelta delta;
      delta.domain = DeltaDomain::Image;
      delta.strip_id = strip_id;
      // Both channels, so the track's own stereo image is untouched and only
      // its relationship to the reference changes.
      delta.polarity_invert_left = true;
      delta.polarity_invert_right = true;
      delta.reason = "inverted the polarity of both channels of " + strip_id +
                     " because it is opposed to " + choice.invert_partner +
                     ", the reference side of the pair";
      deltas.push_back(std::move(delta));
    }

    if (choice.has_delay) {
      SceneDelta delta;
      delta.domain = DeltaDomain::Image;
      delta.strip_id = strip_id;
      delta.channel_delay_samples = choice.delay_samples;
      delta.reason = "delayed " + strip_id + " by " + std::to_string(choice.delay_samples) +
                     " samples so it lines up with " + choice.delay_partner;
      if (choice.delay_samples > kMaxSuggestedDelaySamples) {
        delta.reason +=
            ", which is beyond the largest delay a scene accepts, so the two will not "
            "fully line up";
      }
      deltas.push_back(std::move(delta));
    }
  }
}

void append_mono_fold(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                      float strength, std::vector<SceneDelta>& deltas) {
  for (const MonoRisk& risk : mix.mono_risks) {
    if (risk.track_index < 0) continue;
    const std::size_t index = static_cast<std::size_t>(risk.track_index);
    if (index >= profiles.size()) continue;
    if (!profiles[index].usable) continue;
    // The index is authoritative. MonoRisk::strip_id is a copy taken when the
    // risk was measured, and a caller assembling a profile by hand can leave
    // the two disagreeing.
    const std::string& strip_id = profiles[index].strip_id;

    if (risk.wide_low_end) {
      util::json::Object params;
      params.emplace("amount", util::json::Value(kMonoMakerAmount));
      params.emplace("frequencyHz", util::json::Value(kMonoMakerCrossoverHz));

      SceneDelta delta;
      delta.domain = DeltaDomain::Image;
      delta.strip_id = strip_id;
      // Pre-fader: the fold repairs the source material, so it belongs before
      // the level decision rather than after it. Emitted at full amount however
      // low the strength is, for the same reason polarity and delay are: a
      // cancellation is not half-wrong at half strength.
      delta.inserts.emplace_back(api::InsertSlot::PreFader, kMonoMakerProcessor,
                                 util::json::dump(util::json::Value(std::move(params))));
      delta.reason = "folded the low end of " + strip_id + " below " +
                     format_rounded(kMonoMakerCrossoverHz) +
                     " Hz to mono because its low end is wide enough to collapse when the mix is "
                     "summed";
      deltas.push_back(std::move(delta));
    }

    // Width is a taste decision rather than a repair, so unlike the fold it
    // follows the suggestion strength.
    const float width = kNeutralWidth + (kNarrowedWidth - kNeutralWidth) * strength;
    SceneDelta delta;
    delta.domain = DeltaDomain::Image;
    delta.strip_id = strip_id;
    delta.width = width;
    if (width < kNeutralWidth) {
      delta.reason = "narrowed " + strip_id + " to " + format_percent(width) +
                     "% width because its stereo image partly cancels when the mix is summed to "
                     "mono";
    } else {
      delta.reason = "left the width of " + strip_id +
                     " alone because the suggestion strength allows no narrowing";
    }
    deltas.push_back(std::move(delta));
  }
}

}  // namespace

std::vector<SceneDelta> decide_image(const std::vector<TrackProfile>& profiles,
                                     const MixProfile& mix, const MixAssistantConfig& config) {
  std::vector<SceneDelta> deltas;
  if (!config.enable_image) return deltas;
  if (profiles.empty()) return deltas;

  // A non-finite strength is a caller error rather than a taste, and clamping it
  // would carry the NaN into every pan. It reads as no strength at all, which
  // leaves the aesthetic decisions unmade while the physical corrections —
  // polarity, delay, the low-end fold — still land.
  const float strength =
      std::isfinite(config.suggestion_strength)
          ? std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength)
          : kMinSuggestionStrength;

  // Fixed emission order: placement, then polarity and delay, then the mono
  // fold. Nothing downstream depends on it, but a stable order is what makes
  // the suggestion reproducible and its explanation readable.
  append_placement(profiles, mix, strength, deltas);
  append_alignment(profiles, collect_alignment(profiles, mix), deltas);
  append_mono_fold(profiles, mix, strength, deltas);
  return deltas;
}

}  // namespace sonare::mixing::assistant
