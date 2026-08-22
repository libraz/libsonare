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

// Where to look for residue in each class, not a decision to filter it. Each
// corner is placed just under the lowest note the class ordinarily plays, which
// is the frequency below which stand rumble, handling noise, proximity boost and
// room separate from the part itself.
//
// Whether anything worth removing is actually down there is a measurement, taken
// against the share thresholds below. The corner describes the class, not the
// take: a part written under its class's usual register is playing real material
// there, and a rule that reads only the label removes it.
//
// Kick and bass have no corner at all. Their fundamentals live inside the swept
// range and they are what the low end is made of, so there is nothing to look
// under.
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

// How much of a band a part has to carry before the band is what the part is
// built around, and how little before the part demonstrably does not depend on
// it. Both are shares of the track's own energy, so both are read against the
// 0.143 an even spread over the seven bands would put in each one.
//
// That a spectrum divides into regions essential and non-essential to a part is
// ordinary recording practice rather than anything derived here; see Izhaki,
// "Mixing Audio: Concepts, Practices and Tools", Focal Press, 2008; Senior,
// "Mixing Secrets for the Small Studio", 2011; and Owsinski, "The Mixing
// Engineer's Handbook". How it is measured is this module's own: the share of
// its own energy the track already puts in the band.
//
// Essential is set at nearly three times an even share, and deliberately above
// the 0.33 a part spread across three bands carries in each of them. Such a part
// is exactly the one that can afford to give a band up, and a threshold that
// called it essential everywhere would mean it never could.
constexpr float kEssentialBandShare = 0.40f;

// Non-essential is half an even share. There is content in the band, but a few
// dB out of one carrying a fourteenth of the part cannot change what the part
// is.
constexpr float kNonEssentialBandShare = 0.07f;

// The gap between the two is deliberate, not an oversight. A band that sits
// between them is neither clearly needed nor clearly disposable, and that is a
// real answer: pushing it to one side or the other would invent a decision the
// measurement does not support, and the invention would be spent removing
// somebody's material.
static_assert(kNonEssentialBandShare < kEssentialBandShare,
              "a band must be able to be neither essential nor disposable");

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

// Share of a track's energy that has to sit below its corner before a high-pass
// is proposed. Under this there is nothing down there to remove, and the insert
// would do nothing but make the suggestion look busier than it is.
constexpr float kMinLowEnergyShare = 0.005f;

// Share above which the content under the corner is the part's own material
// rather than residue, and filtering it would take real signal out. This is the
// case a class-only rule gets wrong: an instrument played or written below its
// class's usual register still reads as that class, and the corner it inherits
// then sits over notes it is actually playing. A tenth of the track's energy is
// far more than rumble and room can account for.
constexpr float kMaxLowEnergyShare = 0.10f;

// The top analysis band runs to Nyquist, so it has no geometric centre that
// stays put across sample rates. 16 kHz sits inside the band at every rate the
// library runs at and is where the air region is conventionally addressed.
constexpr float kAirBandCenterHz = 16000.0f;

// Returned when a cut's centre could not be measured. Negative rather than zero
// so it cannot be mistaken for a frequency; every real answer is positive.
constexpr float kNoMeasuredCenter = -1.0f;

// Width the overlap curve is smoothed to before its peak is taken, in octaves.
// A raw spectrum's single loudest bin is noise rather than a resonance, and a
// filter placed on it would move between renders for reasons nobody can hear.
//
// Expressed in octaves rather than in bins because a fixed count means something
// different in every band: at the default geometry the sub band holds barely two
// bins while the air band holds five hundred. A sixth of an octave is the width
// a spectrum is conventionally smoothed to when the question asked of it is
// where a resonance sits — wide enough that a lone bin cannot carry the answer,
// narrow enough to still separate two features inside one analysis band.
//
// The width is resolved once per band, from that band's own centre, and is the
// same for every candidate inside it. A width that grew with the candidate would
// not be a smoothing at all: the narrowest window that still covers a feature
// gives the highest average, so the peak slides down to whichever candidate
// first reaches the feature instead of sitting on it.
constexpr float kSmoothingOctaves = 1.0f / 6.0f;

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

// A share in [0, 1] read back as the percentage a mixer would say out loud.
std::string format_percent(float share) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(kReasonDecimals) << share * 100.0f;
  return out.str() + "%";
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

/// @brief The band's span, clamped to what the sample rate can actually carry.
struct BandSpan {
  float low_hz = 0.0f;
  float high_hz = 0.0f;
  bool valid = false;
};

// The top band's nominal edge is infinite and the real one is Nyquist. A low
// sample rate does the same thing to an ordinary band: at 8 kHz the high band
// starts above Nyquist and does not exist at all, and a band that reaches past
// Nyquist keeps only the part below it.
BandSpan band_span(int band, int sample_rate) {
  BandSpan span;
  if (sample_rate <= 0) return span;
  const BandEdge edge = kBands[static_cast<std::size_t>(band)];
  span.low_hz = edge.low_hz;
  span.high_hz = std::min(edge.high_hz, 0.5f * static_cast<float>(sample_rate));
  span.valid = span.high_hz > span.low_hz;
  return span;
}

// Frequency inside @p band where the two tracks genuinely share the spectrum, or
// kNoMeasuredCenter when it could not be measured.
//
// The measure is the per-bin minimum of the two tracks' spectra, each normalized
// by its own energy inside this band. That is the height of the overlap between
// the two distributions — the part of the bin both of them really occupy — and
// it is bounded above by the weaker contributor, so a bin only one track sits in
// scores zero.
//
// Two alternatives were rejected. Their sum is the loudest place in the band
// rather than the most contested one: one track alone can win it, which is
// exactly not a collision. Their product does require both, but it multiplies
// the two dynamic ranges together, so a bin where one track has a resonance and
// the other has merely ordinary content outranks a bin where both are strong,
// and the peak then follows the loudest single resonance instead of the overlap.
//
// Normalizing per band rather than per track is what makes the comparison about
// shape: without it the louder track's level decides the minimum everywhere and
// the measure collapses into the quieter track's own spectrum.
float interference_peak_hz(const TrackProfile& victim, const TrackProfile& counterpart, int band) {
  const MeanPowerSpectrum& mine = victim.spectrum;
  const MeanPowerSpectrum& theirs = counterpart.spectrum;
  // Every track in one call is analysed with the same geometry, so a pair that
  // disagrees about it was not measured together and cannot be compared bin for
  // bin. Resampling one onto the other would invent a measurement.
  if (mine.n_fft <= 0 || mine.n_fft != theirs.n_fft || mine.sample_rate <= 0 ||
      mine.sample_rate != theirs.sample_rate) {
    return kNoMeasuredCenter;
  }
  const std::size_t bins =
      std::min({static_cast<std::size_t>(std::max(mine.n_bins, 0)), mine.power.size(),
                static_cast<std::size_t>(std::max(theirs.n_bins, 0)), theirs.power.size()});
  if (bins == 0) return kNoMeasuredCenter;

  const BandSpan span = band_span(band, mine.sample_rate);
  if (!span.valid) return kNoMeasuredCenter;

  const double bin_hz = static_cast<double>(mine.sample_rate) / static_cast<double>(mine.n_fft);
  // Bins whose centre lies inside the span. Bin 0 is DC and falls out of every
  // band on its own, since no band starts at zero.
  const double first_exact = std::ceil(static_cast<double>(span.low_hz) / bin_hz);
  const double last_exact = std::floor(static_cast<double>(span.high_hz) / bin_hz);
  if (!(first_exact <= last_exact) || first_exact < 0.0) return kNoMeasuredCenter;
  const std::size_t first = static_cast<std::size_t>(first_exact);
  if (first >= bins) return kNoMeasuredCenter;
  const std::size_t last = std::min(bins - 1, static_cast<std::size_t>(last_exact));

  double mine_total = 0.0;
  double theirs_total = 0.0;
  for (std::size_t bin = first; bin <= last; ++bin) {
    mine_total += static_cast<double>(mine.power[bin]);
    theirs_total += static_cast<double>(theirs.power[bin]);
  }
  // A band one of them has nothing in is not a band they contest, whatever the
  // dominance matrix said about the pair over the whole track.
  if (!(mine_total > 0.0) || !(theirs_total > 0.0)) return kNoMeasuredCenter;

  std::vector<double> overlap(bins, 0.0);
  for (std::size_t bin = 0; bin < bins; ++bin) {
    overlap[bin] = std::min(static_cast<double>(mine.power[bin]) / mine_total,
                            static_cast<double>(theirs.power[bin]) / theirs_total);
  }

  // Half the smoothing window, in bins, for this band. Taken from the span's
  // geometric centre, so it is one width for the whole band and scales with
  // frequency between bands. At least one bin either side: low down there are
  // not enough bins for a sixth of an octave to mean anything, and three bins is
  // what the smoothing degenerates to there.
  const double half_ratio = std::pow(2.0, 0.5 * static_cast<double>(kSmoothingOctaves)) - 1.0;
  const double center_bin =
      std::sqrt(static_cast<double>(span.low_hz) * static_cast<double>(span.high_hz)) / bin_hz;
  const std::size_t half =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(center_bin * half_ratio)));

  // Triangular weights. A rectangular window scores every candidate that happens
  // to contain the whole feature identically, and the tie then falls to
  // iteration order, which is not a decision anybody wrote; weights that fall
  // off with distance put the maximum on the feature itself.
  //
  // They are deliberately not normalized. Every candidate in this band is scored
  // with the same kernel, so the divisor would be a constant and cannot change
  // which one wins. What the shape does decide is how much taller a lone bin has
  // to be than a broad feature to beat it: by a factor of the half width plus
  // one, four times over at the default geometry in the mid band.
  const double weight_step = 1.0 / static_cast<double>(half + 1);

  // The kernel reads whatever the spectrum holds either side, including bins
  // outside the band, while only in-band bins may win. Truncating it at the band
  // edge instead would score a rim bin on fewer neighbours than an interior one
  // and hand it the peak on nothing but arithmetic.
  double best = 0.0;
  std::size_t best_bin = first;
  bool found = false;
  for (std::size_t bin = first; bin <= last; ++bin) {
    const std::size_t from = bin > half ? bin - half : 0;
    const std::size_t to = std::min(bins - 1, bin + half);
    double score = 0.0;
    for (std::size_t index = from; index <= to; ++index) {
      const std::size_t distance = index > bin ? index - bin : bin - index;
      score += overlap[index] * (1.0 - static_cast<double>(distance) * weight_step);
    }
    if (!found || score > best) {
      best = score;
      best_bin = bin;
      found = true;
    }
  }
  // Every candidate scored zero, so the two tracks share no bin in this band and
  // there is no overlap to point the filter at.
  if (!(best > 0.0)) return kNoMeasuredCenter;

  const float peak_hz = static_cast<float>(static_cast<double>(best_bin) * bin_hz);
  // The candidates were taken from inside the span, so this cannot leave it. The
  // clamp is what makes that a guarantee rather than a property of the loop:
  // the band that justified the cut and the place the filter lands have to stay
  // the same thing.
  return std::clamp(peak_hz, span.low_hz, span.high_hz);
}

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
  /// @brief True when @ref center_hz is the measured overlap rather than the
  ///        band's geometric centre.
  bool center_measured = false;
  /// @brief Strip id of the part the cut is making room for.
  std::string counterpart_id;
  /// @brief Share of its own energy the counterpart puts in this band.
  float counterpart_share = 0.0f;
  /// @brief Share of its own energy the track being carved puts in this band.
  float victim_share = 0.0f;
};

/// @brief The deepest cut one track has been asked for in one band.
/// @details The counterpart travels with the depth because the centre frequency
///          is a property of the pair, not of the band: it is the place where
///          this victim and the part it is making room for actually share the
///          spectrum. Keeping only the depth would leave the second stage
///          guessing which of several collisions it is placing a filter for.
struct BandRequest {
  float depth_db = 0.0f;
  int counterpart = -1;
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

// The parenthesis says what kind of number the frequency is and why this band
// rather than another. A reader who sees "1247 Hz (mid)" with no qualifier can
// take the figure for a label attached to the band rather than for a place that
// was measured, and the two now mean different things: only the fallback lands
// on the grid. The two shares are what the band was chosen on, so a reader can
// see that the other part is built around it and this one is not.
std::string describe_cuts(const std::vector<BandCut>& cuts, const std::string& victim_id) {
  std::string text;
  for (std::size_t index = 0; index < cuts.size(); ++index) {
    if (index != 0) text += (index + 1 == cuts.size()) ? " and " : ", ";
    const BandCut& cut = cuts[index];
    const char* band_name = kBandNames[static_cast<std::size_t>(cut.band)];
    text += format_db(cut.depth_db) + " dB at " + format_hz(cut.center_hz) + " Hz (";
    text += cut.center_measured ? std::string("measured overlap in ") + band_name
                                : std::string(band_name) + " band centre";
    text += ", which " + cut.counterpart_id + " needs at " + format_percent(cut.counterpart_share) +
            " of its energy and " + victim_id + " can spare at " +
            format_percent(cut.victim_share) + " of its own)";
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

  // Deepest cut each track has been asked for in each band, before the ceiling,
  // and the collision that asked for it. A band contested by several tracks is
  // carved once, to the depth the worst of those collisions justifies; stacking
  // one cut per collision is how a track ends up hollowed out by a rule that
  // looked conservative per pair. The worst collision is also the one the cut is
  // placed for, so it is the one whose counterpart is kept.
  std::vector<BandRequest> requested(profiles.size() * static_cast<std::size_t>(kBandCount));

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
        // The victim is chosen by role priority, so it is *not* necessarily the
        // maskee: the counterpart is whichever of the pair is not giving way.
        // Reading these two the other way round would carve the opposite track
        // in every collision, and nothing downstream could tell.
        const int counterpart = victim == masker ? maskee : masker;

        // Dominance says the two parts are in each other's way; it does not say
        // the band is worth taking from this one and giving to that one. The cut
        // is proposed only when the part being made room for is built around the
        // band and the part giving way is not. Without this test a track can be
        // carved in the band that is the whole point of the part, for a
        // counterpart that has almost nothing there — measured on this repo's own
        // fixtures, every cut the stage produced was of exactly that kind.
        //
        // Both tests are written the positive way and negated, so a band that is
        // neither clearly essential nor clearly disposable falls out here rather
        // than passing one of them by default.
        if (!(occupancy_at(profiles[static_cast<std::size_t>(counterpart)], band) >=
              kEssentialBandShare)) {
          continue;
        }
        if (!(occupancy_at(profiles[static_cast<std::size_t>(victim)], band) <=
              kNonEssentialBandShare)) {
          continue;
        }

        const float depth = (entry.ratio - kInterferenceRatio) * kCutDbPerRatioExcess * strength;
        BandRequest& slot =
            requested[static_cast<std::size_t>(victim) * static_cast<std::size_t>(kBandCount) +
                      static_cast<std::size_t>(band)];
        // Strictly greater, so an equally deep collision found later does not
        // displace the counterpart already recorded. Ties would otherwise be
        // broken by the iteration order, which is not a decision anyone wrote.
        if (depth > slot.depth_db) {
          slot.depth_db = depth;
          slot.counterpart = counterpart;
        }
      }
    }
  }

  for (int track = 0; track < track_count; ++track) {
    if (considered[static_cast<std::size_t>(track)] == 0) continue;
    const TrackProfile& profile = profiles[static_cast<std::size_t>(track)];

    // The corner actually inserted, which is kNoHighPass whenever the filter was
    // not proposed. The peaking-cut loop below reads this rather than the class
    // table: a band is only redundant if something really did remove it.
    float high_pass_hz = kNoHighPass;
    // Switched off, the measurement is not taken at all rather than taken and
    // thrown away.
    if (config.enable_high_pass) {
      const float corner = source_high_pass_hz(profile.source);
      if (corner > kNoHighPass) {
        const float share = profile.spectrum.energy_share_below(corner);
        // Both bounds written as positive tests, so a share that is not a real
        // number proposes nothing rather than passing one of them by default.
        if (share >= kMinLowEnergyShare && share <= kMaxLowEnergyShare) {
          high_pass_hz = corner;
          SceneDelta delta;
          delta.domain = DeltaDomain::Eq;
          delta.strip_id = profile.strip_id;
          delta.inserts.emplace_back(api::InsertSlot::PreFader, kCutFilterProcessorName,
                                     high_pass_params_json(high_pass_hz));
          delta.reason = "high-passed " + profile.strip_id + " at " + format_hz(high_pass_hz) +
                         " Hz, where " + format_percent(share) +
                         " of its energy sits below the corner and reads as residue rather than "
                         "material the part is made of";
          deltas.push_back(std::move(delta));
        }
      }
    }

    std::vector<BandCut> cuts;
    for (int band = 0; band < kBandCount; ++band) {
      // A peaking cut inside a range the high-pass has already removed changes
      // nothing and only makes the suggestion look busier.
      if (band_high_hz(band) <= high_pass_hz) continue;
      const BandRequest& request =
          requested[static_cast<std::size_t>(track) * static_cast<std::size_t>(kBandCount) +
                    static_cast<std::size_t>(band)];
      const float wanted = request.depth_db;
      if (wanted <= 0.0f) continue;
      const float depth = std::min(wanted, max_cut_db);
      if (depth < kMinAudibleCutDb) continue;
      // The counterpart is written into the slot together with the depth, so a
      // positive depth always carries one. Tested rather than assumed, because
      // everything below reads the counterpart's profile.
      if (request.counterpart < 0) continue;
      BandCut cut;
      cut.band = band;
      cut.depth_db = depth;
      cut.ceiling_reached = wanted > max_cut_db;
      const TrackProfile& counterpart = profiles[static_cast<std::size_t>(request.counterpart)];
      cut.counterpart_id = counterpart.strip_id;
      cut.counterpart_share = occupancy_at(counterpart, band);
      cut.victim_share = occupancy_at(profile, band);

      // The band decided that there is a collision; where inside it the two
      // parts actually meet is a second, narrower question. A band is up to two
      // octaves wide, so its geometric centre can be an octave away from the
      // overlap the cut was justified by.
      const float measured = interference_peak_hz(profile, counterpart, band);
      cut.center_measured = measured > 0.0f;
      // Nothing measurable — no spectrum, no shared energy, a geometry that
      // cannot map a bin to a frequency — falls back to the band's centre, which
      // is where every cut used to sit.
      cut.center_hz = cut.center_measured ? measured : band_center_hz(band);
      // The band test above only drops a band lying entirely under the corner.
      // Most corners fall *inside* a band — 80 Hz sits in the 60-250 Hz low
      // band — so the band survives and the cut can still land below the filter
      // that was just inserted. There it would change nothing while the
      // explanation announced a correction, which is worse than the busier
      // suggestion the band test exists to avoid.
      if (cut.center_hz <= high_pass_hz) continue;
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
    delta.reason = "carved " + describe_cuts(cuts, profile.strip_id) + " out of " +
                   profile.strip_id + " to make room for the parts it shares those bands with" +
                   describe_ceiling(cuts, max_cut_db);
    deltas.push_back(std::move(delta));
  }

  return deltas;
}

}  // namespace sonare::mixing::assistant
