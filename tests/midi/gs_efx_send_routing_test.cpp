/// @file gs_efx_send_routing_test.cpp
/// @brief GS EFX routing/automation fixes for the SC-88 SoundFont player:
///        (1) an insertion-effect part sends its POST-effect signal to the
///        system reverb, scaled by the EFX unit's send amount (GS 40 03 17),
///        instead of the clean pre-effect signal; (2) a parameter-only EFX
///        change updates the live insert processors in place rather than
///        rebuilding the chain (which would zero their DSP state).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#include "util/json.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;

/// A unit holding @p type with that type's own power-on parameters, which is
/// the state selecting it over the wire leaves.
GsEfx efx_holding(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

// GS SysEx (Roland DT1, framed): enable EFX on part 1 (channel 0), select
// Overdrive (01 10), write EFX PARAMETER 2 (40 03 04) at max. Checksums per the
// DT1 rule. That slot is the amp selector rather than the drive, which is at
// PARAMETER 1; what the case needs of it is only that it is a parameter write.
constexpr uint8_t kPartOn[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
constexpr uint8_t kOdType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
constexpr uint8_t kOdDrive[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x04, 0x7F, 0x3A, 0xF7};
// A genuine TYPE change: select Stereo Chorus (01 42).
constexpr uint8_t kChorusType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                   0x03, 0x00, 0x01, 0x42, 0x7A, 0xF7};

// Only used by the post-effect reverb-routing test below, which is itself
// gated on SONARE_MIDI_WITH_FX and SONARE_WITH_MASTERING (it needs the EFX
// insertion chain, built through mastering::api::make_insert, to build).
#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
using sonare::test::event;

/// Framed EFX -> reverb send write (GS address 40 03 17) with the DT1 checksum
/// computed for @p value.
std::array<uint8_t, 11> efx_reverb_send(uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x17, value, 0x00, 0xF7};
  // Checksum sums the address + data bytes (40 03 17 value = indices 5..8).
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// Fixture: program 1 = a short one-shot burst (so the dry signal ends well
/// before the reverb tail is measured).
std::shared_ptr<Sf2File> make_fixture() {
  constexpr double kTwoPi = 6.28318530717958647692;
  Sf2Builder b;
  std::vector<float> burst(256);
  for (size_t i = 0; i < burst.size(); ++i) {
    const float envl = 1.0f - static_cast<float>(i) / 256.0f;
    burst[i] = envl * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 16.0));
  }
  const int burst_id = b.add_sample("burst", burst, 48000, 60, 0, 256);
  Sf2Builder::ZoneSpec oneshot;
  oneshot.target = burst_id;
  const int burst_inst = b.add_instrument("burst", {oneshot});
  Sf2Builder::ZoneSpec pz;
  pz.target = burst_inst;
  b.add_preset("Burst", 0, 1, {pz});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}
#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

/// A minimal insert processor that only counts lifecycle/parameter calls, so a
/// test can tell an in-place parameter update (set_parameter, no prepare/reset)
/// apart from a full rebuild (a fresh processor, prepare()). The named
/// parameters are supplied by the case, so each one stands in for the insert the
/// type it selects would really build.
struct EfxCounters {
  int prepares = 0;
  int resets = 0;
  int set_params = 0;
};

class CountingInsert final : public sonare::rt::ProcessorBase {
 public:
  CountingInsert(std::shared_ptr<EfxCounters> counters, std::vector<std::string> keys)
      : counters_(std::move(counters)), keys_(std::move(keys)) {}
  void prepare(double, int) override { ++counters_->prepares; }
  void process(float* const*, int, int) override {}
  void reset() override { ++counters_->resets; }
  bool set_parameter(unsigned int, float) override {
    ++counters_->set_params;
    return true;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (size_t i = 0; i < keys_.size(); ++i) {
      out.push_back({keys_[i], static_cast<unsigned int>(i)});
    }
    return out;
  }

 private:
  std::shared_ptr<EfxCounters> counters_;
  std::vector<std::string> keys_;
};

/// The keys the Overdrive translation emits.
const std::vector<std::string> kAmpSimKeys = {"drive", "ampModel", "levelDb"};

/// The stereo delay's automatable keys, which the delay types' translations now
/// write into.
const std::vector<std::string> kStereoDelayKeys = {"delayTimeLMs", "delayTimeRMs", "feedback",
                                                   "pingPong",     "dryWet",       "dampingHz"};

/// The rotary's automatable keys. Its acceleration fields are deliberately NOT
/// among them: they size and shape the glide rather than ride it, so the rotary
/// translation's edits fall back to a rebuild by construction.
const std::vector<std::string> kRotaryKeys = {"rateHz", "depthMs", "tremolo", "dryWet",
                                              "drumRateHz"};

/// A framed GS DT1 write of one byte into the EFX parameter block at @p offset,
/// with the checksum. Offset 0x03 is EFX PARAMETER 1.
std::array<uint8_t, 11> efx_param_write(uint8_t offset, uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset, value, 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// A framed EFX type selection.
std::array<uint8_t, 12> efx_type_write(uint8_t msb, uint8_t lsb) {
  std::array<uint8_t, 12> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9];
  m[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

}  // namespace

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
TEST_CASE("a GS EFX part sends its post-effect signal to reverb", "[midi][sf2][gsefx]") {
  // The insertion-effect (Overdrive) stage must actually build for the part to
  // be bussed; skip if the amp-sim insert is unavailable in this build.
  if (sonare::mastering::api::make_insert("saturation.ampSim", "{}") == nullptr) return;

  auto reverb_tail = [](uint8_t send) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.insert_factory = [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    Sf2Player player(cfg);
    player.set_soundfont(make_fixture());
    player.prepare(kOutRate, 256);
    // Route part 1 (channel 0) through the Overdrive insertion effect.
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    player.on_control_sysex(kOdType, sizeof(kOdType));
    player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
    // Set the EFX -> reverb send amount (GS 40 03 17).
    const std::array<uint8_t, 11> rev = efx_reverb_send(send);
    player.on_control_sysex(rev.data(), rev.size());

    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));

    std::vector<float> left(24000, 0.0f);
    std::vector<float> right(24000, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    player.process(chans, 2, 24000);
    // Well after the dry burst (~5 ms): only the post-effect reverb tail is here.
    return rms(left, 4800, 24000) + rms(right, 4800, 24000);
  };

  // The dry bus (send 0) is identical for every send amount, so any increase in
  // the measured tail is purely the reverb return derived from the POST-effect
  // signal. Before the fix the EFX send was never applied and all three would be
  // equal (the pre-effect CC path only).
  const float dry = reverb_tail(0);
  const float mid = reverb_tail(64);
  const float full = reverb_tail(127);
  REQUIRE(mid > dry + 1e-6f);
  REQUIRE(full > mid + 1e-6f);
}
#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("a GS EFX parameter-only change updates the insert in place", "[midi][sf2][gsefx]") {
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [counters](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, kAmpSimKeys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  // Route part 1 (channel 0) through an EFX and select Overdrive: the chain is
  // built and every stage of it prepared. The count is the chain's length --
  // the drive block plus the output stage the unit puts after every effect --
  // and what matters below is that it does not move again.
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  player.on_control_sysex(kOdType, sizeof(kOdType));
  REQUIRE(counters->prepares == static_cast<int>(gs_efx_insert_chain(efx_holding(0x0110)).size()));
  const int prepares_after_build = counters->prepares;
  const int set_params_before = counters->set_params;

  // A parameter-only edit (40 03 04, the byte beside the drive) must NOT rebuild
  // the chain: it is resolved on the control thread and applied to the live
  // processor on the audio thread at the next block. on_control_sysex enqueues
  // but does not touch the processor; the set_parameter lands during the
  // following process() call.
  player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  REQUIRE(counters->prepares == prepares_after_build);  // not rebuilt
  REQUIRE(counters->set_params == set_params_before);   // not applied synchronously

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, 256);  // audio thread drains the queue and applies it

  REQUIRE(counters->prepares == prepares_after_build);  // still no rebuild
  REQUIRE(counters->resets == 0);                       // DSP state preserved
  REQUIRE(counters->set_params > set_params_before);    // applied on the audio thread

  // A genuine EFX TYPE change still triggers a full rebuild (a fresh processor,
  // hence another prepare()): exact behaviour is preserved when it is needed.
  player.on_control_sysex(kChorusType, sizeof(kChorusType));
  REQUIRE(counters->prepares > prepares_after_build);
}

TEST_CASE("the measured EFX translations reach automatable insert parameters",
          "[midi][sf2][gsefx]") {
  // The failure this is the net for: a field added to an insert for a measured
  // conversion, wired into the translation, and never given a descriptor. The
  // edit is not lost — enqueue_efx_param_updates falls back to a rebuild — but
  // every parameter edit then zeroes the delay and reverb tails it was written
  // to preserve, and nothing else in the tree can see that happen.
  auto run = [](const std::vector<std::string>& keys, const std::array<uint8_t, 12>& type,
                const std::array<uint8_t, 11>& edit) {
    auto counters = std::make_shared<EfxCounters>();
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.insert_factory = [counters, keys](std::string_view, std::string_view) {
      return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
    };
    Sf2Player player(cfg);
    player.prepare(kOutRate, 256);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    player.on_control_sysex(type.data(), type.size());
    REQUIRE(counters->prepares >= 1);
    const int prepares_after_build = counters->prepares;
    player.on_control_sysex(edit.data(), edit.size());
    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    player.process(chans, 2, 256);
    return std::pair<int, int>{counters->prepares - prepares_after_build, counters->set_params};
  };

  SECTION("a delay time edit is applied in place") {
    // Stereo Delay (01 50), EFX PARAMETER 1 at 40 03 03: the left tap's time.
    const auto result =
        run(kStereoDelayKeys, efx_type_write(0x01, 0x50), efx_param_write(0x03, 40));
    REQUIRE(result.first == 0);  // no rebuild
    REQUIRE(result.second > 0);  // applied through set_parameter
  }

  SECTION("a damping edit is applied in place") {
    // Same type, EFX PARAMETER 8 at 40 03 0A: the corner of the pole inside the
    // feedback loop, which is the field the damping measurement added.
    const auto result =
        run(kStereoDelayKeys, efx_type_write(0x01, 0x50), efx_param_write(0x0A, 40));
    REQUIRE(result.first == 0);
    REQUIRE(result.second > 0);
  }

  SECTION("a rotary acceleration edit rebuilds, because the glide is structure") {
    // Rotary (01 22), EFX PARAMETER 7 at 40 03 09: the horn rotor's acceleration
    // byte. It reaches accelTauS / undershootHz, neither of which the rotary
    // publishes as an automatable parameter, so the edit costs a rebuild. Pinned
    // rather than fixed: a time constant read on the audio thread mid-glide has
    // no defined arrival, and the insert says so by not offering the id.
    const auto result = run(kRotaryKeys, efx_type_write(0x01, 0x22), efx_param_write(0x09, 40));
    REQUIRE(result.first > 0);
    REQUIRE(result.second == 0);
  }
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)
namespace {

namespace json = sonare::util::json;
namespace s = sonare::midi::synth;

/// A control a binding row drives that its insert does not publish as
/// realtime-automatable, with the reason it does not.
///
/// An entry excusing nothing fails, so a descriptor added later retires its
/// entry rather than leaving a note that has stopped being true.
struct Unautomated {
  std::string_view stage;
  std::string_view key;
  std::string_view reason;
};

/// Two are lengths an insert sizes a buffer by in prepare(); the rest are the
/// rotary's acceleration fields, the glide's shape rather than anything a
/// running rotor rides.
constexpr std::array<Unautomated, 6> kUnautomated = {{
    {"effects.reverb.dattorro", "preDelayMs",
     "the pre-delay line is sized by it in prepare(), so a live write would allocate on the audio "
     "thread; an edit of the byte rebuilds the reverb, which is the cost the insert already "
     "charges any caller for this key"},
    {"effects.modulation.pitchShifter", "windowMs",
     "the grain buffers are sized by it in prepare(), so a live write would allocate on the audio "
     "thread; an edit of the byte rebuilds the shifter, the same cost the reverb's pre-delay "
     "charges"},
    {"effects.modulation.rotary", "undershootHz",
     "one byte writes this with accelTauS and decelTauS, and a time constant read mid-glide has "
     "no defined arrival, so publishing this half alone would apply the byte partly in place and "
     "partly by rebuild"},
    {"effects.modulation.rotary", "accelTauS",
     "a time constant read on the audio thread mid-glide has no defined arrival, so the insert "
     "offers no id for it and an edit of the byte rebuilds the rotor"},
    {"effects.modulation.rotary", "decelTauS",
     "the same byte writes it with accelTauS, for the same reason"},
    {"effects.modulation.rotary", "drumUndershootHz",
     "its byte drives nothing else, so it could be ridden; the drum rotor publishes what the horn "
     "rotor publishes, and one rotor automating a field the other cannot is a difference in the "
     "insert rather than in the machine"},
}};

}  // namespace

TEST_CASE("every EFX binding drives a control its insert can automate", "[midi][sf2][gsefx]") {
  // The failure this is the net for: a field added to an insert for a measured
  // conversion, wired into a binding row, and never given a realtime
  // descriptor. The edit is not lost -- a parameter write that finds no id
  // falls back to rebuilding the chain -- but every edit then zeroes the delay
  // and reverb tails the in-place path exists to preserve.
  //
  // The case above measures that against a stand-in insert whose descriptors
  // the case itself supplies, so it cannot answer for the inserts the bindings
  // actually name. This one asks the factory.
  std::map<std::string, std::set<std::string>> automatable;
  std::map<std::string, std::set<std::string>> accepted;
  for (std::string_view stage : s::kGsEfxBindingStages) {
    const std::string name(stage);
    const json::Value parsed =
        json::parse_strict(sonare::mastering::api::insert_param_info_json(name));
    INFO("insert " << name);
    REQUIRE(parsed.is_array());
    std::set<std::string> ids;
    for (const json::Value& parameter : parsed.as_array()) {
      const json::Value* id = parameter.find("name");
      REQUIRE(id != nullptr);
      ids.insert(id->as_string());
    }
    // An insert name the factory does not know and one whose build feature is
    // off both answer with an empty array, which would excuse every key on it.
    REQUIRE_FALSE(ids.empty());
    automatable.emplace(name, std::move(ids));

    const std::vector<std::string> keys = sonare::mastering::api::insert_param_names(name);
    REQUIRE_FALSE(keys.empty());  // same two ways of answering nothing
    accepted.emplace(name, std::set<std::string>(keys.begin(), keys.end()));
  }

  std::set<std::pair<std::string, std::string>> seen;
  std::set<std::size_t> used;
  int checked = 0;
  for (const s::GsEfxBinding& row : s::kGsEfxBindings) {
    const std::string stage(s::kGsEfxBindingStages[row.stage]);
    const std::string key(s::kGsEfxBindingKeys[row.key]);
    if (!seen.emplace(stage, key).second) continue;  // one verdict per control
    ++checked;

    // A key the insert does not read is dropped in silence at construction, so
    // there is no excused list for this half: the byte reaches nothing at all
    // rather than reaching something that cannot be ridden.
    {
      INFO(stage << "." << key << " is driven by a binding row and is not a key that insert reads");
      CHECK(accepted[stage].count(key) != 0);
    }

    if (automatable[stage].count(key) != 0) continue;
    std::size_t excused = kUnautomated.size();
    for (std::size_t i = 0; i < kUnautomated.size(); ++i) {
      if (kUnautomated[i].stage == stage && kUnautomated[i].key == key) excused = i;
    }
    INFO(stage << "." << key << " is driven by a binding row, is not automatable, and is not"
               << " listed as one that may not be");
    CHECK(excused != kUnautomated.size());
    if (excused != kUnautomated.size()) used.insert(excused);
  }

  for (std::size_t i = 0; i < kUnautomated.size(); ++i) {
    INFO(kUnautomated[i].stage << "." << kUnautomated[i].key
                               << " is listed as one that may not be automated, and no binding row"
                               << " drives it: " << kUnautomated[i].reason);
    CHECK(used.count(i) != 0);
  }

  // A floor rather than an equality: binding one more control is not a
  // regression, and a ceiling would read it as one.
  WARN("distinct (insert, control) pairs checked: " << checked);
  REQUIRE(checked >= 42);
}
#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING
