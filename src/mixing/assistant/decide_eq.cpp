/// @file decide_eq.cpp
/// @brief Static corrective EQ suggestions for the mixing assistant.

#include "mixing/assistant/decide_eq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mastering/eq/cut_filter.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"
#include "util/constants.h"
#include "util/json.h"

namespace sonare::mixing::assistant {

using sonare::constants::kButterworthQ;

namespace {

// Registered insert-factory spellings. A typo in either survives every compile
// and only surfaces when the suggested scene is instantiated, so they are named
// once here rather than repeated at each construction site.
constexpr const char* kParametricProcessorName = "eq.parametric";
constexpr const char* kCutFilterProcessorName = "eq.cutFilter";

// Every peaking cut a track earns is numbered into one ParametricEq, so the
// band grid has to fit inside it. It does so with room to spare, but the two
// numbers live in different modules and this is the only thing tying them
// together.
static_assert(kBandCount <= static_cast<int>(mastering::eq::ParametricEq::kMaxBands),
              "one analysis band per parametric EQ band must fit in a single insert");

/// @brief Role priority: how much of a contested band a source class keeps.
struct SourcePriority {
  SourceClass source;
  int priority;
};

// Ordinary recording practice, written top to bottom as the foreground-to-
// background hierarchy an engineer already works to: when two parts collide in
// a band, the one carrying the song keeps it and the supporting part makes
// room. A lead vocal sits at the top because it is what a listener follows.
// Kick and bass sit high because the low end is the mix's foundation and a hole
// in it is heard as a fault rather than as space. Sustained backdrop material —
// string beds, effect returns — sits at the bottom because it is written to
// fill space and is the part that can afford to give some up.
//
// The numbers are ordinal only. Nothing reads the gaps between them, and they
// are spaced merely so a class can be slotted in later without renumbering.
constexpr std::array<SourcePriority, kSourceClassCount> kSourcePriorities = {{
    {SourceClass::Vocal, 100},
    {SourceClass::Lead, 90},
    {SourceClass::Kick, 85},
    {SourceClass::Snare, 75},
    {SourceClass::Bass, 70},
    {SourceClass::Guitar, 55},
    {SourceClass::Keys, 50},
    {SourceClass::Percussion, 45},
    {SourceClass::Tom, 40},
    {SourceClass::Backing, 35},
    {SourceClass::HiHat, 30},
    {SourceClass::Cymbal, 25},
    {SourceClass::Strings, 20},
    {SourceClass::Fx, 10},
    // Never consulted: an unclassified track is excluded before any priority is
    // read. The row exists so the table covers the enum and the lookup below
    // can never fall through to a guess.
    {SourceClass::Unknown, 0},
}};

/// @brief Corner frequency a source class is high-passed at.
struct SourceHighPass {
  SourceClass source;
  float frequency_hz;
};

// Sentinel for a class whose low end is left alone. Zero rather than an
// optional so the table stays a plain aggregate; no real corner sits at 0 Hz.
constexpr float kNoHighPass = 0.0f;

// A source produces nothing below its lowest fundamental, so what remains down
// there is stand rumble, handling noise, proximity boost and room. Each corner
// is placed just under the lowest note the class ordinarily plays — the routine
// setting, not a measured one — because the point is to remove what the source
// cannot have produced rather than to reshape what it did.
//
// Kick and bass are the two classes whose fundamentals live inside the swept
// range, and they are what the low end is made of, so they are left alone
// entirely.
constexpr std::array<SourceHighPass, kSourceClassCount> kSourceHighPasses = {{
    // Just under E2 (82 Hz), the bottom of an ordinary male range.
    {SourceClass::Vocal, 80.0f},
    // A lead line is written above the bass register whatever plays it.
    {SourceClass::Lead, 80.0f},
    {SourceClass::Kick, kNoHighPass},
    // The shell fundamental sits near 200 Hz; this only removes kick bleed and
    // stand rumble.
    {SourceClass::Snare, 80.0f},
    {SourceClass::Bass, kNoHighPass},
    // Just under the open low E (82 Hz).
    {SourceClass::Guitar, 75.0f},
    // A piano reaches far lower, but a keys part in a full arrangement is not
    // where the bottom octave comes from.
    {SourceClass::Keys, 50.0f},
    // Just under the cello's open C (65 Hz); anything lower is a bass part.
    {SourceClass::Strings, 60.0f},
    // A floor tom's fundamental reaches the low 60s.
    {SourceClass::Tom, 60.0f},
    // Stacked layers only crowd the low mid, and no layer is missed above the
    // corner because the lead carries the line.
    {SourceClass::Backing, 100.0f},
    // Shakers, tambourines and the like produce nothing near the bottom.
    {SourceClass::Percussion, 150.0f},
    // No cymbal rings below this.
    {SourceClass::HiHat, 400.0f},
    {SourceClass::Cymbal, 400.0f},
    // The class covers anything from a sub drop to a noise riser, so there is
    // no lowest fundamental to sit under and no corner that is safe for all of
    // them.
    {SourceClass::Fx, kNoHighPass},
    // Never consulted; see kSourcePriorities.
    {SourceClass::Unknown, kNoHighPass},
}};

// A table missing a class would fall through the lookups below to their
// fallback, which is a decision nobody wrote down. Adding an enumerator without
// a row is a compile error rather than a silently unhandled source.
constexpr bool priorities_cover_every_source_class() {
  for (int value = 0; value < kSourceClassCount; ++value) {
    bool found = false;
    for (std::size_t row = 0; row < kSourcePriorities.size(); ++row) {
      if (static_cast<int>(kSourcePriorities[row].source) == value) found = true;
    }
    if (!found) return false;
  }
  return true;
}

constexpr bool high_passes_cover_every_source_class() {
  for (int value = 0; value < kSourceClassCount; ++value) {
    bool found = false;
    for (std::size_t row = 0; row < kSourceHighPasses.size(); ++row) {
      if (static_cast<int>(kSourceHighPasses[row].source) == value) found = true;
    }
    if (!found) return false;
  }
  return true;
}

static_assert(priorities_cover_every_source_class(), "every source class needs a priority row");
static_assert(high_passes_cover_every_source_class(), "every source class needs a high-pass row");

// Energy share above which a band reads as covered rather than shared. A share
// of 0.5 means the two tracks carry equal band energy and neither is in the
// other's way; 0.65 is roughly a 2:1 energy ratio, about 2.7 dB, which is the
// point where the quieter part stops being audible as a separate line. Placing
// it lower would flag every pair that merely occupies the same region, which
// is most of a mix.
constexpr float kInterferenceRatio = 0.65f;

// Frames where both tracks were actually sounding in the band. Below this the
// pair coincided rather than conflicted, and a standing EQ cut is the wrong
// answer to a passing overlap. At the default 512-sample hop and 48 kHz this
// is roughly a third of a second of real overlap.
constexpr int kMinOverlapFrames = 32;

// Depth a band that is dominated outright (a share of 1) asks for, before the
// configured ceiling is applied. Outright domination is the strongest case the
// measure can report, and even there a 6 dB peaking cut is a corrective move
// rather than a re-voicing of the part.
constexpr float kFullDominanceCutDb = 6.0f;

// Depth is proportional to how far the measured share sits past the threshold,
// so a band that is only just contested is barely touched and only a badly
// buried one gets the full cut.
constexpr float kCutDbPerRatioExcess = kFullDominanceCutDb / (1.0f - kInterferenceRatio);

// Below roughly half a dB a broad peaking cut is under what a listener can pick
// out, so proposing it adds a band that does nothing but make the suggestion
// look busier than it is.
constexpr float kMinAudibleCutDb = 0.5f;

// Bandwidth of a suggested cut. The analysis bands are between one and two
// octaves wide, and a Q near 1.2 spans about one octave: wide enough to address
// the band, narrow enough that it does not reach into its neighbours and turn a
// local correction into a tone change.
//
// Sitting on the narrow side is also what the practice supports. A survey of
// mixing best practices tested the claim that cuts are made with a higher Q
// than boosts and found it holds — P. Pestana and J. D. Reiss, "Intelligent
// Audio Production Strategies Informed by Best Practices", AES 53rd
// International Conference on Semantic Audio, London, 2014. Only cuts are
// suggested here, so the survey bears on this number alone; it says nothing
// about the value beyond placing it above the width a boost would take.
constexpr float kCutQ = 1.2f;

// The top analysis band runs to Nyquist, so it has no geometric centre that
// stays put across sample rates. 16 kHz sits inside the band at every rate the
// library runs at and is where the air region is conventionally addressed.
constexpr float kAirBandCenterHz = 16000.0f;

// Carving a track acts on its classification, so the bar is set above the
// classifier's own acceptance floor: labelling a track wrongly costs a wrong
// word in a report, while cutting it on a wrong label removes real material.
constexpr float kMinSourceConfidence = 0.5f;

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// deepen or invert the cut rather than soften it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;

// One decimal is the resolution a mixer acts on; more digits make the
// explanation read like a measurement rather than a suggestion.
constexpr int kReasonDecimals = 1;

std::string format_db(float value) {
  // Negative zero would print as "-0.0" and read as a real move.
  if (value == 0.0f) value = 0.0f;
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(kReasonDecimals) << value;
  return out.str();
}

std::string format_hz(float value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(0) << value;
  return out.str();
}

int source_priority(SourceClass source) {
  for (const SourcePriority& row : kSourcePriorities) {
    if (row.source == source) return row.priority;
  }
  return 0;
}

float source_high_pass_hz(SourceClass source) {
  for (const SourceHighPass& row : kSourceHighPasses) {
    if (row.source == source) return row.frequency_hz;
  }
  return kNoHighPass;
}

float band_center_hz(int band) {
  const BandEdge edge = kBands[static_cast<std::size_t>(band)];
  if (!std::isfinite(edge.high_hz)) return kAirBandCenterHz;
  return std::sqrt(edge.low_hz * edge.high_hz);
}

float band_high_hz(int band) { return kBands[static_cast<std::size_t>(band)].high_hz; }

// A non-finite share is not a measurement; treating it as zero keeps it out of
// the tie-break instead of letting it win or lose by accident.
float occupancy_at(const TrackProfile& profile, int band) {
  const float share = profile.band_occupancy[static_cast<std::size_t>(band)];
  return std::isfinite(share) ? share : 0.0f;
}

// A track has to be identified before it can be carved: role priority is the
// whole basis of the decision, and applied to a guess it takes real material
// out of a part on the strength of a label nobody stands behind. The same test
// gates both sides of a collision, so an unidentified track neither gives way
// nor makes another track give way.
bool is_considered(const TrackProfile& profile) {
  return profile.usable && profile.source != SourceClass::Unknown &&
         profile.source_confidence >= kMinSourceConfidence;
}

// Returns the track index that gives way in @p band, or -1 when neither should.
int track_that_gives_way(const std::vector<TrackProfile>& profiles, int a, int b, int band) {
  const TrackProfile& first = profiles[static_cast<std::size_t>(a)];
  const TrackProfile& second = profiles[static_cast<std::size_t>(b)];

  const int first_priority = source_priority(first.source);
  const int second_priority = source_priority(second.source);
  if (first_priority != second_priority) return first_priority < second_priority ? a : b;

  // Two parts in the same role, so the tie-break asks which of them the band
  // actually belongs to. The track with the smaller share of its own energy
  // there is not doing its work in that band and is the one that can give it
  // up.
  const float first_share = occupancy_at(first, band);
  const float second_share = occupancy_at(second, band);
  if (first_share != second_share) return first_share < second_share ? a : b;

  // Nothing distinguishes the two parts. Choosing one by index would be a coin
  // flip dressed up as a decision, so neither is carved.
  return -1;
}

/// @brief One suggested peaking cut on one track.
struct BandCut {
  int band = 0;
  float center_hz = 0.0f;
  float depth_db = 0.0f;
  /// @brief True when the configured ceiling, not the measurement, set the depth.
  bool ceiling_reached = false;
};

std::string parametric_params_json(const std::vector<BandCut>& cuts) {
  util::json::Object params;
  for (std::size_t index = 0; index < cuts.size(); ++index) {
    const std::string prefix = "band" + std::to_string(index) + ".";
    params.emplace(prefix + "type",
                   util::json::Value(static_cast<int>(mastering::eq::EqBandType::Peak)));
    params.emplace(prefix + "frequencyHz", util::json::Value(cuts[index].center_hz));
    params.emplace(prefix + "gainDb", util::json::Value(-cuts[index].depth_db));
    params.emplace(prefix + "q", util::json::Value(kCutQ));
    params.emplace(prefix + "enabled", util::json::Value(true));
  }
  return util::json::dump(util::json::Value(std::move(params)));
}

std::string high_pass_params_json(float frequency_hz) {
  util::json::Object params;
  params.emplace("highPassFrequencyHz", util::json::Value(frequency_hz));
  params.emplace("highPassQ", util::json::Value(kButterworthQ));
  params.emplace("highPassSlope",
                 util::json::Value(static_cast<int>(mastering::eq::CutFilterSlope::Db12PerOct)));
  params.emplace("highPassEnabled", util::json::Value(true));
  // The low-pass half is deliberately left out rather than sent disabled: the
  // assistant has no opinion about the top end, and an explicit key would read
  // as one.
  return util::json::dump(util::json::Value(std::move(params)));
}

std::string describe_cuts(const std::vector<BandCut>& cuts) {
  std::string text;
  for (std::size_t index = 0; index < cuts.size(); ++index) {
    if (index != 0) text += (index + 1 == cuts.size()) ? " and " : ", ";
    text += format_db(cuts[index].depth_db) + " dB at " + format_hz(cuts[index].center_hz) +
            " Hz (" + kBandNames[static_cast<std::size_t>(cuts[index].band)] + ")";
  }
  return text;
}

std::string describe_ceiling(const std::vector<BandCut>& cuts, float max_cut_db) {
  std::vector<const char*> held;
  for (const BandCut& cut : cuts) {
    if (cut.ceiling_reached) held.push_back(kBandNames[static_cast<std::size_t>(cut.band)]);
  }
  if (held.empty()) return {};

  std::string bands;
  for (std::size_t index = 0; index < held.size(); ++index) {
    if (index != 0) bands += (index + 1 == held.size()) ? " and " : ", ";
    bands += held[index];
  }
  return "; the " + bands + (held.size() == 1 ? " cut was" : " cuts were") + " held at the " +
         format_db(max_cut_db) + " dB ceiling, so the collision is only partly resolved";
}

}  // namespace

std::vector<SceneDelta> decide_eq(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                                  const MixAssistantConfig& config) {
  std::vector<SceneDelta> deltas;
  if (!config.enable_eq || profiles.empty()) return deltas;

  const int track_count = static_cast<int>(profiles.size());
  const float strength =
      std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength);
  // A ceiling that is not a real number is not a ceiling. Reading it as no
  // headroom keeps an unbounded cut off a strip, which is the failure mode the
  // ceiling exists to prevent.
  const float max_cut_db =
      std::isfinite(config.eq_max_cut_db) ? std::max(config.eq_max_cut_db, 0.0f) : 0.0f;

  std::vector<char> considered(profiles.size(), 0);
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    considered[index] = is_considered(profiles[index]) ? 1 : 0;
  }

  // Deepest cut each track has been asked for in each band, before the ceiling.
  // A band contested by several tracks is carved once, to the depth the worst
  // of those collisions justifies; stacking one cut per collision is how a
  // track ends up hollowed out by a rule that looked conservative per pair.
  std::vector<float> requested_db(profiles.size() * static_cast<std::size_t>(kBandCount), 0.0f);

  for (int masker = 0; masker < track_count; ++masker) {
    if (considered[static_cast<std::size_t>(masker)] == 0) continue;
    for (int maskee = 0; maskee < track_count; ++maskee) {
      if (masker == maskee || considered[static_cast<std::size_t>(maskee)] == 0) continue;
      for (int band = 0; band < kBandCount; ++band) {
        const BandDominance entry = mix.dominance_at(masker, maskee, band);
        if (entry.valid_frames < kMinOverlapFrames) continue;
        // Written as a positive test so a non-finite ratio falls out here
        // rather than propagating into the depth.
        if (!(entry.ratio > kInterferenceRatio)) continue;

        // Energy dominance found the collision; it does not decide who pays for
        // it. The track that gives way is the lower-priority one, which may
        // well be the loud one — a pad burying a vocal is exactly the case an
        // "attenuate the dominant band" rule gets backwards.
        const int victim = track_that_gives_way(profiles, masker, maskee, band);
        if (victim < 0) continue;

        const float depth = (entry.ratio - kInterferenceRatio) * kCutDbPerRatioExcess * strength;
        float& slot =
            requested_db[static_cast<std::size_t>(victim) * static_cast<std::size_t>(kBandCount) +
                         static_cast<std::size_t>(band)];
        slot = std::max(slot, depth);
      }
    }
  }

  for (int track = 0; track < track_count; ++track) {
    if (considered[static_cast<std::size_t>(track)] == 0) continue;
    const TrackProfile& profile = profiles[static_cast<std::size_t>(track)];

    const float high_pass_hz = source_high_pass_hz(profile.source);
    if (high_pass_hz > kNoHighPass) {
      SceneDelta delta;
      delta.domain = DeltaDomain::Eq;
      delta.strip_id = profile.strip_id;
      delta.inserts.emplace_back(api::InsertSlot::PreFader, kCutFilterProcessorName,
                                 high_pass_params_json(high_pass_hz));
      delta.reason = "high-passed " + profile.strip_id + " at " + format_hz(high_pass_hz) +
                     " Hz to clear low-frequency content the source does not produce";
      deltas.push_back(std::move(delta));
    }

    std::vector<BandCut> cuts;
    for (int band = 0; band < kBandCount; ++band) {
      // A peaking cut inside a range the high-pass has already removed changes
      // nothing and only makes the suggestion look busier.
      if (band_high_hz(band) <= high_pass_hz) continue;
      const float wanted =
          requested_db[static_cast<std::size_t>(track) * static_cast<std::size_t>(kBandCount) +
                       static_cast<std::size_t>(band)];
      if (wanted <= 0.0f) continue;
      const float depth = std::min(wanted, max_cut_db);
      if (depth < kMinAudibleCutDb) continue;
      BandCut cut;
      cut.band = band;
      cut.center_hz = band_center_hz(band);
      cut.depth_db = depth;
      cut.ceiling_reached = wanted > max_cut_db;
      cuts.push_back(cut);
    }
    if (cuts.empty()) continue;

    // Every band lands in one insert. A strip drops a second copy of the same
    // processor in the same slot on application, so splitting them would throw
    // a decision away silently.
    SceneDelta delta;
    delta.domain = DeltaDomain::Eq;
    delta.strip_id = profile.strip_id;
    delta.inserts.emplace_back(api::InsertSlot::PreFader, kParametricProcessorName,
                               parametric_params_json(cuts));
    delta.reason = "carved " + describe_cuts(cuts) + " out of " + profile.strip_id +
                   " to make room for the parts it shares those bands with" +
                   describe_ceiling(cuts, max_cut_db);
    deltas.push_back(std::move(delta));
  }

  return deltas;
}

}  // namespace sonare::mixing::assistant
