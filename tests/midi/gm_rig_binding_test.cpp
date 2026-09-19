/// @file gm_rig_binding_test.cpp
/// @brief The bank's per-program default rig (src/midi/synth/docs/voicing.md):
///        the amplifier an electric guitar is heard through is a stage the bank
///        binds, not something baked into the voice. What is checked here is
///        the acceptance criterion the page states — a spec-compliant file
///        selecting program 30 and sending no insertion effect comes out
///        amplified — together with the four cases where the binding must NOT
///        apply, since a default that cannot be cleared is the bake it replaces.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/saturation/amp_presets.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "mixing/api/scene.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gm_fallback_rig;
using sonare::midi::synth::gm_rig_binding;
using sonare::midi::synth::gm_rig_preset_name;
using sonare::midi::synth::GmFallbackRig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2InsertType;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

using sonare::test::event;

/// A SoundFont covering one program only, so every other program falls to the
/// model floor where the binding lives.
std::shared_ptr<Sf2File> one_program_fixture(uint16_t program) {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.9f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sample_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);
  Sf2Builder::ZoneSpec looped;
  looped.gens.push_back({54 /*sampleModes*/, 1});
  looped.target = sample_id;
  const int inst = b.add_instrument("sine", {looped});
  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Sine", 0, program, {pz});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

std::vector<float> render_program(Sf2PlayerConfig cfg, uint8_t program,
                                  std::shared_ptr<Sf2File> soundfont = nullptr,
                                  int num_samples = 24000) {
  if (!(cfg.gain > 0.0f)) cfg.gain = 1.0f;
  // The bank rig realises on the same thread split as the insertion effect, so
  // a single-threaded render has to say it is one.
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  if (soundfont != nullptr) player.set_soundfont(std::move(soundfont));
  player.prepare(kOutRate, 256);
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 52, 110)));
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, num_samples);
  return left;
}

Sf2PlayerConfig with_factory() {
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  return cfg;
}

/// Peak-to-RMS in dB. An amplifier's power stage and its speaker compress, so a
/// signal that has been through one sits closer to its own average than the
/// direct signal does — which is the property no gain change can imitate.
double crest_db(const std::vector<float>& buf) {
  double peak = 0.0;
  double acc = 0.0;
  for (const float s : buf) {
    peak = std::max(peak, static_cast<double>(std::fabs(s)));
    acc += static_cast<double>(s) * s;
  }
  const double r = std::sqrt(acc / static_cast<double>(std::max<size_t>(buf.size(), 1)));
  if (!(r > 0.0) || !(peak > 0.0)) return 0.0;
  return 20.0 * std::log10(peak / r);
}

}  // namespace

TEST_CASE("the bank binds a rig to the electric guitars and to nothing else", "[midi][sf2][rig]") {
  // The six GM electric guitars. 26-28 and 31 share a clean amplifier; the two
  // driven programs each have their own, which is what makes them different
  // programs once the drive left the voice.
  REQUIRE(gm_fallback_rig(0, 26).id == gm_fallback_rig(0, 27).id);
  REQUIRE(gm_fallback_rig(0, 27).id == gm_fallback_rig(0, 28).id);
  REQUIRE(gm_fallback_rig(0, 28).id == gm_fallback_rig(0, 31).id);
  REQUIRE(gm_fallback_rig(0, 26).id != 0);
  REQUIRE(gm_fallback_rig(0, 29).id != gm_fallback_rig(0, 26).id);
  REQUIRE(gm_fallback_rig(0, 30).id != gm_fallback_rig(0, 29).id);
  for (const uint8_t bound : {26, 27, 28, 29, 30, 31}) {
    const GmFallbackRig rig = gm_fallback_rig(0, bound);
    REQUIRE(std::string(rig.preset) != "");
    // Resolving the id back has to give the same binding, since that round trip
    // is how a part's published rig reaches the builder.
    REQUIRE(std::string(gm_rig_binding(rig.id).preset) == std::string(rig.preset));
    REQUIRE(gm_rig_binding(rig.id).drive == rig.drive);
  }
  // The acoustic guitars either side, and the basses. An instrument whose whole
  // sound is its own voice binds nothing, which is every other program.
  int bound_count = 0;
  for (int p = 0; p < 128; ++p) {
    if (gm_fallback_rig(0, static_cast<uint8_t>(p)).id != 0) ++bound_count;
  }
  REQUIRE(bound_count == 6);
  REQUIRE(gm_fallback_rig(0, 24).id == 0);
  REQUIRE(gm_fallback_rig(0, 25).id == 0);
  REQUIRE(gm_fallback_rig(0, 32).id == 0);
  REQUIRE(gm_fallback_rig(0, 33).id == 0);
  // A rhythm part carries no melodic program at all.
  REQUIRE(gm_fallback_rig(128, 30).id == 0);
  REQUIRE(gm_rig_binding(0).id == 0);
  REQUIRE(gm_rig_binding(200).id == 0);
}

#if defined(SONARE_WITH_MASTERING)

TEST_CASE("every amplifier a binding can name is one the mastering module has",
          "[midi][sf2][rig]") {
  // The synth reaches an amplifier only through the injected insert factory, so
  // it names one by string and its selector indexes a list of its own. Nothing
  // in a build holds that list against the real one — a name that drifts is not
  // a compile error, it is an insert the factory declines to make and a voice
  // that silently comes out direct.
  const std::vector<std::string> known = sonare::mastering::saturation::amp_preset_names();
  std::vector<std::string> named;
  for (uint8_t i = 0; const char* name = gm_rig_preset_name(i); ++i) named.emplace_back(name);
  REQUIRE(named == known);
  // And each binding's default still names the amplifier the bank was voiced
  // through, which is what makes the selector free in a shipped build.
  REQUIRE(std::string(gm_rig_binding(1).preset) == "cleanCombo");
  REQUIRE(std::string(gm_rig_binding(2).preset) == "classicCrunch");
  REQUIRE(std::string(gm_rig_binding(3).preset) == "classicCrunch");
}

TEST_CASE("a file that selects program 30 and asks for nothing comes out amplified",
          "[midi][sf2][rig]") {
  Sf2PlayerConfig off = with_factory();
  off.bank_rig_binding = false;
  const std::vector<float> di = render_program(off, 30);
  const std::vector<float> rigged = render_program(with_factory(), 30);
  REQUIRE(crest_db(di) > 0.0);
  // Not a level change: an amplifier compresses, so the rigged signal sits
  // closer to its own average however the trim is set.
  REQUIRE(crest_db(rigged) < crest_db(di) - 3.0);
}

TEST_CASE("each bound rig drives its amplifier where the bank's own level puts it",
          "[midi][sf2][rig]") {
  // The presets are voiced for a full-scale input and the bank's electric guitar
  // arrives 12 dB under one, so what a binding's drive has to be checked against
  // is the level the bank actually delivers rather than the preset's own.
  auto thd_at_bank_level = [](const GmFallbackRig& rig) {
    auto proc = sonare::mastering::api::make_insert(
        "saturation.ampSim", std::string("{\"preset\":\"") + rig.preset +
                                 "\",\"drive\":" + std::to_string(rig.drive) + "}");
    REQUIRE(proc != nullptr);
    proc->prepare(kOutRate, 256);
    // A whole number of blocks: a tail that ran past the last processed block
    // would measure raw input, which sits 48 dB above what the amplifier answers.
    std::vector<float> l(24576, 0.0f), r(24576, 0.0f);
    for (size_t i = 0; i < l.size(); ++i) {
      l[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * 220.0 * double(i) / kOutRate));
      r[i] = l[i];
    }
    for (size_t off = 0; off + 256 <= l.size(); off += 256) {
      float* blk[2] = {l.data() + off, r.data() + off};
      proc->process(blk, 2, 256);
    }
    const std::vector<float> tail(l.begin() + 12288, l.end());
    auto bin = [&](double hz) {
      double re = 0.0, im = 0.0;
      for (size_t i = 0; i < tail.size(); ++i) {
        const double w = kTwoPi * hz * static_cast<double>(i) / kOutRate;
        re += tail[i] * std::cos(w);
        im += tail[i] * std::sin(w);
      }
      return re * re + im * im;
    };
    double harm = 0.0;
    for (int k = 2; k <= 10; ++k) harm += bin(220.0 * k);
    return 100.0 * std::sqrt(harm / std::max(bin(220.0), 1e-30));
  };
  // A clean amplifier is not a transparent one, and an overdriven one is not a
  // fuzz: an order of magnitude between them is the whole difference between
  // programs 27 and 29.
  const double clean = thd_at_bank_level(gm_fallback_rig(0, 27));
  const double crunch = thd_at_bank_level(gm_fallback_rig(0, 29));
  REQUIRE(clean > 0.3);
  REQUIRE(clean < 3.0);
  REQUIRE(crunch > 10.0);
  // Programs 29 and 30 are one amplifier at two gains, which is what separates
  // them on a module — so the distortion guitar has to distort MORE than the
  // overdriven one rather than merely differently. A brighter preset for 30 put
  // its 5 kHz band 12.6 dB over its reference against this rig's 3.6.
  const double lead = thd_at_bank_level(gm_fallback_rig(0, 30));
  REQUIRE(lead > crunch);
}

TEST_CASE("the bank rig is removable, and absent everywhere it was not bound", "[midi][sf2][rig]") {
  Sf2PlayerConfig off = with_factory();
  off.bank_rig_binding = false;
  // Clearing the binding gives back the instrument alone, bit for bit: a
  // default that cannot be cleared is the baked-in effect this design replaces.
  SECTION("cleared by the host") {
    Sf2PlayerConfig on = with_factory();
    // Program 0 binds nothing, so the flag has nothing to change.
    REQUIRE(render_program(on, 0) == render_program(off, 0));
  }
  SECTION("a host that wires no insert factory gets the instrument alone") {
    Sf2PlayerConfig no_factory;
    REQUIRE(render_program(no_factory, 30) == render_program(off, 30));
  }
  SECTION("a SoundFont's own electric guitar keeps the amplifier inside it") {
    // The sample was recorded through one. A second amplifier on top would be
    // exactly the bake the separation removes, so the part stays direct.
    auto sf2 = one_program_fixture(30);
    REQUIRE(render_program(with_factory(), 30, sf2) == render_program(off, 30, sf2));
  }
  SECTION("a part the host gave an insert of its own keeps it") {
    Sf2PlayerConfig configured = with_factory();
    configured.part_inserts[0].type = Sf2InsertType::kProcessor;
    configured.part_inserts[0].stages = {{"saturation.ampSim", R"({"preset":"cleanCombo"})"}};
    Sf2PlayerConfig same = configured;
    same.bank_rig_binding = false;
    REQUIRE(render_program(configured, 30) == render_program(same, 30));
  }
}

TEST_CASE("a program change within one rig does not rebuild the amplifier", "[midi][sf2][rig]") {
  Sf2PlayerConfig cfg = with_factory();
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  auto settle = [&] { player.process(chans, 2, 256); };
  settle();
  REQUIRE_FALSE(player.gs_efx_dirty());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 27)));
  REQUIRE(player.gs_efx_dirty());  // no rig -> clean combo
  settle();
  // 27 and 28 are heard through the same amplifier, so the part keeps the one
  // it is already running rather than rebuilding an identical one and losing
  // its state.
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 28)));
  REQUIRE_FALSE(player.gs_efx_dirty());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 30)));
  REQUIRE(player.gs_efx_dirty());  // a different amplifier
  settle();
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 0)));
  REQUIRE(player.gs_efx_dirty());  // back to an instrument that binds nothing
}

TEST_CASE("a bank rig is a chain, and every block it can carry is a real insert",
          "[midi][sf2][rig]") {
  using sonare::midi::synth::gm_rig_chain;
  using sonare::midi::synth::gm_rig_stage_name;
  // The vocabulary is the GS multi-effect's, and the synth names a block by
  // string for the same reason it names an amplifier by one — it never links
  // the module that makes them. A name that drifts is not a compile error; it
  // is a stage the factory declines to build and a rig quietly one block short.
  const std::vector<std::string> inserts = sonare::mastering::api::insert_factory_names();
  for (uint8_t i = 1; const char* stage = gm_rig_stage_name(i); ++i) {
    INFO(stage);
    REQUIRE(std::find(inserts.begin(), inserts.end(), std::string(stage)) != inserts.end());
  }
  REQUIRE(std::string(gm_rig_stage_name(0)).empty());  // index 0 is no stage

  // Every bound rig realises, and ships as the amplifier alone: the pedal and
  // rack slots are empty until an ear puts something in one.
  for (const uint8_t bound : {26, 27, 28, 29, 30, 31}) {
    const uint8_t id = gm_fallback_rig(0, bound).id;
    INFO("program " << int(bound));
    const std::vector<sonare::midi::synth::GsEfxStage> chain = gm_rig_chain(id);
    REQUIRE(chain.size() == 1);
    REQUIRE(chain.front().name == "saturation.ampSim");
    // The stage carries the binding's own numbers rather than a second spelling
    // of them, which is what a caller reading a rig has to be able to rely on.
    REQUIRE(chain.front().params_json.find(gm_fallback_rig(0, bound).preset) != std::string::npos);
    REQUIRE(sonare::mastering::api::make_insert(chain.front().name, chain.front().params_json) !=
            nullptr);
  }
  REQUIRE(gm_rig_chain(0).empty());
  REQUIRE(gm_rig_chain(200).empty());
}

TEST_CASE("a host describes a rig in the mixing scene's own words", "[midi][sf2][rig]") {
  // The rig format is not a new one. A scene already says what a chain of
  // inserts is — `{"processor": ..., "params": ...}` in signal order — so a host
  // that can save a channel strip can save a guitar rig, and the synth takes the
  // decoded list rather than the JSON, which is what keeps it from linking the
  // mixing module to read one.
  const std::string scene_json = R"({"version":1,"strips":[{"id":"gtr","inserts":[
      {"processor":"dynamics.gate","params":"{}","slot":"pre"},
      {"processor":"saturation.ampSim","params":"{\"preset\":\"britStack\",\"drive\":0.8}","slot":"pre"},
      {"processor":"effects.delay.stereo","params":"{}","slot":"pre"}]}]})";
  const sonare::mixing::api::Scene scene = sonare::mixing::api::scene_from_json(scene_json);
  REQUIRE(scene.strips.size() == 1);
  REQUIRE(scene.strips.front().inserts.size() == 3);

  Sf2PlayerConfig cfg = with_factory();
  cfg.part_inserts[0].type = Sf2InsertType::kProcessor;
  for (const sonare::mixing::api::Insert& insert : scene.strips.front().inserts) {
    cfg.part_inserts[0].stages.push_back({insert.processor_name, insert.params_json});
  }
  // Three stages in, three stages out, and audibly not the bank's own rig: the
  // host's chain replaces the default the same way one stage always did.
  const std::vector<float> host_rig = render_program(cfg, 30);
  const std::vector<float> bank_rig = render_program(with_factory(), 30);
  REQUIRE(host_rig != bank_rig);
  REQUIRE(crest_db(host_rig) > 0.0);
}

TEST_CASE("a rig's tone controls are absent until one is turned", "[midi][sf2][rig]") {
  using sonare::midi::synth::gm_rig_chain;
  // The amp builder reads an absent key as "keep the preset's own voicing", so a
  // control at rest has to be left out of the params rather than sent as 0 dB.
  // Sending zero would flatten a stack every preset voices on purpose, and it
  // would do it silently -- the render would simply be a different amplifier.
  for (const uint8_t bound : {26, 27, 28, 29, 30, 31}) {
    INFO("program " << int(bound));
    const std::string params = gm_rig_chain(gm_fallback_rig(0, bound).id).front().params_json;
    for (const char* control : {"bassDb", "midDb", "trebleDb", "presenceDb"}) {
      INFO(control);
      REQUIRE(params.find(control) == std::string::npos);
    }
    // What a rig always says: which amplifier, how hard it is driven, and the
    // two trims either side of it.
    for (const char* always : {"preset", "inputDb", "drive", "levelDb"}) {
      INFO(always);
      REQUIRE(params.find(always) != std::string::npos);
    }
    REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", params) != nullptr);
  }
}

#endif  // SONARE_WITH_MASTERING
