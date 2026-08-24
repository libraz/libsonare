/// @file decide_structure.cpp
/// @brief Bus, VCA and effect-send topology for the mixing assistant.

#include "mixing/assistant/decide_structure.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The reverb and the stereo delay are built by the insert factory only when the
// optional FX suite is compiled in, and a scene naming an insert the factory
// cannot build fails to load rather than degrading. Everything that exists only
// to propose an effect bus therefore lives behind the same switch, including
// the headers it needs.
#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX

#include "util/constants.h"
#include "util/json.h"
#include "util/number_format.h"
#endif

namespace sonare::mixing::assistant {

#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX
using sonare::constants::kDefaultBpm;
#endif

namespace {

// --- Identifiers -----------------------------------------------------------

// The master bus carries both the conventional id and the role the mixer
// resolves it by. The role is what actually selects it, so a master whose id
// had to be renamed around a track of the same name is still the master.
constexpr const char* kMasterBusId = "master";
constexpr const char* kMasterBusRole = "master";
// A subgroup sums parts that travel together. The other role this stage uses,
// the aux that receives sends, is declared with the effect buses because it is
// reachable only when they are.
constexpr const char* kSubgroupBusRole = "subgroup";
// Appended to a subgroup's bus id to name the VCA group riding it.
constexpr const char* kVcaIdSuffix = "Vca";
// First numeric suffix tried when a generated id is already taken. Counting
// starts at two because the unsuffixed name is the first candidate.
constexpr std::size_t kFirstIdSuffix = 2;

/// @brief Hands out generated identifiers that cannot collide with a track's.
/// @details Strips and buses resolve in one node namespace when the mixer
///          builds its routing graph, so a bus, a VCA group or a return strip
///          sharing an id with a track is a scene that refuses to load rather
///          than a cosmetic clash. Every generated id is allocated here, and a
///          name already in use takes an ascending numeric suffix
///          (`master` -> `master2`). Counting rather than hashing keeps the
///          result the same for the same input.
class IdAllocator {
 public:
  explicit IdAllocator(const std::vector<TrackProfile>& profiles) {
    for (const TrackProfile& profile : profiles) taken_.insert(profile.strip_id);
  }

  std::string allocate(const std::string& preferred) {
    if (taken_.insert(preferred).second) return preferred;
    // One more attempt than there are names in the set, so at least one
    // candidate is guaranteed to be free.
    const std::size_t limit = taken_.size() + kFirstIdSuffix;
    for (std::size_t suffix = kFirstIdSuffix; suffix <= limit; ++suffix) {
      std::string candidate = preferred + std::to_string(suffix);
      if (taken_.insert(candidate).second) return candidate;
    }
    return preferred;  // Unreachable: the loop tries more names than the set holds.
  }

 private:
  std::set<std::string> taken_;
};

// --- Reason formatting -----------------------------------------------------

// "a", "a and b", "a, b and c" — the form the explanation line reads in.
std::string join_names(const std::vector<std::string>& names) {
  std::string out;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index > 0) out += (index + 1 == names.size()) ? " and " : ", ";
    out += names[index];
  }
  return out;
}

// --- Subgroup table --------------------------------------------------------

/// @brief One source class and the subgroup bus it feeds.
struct SubgroupRow {
  SourceClass source;
  /// @brief camelCase bus id, or null when the class gets no subgroup.
  const char* bus_id;
};

// ---------------------------------------------------------------------------
// These groupings are studio convention, not physics.
// ---------------------------------------------------------------------------
// Each row is the bus a part of that class sits on in a customary console or
// session template. Classes share a bus only where the parts are recorded as
// one instrument — the kit mics — and are kept apart everywhere else, because a
// subgroup fader that moves two unrelated families together is worse than no
// subgroup at all. Membership is decided by what the part is and by nothing
// else: no masking figure, no spectral overlap and no cross-track measurement
// takes part.
constexpr std::array<SubgroupRow, static_cast<std::size_t>(kSourceClassCount)> kSubgroupTable = {{
    // An unclassified track joins nothing. Guessing a subgroup puts a part
    // under a fader that will be moved for reasons that have nothing to do
    // with it.
    {SourceClass::Unknown, nullptr},
    // The five kit rows share one bus: they are close mics on a single
    // instrument, and the whole point of a drum bus is that they move together.
    {SourceClass::Kick, "drumBus"},
    {SourceClass::Snare, "drumBus"},
    {SourceClass::HiHat, "drumBus"},
    {SourceClass::Tom, "drumBus"},
    {SourceClass::Cymbal, "drumBus"},
    {SourceClass::Bass, "bassBus"},
    {SourceClass::Guitar, "gtrBus"},
    {SourceClass::Keys, "keysBus"},
    {SourceClass::Strings, "strBus"},
    // A lead line is its own element rather than a member of the vocal stack;
    // it is as often an instrument as a voice, and folding it in would ride a
    // lead synth with the backing vocals.
    {SourceClass::Lead, "leadBus"},
    // The lead voice and the stack behind it are balanced against each other
    // and then moved as one, which is exactly what a vocal bus is for.
    {SourceClass::Vocal, "voxBus"},
    {SourceClass::Backing, "voxBus"},
    // Percussion overdubs are separate instruments from the kit, so they get
    // their own bus rather than joining the close mics.
    {SourceClass::Percussion, "percBus"},
    // Named for sound effects rather than for the effect buses further down;
    // this is a source class, not a return.
    {SourceClass::Fx, "sfxBus"},
}};

// Verifies the table is indexed by SourceClass, so adding a class without a row
// is a build failure rather than a silent absence of routing.
constexpr bool covers_every_class() {
  for (std::size_t index = 0; index < kSubgroupTable.size(); ++index) {
    if (static_cast<std::size_t>(kSubgroupTable[index].source) != index) return false;
  }
  return true;
}
static_assert(covers_every_class(),
              "the subgroup table must hold one row per SourceClass, in enum order");

const char* subgroup_bus_id_for(SourceClass source) noexcept {
  const auto index = static_cast<std::size_t>(source);
  if (index >= kSubgroupTable.size()) return nullptr;
  return kSubgroupTable[index].bus_id;
}

/// @brief A candidate subgroup and the tracks that would feed it.
struct Subgroup {
  /// @brief Id from the table, before collision checking.
  const char* preferred_bus_id = nullptr;
  /// @brief Allocated ids; empty until the subgroup is known to be created.
  std::string bus_id;
  std::string vca_id;
  std::vector<std::string> members;
};

// Candidate subgroups in table order rather than in track order, so the bus
// list a session produces does not depend on which stem the caller loaded
// first.
std::vector<Subgroup> collect_subgroups(const std::vector<TrackProfile>& profiles) {
  std::vector<Subgroup> subgroups;
  for (const SubgroupRow& row : kSubgroupTable) {
    if (row.bus_id == nullptr) continue;
    const bool seen = std::any_of(subgroups.begin(), subgroups.end(), [&](const Subgroup& group) {
      return std::string(group.preferred_bus_id) == row.bus_id;
    });
    if (seen) continue;
    Subgroup group;
    group.preferred_bus_id = row.bus_id;
    subgroups.push_back(std::move(group));
  }

  for (const TrackProfile& profile : profiles) {
    // An unusable track joins no subgroup. The profiler could not measure it,
    // so the class it carries is not a reading, and a part behind a group
    // fader is a part somebody will forget is there.
    if (!profile.usable) continue;
    // Classification confidence is not re-tested the way the level and EQ
    // stages test it. The classifier already reports anything it cannot resolve
    // as Unknown, and the two outcomes are not symmetric here: a part on a
    // plausible-but-wrong subgroup is one drag away from the right one, while a
    // part left off every bus is one a mixer has to notice is missing.
    const char* bus_id = subgroup_bus_id_for(profile.source);
    if (bus_id == nullptr) continue;
    for (Subgroup& group : subgroups) {
      if (std::string(group.preferred_bus_id) == bus_id) {
        group.members.push_back(profile.strip_id);
        break;
      }
    }
  }

  // A subgroup for one part is a second fader for that part: another place for
  // gain to hide, and nothing moves together that was not already together.
  subgroups.erase(std::remove_if(subgroups.begin(), subgroups.end(),
                                 [](const Subgroup& group) {
                                   return group.members.size() <
                                          static_cast<std::size_t>(kMinTracksPerSubgroup);
                                 }),
                  subgroups.end());
  return subgroups;
}

// Returns why the track was not put on a subgroup. Every branch ends in a
// direct connection to master rather than in no connection at all: an unrouted
// strip is silent, and a part disappearing because the classifier could not
// read it is the one failure a mixer would not think to look for.
std::string direct_route_reason(const TrackProfile& profile, const std::string& master_id) {
  if (!profile.usable) {
    return "routed " + profile.strip_id + " straight to " + master_id +
           " because the profiler marked it unusable, so it is heard rather than left unrouted";
  }
  if (subgroup_bus_id_for(profile.source) == nullptr) {
    return "routed " + profile.strip_id + " straight to " + master_id +
           " because it was never classified into a subgroup, so it is heard rather than left "
           "unrouted";
  }
  return "routed " + profile.strip_id + " straight to " + master_id + " because it is the only " +
         source_class_to_string(profile.source) + " part and a subgroup for one part adds nothing";
}

#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX

// --- Effect sends ----------------------------------------------------------

namespace json = sonare::util::json;

// An effect bus receives sends rather than a summed group, which is the role
// the mixer reads to keep it out of the default master routing.
constexpr const char* kAuxBusRole = "aux";

// One decimal is the resolution a mixer acts on; more digits make the
// explanation read like a measurement rather than a suggestion.
constexpr int kReasonDecimals = 1;

std::string format_db(float value) {
  // Negative zero would print as "-0.0" and read as a real downward move.
  if (value == 0.0f) value = 0.0f;
  // The decimal point is a point wherever the host runs. A reason string is read
  // by a person and parsed by nobody, but a comma in the middle of a number
  // reads as a second number.
  return sonare::util::format_fixed(static_cast<double>(value), kReasonDecimals);
}

// Not a level: the sentinel says the class is not fed to that effect at all. A
// send of -60 dB is still a routing decision a mixer can find and raise, and an
// absent one is not, so the two must not be spelled the same way.
constexpr float kNoSendDb = -1000.0f;

/// @brief One source class and how much of it each effect return receives.
struct EffectSendRow {
  SourceClass source;
  float reverb_db;
  float delay_db;
};

// ---------------------------------------------------------------------------
// These send levels are studio convention, not physics.
// ---------------------------------------------------------------------------
// Every value is a customary starting amount from recording practice, in dB
// below the strip's post-fader signal. They are deliberately conservative — a
// place to start from, not a finished effect balance — so nothing downstream may
// depend on an exact number; only the ordering between classes carries meaning.
//
// The reverb column is the one part of that which is pinned rather than chosen.
// A survey of professional practice measures the reverb return sitting about
// 9 LU below the direct sound (Pestana and Reiss, "Intelligent Audio Production
// Strategies Informed by Best Practices", AES 53rd International Conference on
// Semantic Audio, 2014). A send amount is not that relative loudness, so the
// whole column is offset together until a rendered mix measures the return
// there, and the spacing between its rows — the part that carries meaning — is
// left alone. Moving one row changes the ordering; moving the column moves the
// return level.
//
// Two rules run through the whole table. Low-frequency parts are not sent: a
// reverb tail under 150 Hz turns the bottom of the mix to mud, which is why the
// kick and the bass are dry on nearly every record. Broadband noise is not sent
// either: a cymbal or a hi-hat into a reverb comes back as hiss, and the
// overheads already carry the room the kit was recorded in.
constexpr std::array<EffectSendRow, static_cast<std::size_t>(kSourceClassCount)> kEffectSendTable =
    {{
        // Never sent: an unclassified track gets no treatment at all. Present so
        // the table covers the enum and a new class cannot be forgotten.
        {SourceClass::Unknown, kNoSendDb, kNoSendDb},
        // The two foundation parts stay dry; see the note above.
        {SourceClass::Kick, kNoSendDb, kNoSendDb},
        // The classic reverb feed: the backbeat is where the room is heard, and
        // a short slap off the snare is the oldest trick in the delay's book.
        {SourceClass::Snare, -12.5f, -22.0f},
        {SourceClass::HiHat, kNoSendDb, kNoSendDb},
        // Fills read as gestures, so the tail is what makes them land, but a tom
        // is still a low drum and gets less than the snare.
        {SourceClass::Tom, -14.5f, kNoSendDb},
        {SourceClass::Cymbal, kNoSendDb, kNoSendDb},
        {SourceClass::Bass, kNoSendDb, kNoSendDb},
        // A rhythm bed wants depth rather than a tail it would smear itself
        // with, so it takes the smallest reverb send of the tonal parts.
        {SourceClass::Guitar, -20.5f, -20.0f},
        // Comping fills the space between the other parts already; more tail
        // only makes the mids denser.
        {SourceClass::Keys, -18.5f, kNoSendDb},
        // Sustained and already spacious, but a string bed with no tail sits in
        // front of the mix instead of behind it.
        {SourceClass::Strings, -16.5f, kNoSendDb},
        // A focus part just behind the voice, treated the same way and a little
        // less.
        {SourceClass::Lead, -13.5f, -14.0f},
        // The most treated element on a popular record, and the one the effect
        // buses exist for in the first place.
        {SourceClass::Vocal, -10.5f, -14.0f},
        // Pushed further back than the lead voice, which is what the stack is
        // for, so it takes more tail and less repeat.
        {SourceClass::Backing, -14.5f, -20.0f},
        // A colour layer over the kit: enough tail to sit in the same room, not
        // enough to blur the hits.
        {SourceClass::Percussion, -20.5f, kNoSendDb},
        // An effects track normally arrives with its own treatment printed, so
        // adding more would double it.
        {SourceClass::Fx, kNoSendDb, kNoSendDb},
    }};

constexpr bool send_table_covers_every_class() {
  for (std::size_t index = 0; index < kEffectSendTable.size(); ++index) {
    if (static_cast<std::size_t>(kEffectSendTable[index].source) != index) return false;
  }
  return true;
}
static_assert(send_table_covers_every_class(),
              "the effect send table must hold one row per SourceClass, in enum order");

// suggestion_strength is documented as a [0, 1] scale. A value outside it would
// exaggerate or invert a send rather than weaken it, which is not a weaker
// suggestion but a different one.
constexpr float kMinSuggestionStrength = 0.0f;
constexpr float kMaxSuggestionStrength = 1.0f;
// Range the strength control moves a send over, in dB. At full strength the
// table value is used as written; a weaker suggestion pulls every send down by
// up to this much, which is far enough that the effect stops taking part in the
// balance while the routing stays where a mixer can find it.
constexpr float kSendStrengthRangeDb = 24.0f;

// --- Effect bus identifiers and settings -----------------------------------

constexpr const char* kReverbBusId = "reverbBus";
constexpr const char* kReverbReturnId = "reverbReturn";
constexpr const char* kReverbProcessor = "effects.reverb.plate";
constexpr const char* kReverbLabel = "plate reverb";

constexpr const char* kDelayBusId = "delayBus";
constexpr const char* kDelayReturnId = "delayReturn";
constexpr const char* kDelayProcessor = "effects.delay.stereo";
constexpr const char* kDelayLabel = "stereo delay";

// A tail long enough to be heard as a room and short enough not to run into the
// next bar at a moderate tempo. The general-purpose plate setting.
constexpr float kReverbDecaySec = 1.6f;
// Pre-delay keeps the onset of a part clear of its own tail, which is what lets
// a vocal stay intelligible with a reverb on it. Subjective testing across
// professional practice reports a strong benefit in exceeding 30-40 ms
// (Pestana and Reiss, "Intelligent Audio Production Strategies Informed by Best
// Practices", AES 53rd International Conference on Semantic Audio, 2014), so the
// general-purpose setting clears the top of that band. It stops there because a
// gap much beyond this detaches the tail from the source and is heard as a
// separate event rather than as the room the part is sitting in.
constexpr float kReverbPreDelayMs = 45.0f;

// Milliseconds in a minute: the conversion from a tempo in BPM to a beat.
constexpr float kMillisecondsPerMinute = 60000.0f;
// The assistant is handed bare stems with no tempo map, so the delay is voiced
// against the tempo the transport itself falls back to rather than against a
// number invented here.
constexpr float kQuarterNoteMs = kMillisecondsPerMinute / static_cast<float>(kDefaultBpm);
// Three quarters of a beat is a dotted eighth. Setting the two sides of a
// stereo delay to a quarter and a dotted eighth is the customary pairing: the
// repeats interleave instead of landing on top of each other.
constexpr float kDottedEighthPerQuarter = 0.75f;
constexpr float kDottedEighthNoteMs = kQuarterNoteMs * kDottedEighthPerQuarter;
// Few enough repeats to read as an echo rather than as a wash.
constexpr float kDelayFeedback = 0.3f;
// Full ping-pong, which is what puts the repeats either side of the part
// instead of on top of it.
constexpr float kDelayPingPong = 1.0f;

// An effect on a return strip is heard only through the send, so the processor
// runs fully wet. A dry component here would sum a second, undelayed copy of
// every contributing part into the master.
constexpr float kReturnDryWet = 1.0f;

/// @brief Everything that differs between the two effect buses.
struct EffectBusSpec {
  const char* preferred_bus_id = nullptr;
  const char* preferred_return_id = nullptr;
  const char* processor_name = nullptr;
  /// @brief How the effect is named in the explanation.
  const char* label = nullptr;
  /// @brief Column of @ref kEffectSendTable this bus is fed from.
  float EffectSendRow::*send_field = nullptr;
  /// @brief Params for the return strip's insert.
  json::Object params;
};

float table_send_db(SourceClass source, float EffectSendRow::*send_field) noexcept {
  const auto index = static_cast<std::size_t>(source);
  if (index >= kEffectSendTable.size()) return kNoSendDb;
  return kEffectSendTable[index].*send_field;
}

json::Object reverb_params() {
  json::Object params;
  params["decaySec"] = json::Value(kReverbDecaySec);
  params["preDelayMs"] = json::Value(kReverbPreDelayMs);
  params["dryWet"] = json::Value(kReturnDryWet);
  return params;
}

json::Object delay_params() {
  json::Object params;
  params["delayTimeLMs"] = json::Value(kDottedEighthNoteMs);
  params["delayTimeRMs"] = json::Value(kQuarterNoteMs);
  params["feedback"] = json::Value(kDelayFeedback);
  params["pingPong"] = json::Value(kDelayPingPong);
  params["dryWet"] = json::Value(kReturnDryWet);
  return params;
}

/// @brief One track feeding an effect bus, and how much of it goes.
struct Contribution {
  const TrackProfile* profile = nullptr;
  float send_db = 0.0f;
};

// Realises one effect bus in the shape the `vocalReverbSend` scene preset uses:
// an aux bus, a return strip carrying the effect, a bus -> return edge, a
// return -> master edge, and one send per contributing strip. The processor
// sits on the return rather than on the bus so the send level alone decides how
// much of a part is treated.
void append_effect_bus(const std::vector<TrackProfile>& profiles, const std::string& master_id,
                       const EffectBusSpec& spec, float strength, IdAllocator& ids,
                       std::vector<SceneDelta>& deltas) {
  std::vector<Contribution> contributions;
  for (const TrackProfile& profile : profiles) {
    if (!profile.usable) continue;
    const float table_db = table_send_db(profile.source, spec.send_field);
    if (table_db == kNoSendDb) continue;
    Contribution contribution;
    contribution.profile = &profile;
    contribution.send_db = table_db - (kMaxSuggestionStrength - strength) * kSendStrengthRangeDb;
    contributions.push_back(contribution);
  }
  // A bus and a return strip with nothing feeding them are two nodes of pure
  // overhead in every scene that carries them.
  if (contributions.empty()) return;

  const std::string bus_id = ids.allocate(spec.preferred_bus_id);
  const std::string return_id = ids.allocate(spec.preferred_return_id);

  SceneDelta bus_delta;
  bus_delta.domain = DeltaDomain::Structure;
  bus_delta.buses.push_back(api::Bus(bus_id, kAuxBusRole));
  bus_delta.connections.push_back({bus_id, return_id});
  bus_delta.connections.push_back({return_id, master_id});
  bus_delta.reason = "created the " + bus_id + " aux bus feeding the " + return_id +
                     " return strip, which sums into " + master_id;
  deltas.push_back(std::move(bus_delta));

  SceneDelta return_delta;
  return_delta.domain = DeltaDomain::Structure;
  return_delta.strip_id = return_id;
  return_delta.inserts.push_back(api::Insert(api::InsertSlot::PostFader, spec.processor_name,
                                             json::dump(json::Value(spec.params))));
  return_delta.reason = "put a fully wet " + std::string(spec.label) + " on the " + return_id +
                        " return strip, so the send level alone decides how much of a part is "
                        "treated";
  deltas.push_back(std::move(return_delta));

  for (const Contribution& contribution : contributions) {
    const TrackProfile& profile = *contribution.profile;
    SceneDelta send_delta;
    send_delta.domain = DeltaDomain::Structure;
    send_delta.strip_id = profile.strip_id;
    // Post-fader, so the wet amount follows the part when its fader moves. A
    // pre-fader send would leave the effect behind the moment the balance
    // changed.
    send_delta.sends.push_back({profile.strip_id + "-to-" + bus_id, bus_id, contribution.send_db,
                                api::SendTiming::PostFader});
    send_delta.reason = "sent " + profile.strip_id + " to the " + spec.label + " at " +
                        format_db(contribution.send_db) + " dB as a " +
                        source_class_to_string(profile.source) + " part";
    deltas.push_back(std::move(send_delta));
  }
}

// Proposes the reverb bus and then the delay bus, in that order.
void append_effect_buses(const std::vector<TrackProfile>& profiles, const std::string& master_id,
                         const MixAssistantConfig& config, IdAllocator& ids,
                         std::vector<SceneDelta>& deltas) {
  const float strength =
      std::clamp(config.suggestion_strength, kMinSuggestionStrength, kMaxSuggestionStrength);
  // A bus nobody sends to is dead weight, so a caller asking for no level-like
  // suggestion gets the routing without the effects rather than an effect
  // return sitting at silence.
  if (strength <= kMinSuggestionStrength) return;

  EffectBusSpec reverb;
  reverb.preferred_bus_id = kReverbBusId;
  reverb.preferred_return_id = kReverbReturnId;
  reverb.processor_name = kReverbProcessor;
  reverb.label = kReverbLabel;
  reverb.send_field = &EffectSendRow::reverb_db;
  reverb.params = reverb_params();
  append_effect_bus(profiles, master_id, reverb, strength, ids, deltas);

  EffectBusSpec delay;
  delay.preferred_bus_id = kDelayBusId;
  delay.preferred_return_id = kDelayReturnId;
  delay.processor_name = kDelayProcessor;
  delay.label = kDelayLabel;
  delay.send_field = &EffectSendRow::delay_db;
  delay.params = delay_params();
  append_effect_bus(profiles, master_id, delay, strength, ids, deltas);
}

#endif  // SONARE_BUILD_FX

}  // namespace

std::vector<SceneDelta> decide_structure(const std::vector<TrackProfile>& profiles,
                                         const MixProfile& mix, const MixAssistantConfig& config) {
  // Which subgroup a track belongs to is a question about what the part is, so
  // the cross-track measurements take no part in it; see the header. The
  // parameter is kept for signature symmetry with the other decision domains.
  (void)mix;

  std::vector<SceneDelta> deltas;
  if (!config.enable_structure) return deltas;

  IdAllocator ids(profiles);
  // Allocated first so the master keeps its conventional name whenever a track
  // has not already taken it.
  const std::string master_id = ids.allocate(kMasterBusId);

  // Emitted unconditionally: every other edge this stage creates ends here, and
  // a scene with no master renders silence.
  SceneDelta master_delta;
  master_delta.domain = DeltaDomain::Structure;
  master_delta.buses.push_back(api::Bus(master_id, kMasterBusRole));
  master_delta.reason =
      "created the " + master_id + " bus, which every strip and subgroup sums into";
  deltas.push_back(std::move(master_delta));

  std::vector<Subgroup> subgroups = collect_subgroups(profiles);
  // Ids are taken in table order, so the same session always produces the same
  // names even when one of them had to be suffixed around a track.
  for (Subgroup& group : subgroups) {
    group.bus_id = ids.allocate(group.preferred_bus_id);
    group.vca_id = ids.allocate(group.bus_id + kVcaIdSuffix);
  }

  // Which strips ended up on a subgroup; everything else routes to master.
  std::set<std::string> bussed;
  for (const Subgroup& group : subgroups) {
    for (const std::string& member : group.members) bussed.insert(member);
  }

  for (const Subgroup& group : subgroups) {
    SceneDelta bus_delta;
    bus_delta.domain = DeltaDomain::Structure;
    bus_delta.buses.push_back(api::Bus(group.bus_id, kSubgroupBusRole));
    for (const std::string& member : group.members) {
      bus_delta.connections.push_back({member, group.bus_id});
    }
    bus_delta.connections.push_back({group.bus_id, master_id});
    bus_delta.reason = "grouped " + join_names(group.members) + " onto the " + group.bus_id +
                       " subgroup and routed it to " + master_id;
    deltas.push_back(std::move(bus_delta));

    SceneDelta vca_delta;
    vca_delta.domain = DeltaDomain::Structure;
    // Unity: the group exists so the section can be ridden later, and a VCA
    // that arrives with an offset already dialled in is a level decision the
    // gain and balance stages own.
    vca_delta.vca_groups.push_back({group.vca_id, 0.0f, group.members});
    vca_delta.reason = "added the " + group.vca_id + " group over the " + group.bus_id +
                       " tracks, so the section rides on one control without moving the summing "
                       "path its inserts and sends hang off";
    deltas.push_back(std::move(vca_delta));
  }

  for (const TrackProfile& profile : profiles) {
    if (bussed.count(profile.strip_id) != 0) continue;
    SceneDelta delta;
    delta.domain = DeltaDomain::Structure;
    delta.connections.push_back({profile.strip_id, master_id});
    delta.reason = direct_route_reason(profile, master_id);
    deltas.push_back(std::move(delta));
  }

#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX
  append_effect_buses(profiles, master_id, config, ids, deltas);
#endif

  return deltas;
}

}  // namespace sonare::mixing::assistant
