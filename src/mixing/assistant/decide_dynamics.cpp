/// @file decide_dynamics.cpp
/// @brief Dynamics-processing decisions for the mixing assistant.

#include "mixing/assistant/decide_dynamics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "util/constants.h"
#include "util/json.h"

namespace sonare::mixing::assistant {

using sonare::constants::kFloorDb;

namespace {

namespace json = sonare::util::json;

// --- Eligibility -----------------------------------------------------------

// A silent track's integrated loudness arrives either as -inf (the BS.1770
// convention for a program with no gated block) or pinned at the numerical dB
// floor. Every threshold on this stage is an offset from that measurement, so a
// track without one has nothing to offset from. The headroom mirrors the gain
// stage's: comparing against the floor with == would miss a value that landed a
// hair above it after gating and averaging.
constexpr float kSilenceFloorHeadroomDb = 1.0f;
constexpr float kSilenceLufsThreshold = kFloorDb + kSilenceFloorHeadroomDb;

// Classifier confidence below which the class is a guess rather than a reading.
// Every decision on this stage is selected by class, so acting on a guess means
// putting a de-esser on a hi-hat. The classifier already refuses to label what
// it cannot resolve; this is the second half of the same caution, drawn at the
// midpoint of the [0, 1] confidence range.
constexpr float kMinClassConfidence = 0.5f;

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// invert or exaggerate a ratio rather than weaken it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;

// --- Reason formatting -----------------------------------------------------

// One decimal is the resolution a mixer acts on; more digits make the
// explanation read like a measurement rather than a suggestion.
constexpr int kReasonDecimals = 1;
// A sustain ratio and a dominance ratio are both fractions of one, so they need
// a digit more than a dB figure to say anything.
constexpr int kFractionDecimals = 2;

std::string format_fixed(float value, int decimals) {
  // Negative zero would print as "-0.0" and read as a real downward move.
  if (value == 0.0f) value = 0.0f;
  std::ostringstream out;
  out << std::fixed << std::setprecision(decimals) << value;
  return out.str();
}

std::string format_db(float value) { return format_fixed(value, kReasonDecimals); }

std::string format_fraction(float value) { return format_fixed(value, kFractionDecimals); }

// --- Parameter objects -----------------------------------------------------

// util/json serialises a non-finite double as `null`, and the insert factory
// reads a params object expecting numbers, so a value that slipped through as
// NaN or infinity would arrive as a malformed parameter rather than a bad one.
// Dropping it instead leaves the processor on its own default, which is the
// recoverable outcome.
void put_number(json::Object& params, const char* key, float value) {
  if (!std::isfinite(value)) return;
  params[key] = json::Value(value);
}

void put_bool(json::Object& params, const char* key, bool value) {
  params[key] = json::Value(value);
}

std::string dump_params(json::Object params) { return json::dump(json::Value(std::move(params))); }

// --- Dynamics profile response ---------------------------------------------

// Crest factor of a moderately dynamic, already-controlled mix element. A track
// measuring above it still has its transients intact and wants a later, gentler
// hand; one below it arrives pre-compressed and can take a firmer one. The
// value is the middle of the range a tracked acoustic part usually lands in,
// and it is a reference point rather than a target: nothing tries to move a
// track towards it.
constexpr float kReferenceCrestDb = 12.0f;
// Largest crest-factor deviation acted on, in dB. A reading far off the
// reference is usually one stray peak or one dense passage rather than a
// consistent property of the part, so the response saturates instead of
// following it out.
constexpr float kMaxCrestDeviationDb = 12.0f;

// Ratio softening per dB of crest-factor excess. A track whose peaks stand well
// above its body is already carrying its own dynamics; compressing it at the
// class's nominal ratio would flatten exactly what makes it read. At the
// saturation point this moves a 3:1 to 2.4:1, which is one notch on a console,
// not a different decision.
constexpr float kRatioPerCrestDb = -0.05f;
// Attack lengthening per dB of crest-factor excess, as a fraction of the
// class's nominal attack. A transient that stands further above the body needs
// longer to get past the detector before the gain starts moving, or the
// compressor removes the leading edge instead of controlling the level. At the
// saturation point the attack runs 1.6x the nominal one and a pre-compressed
// track's runs 0.4x.
constexpr float kAttackScalePerCrestDb = 0.05f;
// Threshold lift per dB of crest-factor excess. The threshold is placed against
// the average level, so on a spiky part the peaks sit far above it and the
// compressor would spend the whole track in gain reduction. Following half the
// excess keeps it working on the peaks; following all of it would make the
// compressor inert on exactly the material that needs it.
constexpr float kThresholdPerCrestDb = 0.5f;

// Release lengthening at a fully sustained track, as a fraction of the class's
// nominal release. A sustained part has no gaps for the gain to recover in, so
// a release sized for a transient part modulates the note itself; doubling it
// at sustain_ratio 1 puts the recovery beyond the note rather than inside it.
constexpr float kReleaseSustainScale = 1.0f;

// Ratio bounds for anything this stage suggests. Below the lower bound the
// compressor is doing nothing a fader could not do; above the upper one it is a
// limiter, which is a different decision made with a different tool.
constexpr float kMinSuggestedRatio = 1.2f;
constexpr float kMaxSuggestedRatio = 8.0f;
// Shortest attack the crest-factor scaling may produce. A zero attack is a
// legal configuration but it removes the leading edge of every event, which is
// never what a suggestion should do on its own.
constexpr float kMinAttackMs = 0.1f;

// Knee width for every suggested compressor, in dB. A suggestion is acted on
// without the user having watched the gain-reduction meter, so the onset of
// compression has to be inaudible rather than exact; a soft knee this wide
// spreads it over a range the material crosses constantly.
constexpr float kCompressorKneeDb = 6.0f;

/// @brief One class's compression starting point, before the dynamics profile
///        adjusts it.
/// @details The threshold is an offset from the track's measured integrated
///          loudness, never an absolute level.
struct CompressionRow {
  SourceClass source;
  float ratio;
  float threshold_offset_db;
  float attack_ms;
  float release_ms;
};

// The per-class table. Every row is a starting point that the measured crest
// factor and sustain ratio then move; the reason each row sits where it does is
// on the row.
constexpr std::array<CompressionRow, 14> kCompressionTable = {{
    // A kick is one short event per beat. The late attack lets the beater click
    // through untouched, and the release has recovered before the next hit at
    // any ordinary tempo.
    {SourceClass::Kick, 4.0f, -6.0f, 12.0f, 120.0f},
    // The same shape as the kick with a slower recovery: a snare's body and the
    // room around it keep decaying long after the stick.
    {SourceClass::Snare, 4.0f, -6.0f, 15.0f, 150.0f},
    // Hats are already short and stand well above their own body. A fast, gentle
    // setting evens out the playing without breathing on the decay.
    {SourceClass::HiHat, 2.0f, -4.0f, 3.0f, 60.0f},
    // Toms have the longest decay on the kit, so the release has to outlast it
    // or the gain audibly pumps inside a single hit.
    {SourceClass::Tom, 3.0f, -6.0f, 15.0f, 200.0f},
    // A cymbal is almost all decay: the gentlest ratio in the table and the
    // longest release, so the wash keeps a steady level instead of surging.
    {SourceClass::Cymbal, 2.0f, -4.0f, 5.0f, 300.0f},
    // The part that has to sit at one level for the whole song. The deepest
    // threshold offset in the table means it is under compression most of the
    // time rather than in bursts, and the late attack keeps each note's edge.
    {SourceClass::Bass, 4.0f, -8.0f, 20.0f, 120.0f},
    // A plucked or strummed part: enough attack for the pick to survive, enough
    // release to follow the strum rather than each individual string.
    {SourceClass::Guitar, 3.0f, -6.0f, 15.0f, 150.0f},
    // Keys carry chords whose voices enter separately, so the recovery is slower
    // than a guitar's and the ratio gentler; a firm one would turn a held chord
    // into a swell.
    {SourceClass::Keys, 2.5f, -6.0f, 15.0f, 200.0f},
    // Bowed material has no transient worth protecting and changes level over a
    // phrase rather than a note, so it gets the slowest attack in the table and
    // a release long enough to leave the phrasing alone.
    {SourceClass::Strings, 2.0f, -5.0f, 30.0f, 300.0f},
    // A lead has to stay in front, so it is treated like a vocal but with a
    // faster attack: a lead line is usually the more transient of the two.
    {SourceClass::Lead, 3.0f, -6.0f, 10.0f, 120.0f},
    // The part a listener tracks through the whole song. A deep threshold offset
    // and a quick attack hold it steady; the short release keeps it moving with
    // the phrase instead of sagging behind it.
    {SourceClass::Vocal, 3.0f, -8.0f, 8.0f, 100.0f},
    // Backing vocals sit behind the lead, so they are compressed harder: the
    // point is that they never step forward, not that they stay expressive.
    {SourceClass::Backing, 4.0f, -8.0f, 8.0f, 120.0f},
    // Hand percussion is short, dense and played quietly between the louder
    // hits. A fast attack and release track the pattern rather than the bar.
    {SourceClass::Percussion, 3.0f, -5.0f, 5.0f, 80.0f},
    // An effect or texture is in the mix for its shape, so it gets the lightest
    // treatment in the table: enough to stop it jumping, not enough to reshape
    // it.
    {SourceClass::Fx, 2.0f, -4.0f, 20.0f, 250.0f},
}};

const CompressionRow* compression_row(SourceClass source) noexcept {
  for (const CompressionRow& row : kCompressionTable) {
    if (row.source == source) return &row;
  }
  return nullptr;
}

// --- Class-specific tools --------------------------------------------------

// Classes a listener hears by their leading edge. What arrives first is the
// hit, so the transient is the part worth a dedicated control; a hi-hat and a
// cymbal are deliberately absent, because shaping the attack of a part that is
// almost entirely decay changes its tone rather than its envelope.
constexpr std::array<SourceClass, 4> kTransientShapedSources = {
    SourceClass::Kick, SourceClass::Snare, SourceClass::Tom, SourceClass::Percussion};

// Classes carrying a sung line, which is what a level rider and a de-esser are
// for. SourceClass::Lead is deliberately absent: the classifier's lead class
// covers a sung line and a lead instrument alike, and a de-esser on a lead synth
// would be working on material that has no sibilance in it at all.
constexpr std::array<SourceClass, 2> kVoiceSources = {SourceClass::Vocal, SourceClass::Backing};

// Classes where a gate is a repair rather than an effect: close-miked kit
// sources, whose problem is the rest of the kit arriving down the same mic.
// Nothing else is gated. See the gate constants below for why the list is this
// short.
constexpr std::array<SourceClass, 3> kGateableSources = {SourceClass::Kick, SourceClass::Snare,
                                                         SourceClass::Tom};

template <std::size_t N>
bool contains_source(const std::array<SourceClass, N>& sources, SourceClass source) noexcept {
  for (const SourceClass candidate : sources) {
    if (candidate == source) return true;
  }
  return false;
}

// Transient shaper. The gains are small on purpose: the shaper runs on top of a
// compressor that has already been sized for the same track, so its job is to
// give back the edge the compressor took, not to redesign the envelope.
constexpr float kTransientAttackGainDb = 2.0f;
// A touch of sustain trim tightens the tail between hits, which is where a
// close-miked drum picks up the rest of the kit.
constexpr float kTransientSustainGainDb = -1.0f;
// Hard bound on how far the shaper may move the gain whatever the detector
// sees. An unexpected transient must not be able to produce a jump larger than
// the suggestion itself describes.
constexpr float kTransientMaxGainDb = 6.0f;

// Vocal rider. The move is bounded well inside the compressor's range: the
// rider handles the slow drift between phrases and the compressor handles the
// syllables, and a rider allowed to move further would start doing both.
constexpr float kRiderMaxMoveDb = 3.0f;
// How far below the track's own program level the rider stops chasing. Breath,
// headphone bleed and room tone all live under here, and a rider that follows
// them pumps the noise up between phrases.
constexpr float kRiderNoiseFloorOffsetDb = -30.0f;

// De-esser. Sibilance concentrates between roughly 6 and 10 kHz for most
// voices; the middle of that span is the least wrong single frequency to
// suggest without measuring the individual singer.
constexpr float kDeEsserFrequencyHz = 6500.0f;
// Threshold offset from the track's program level. Sibilant peaks stand above
// the sung level, so a threshold just under it catches them without the
// de-esser working on ordinary vowels.
constexpr float kDeEsserThresholdOffsetDb = -6.0f;
constexpr float kDeEsserRatio = 3.0f;
// Ceiling on the reduction, so a false trigger dulls the top of the voice for a
// moment instead of removing it.
constexpr float kDeEsserMaxReductionDb = 5.0f;
// kBands[5] is 6-12 kHz and kBands[6] is 12 kHz to Nyquist; together they cover
// the span sibilance occupies.
constexpr int kSibilantFirstBand = 5;
// Share of a track's total energy that has to sit in those bands before a
// de-esser is worth suggesting. A voice recorded with a dark mic, or one that
// has already been de-essed, has little energy up there and does not need the
// insert; the threshold is low because band occupancy is a share of the whole
// spectrum and the top two bands are a small part of any voice's.
constexpr float kSibilanceOccupancyThreshold = 0.03f;

// --- Gate ------------------------------------------------------------------
//
// A gate is the one suggestion here that can remove material rather than shape
// it: a threshold set a few dB too high deletes the quiet hits, and the
// listener hears a missing note rather than a wrong setting. Every constant
// below is therefore drawn on the side of the gate doing nothing.

// Only a class the classifier is nearly certain about is gated. This is well
// above kMinClassConfidence, which governs the shaping decisions; a de-esser on
// a misread track is a wrong tone, a gate on one is a missing part.
constexpr float kGateMinConfidence = 0.85f;
// Gating needs gaps to close in. A sustained part has none, so a gate on it can
// only ever close during the material itself.
constexpr float kGateMaxSustainRatio = 0.25f;
// The hits also have to stand clearly above whatever sits between them, or
// there is no level at which the gate separates the two. This is above
// kReferenceCrestDb: an ordinarily dynamic part is not dynamic enough.
constexpr float kGateMinCrestDb = 14.0f;
// And there have to be discrete events at all. One onset every two seconds is
// the loosest reading that still describes a part being played rather than a
// single sustained gesture.
constexpr float kGateMinOnsetsPerSec = 0.5f;

// Threshold offset from the program level. Deep enough that only material well
// below the part itself can close the gate; bleed from the rest of a kit
// arrives around here, the part does not.
constexpr float kGateThresholdOffsetDb = -20.0f;
// Hysteresis between the opening and the closing threshold, so a signal sitting
// on the threshold does not chatter the gate open and shut.
constexpr float kGateHysteresisDb = 6.0f;
// Fast enough to be open before the transient it was triggered by.
constexpr float kGateAttackMs = 0.5f;
// The gate stays open for the body of a hit before it is allowed to start
// closing, so a decaying drum is never cut off mid-decay.
constexpr float kGateHoldMs = 80.0f;
// And it closes slowly, so the transition reads as the tail ending rather than
// as a gate.
constexpr float kGateReleaseMs = 200.0f;
// The most a suggested gate may attenuate by, as a positive depth. The gate's
// own range_db is negative, so this is negated on the way out. A partial duck
// rather than a mute is the whole conservatism argument: when the gate does
// fire on the wrong material the listener hears the part dip, which is
// recoverable, instead of the part vanishing, which is not.
constexpr float kGateMaxAttenuationDb = 12.0f;

// --- Sidechain -------------------------------------------------------------

// The one key relationship the assistant proposes on its own. A kick and a bass
// are the pair that reliably contend for the same octave in the same moment,
// and ducking the bass under the kick is the standard answer; every other
// ducking relationship is a production choice rather than a mix repair.
constexpr SourceClass kSidechainKeySource = SourceClass::Kick;
constexpr SourceClass kSidechainTargetSource = SourceClass::Bass;

// kBands[0] is 20-60 Hz and kBands[1] is 60-250 Hz. Those two are where a kick
// and a bass actually collide; a pair that only meets higher up is sharing the
// harmonics, which is an EQ decision.
constexpr int kLowBandCount = 2;

// Frames in which both tracks clear the band's energy floor before the pair
// counts as contending. At a 512-sample hop and 48 kHz this is roughly a third
// of a second of the two genuinely sounding together; below it they coincide
// too rarely for a duck to be doing anything except adding movement.
constexpr int kMinSidechainOverlapFrames = 32;
// The key source's share of the band while both sound. 0.5 is parity, so this
// asks for a little more: ducking a bass under a kick that is already the
// quieter of the two just makes the low end smaller without making it clearer.
constexpr float kSidechainDominanceThreshold = 0.55f;

// Ducking amounts. The threshold is an offset from the *key* track's level, not
// the ducked track's, because the detector listens to the key.
constexpr float kDuckingThresholdOffsetDb = -12.0f;
constexpr float kDuckingRatio = 4.0f;
// Fast enough that the space is open by the time the kick's body arrives.
constexpr float kDuckingAttackMs = 5.0f;
// And recovered before the next beat at any ordinary tempo, so the bass is only
// out of the way while the kick is there.
constexpr float kDuckingReleaseMs = 120.0f;
// A few dB of movement clears the collision. Deeper ducking is an audible
// production effect, and an assistant that produces one has made a creative
// decision on the user's behalf.
constexpr float kDuckingMaxDepthDb = 4.0f;

// --- Shared helpers --------------------------------------------------------

/// @brief Scales a compression ratio towards unity by the suggestion strength.
/// @details A ratio is a multiplier around 1, not a level, so it is weakened by
///          moving it towards 1 rather than by multiplying it.
float scale_ratio(float ratio, float strength) noexcept { return 1.0f + (ratio - 1.0f) * strength; }

/// @brief The track's crest factor as a bounded deviation from the reference.
float crest_deviation_db(const TrackProfile& profile) noexcept {
  const float crest = profile.base.loudness.crest_factor_db;
  if (!std::isfinite(crest)) return 0.0f;
  return std::clamp(crest - kReferenceCrestDb, -kMaxCrestDeviationDb, kMaxCrestDeviationDb);
}

/// @brief The track's sustain ratio, clamped to the [0, 1] it is documented as.
float sustain_ratio(const TrackProfile& profile) noexcept {
  const float sustain = profile.base.dynamics.sustain_ratio;
  if (!std::isfinite(sustain)) return 0.0f;
  return std::clamp(sustain, 0.0f, 1.0f);
}

/// @brief True when the track can be treated at all.
/// @details Each condition is tested here rather than deferred to
///          TrackProfile::usable. The profiler makes the same calls, but this
///          stage has to be correct when it is handed a hand-built profile too.
bool is_treatable(const TrackProfile& profile) noexcept {
  if (!profile.usable) return false;
  if (profile.source == SourceClass::Unknown) return false;
  if (!std::isfinite(profile.source_confidence) ||
      profile.source_confidence < kMinClassConfidence) {
    return false;
  }
  const float measured_lufs = profile.base.loudness.integrated_lufs;
  return std::isfinite(measured_lufs) && measured_lufs > kSilenceLufsThreshold;
}

/// @brief Share of the track's energy sitting in the sibilant bands.
float sibilant_occupancy(const TrackProfile& profile) noexcept {
  float total = 0.0f;
  for (int band = kSibilantFirstBand; band < kBandCount; ++band) {
    const float share = profile.band_occupancy[static_cast<std::size_t>(band)];
    if (std::isfinite(share)) total += share;
  }
  return total;
}

/// @brief Remembers which processor already occupies a strip's slot.
/// @details apply_deltas drops a second insert of the same name in the same
///          slot and reports it, so emitting one would turn a decision into a
///          note about a decision. Two decisions can legitimately reach for the
///          same tool on the same strip — a bass can be both the compressed
///          track and the ducked one — so the ledger is checked, not assumed.
class InsertLedger {
 public:
  bool claim(const std::string& strip_id, api::InsertSlot slot, const char* processor_name) {
    std::string key = strip_id;
    key.push_back('\0');
    key.push_back(slot == api::InsertSlot::PreFader ? 'p' : 'P');
    key.push_back('\0');
    key += processor_name;
    return claimed_.insert(std::move(key)).second;
  }

 private:
  std::set<std::string> claimed_;
};

/// @brief Builds a dynamics delta carrying exactly one insert.
SceneDelta make_insert_delta(const TrackProfile& profile, api::InsertSlot slot,
                             const char* processor_name, json::Object params, std::string reason,
                             std::string sidechain_key = {}) {
  SceneDelta delta;
  delta.domain = DeltaDomain::Dynamics;
  delta.strip_id = profile.strip_id;
  delta.reason = std::move(reason);
  delta.inserts.emplace_back(slot, processor_name, dump_params(std::move(params)),
                             std::move(sidechain_key));
  return delta;
}

// --- Per-track decisions ---------------------------------------------------

void decide_compressor(const TrackProfile& profile, float strength, InsertLedger& ledger,
                       std::vector<SceneDelta>& deltas) {
  const CompressionRow* row = compression_row(profile.source);
  if (row == nullptr) return;
  if (!ledger.claim(profile.strip_id, api::InsertSlot::PreFader, "dynamics.compressor")) return;

  const float crest = crest_deviation_db(profile);
  const float sustain = sustain_ratio(profile);

  const float ratio = scale_ratio(
      std::clamp(row->ratio + crest * kRatioPerCrestDb, kMinSuggestedRatio, kMaxSuggestedRatio),
      strength);
  const float threshold_db = profile.base.loudness.integrated_lufs + row->threshold_offset_db +
                             crest * kThresholdPerCrestDb;
  const float attack_ms =
      std::max(kMinAttackMs, row->attack_ms * (1.0f + crest * kAttackScalePerCrestDb));
  const float release_ms = row->release_ms * (1.0f + sustain * kReleaseSustainScale);

  json::Object params;
  put_number(params, "thresholdDb", threshold_db);
  put_number(params, "ratio", ratio);
  put_number(params, "attackMs", attack_ms);
  put_number(params, "releaseMs", release_ms);
  put_number(params, "kneeDb", kCompressorKneeDb);
  // The gain stage has already put the track on its loudness target, so the
  // compressor gives back what it takes rather than leaving the track sitting
  // lower than the stage that ran before it decided.
  put_bool(params, "autoMakeup", true);

  const std::string reason = "compressed " + profile.strip_id + " at " + format_db(ratio) +
                             ":1 above " + format_db(threshold_db) + " dB, with the " +
                             format_db(attack_ms) + " ms attack and " + format_db(release_ms) +
                             " ms release its " + format_db(profile.base.loudness.crest_factor_db) +
                             " dB crest factor and " + format_fraction(sustain) +
                             " sustain ratio call for";
  deltas.push_back(make_insert_delta(profile, api::InsertSlot::PreFader, "dynamics.compressor",
                                     std::move(params), reason));
}

void decide_transient_shaper(const TrackProfile& profile, float strength, InsertLedger& ledger,
                             std::vector<SceneDelta>& deltas) {
  if (!contains_source(kTransientShapedSources, profile.source)) return;
  if (!ledger.claim(profile.strip_id, api::InsertSlot::PreFader, "dynamics.transientShaper")) {
    return;
  }

  const float attack_gain_db = kTransientAttackGainDb * strength;
  const float sustain_gain_db = kTransientSustainGainDb * strength;

  json::Object params;
  put_number(params, "attackGainDb", attack_gain_db);
  put_number(params, "sustainGainDb", sustain_gain_db);
  put_number(params, "maxGainDb", kTransientMaxGainDb);

  const std::string reason = "shaped " + profile.strip_id + " with " + format_db(attack_gain_db) +
                             " dB of attack and " + format_db(sustain_gain_db) +
                             " dB of sustain, bounded to " + format_db(kTransientMaxGainDb) +
                             " dB of movement, because a percussive part reads by its leading edge";
  deltas.push_back(make_insert_delta(profile, api::InsertSlot::PreFader, "dynamics.transientShaper",
                                     std::move(params), reason));
}

void decide_vocal_rider(const TrackProfile& profile, const MixAssistantConfig& config,
                        float strength, InsertLedger& ledger, std::vector<SceneDelta>& deltas) {
  if (!contains_source(kVoiceSources, profile.source)) return;
  if (!ledger.claim(profile.strip_id, api::InsertSlot::PreFader, "dynamics.vocalRider")) return;

  // The rider aims at the same absolute level the gain stage staged towards, so
  // the two agree on where the track belongs. A configuration without a real
  // target leaves the rider holding the track at its own measured level.
  const float target_db = std::isfinite(config.target_track_lufs)
                              ? config.target_track_lufs
                              : profile.base.loudness.integrated_lufs;
  const float max_move_db = kRiderMaxMoveDb * strength;

  json::Object params;
  put_number(params, "targetDb", target_db);
  put_number(params, "maxBoostDb", max_move_db);
  put_number(params, "maxCutDb", max_move_db);
  put_number(params, "noiseFloorDb",
             profile.base.loudness.integrated_lufs + kRiderNoiseFloorOffsetDb);
  // A stereo voice detected channel by channel would be ridden by two
  // independent gains, which pulls its image apart as it moves.
  put_bool(params, "linkedDetection", true);

  const std::string reason = "rode " + profile.strip_id + " towards " + format_db(target_db) +
                             " dB within " + format_db(max_move_db) +
                             " dB, so the voice keeps its place between phrases without the "
                             "compressor doing the whole job";
  deltas.push_back(make_insert_delta(profile, api::InsertSlot::PreFader, "dynamics.vocalRider",
                                     std::move(params), reason));
}

void decide_deesser(const TrackProfile& profile, float strength, InsertLedger& ledger,
                    std::vector<SceneDelta>& deltas) {
  if (!contains_source(kVoiceSources, profile.source)) return;
  const float sibilance = sibilant_occupancy(profile);
  if (sibilance < kSibilanceOccupancyThreshold) return;
  if (!ledger.claim(profile.strip_id, api::InsertSlot::PreFader, "dynamics.deesser")) return;

  const float ratio = scale_ratio(kDeEsserRatio, strength);
  const float threshold_db = profile.base.loudness.integrated_lufs + kDeEsserThresholdOffsetDb;
  const float range_db = kDeEsserMaxReductionDb * strength;

  json::Object params;
  put_number(params, "frequencyHz", kDeEsserFrequencyHz);
  put_number(params, "thresholdDb", threshold_db);
  put_number(params, "ratio", ratio);
  put_number(params, "rangeDb", range_db);

  const std::string reason =
      "de-essed " + profile.strip_id + " at " + format_db(kDeEsserFrequencyHz) + " Hz above " +
      format_db(threshold_db) + " dB by no more than " + format_db(range_db) + " dB, because " +
      format_fraction(sibilance) + " of its energy sits in the sibilant bands";
  deltas.push_back(make_insert_delta(profile, api::InsertSlot::PreFader, "dynamics.deesser",
                                     std::move(params), reason));
}

void decide_gate(const TrackProfile& profile, float strength, InsertLedger& ledger,
                 std::vector<SceneDelta>& deltas) {
  if (!contains_source(kGateableSources, profile.source)) return;
  if (profile.source_confidence < kGateMinConfidence) return;
  if (sustain_ratio(profile) > kGateMaxSustainRatio) return;
  const float crest = profile.base.loudness.crest_factor_db;
  if (!std::isfinite(crest) || crest < kGateMinCrestDb) return;
  const float attack_density = profile.base.dynamics.attack_density;
  if (!std::isfinite(attack_density) || attack_density < kGateMinOnsetsPerSec) return;
  if (!ledger.claim(profile.strip_id, api::InsertSlot::PreFader, "dynamics.gate")) return;

  const float threshold_db = profile.base.loudness.integrated_lufs + kGateThresholdOffsetDb;
  // The gate's range is an attenuation, so it is negative; scaling it towards
  // zero is what a weaker suggestion means here.
  const float range_db = -kGateMaxAttenuationDb * strength;

  json::Object params;
  put_number(params, "thresholdDb", threshold_db);
  put_number(params, "closeThresholdDb", threshold_db - kGateHysteresisDb);
  put_number(params, "attackMs", kGateAttackMs);
  put_number(params, "holdMs", kGateHoldMs);
  put_number(params, "releaseMs", kGateReleaseMs);
  put_number(params, "rangeDb", range_db);

  const std::string reason =
      "gated " + profile.strip_id + " below " + format_db(threshold_db) + " dB by no more than " +
      format_db(-range_db) + " dB, which attenuates the bleed between its " +
      format_fraction(attack_density) + " hits per second rather than muting anything";
  deltas.push_back(make_insert_delta(profile, api::InsertSlot::PreFader, "dynamics.gate",
                                     std::move(params), reason));
}

// --- Sidechain -------------------------------------------------------------

/// @brief The strongest low-band contention between a key and a target track.
/// @details Returns the band index with the highest dominance among the bands
///          that pass both tests, or -1 when the pair does not contend.
int contended_low_band(const MixProfile& mix, int key_index, int target_index) noexcept {
  int best_band = -1;
  float best_ratio = kSidechainDominanceThreshold;
  for (int band = 0; band < kLowBandCount; ++band) {
    const BandDominance dominance = mix.dominance_at(key_index, target_index, band);
    if (dominance.valid_frames < kMinSidechainOverlapFrames) continue;
    if (dominance.ratio < best_ratio) continue;
    best_ratio = dominance.ratio;
    best_band = band;
  }
  return best_band;
}

void decide_sidechain(const std::vector<TrackProfile>& profiles, const MixProfile& mix,
                      float strength, InsertLedger& ledger, std::vector<SceneDelta>& deltas) {
  // The dominance matrix is indexed by position in the profile vector, so a mix
  // profile describing a different number of tracks describes different tracks.
  if (mix.track_count != static_cast<int>(profiles.size())) return;

  for (std::size_t target = 0; target < profiles.size(); ++target) {
    const TrackProfile& ducked = profiles[target];
    if (ducked.source != kSidechainTargetSource || !is_treatable(ducked)) continue;

    // At most one duck per ducked track. Two keys pointing at the same strip
    // would be two copies of the same processor in the same slot, and the
    // second is dropped rather than summed.
    int best_key = -1;
    int best_band = -1;
    float best_ratio = 0.0f;
    for (std::size_t key = 0; key < profiles.size(); ++key) {
      if (key == target) continue;
      const TrackProfile& keying = profiles[key];
      if (keying.source != kSidechainKeySource || !is_treatable(keying)) continue;
      const int band = contended_low_band(mix, static_cast<int>(key), static_cast<int>(target));
      if (band < 0) continue;
      const float ratio =
          mix.dominance_at(static_cast<int>(key), static_cast<int>(target), band).ratio;
      if (ratio <= best_ratio) continue;
      best_ratio = ratio;
      best_key = static_cast<int>(key);
      best_band = band;
    }
    if (best_key < 0) continue;

    const TrackProfile& keying = profiles[static_cast<std::size_t>(best_key)];
    if (!ledger.claim(ducked.strip_id, api::InsertSlot::PreFader, "dynamics.duckingProcessor")) {
      continue;
    }

    const float ratio = scale_ratio(kDuckingRatio, strength);
    const float depth_db = kDuckingMaxDepthDb * strength;

    json::Object params;
    put_number(params, "thresholdDb",
               keying.base.loudness.integrated_lufs + kDuckingThresholdOffsetDb);
    put_number(params, "ratio", ratio);
    put_number(params, "attackMs", kDuckingAttackMs);
    put_number(params, "releaseMs", kDuckingReleaseMs);
    put_number(params, "rangeDb", depth_db);

    const std::string reason =
        "ducked " + ducked.strip_id + " by up to " + format_db(depth_db) + " dB under " +
        keying.strip_id + ", which carries " + format_fraction(best_ratio) + " of the " +
        kBandNames[static_cast<std::size_t>(best_band)] + " band whenever the two sound together";
    deltas.push_back(make_insert_delta(ducked, api::InsertSlot::PreFader,
                                       "dynamics.duckingProcessor", std::move(params), reason,
                                       keying.strip_id));
  }
}

}  // namespace

std::vector<SceneDelta> decide_dynamics(const std::vector<TrackProfile>& profiles,
                                        const MixProfile& mix, const MixAssistantConfig& config) {
  std::vector<SceneDelta> deltas;
  if (!config.enable_dynamics) return deltas;

  const float strength =
      std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength);
  InsertLedger ledger;

  for (const TrackProfile& profile : profiles) {
    if (!is_treatable(profile)) continue;
    decide_compressor(profile, strength, ledger, deltas);
    decide_transient_shaper(profile, strength, ledger, deltas);
    decide_vocal_rider(profile, config, strength, ledger, deltas);
    decide_deesser(profile, strength, ledger, deltas);
    decide_gate(profile, strength, ledger, deltas);
  }

  // Last, so a duck is read against a chain whose per-track decisions are
  // already in place.
  decide_sidechain(profiles, mix, strength, ledger, deltas);

  return deltas;
}

}  // namespace sonare::mixing::assistant
