/// @file gs_efx_types_test.cpp
/// @brief Per-type coverage of the GS insertion-effect (EFX) map.
///
/// The SC-88Pro defines 64 EFX types, enumerated in kEfxTypes, and every one of
/// them realises an insert chain. The cases hold the mapping to what it cannot
/// check about itself: no type is mapped without a row here, every stage is one
/// insert_factory builds, two types realising the identical chain are listed
/// with the reason, a parameter edit never changes a chain's shape, and every
/// one of the 770 printed (type, slot) parameters does what its binding file
/// says -- a translated or designed byte emits its control at every boundary
/// byte and moves it monotonically, an enables byte moves a stage's enabled
/// flag, and no slot outside the binding files moves a chain at all. The last
/// is the one that can see a WRONG slot, which a sweep of the byte a case names
/// never can.
///
/// **Parameter combinations.** The axes are the type (65 numbers, the 64 plus
/// the alias), the slot (20) and the byte. The conversion law is a function of
/// (type, slot) -- the generated rows assign it -- so it is not a free axis, and
/// (type, slot) x the seven boundary bytes is small enough to run whole.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/gs_efx_join.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/gs_efx_tables.h"
#include "midi/synth/gs_layer.h"
#include "rt/processor_base.h"
#include "util/json.h"

namespace {

using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_insert_name;
using sonare::midi::synth::gs_efx_insert_params;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxStage;

namespace s = sonare::midi::synth;

/// One EFX type: its number and the manual's name.
struct EfxType {
  uint16_t type;
  const char* name;
};

/// Every SC-88Pro EFX type. The five groups are the manual's own: single
/// effects (MSB 01), series-2 composites (02), guitar/bass multis (04), the
/// keyboard multi (05), and the parallel-2 types (11). 0x0300 is not a 65th
/// type — it is the second number the manual prints for Rotary Multi — and is
/// listed separately in kAliasTypes.
constexpr EfxType kEfxTypes[] = {
    // MSB 01 — single effects.
    {0x0100, "Stereo-EQ"},
    {0x0101, "Spectrum"},
    {0x0102, "Enhancer"},
    {0x0103, "Humanizer"},
    {0x0110, "Overdrive"},
    {0x0111, "Distortion"},
    {0x0120, "Phaser"},
    {0x0121, "Auto Wah"},
    {0x0122, "Rotary"},
    {0x0123, "Stereo Flanger"},
    {0x0124, "Step Flanger"},
    {0x0125, "Tremolo"},
    {0x0126, "Auto Pan"},
    {0x0130, "Compressor"},
    {0x0131, "Limiter"},
    {0x0140, "Hexa Chorus"},
    {0x0141, "Tremolo Chorus"},
    {0x0142, "Stereo Chorus"},
    {0x0143, "Space-D"},
    {0x0144, "3D Chorus"},
    {0x0150, "Stereo Delay"},
    {0x0151, "Modulation Delay"},
    {0x0152, "3 Tap Delay"},
    {0x0153, "4 Tap Delay"},
    {0x0154, "Time Control Delay"},
    {0x0155, "Reverb"},
    {0x0156, "Gate Reverb"},
    {0x0157, "3D Delay"},
    {0x0160, "2 Voice Pitch Shifter"},
    {0x0161, "Feedback Pitch Shifter"},
    {0x0170, "3D Auto"},
    {0x0171, "3D Manual"},
    {0x0172, "Lo-Fi 1"},
    {0x0173, "Lo-Fi 2"},
    // MSB 02 — series-2 composites.
    {0x0200, "OD -> Chorus"},
    {0x0201, "OD -> Flanger"},
    {0x0202, "OD -> Delay"},
    {0x0203, "DS -> Chorus"},
    {0x0204, "DS -> Flanger"},
    {0x0205, "DS -> Delay"},
    {0x0206, "EH -> Chorus"},
    {0x0207, "EH -> Flanger"},
    {0x0208, "EH -> Delay"},
    {0x0209, "Chorus -> Delay"},
    {0x020A, "Flanger -> Delay"},
    {0x020B, "Chorus -> Flanger"},
    {0x020C, "Rotary Multi"},
    // MSB 04 — guitar and bass multis. These realise onto the existing amp
    // simulation rather than a second amplifier: the hardware separates the amp
    // type from the amp switch too (src/midi/synth/docs/voicing.md).
    {0x0400, "GTR Multi 1"},
    {0x0401, "GTR Multi 2"},
    {0x0402, "GTR Multi 3"},
    {0x0403, "Clean Gt Multi 1"},
    {0x0404, "Clean Gt Multi 2"},
    {0x0405, "Bass Multi"},
    {0x0406, "Rhodes Multi"},
    // MSB 05 — the keyboard multi.
    {0x0500, "Keyboard Multi"},
    // MSB 11 — parallel-2: two halves side by side (GsEfxStage::branch).
    {0x1100, "Cho/Delay"},
    {0x1101, "FL/Delay"},
    {0x1102, "Cho/Flanger"},
    {0x1103, "OD1/OD2"},
    {0x1104, "OD/Rotary"},
    {0x1105, "OD/Phaser"},
    {0x1106, "OD/Auto Wah"},
    {0x1107, "PH/Rotary"},
    {0x1108, "PH/Auto Wah"},
};

static_assert(sizeof(kEfxTypes) / sizeof(kEfxTypes[0]) == 64,
              "the SC-88Pro defines 64 EFX types; a row was added or lost");

/// Type numbers that are not one of the 64 but do resolve to a chain. Keeping
/// them out of kEfxTypes stops the 64-count assertion from drifting, and the
/// coverage sweep still expects them to be mapped.
constexpr EfxType kAliasTypes[] = {
    {0x0300, "Rotary Multi (the manual's second number for 0x020C)"},
};

/// A group of types that realise the identical chain AND identical parameters,
/// with the reason they are currently indistinguishable. Anything the mapping
/// collides that is not listed here is drift.
struct EfxCollision {
  const char* reason;
  std::vector<uint16_t> types;
};

const std::vector<EfxCollision>& collisions() {
  static const std::vector<EfxCollision> kCollisions = {
      {"one effect the manual prints under two type numbers", {0x020C, 0x0300}},
  };
  return kCollisions;
}

/// A unit holding @p type, in the state selecting it over the wire leaves: the
/// type's own twenty power-on parameters, not a block of zeros. Built directly
/// rather than through apply_gs_efx_sysex, so the round-trip case below still
/// compares two ways of arriving at the state instead of one way twice.
GsEfx make_efx(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const s::GsEfxTypeDefaults* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}

/// The chain's identity: every stage name, whether it is on, and its
/// parameters, in order. Two types with the same signature are
/// indistinguishable to a listener.
std::string signature(const std::vector<GsEfxStage>& chain) {
  std::string out;
  for (const GsEfxStage& stage : chain) {
    out += stage.name;
    out += stage.enabled ? "|on|" : "|off|";
    out += stage.params_json;
    out += '\n';
  }
  return out;
}

std::vector<std::string> stage_names(const std::vector<GsEfxStage>& chain) {
  std::vector<std::string> out;
  out.reserve(chain.size());
  for (const GsEfxStage& stage : chain) out.push_back(stage.name);
  return out;
}

/// Reads a JSON number field out of an insert's params object. Returns false
/// when the key is absent.
bool json_number(const std::string& json, const std::string& key, double& out) {
  const std::string needle = "\"" + key + "\":";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return false;
  out = std::strtod(json.c_str() + at + needle.size(), nullptr);
  return true;
}

/// A GS DT1 message writing @p values from EFX block address 40 03 @p offset,
/// with the Roland checksum.
std::vector<uint8_t> efx_sysex(uint8_t offset, const std::vector<uint8_t>& values) {
  std::vector<uint8_t> msg = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset};
  unsigned sum = 0x40u + 0x03u + offset;
  for (uint8_t v : values) {
    msg.push_back(v);
    sum += v;
  }
  msg.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  msg.push_back(0xF7);
  return msg;
}

std::string hex4(uint16_t type) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out = "0x";
  for (int shift = 12; shift >= 0; shift -= 4) out += kDigits[(type >> shift) & 0xF];
  return out;
}

/// The params of the one stage named @p name at @p ordinal, or nullptr.
const GsEfxStage* find_stage(const std::vector<GsEfxStage>& chain, const std::string& name,
                             uint8_t ordinal) {
  for (const GsEfxStage& stage : chain) {
    if (stage.name == name && stage.ordinal == ordinal) return &stage;
  }
  return nullptr;
}

/// The params of the one stage named @p name, or an empty string when the chain
/// carries no such stage or more than one (which would make the lookup silently
/// pick a side).
std::string stage_params(const std::vector<GsEfxStage>& chain, const std::string& name) {
  const std::string* found = nullptr;
  for (const GsEfxStage& stage : chain) {
    if (stage.name != name) continue;
    if (found != nullptr) return {};
    found = &stage.params_json;
  }
  return found != nullptr ? *found : std::string{};
}

/// Counts what it checked, so a run that stopped reaching the population reports
/// as a smaller number rather than as a clean one.
class Tally {
 public:
  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

/// The byte values the boundary axis takes: the two ends, the centre and its
/// neighbours, which is where every conversion in the archive has a knot.
constexpr uint8_t kValues[] = {0, 1, 63, 64, 65, 126, 127};

/// The boundary bytes a slot accepts, in order: kValues clamped into the
/// printed range, and into the printed list of states where the page prints
/// one (a byte past the list is never taken, gs_efx_parameter_takes).
std::vector<uint8_t> boundary_bytes(const s::GsEfxJoinRow& row) {
  int hi = row.byte_hi;
  const int states = s::gs_efx_printed_states(row.type, row.slot);
  if (states > 0) hi = std::min(hi, states - 1);
  std::vector<uint8_t> out;
  for (uint8_t value : kValues) {
    const auto clamped = static_cast<uint8_t>(std::clamp<int>(value, row.byte_lo, hi));
    if (out.empty() || out.back() != clamped) out.push_back(clamped);
  }
  return out;
}

/// Whether @p values run one way. A measured table keeps its reading noise at
/// the ends (the pan table reads 127 0.03 dB under 126), so a reversal under a
/// thousandth of the span the values cover is not a turn.
bool monotone(const std::vector<double>& values) {
  const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
  const double slack = 1e-3 * (*hi - *lo);
  bool rising = true;
  bool falling = true;
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i] < values[i - 1] - slack) rising = false;
    if (values[i] > values[i - 1] + slack) falling = false;
  }
  return rising || falling;
}

/// Every type the table declares.
std::vector<EfxType> all_rows() {
  std::vector<EfxType> rows(std::begin(kEfxTypes), std::end(kEfxTypes));
  rows.insert(rows.end(), std::begin(kAliasTypes), std::end(kAliasTypes));
  return rows;
}

/// The generated rows of one (type, slot).
std::vector<const s::GsEfxBindingRow*> rows_of(uint16_t type, uint8_t slot) {
  std::vector<const s::GsEfxBindingRow*> out;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    if (row.type == type && row.slot == slot) out.push_back(&row);
  }
  return out;
}

std::vector<const s::GsEfxEnable*> enables_of(uint16_t type, uint8_t slot) {
  std::vector<const s::GsEfxEnable*> out;
  for (const s::GsEfxEnable& enable : s::kGsEfxEnables) {
    if (enable.type == type && enable.slot == slot) out.push_back(&enable);
  }
  return out;
}

/// Sets every switch and selector byte of @p efx's type that names (@p stage,
/// @p ordinal) to a value turning that stage on, so a control is read on a
/// stage that sounds -- the flanger a CF Sel picks, not only the chorus.
void turn_on(GsEfx& efx, std::string_view stage, uint8_t ordinal) {
  for (const s::GsEfxEnable& enable : s::kGsEfxEnables) {
    if (enable.type != efx.type && enable.type != s::gs_efx_alias_type(efx.type)) continue;
    for (uint8_t i = 0; i < enable.n_stages; ++i) {
      if (s::kGsEfxRowStages[enable.stages[i]] != stage || enable.ordinals[i] != ordinal) continue;
      if (enable.mode == s::kGsEfxEnableSelect) {
        efx.params[enable.slot] = i;
        continue;
      }
      for (uint8_t byte = 0; byte < 128; ++byte) {
        if (s::gs_efx_enable_on(enable, byte, i)) {
          efx.params[enable.slot] = byte;
          break;
        }
      }
    }
  }
}

/// A chain skeleton alone: no binding row and no enable row applied.
constexpr s::GsEfxRowView kNoRows{nullptr, 0, nullptr, 0};

}  // namespace

TEST_CASE("every GS EFX type realises a chain", "[midi][sf2][gs][efxtypes]") {
  for (const EfxType& row : all_rows()) {
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      const auto chain = gs_efx_insert_chain(make_efx(row.type));
      // An empty chain is a silent bypass wearing a mapping.
      REQUIRE_FALSE(chain.empty());
      for (const GsEfxStage& stage : chain) REQUIRE_FALSE(stage.name.empty());
    }
  }
}

TEST_CASE("no EFX type is mapped without a table row", "[midi][sf2][gs][efxtypes]") {
  // Exhaustive over the type-number space the GS wire can carry for the defined
  // category MSBs: a mapping added to gs_layer without a row here fails by name
  // rather than passing unnoticed.
  std::set<uint16_t> declared;
  for (const EfxType& row : all_rows()) declared.insert(row.type);

  std::vector<uint16_t> undeclared_but_mapped;
  for (unsigned msb = 0x00; msb <= 0x11; ++msb) {
    for (unsigned lsb = 0x00; lsb <= 0x7F; ++lsb) {
      const auto type = static_cast<uint16_t>((msb << 8) | lsb);
      if (gs_efx_insert_chain(make_efx(type)).empty()) continue;
      if (declared.count(type) == 0) undeclared_but_mapped.push_back(type);
    }
  }
  std::string names;
  for (uint16_t type : undeclared_but_mapped) names += hex4(type) + " ";
  INFO("mapped with no row in kEfxTypes/kAliasTypes: " << names);
  REQUIRE(undeclared_but_mapped.empty());

  // Thru (type 0) is the power-on state, not an effect: it must never realise.
  REQUIRE(gs_efx_insert_chain(make_efx(0x0000)).empty());
}

TEST_CASE("gs_efx_insert_name names every single-effect type", "[midi][sf2][gs][efxtypes]") {
  // The single effects are the MSB-01 group; composites have no single name and
  // are read through the chain.
  for (const EfxType& row : kEfxTypes) {
    if ((row.type >> 8) != 0x01) continue;
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      REQUIRE_FALSE(gs_efx_insert_name(row.type).empty());
    }
  }
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)

TEST_CASE("every EFX chain stage names a processor the insert factory builds",
          "[midi][sf2][gs][efxtypes]") {
  // The failure this catches: a mapping that looks complete and produces
  // nothing, because make_insert returns null for a name that does not exist.
  // Building also parses the stage's params JSON, so a malformed object throws.
  const auto names = sonare::mastering::api::insert_factory_names();
  for (const EfxType& row : all_rows()) {
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      for (const GsEfxStage& stage : gs_efx_insert_chain(make_efx(row.type))) {
        INFO("stage " << stage.name << " params " << stage.params_json);
        REQUIRE(std::find(names.begin(), names.end(), stage.name) != names.end());
        REQUIRE(sonare::mastering::api::make_insert(stage.name, stage.params_json) != nullptr);
      }
    }
  }
}

TEST_CASE("every EFX parameter key is one its insert reads", "[midi][sf2][gs][efxtypes]") {
  // A key the processor does not read is silently ignored, so a translation
  // aimed at a misspelled key is a no-op that no audible test would catch.
  // Swept over the boundary bytes, filling every slot with the same value.
  for (uint8_t value : kValues) {
    for (const EfxType& row : all_rows()) {
      GsEfx efx = make_efx(row.type);
      efx.params.fill(value);
      DYNAMIC_SECTION(hex4(row.type) << " " << row.name << " at " << static_cast<int>(value)) {
        for (const GsEfxStage& stage : gs_efx_insert_chain(efx)) {
          std::vector<std::string> unknown;
          auto processor =
              sonare::mastering::api::make_insert(stage.name, stage.params_json, &unknown);
          REQUIRE(processor != nullptr);
          std::string joined;
          for (const std::string& key : unknown) joined += key + " ";
          INFO("stage " << stage.name << " ignored: " << joined);
          REQUIRE(unknown.empty());
        }
      }
    }
  }
}

TEST_CASE("every CONTROL destination reaches a realtime key", "[midi][sf2][gs][efxtypes]") {
  // A `+` or `#` slot is what CONTROL SOURCE modulates while the chain plays,
  // so its control has to be one the insert moves in place; a key with no
  // realtime id would turn every modulation step into a rebuild.
  namespace json = sonare::util::json;
  Tally tally;
  std::map<std::string, std::set<std::string>> automatable;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    if (row.printed_mark == 0) continue;
    const std::string stage(s::kGsEfxRowStages[row.stage]);
    const std::string key(s::kGsEfxRowKeys[row.key]);
    if (automatable.count(stage) == 0) {
      std::set<std::string> ids;
      const json::Value parsed =
          json::parse_strict(sonare::mastering::api::insert_param_info_json(stage));
      REQUIRE(parsed.is_array());
      for (const json::Value& parameter : parsed.as_array()) {
        const json::Value* name = parameter.find("name");
        const json::Value* id = parameter.find("id");
        if (name != nullptr && id != nullptr && !id->is_null()) ids.insert(name->as_string());
      }
      automatable.emplace(stage, std::move(ids));
    }
    tally.same(automatable[stage].count(key) == 1,
               hex4(row.type) + " slot " + std::to_string(row.slot) + " (" +
                   static_cast<char>(row.printed_mark) + ") drives " + stage + "." + key +
                   ", which has no realtime id");
  }
  WARN("marked destinations checked: " << tally.count());
  REQUIRE(tally.count() >= 42);
}

#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("two EFX types realise the same chain only where that is documented",
          "[midi][sf2][gs][efxtypes]") {
  std::map<std::string, std::vector<uint16_t>> by_signature;
  for (const EfxType& row : all_rows()) {
    by_signature[signature(gs_efx_insert_chain(make_efx(row.type)))].push_back(row.type);
  }

  std::set<std::vector<uint16_t>> declared;
  for (const EfxCollision& group : collisions()) {
    std::vector<uint16_t> sorted = group.types;
    std::sort(sorted.begin(), sorted.end());
    declared.insert(sorted);
  }

  for (const auto& entry : by_signature) {
    if (entry.second.size() < 2) continue;
    std::vector<uint16_t> sorted = entry.second;
    std::sort(sorted.begin(), sorted.end());
    std::string names;
    for (uint16_t type : sorted) names += hex4(type) + " ";
    INFO("undocumented collision: " << names << "-> " << entry.first);
    REQUIRE(declared.count(sorted) == 1);
  }

  // The reverse: a documented collision that no longer collides is a stale
  // blessing, and the next type to take that shape would inherit it unexamined.
  for (const EfxCollision& group : collisions()) {
    std::vector<uint16_t> sorted = group.types;
    std::sort(sorted.begin(), sorted.end());
    std::string names;
    for (uint16_t type : sorted) names += hex4(type) + " ";
    INFO("declared collision no longer collides: " << names << " (" << group.reason << ")");
    const std::string first = signature(gs_efx_insert_chain(make_efx(sorted.front())));
    for (uint16_t type : sorted) {
      REQUIRE(signature(gs_efx_insert_chain(make_efx(type))) == first);
    }
    REQUIRE(by_signature[first].size() == sorted.size());
  }
}

TEST_CASE("a parameter-only edit never changes an EFX chain's shape", "[midi][sf2][gs][efxtypes]") {
  // Exhaustive over type, slot and boundary byte. Structure comes from the type
  // alone: parameters voice the stages and switch them on or off, they never add
  // or remove one.
  for (const EfxType& row : all_rows()) {
    const std::vector<std::string> shape = stage_names(gs_efx_insert_chain(make_efx(row.type)));
    for (size_t index = 0; index < GsEfx{}.params.size(); ++index) {
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(row.type);
        efx.params[index] = value;
        INFO(hex4(row.type) << " " << row.name << " param " << (index + 1) << " = "
                            << static_cast<int>(value));
        REQUIRE(stage_names(gs_efx_insert_chain(efx)) == shape);
      }
    }
  }
}

TEST_CASE("the chain skeleton selects the modern method of each insert",
          "[midi][sf2][gs][efxtypes]") {
  // The inserts default to today's behaviour; the GS modern chain asks for the
  // newer method, so a stage that lost its key would fall back in silence.
  const std::map<std::string, std::vector<std::pair<std::string, double>>> kMethods = {
      {"effects.modulation.chorus", {{"interpolation", 1}}},
      {"effects.modulation.flanger", {{"interpolation", 1}}},
      {"effects.modulation.ensemble", {{"interpolation", 1}}},
      {"effects.delay.stereo", {{"interpolation", 1}}},
      {"effects.modulation.rotary", {{"interpolation", 1}, {"model", 1}}},
      {"effects.modulation.pitchShifter", {{"interpolation", 1}, {"antiAlias", 1}}},
      {"effects.modulation.wah", {{"sweepLaw", 1}}},
      {"effects.modulation.autoWah", {{"sweepLaw", 1}}},
      {"spectral.presenceEnhancer", {{"aliasing", 1}}},
  };
  Tally tally;
  std::set<std::string> seen;
  for (const EfxType& row : all_rows()) {
    for (const GsEfxStage& stage : gs_efx_insert_chain(make_efx(row.type))) {
      const auto found = kMethods.find(stage.name);
      if (found == kMethods.end()) continue;
      seen.insert(stage.name);
      for (const auto& [key, value] : found->second) {
        double carried = 0.0;
        tally.same(json_number(stage.params_json, key, carried) && carried == value,
                   hex4(row.type) + " " + stage.name + " does not select " + key);
      }
    }
  }
  // Every insert the table names is reached by some type, or the entry is stale.
  tally.same(seen.size() == kMethods.size(), "an insert with a modern method is in no chain");
  WARN("comparisons: " << tally.count());
}

TEST_CASE("EFX parameter translations move their insert control monotonically",
          "[midi][sf2][gs][efxtypes]") {
  // Only the confirmed parameter positions are translated, so only they are
  // swept. Each sweep asserts the direction the manual gives, over every byte
  // value rather than a sampled few.
  // The drive types' Drive byte and the pitch shifters' balance byte each reach
  // one control through their row, and the skeleton writes nothing from them:
  // one printed byte, one writer.
  const auto sweep_one_writer = [](uint16_t type, int slot, const std::string& stage,
                                   const std::string& key, const std::string& skeleton_key) {
    GsEfx efx = make_efx(type);
    double previous = -1000.0;
    double first = 0.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[static_cast<size_t>(slot)] = static_cast<uint8_t>(value);
      double control = 0.0;
      REQUIRE(json_number(stage_params(gs_efx_insert_chain(efx), stage), key, control));
      double unused = 0.0;
      INFO(hex4(type) << " PARAMETER " << (slot + 1) << " = " << value);
      REQUIRE_FALSE(json_number(gs_efx_insert_params(efx), skeleton_key, unused));
      REQUIRE(control >= previous);
      if (value == 0) first = control;
      previous = control;
    }
    REQUIRE(previous > first);
    return first;
  };

  SECTION("Overdrive and Distortion drive rises with EFX PARAMETER 1, through inputDb alone") {
    // PARAMETER 1 is a gain in front of a fixed curve (40 03 03); the byte beside
    // it picks the curve. The amp's own drive stays at the insert default.
    sweep_one_writer(0x0110, 0, "saturation.ampSim", "inputDb", "drive");
    sweep_one_writer(0x0111, 0, "saturation.ampSim", "inputDb", "drive");
    double od_model = 0.0;
    double ds_model = 0.0;
    REQUIRE(json_number(gs_efx_insert_params(make_efx(0x0110)), "ampModel", od_model));
    REQUIRE(json_number(gs_efx_insert_params(make_efx(0x0111)), "ampModel", ds_model));
    REQUIRE(od_model != ds_model);  // the two types keep their own voicings
  }

  SECTION("effect balance rises with EFX PARAMETER 16, and 0 is all direct") {
    // A balance of 0 is all direct signal, a setting rather than a silence.
    for (const uint16_t type : {uint16_t{0x0160}, uint16_t{0x0161}}) {
      REQUIRE(sweep_one_writer(type, 15, "effects.modulation.pitchShifter", "dryWet", "dryWet") ==
              0.0);
    }
  }

  SECTION("output level rises with EFX PARAMETER 20, and 0 is the value zero") {
    GsEfx efx = make_efx(0x0110);
    // Swept from 0, because 0 is a level a file can ask for rather than an
    // absence: selecting the type loads this slot's own default, so no state of
    // the block means "unset".
    double previous = -1000.0;
    for (int value = 0; value <= 127; ++value) {
      efx.params[19] = static_cast<uint8_t>(value);
      double level = 0.0;
      REQUIRE(
          json_number(stage_params(gs_efx_insert_chain(efx), "utility.gain"), "levelDb", level));
      INFO("PARAMETER 20 = " << value);
      REQUIRE(level >= previous);
      previous = level;
    }
    REQUIRE(previous == 0.0);  // unity (127) is 0 dB
  }

  SECTION("pitch shift rises with EFX PARAMETER 1 and centres on 64") {
    GsEfx efx = make_efx(0x0160);
    double previous = -1000.0;
    for (int value = 1; value <= 127; ++value) {
      efx.params[0] = static_cast<uint8_t>(value);
      double semitones = 0.0;
      REQUIRE(json_number(stage_params(gs_efx_insert_chain(efx), "effects.modulation.pitchShifter"),
                          "semitones", semitones));
      INFO("PARAMETER 1 = " << value);
      REQUIRE(semitones >= previous);
      previous = semitones;
    }
    efx.params[0] = 64;
    double centre = 1.0;
    REQUIRE(json_number(stage_params(gs_efx_insert_chain(efx), "effects.modulation.pitchShifter"),
                        "semitones", centre));
    REQUIRE(centre == 0.0);
  }
}

TEST_CASE("Tremolo realises as amplitude modulation, not as a ring modulator",
          "[midi][sf2][gs][efxtypes]") {
  // Tremolo is UNIPOLAR amplitude modulation and ring modulation is bipolar, so
  // the mapping is only honest if the modulator never crosses zero. The insert's
  // dry and wet terms multiply the SAME input, so they collapse to one envelope
  //   out = dry*x + wet*x*sin = x*((1 - wet) + wet*sin)
  // whose minimum is 1 - 2*wet: it stays non-negative for every wet <= 0.5.
  const auto chain = gs_efx_insert_chain(make_efx(0x0125));
  REQUIRE_FALSE(chain.empty());
  REQUIRE(chain[0].name == "effects.modulation.ringModulator");
  const std::string modulator = stage_params(chain, "effects.modulation.ringModulator");
  double carrier = 0.0;
  double wet = 0.0;
  REQUIRE(json_number(modulator, "carrierHz", carrier));
  REQUIRE(json_number(modulator, "dryWet", wet));
  REQUIRE(carrier > 0.0);
  REQUIRE(carrier < 20.0);
  REQUIRE(wet > 0.0);
  REQUIRE(wet < 0.5);

  // The Mod Depth byte holds that bound at every setting: its law tops out at 0.5.
  GsEfx deepest = make_efx(0x0125);
  deepest.params[2] = 127;
  double deepest_wet = 1.0;
  REQUIRE(
      json_number(stage_params(gs_efx_insert_chain(deepest), "effects.modulation.ringModulator"),
                  "dryWet", deepest_wet));
  REQUIRE(deepest_wet <= 0.5);

  // Tremolo Chorus prints no depth byte; its fixed depth is inside the same bound.
  const auto tremolo_chorus = gs_efx_insert_chain(make_efx(0x0141));
  REQUIRE(stage_names(tremolo_chorus).size() >= 2);
  REQUIRE(stage_names(tremolo_chorus)[0] == "effects.modulation.chorus");
  REQUIRE(stage_names(tremolo_chorus)[1] == "effects.modulation.ringModulator");
  double chain_wet = 0.0;
  REQUIRE(json_number(stage_params(tremolo_chorus, "effects.modulation.ringModulator"), "dryWet",
                      chain_wet));
  REQUIRE(chain_wet > 0.0);
  REQUIRE(chain_wet < 0.5);
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)

TEST_CASE("the Tremolo voicing never inverts the phase it modulates", "[midi][sf2][gs][efxtypes]") {
  // A constant +1 input makes the output BE the gain envelope, so any sample at
  // or below zero is a sign flip and nothing else. Half a second covers several
  // periods of the sub-audio carrier, troughs included.
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlock = 512;
  constexpr int kFrames = 24000;

  auto envelope = [&](const std::string& params) {
    auto processor =
        sonare::mastering::api::make_insert("effects.modulation.ringModulator", params);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, kBlock);
    std::vector<float> buffer(kFrames, 1.0f);
    for (int at = 0; at < kFrames; at += kBlock) {
      const int n = std::min(kBlock, kFrames - at);
      float* channel = buffer.data() + at;
      float* channels[] = {channel};
      processor->process(channels, 1, n);
    }
    return buffer;
  };

  const auto tremolo = envelope(gs_efx_insert_chain(make_efx(0x0125))[0].params_json);
  const float low = *std::min_element(tremolo.begin(), tremolo.end());
  const float high = *std::max_element(tremolo.begin(), tremolo.end());
  INFO("envelope spans " << low << " .. " << high);
  REQUIRE(low > 0.0f);              // unipolar: the trough closes the gate, never inverts
  REQUIRE(high <= 1.0f + 1.0e-6f);  // and the peak is unity, so the type adds no gain
  REQUIRE(high - low > 0.25f);      // and it is a real tremolo, not a near-flat gain

  // The bound is measured, not asserted: the same insert one step past dryWet
  // 0.5 does invert, which is what makes "under 0.5" a boundary rather than a
  // number chosen to make the case pass.
  const auto bipolar = envelope("{\"carrierHz\":5.0,\"dryWet\":0.60}");
  REQUIRE(*std::min_element(bipolar.begin(), bipolar.end()) < 0.0f);

  // The deepest Mod Depth byte closes the gate at the trough and goes no further.
  GsEfx deepest = make_efx(0x0125);
  deepest.params[2] = 127;
  const auto closed = envelope(gs_efx_insert_chain(deepest)[0].params_json);
  REQUIRE(*std::min_element(closed.begin(), closed.end()) >= -1.0e-6f);
}

#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("an EFX type set over the wire reads back the same chain", "[midi][sf2][gs][efxtypes]") {
  for (const EfxType& row : all_rows()) {
    DYNAMIC_SECTION(hex4(row.type) << " " << row.name) {
      GsEfx efx;
      const auto msb = static_cast<uint8_t>((row.type >> 8) & 0x7Fu);
      const auto lsb = static_cast<uint8_t>(row.type & 0x7Fu);
      const auto write = efx_sysex(0x00, {msb, lsb});
      bool type_changed = false;
      REQUIRE(apply_gs_efx_sysex(efx, write.data(), write.size(), &type_changed));
      REQUIRE(efx.type == row.type);
      REQUIRE(type_changed);  // every row is a real type, never the Thru default

      const std::string expected = signature(gs_efx_insert_chain(make_efx(row.type)));
      REQUIRE(signature(gs_efx_insert_chain(efx)) == expected);
      // Reading the chain does not consume the state: a second read is equal.
      REQUIRE(signature(gs_efx_insert_chain(efx)) == expected);

      // A parameter write after the type write reports no type change and keeps
      // the chain's shape, which is the condition the realiser uses to update
      // the live processors in place instead of rebuilding them.
      const std::vector<std::string> shape = stage_names(gs_efx_insert_chain(efx));
      const uint8_t before = efx.params[1];
      const auto param = efx_sysex(0x04, {0x50});  // EFX PARAMETER 2 = 80
      bool param_changed_type = true;
      REQUIRE(apply_gs_efx_sysex(efx, param.data(), param.size(), &param_changed_type));
      REQUIRE_FALSE(param_changed_type);
      // A slot printing a list of states does not take a byte past it.
      REQUIRE(efx.params[1] == (s::gs_efx_printed_states(row.type, 1) == 0 ? 0x50 : before));
      REQUIRE(stage_names(gs_efx_insert_chain(efx)) == shape);
    }
  }
}

TEST_CASE("every adjudicated EFX byte does what its binding file says",
          "[midi][sf2][gs][efxtypes]") {
  // The chain the generated rows build and the join the tests read are two
  // renderings of the same files by two generators; each (type, slot) is held
  // to the form the join gives it, measured on the chain.
  Tally tally;
  std::map<uint8_t, int> rows_by_form;
  int controls = 0;

  for (const s::GsEfxJoinRow& row : s::kGsEfxJoin) {
    ++rows_by_form[row.form];
    const std::string label = hex4(row.type) + " slot " + std::to_string(row.slot);
    const std::vector<uint8_t> bytes = boundary_bytes(row);
    tally.same(bytes.size() >= 2, label + " is printed over a single byte");

    if (row.form == s::kGsEfxJoinEnables) {
      const auto enables = enables_of(row.type, row.slot);
      tally.same(!enables.empty(), label + " is an enables row and no generated rule reads it");
      tally.same(rows_of(row.type, row.slot).empty(),
                 label + " is an enables row and also drives a control");
      // The byte moves the enabled flag of a stage it names, and nothing else.
      std::set<std::string> patterns;
      std::set<std::string> params;
      for (uint8_t value : bytes) {
        GsEfx efx = make_efx(row.type);
        efx.params[row.slot] = value;
        const auto chain = gs_efx_insert_chain(efx);
        std::string pattern;
        std::string voiced;
        for (const GsEfxStage& stage : chain) {
          pattern += stage.enabled ? '1' : '0';
          voiced += stage.params_json;
        }
        patterns.insert(pattern);
        params.insert(voiced);
      }
      tally.same(patterns.size() >= 2, label + " is an enables row and switches no stage");
      tally.same(params.size() == 1, label + " is an enables row and moves a control");
      continue;
    }

    const auto bound = rows_of(row.type, row.slot);
    tally.same(!bound.empty(), label + " is assigned and the binding table drives nothing from it");
    tally.same(enables_of(row.type, row.slot).empty(),
               label + " drives a control and is also an enables rule");
    uint8_t mask = 0;
    for (const s::GsEfxBindingRow* binding : bound) {
      mask = static_cast<uint8_t>(mask | (1u << binding->ordinal));
      tally.same(binding->kind == (row.form == s::kGsEfxJoinTranslated ? s::kGsEfxRowTranslated
                                                                       : s::kGsEfxRowDesigned),
                 label + " is rendered in two forms by the two generators");
      tally.same(binding->printed_mark == row.printed_mark,
                 label + " carries two marks in the two renderings");
    }
    tally.same(mask == row.ordinal_mask, label + " drives other ordinals than its row names");

    for (const s::GsEfxBindingRow* binding : bound) {
      ++controls;
      const std::string stage(s::kGsEfxRowStages[binding->stage]);
      const std::string key(s::kGsEfxRowKeys[binding->key]);
      const std::string what =
          label + " (" + stage + "#" + std::to_string(binding->ordinal) + "." + key + ")";
      // Emitted at every boundary byte, carrying the row's own law's reading, and
      // moving monotonically: a key written at a constant is one the wire cannot
      // reach, and it reads exactly like a translation to anything that greps.
      std::vector<double> emitted;
      for (uint8_t value : bytes) {
        GsEfx efx = make_efx(row.type);
        turn_on(efx, stage, binding->ordinal);
        efx.params[row.slot] = value;
        const auto chain = gs_efx_insert_chain(efx);
        const GsEfxStage* found = find_stage(chain, stage, binding->ordinal);
        double number = 0.0;
        if (found == nullptr || !json_number(found->params_json, key, number)) {
          tally.same(false, what + " is not emitted at byte " + std::to_string(value));
          break;
        }
        tally.same(found->enabled, what + " is read on a stage no byte turns on");
        const double expected = s::gs_efx_binding_value(*binding, value);
        // Rendered through std::to_string, so six decimal places is the width of
        // the comparison rather than a tolerance chosen for the quantity.
        tally.same(std::fabs(number - expected) <= 5e-7 * std::max(1.0, std::fabs(expected)),
                   what + " at byte " + std::to_string(value) + " carries " +
                       std::to_string(number) + " where its law reads " + std::to_string(expected));
        emitted.push_back(number);
      }
      if (emitted.size() != bytes.size()) continue;
      // An enumeration has no direction -- a printed list of states, or a
      // measured table of settings (wave, width, post gain, window, corner) --
      // and a frequency column's printed bypass reads 0 Hz where it means no corner.
      std::vector<double> ordered;
      for (double value : emitted) {
        const bool bypass = binding->law.form == s::kGsEfxFormNone &&
                            binding->conv_class == s::kGsEfxClassFreq && value == 0.0;
        if (!bypass) ordered.push_back(value);
      }
      const bool measured = binding->law.form == s::kGsEfxFormNone;
      const bool settings = measured && (binding->conv_class == s::kGsEfxClassWave ||
                                         binding->conv_class == s::kGsEfxClassWidth ||
                                         binding->conv_class == s::kGsEfxClassPostGain ||
                                         binding->conv_class == s::kGsEfxClassWindow ||
                                         binding->conv_class == s::kGsEfxClassCorner);
      if (!settings && s::gs_efx_printed_states(row.type, row.slot) == 0) {
        tally.same(monotone(ordered), what + " does not move monotonically over its bytes");
      }
      tally.same(std::set<double>(emitted.begin(), emitted.end()).size() >= 2,
                 what + " is emitted at a constant, which drives nothing");
    }
  }

  // The counts the files declare against the counts this run reached.
  tally.same(rows_by_form[s::kGsEfxJoinTranslated] == s::kGsEfxJoinTranslatedRows,
             "the rendered translated rows are not the count the header declares");
  tally.same(rows_by_form[s::kGsEfxJoinDesigned] == s::kGsEfxJoinDesignedRows,
             "the rendered designed rows are not the count the header declares");
  tally.same(rows_by_form[s::kGsEfxJoinEnables] == s::kGsEfxJoinEnablesRows,
             "the rendered enables rows are not the count the header declares");
  tally.same(controls == static_cast<int>(s::kGsEfxBindingRows.size()),
             "a generated binding row belongs to no adjudicated slot");
  tally.same(s::kGsEfxJoin.size() == 770, "the join does not adjudicate all 770 printed slots");

  WARN("slots: " << s::kGsEfxJoin.size() << "  controls: " << controls
                 << "  comparisons: " << tally.count());
  REQUIRE(tally.count() >= 5000);
}

TEST_CASE("a measured pair is translated through the law the archive read",
          "[midi][sf2][gs][efxtypes]") {
  // A row is free to carry or invent a law for a pair nothing measured, not to
  // name a different one for a pair that was. Rotary Multi is filed under the
  // other of its two type numbers in the archive, so both are looked up.
  Tally tally;
  for (const s::GsEfxSlotConversion& entry : s::kGsEfxSlotConversions) {
    const std::string label = hex4(entry.type) + " slot " + std::to_string(entry.parameter);
    auto bound = rows_of(entry.type, entry.parameter);
    if (bound.empty()) bound = rows_of(s::gs_efx_alias_type(entry.type), entry.parameter);
    tally.same(!bound.empty(), label + " has a measured law and drives no control");
    for (const s::GsEfxBindingRow* binding : bound) {
      tally.same(binding->kind == s::kGsEfxRowTranslated &&
                     binding->law.form == s::kGsEfxFormNone &&
                     binding->conv_class == entry.conversion_class && binding->table == entry.table,
                 label + " drives " + std::string(s::kGsEfxRowKeys[binding->key]) +
                     " through a law other than the one the archive measured");
    }
  }
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= static_cast<int>(s::kGsEfxSlotConversions.size()));
}

TEST_CASE("only the bytes the binding files adjudicate move a chain", "[midi][sf2][gs][efxtypes]") {
  // The inverse of the case above, and the one that can see a wrong slot: every
  // slot of every type is swept, and one that moves anything has to be a slot a
  // binding file adjudicates.
  Tally tally;
  std::set<std::pair<uint16_t, int>> adjudicated;
  for (const s::GsEfxJoinRow& row : s::kGsEfxJoin) {
    adjudicated.insert({row.type, row.slot});
    adjudicated.insert({s::gs_efx_alias_type(row.type), row.slot});
  }
  int moved = 0;
  for (const EfxType& type_row : all_rows()) {
    const std::string base = signature(gs_efx_insert_chain(make_efx(type_row.type)));
    for (size_t slot = 0; slot < GsEfx{}.params.size(); ++slot) {
      bool moves = false;
      for (uint8_t value : kValues) {
        GsEfx efx = make_efx(type_row.type);
        efx.params[slot] = value;
        if (signature(gs_efx_insert_chain(efx)) != base) moves = true;
      }
      if (!moves) continue;
      ++moved;
      tally.same(adjudicated.count({type_row.type, static_cast<int>(slot)}) == 1,
                 hex4(type_row.type) + " slot " + std::to_string(slot) +
                     " moves the chain and no binding file adjudicates it");
    }
  }
  WARN("slots that move a chain: " << moved << "  comparisons: " << tally.count());
  REQUIRE(tally.count() >= 770);
}

TEST_CASE("the chain skeleton writes no control a binding row owns", "[midi][sf2][gs][efxtypes]") {
  // A key the skeleton wrote would be replaced by the row naming it, and while
  // the two agree in value nothing downstream can tell which of them wrote it.
  // So the skeleton is built with no rows at all and may carry none of the keys
  // its type's rows bind.
  Tally tally;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    const std::string stage(s::kGsEfxRowStages[row.stage]);
    const std::string key(s::kGsEfxRowKeys[row.key]);
    for (uint8_t value : kValues) {
      GsEfx efx = make_efx(row.type);
      efx.params[row.slot] = value;
      const auto skeleton = gs_efx_insert_chain(efx, kNoRows);
      const GsEfxStage* found = find_stage(skeleton, stage, row.ordinal);
      double unused = 0.0;
      tally.same(found == nullptr || !json_number(found->params_json, key, unused),
                 hex4(row.type) + " slot " + std::to_string(row.slot) + ": the skeleton writes " +
                     stage + "." + key + ", which a binding row owns");
    }
  }
  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= static_cast<int>(s::kGsEfxBindingRows.size()));
}

namespace {

/// A fresh unit holding @p type, selected over the wire so its power-on bytes
/// load, which is how the archive took every sweep.
GsEfx selected(uint16_t type) {
  GsEfx efx;
  const auto write = efx_sysex(
      0x00, {static_cast<uint8_t>((type >> 8) & 0x7Fu), static_cast<uint8_t>(type & 0x7Fu)});
  apply_gs_efx_sysex(efx, write.data(), write.size());
  return efx;
}

void write_parameter(GsEfx& efx, uint8_t slot, uint8_t value) {
  const auto write = efx_sysex(static_cast<uint8_t>(0x03 + slot), {value});
  apply_gs_efx_sysex(efx, write.data(), write.size());
}

double equaliser_hz(const GsEfx& efx, const std::string& key) {
  for (const GsEfxStage& stage : gs_efx_insert_chain(efx)) {
    double hz = 0.0;
    if (stage.name == "eq.parametric" && json_number(stage.params_json, key, hz)) return hz;
  }
  return 0.0;
}

}  // namespace

TEST_CASE("a byte past a printed list of states is not taken", "[midi][sf2][gs][efxtypes]") {
  Tally tally;

  // 01 00's corners power up in the second state, and the archive read each one
  // past byte 1 with the type reloaded before every take. Floor: one band of the
  // twelfth-octave set the half-gain points are read on.
  constexpr double kCornerFloorOctaves = 1.0 / 12.0;
  const std::array<uint8_t, 8> kSettings = {2, 3, 4, 32, 64, 96, 126, 127};
  const std::array<double, 8> kLowHz = {222.7, 210.2, 222.7, 222.7, 210.2, 222.7, 210.2, 210.2};
  const std::array<double, 8> kHighHz = {11313.7, 10678.7, 10678.7, 10678.7,
                                         11313.7, 11313.7, 10678.7, 10678.7};
  for (std::size_t i = 0; i < kSettings.size(); ++i) {
    GsEfx efx = selected(0x0100);
    write_parameter(efx, 0, kSettings[i]);
    write_parameter(efx, 2, kSettings[i]);
    const std::string at = ", setting " + std::to_string(kSettings[i]);
    tally.same(std::fabs(std::log2(equaliser_hz(efx, "band0.frequencyHz") / kLowHz[i])) <=
                   kCornerFloorOctaves,
               "low corner" + at);
    tally.same(std::fabs(std::log2(equaliser_hz(efx, "band3.frequencyHz") / kHighHz[i])) <=
                   kCornerFloorOctaves,
               "high corner" + at);
  }

  // Standing in the first state, nought against two is null.
  GsEfx first = selected(0x0100);
  write_parameter(first, 0, 0);
  const double first_hz = equaliser_hz(first, "band0.frequencyHz");
  write_parameter(first, 0, 2);
  tally.same(equaliser_hz(first, "band0.frequencyHz") == first_hz,
             "a corner standing at 0 holds it against 2");

  // Every listed slot, from each end of its list: one past the top and the top
  // of the byte leave it where it stood.
  for (const s::GsEfxStateList& list : s::kGsEfxStateLists) {
    for (const uint8_t standing : {uint8_t{0}, static_cast<uint8_t>(list.states - 1)}) {
      GsEfx efx = selected(list.type);
      write_parameter(efx, list.parameter, standing);
      for (const uint8_t past : {list.states, uint8_t{127}}) {
        write_parameter(efx, list.parameter, past);
        tally.same(efx.params[list.parameter] == standing,
                   hex4(list.type) + " slot " + std::to_string(list.parameter) + " at " +
                       std::to_string(standing) + " holds against " + std::to_string(past));
      }
    }
  }

  // A slot printing no list takes every byte.
  GsEfx drive = selected(0x0110);
  write_parameter(drive, 0, 127);
  tally.same(drive.params[0] == 127, "the overdrive's drive takes 127");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 16 + 1 + 4 * static_cast<int>(s::kGsEfxStateLists.size()) + 1);
}
