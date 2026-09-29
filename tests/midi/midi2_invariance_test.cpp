/// @file midi2_invariance_test.cpp
/// @brief A MIDI 1.0 performance and the same performance converted to MIDI 2.0
///        render sample for sample identically on every synth.
///
/// Two conversions are held to the MIDI 1.0 render: the Default Translation
/// (Midi1ToMidi2Translator) and the translation CoreMIDI's 2.0 input port
/// performs, which scales every value min-center-max (Registered / Assignable
/// Controller data included), keeps a velocity-0 Note On as an MT 0x2 message and
/// delivers CC 0 / CC 32 both as Control Changes and as the Program Change bank.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::Midi1ToMidi2Translator;
using sonare::midi::SynthWaveform;
using sonare::midi::Ump;
using sonare::midi::UmpMessageType;
using sonare::midi::UmpStatus;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::event;
using sonare::test::render_stereo;

namespace midi = sonare::midi;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kStepFrames = 480;
constexpr int kReleaseFrames = 960;
constexpr uint8_t kMelodicChannel = 0;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kNote = 60;
constexpr uint8_t kDrumNote = 38;
constexpr const char* kSf2Path = "tests/fixtures/sf2/minimal_gs.sf2";

/// Events sent together, then one render of `frames`.
struct Step {
  std::vector<Ump> events;
  int frames;
};
using Performance = std::vector<Step>;

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

// ---- The MIDI 1.0 performance ----------------------------------------------

Ump cc(uint8_t ch, uint8_t num, uint8_t value) {
  return midi::make_midi1_control_change(0, ch, num, value);
}

void add_data_entry(std::vector<Ump>& e, uint8_t ch, bool nrpn, uint8_t sel_msb, uint8_t sel_lsb,
                    uint8_t data_msb, uint8_t data_lsb) {
  e.push_back(cc(ch, nrpn ? 99 : 101, sel_msb));
  e.push_back(cc(ch, nrpn ? 98 : 100, sel_lsb));
  e.push_back(cc(ch, 6, data_msb));
  e.push_back(cc(ch, 38, data_lsb));
}

/// One channel's performance covering every message the translations touch.
Performance make_performance(uint8_t ch, uint8_t note, uint8_t bank_msb, uint8_t bank_lsb,
                             uint8_t program) {
  const uint8_t second = static_cast<uint8_t>(note + 7);
  Performance perf;

  Step s1{{}, kStepFrames};
  auto& e1 = s1.events;
  e1.push_back(cc(ch, 0, bank_msb));
  e1.push_back(cc(ch, 32, bank_lsb));
  e1.push_back(midi::make_midi1_program_change(0, ch, program));
  e1.push_back(cc(ch, 7, 100));
  e1.push_back(cc(ch, 10, 40));
  e1.push_back(cc(ch, 11, 110));
  e1.push_back(cc(ch, 1, 50));
  add_data_entry(e1, ch, false, 0, 0, 12, 0);   // RPN 0/0 bend range (zero-extended in 2.0)
  add_data_entry(e1, ch, false, 0, 1, 70, 33);  // RPN 0/1 fine tuning
  add_data_entry(e1, ch, false, 0, 40, 30, 5);  // RPN index >= 32, scaled min-center-max
  add_data_entry(e1, ch, true, 1, 8, 70, 0);    // NRPN 1/8
  e1.push_back(cc(ch, 101, 127));               // null function
  e1.push_back(cc(ch, 100, 127));
  e1.push_back(cc(ch, 96, 0));
  e1.push_back(cc(ch, 97, 0));
  e1.push_back(midi::make_midi1_note_on(0, ch, note, 100));
  e1.push_back(midi::make_midi1_note_on(0, ch, second, 80));
  perf.push_back(s1);

  Step s2{{}, kStepFrames};
  auto& e2 = s2.events;
  e2.push_back(cc(ch, 64, 127));
  e2.push_back(cc(ch, 66, 127));
  e2.push_back(cc(ch, 67, 127));
  e2.push_back(midi::make_midi1_pitch_bend(0, ch, 0x3000));
  e2.push_back(midi::make_midi1_channel_pressure(0, ch, 90));
  e2.push_back(midi::make_midi1_poly_pressure(0, ch, note, 70));
  perf.push_back(s2);

  Step s3{{}, kStepFrames};
  auto& e3 = s3.events;
  e3.push_back(cc(ch, 7, 20));
  e3.push_back(cc(ch, 10, 100));
  e3.push_back(cc(ch, 11, 60));
  e3.push_back(cc(ch, 1, 100));
  e3.push_back(midi::make_midi1_pitch_bend(0, ch, 0x0800));
  e3.push_back(midi::make_midi1_channel_pressure(0, ch, 20));
  e3.push_back(midi::make_midi1_poly_pressure(0, ch, note, 10));
  perf.push_back(s3);

  Step s4{{}, kStepFrames};
  auto& e4 = s4.events;
  add_data_entry(e4, ch, false, 0, 0, 2, 0);
  e4.push_back(midi::make_midi1_pitch_bend(0, ch, 0x3FFF));
  e4.push_back(cc(ch, 96, 0));
  e4.push_back(cc(ch, 97, 0));
  perf.push_back(s4);

  Step s5{{}, kStepFrames};
  auto& e5 = s5.events;
  e5.push_back(cc(ch, 64, 0));
  e5.push_back(cc(ch, 66, 0));
  e5.push_back(cc(ch, 67, 0));
  e5.push_back(midi::make_midi1_note_on(0, ch, second, 0));
  e5.push_back(midi::make_midi1_program_change(0, ch, static_cast<uint8_t>((program + 1) & 0x7F)));
  perf.push_back(s5);

  Step s6{{}, kStepFrames};
  s6.events.push_back(midi::make_midi1_note_on(0, ch, static_cast<uint8_t>(note + 12), 1));
  s6.events.push_back(midi::make_midi1_note_off(0, ch, note, 64));
  perf.push_back(s6);

  perf.push_back(
      {{midi::make_midi1_note_on(0, ch, static_cast<uint8_t>(note + 12), 0)}, kReleaseFrames});
  return perf;
}

// ---- The two MIDI 2.0 conversions -------------------------------------------

enum class Route { kMidi1, kDefaultTranslation, kCoreMidi };

/// CoreMIDI's measured conversion: min-center-max for every value including RPN /
/// NRPN data, a velocity-0 Note On left as MT 0x2, CC 0 / CC 32 delivered as
/// Control Changes and folded into the Program Change bank.
struct CoreMidiTranslator {
  struct Channel {
    uint8_t bank_msb = 0;
    uint8_t bank_lsb = 0;
    bool bank_valid = false;
    bool nrpn = false;
    uint8_t sel_msb = 0;
    uint8_t sel_lsb = 0;
    bool sel_msb_valid = false;
    bool sel_lsb_valid = false;
    uint8_t data_msb = 0;
    bool data_msb_valid = false;
  };
  Channel channels[16];

  std::vector<Ump> translate(const Ump& ump) {
    if (ump.message_type() != UmpMessageType::kMidi1ChannelVoice) return {ump};
    Channel& ch = channels[ump.channel()];
    const uint8_t channel = ump.channel();
    const uint8_t d1 = static_cast<uint8_t>((ump.words[0] >> 8u) & 0x7Fu);
    const uint8_t d2 = static_cast<uint8_t>(ump.words[0] & 0x7Fu);
    const auto status = static_cast<UmpStatus>(ump.status_nibble());
    if (status == UmpStatus::kNoteOn && d2 == 0) return {ump};
    if (status == UmpStatus::kProgramChange) {
      return {midi::make_midi2_program_change(ump.group, channel, d1, ch.bank_msb, ch.bank_lsb,
                                              ch.bank_valid)};
    }
    if (status != UmpStatus::kControlChange) return {midi::midi1_to_midi2(ump)};
    switch (d1) {
      case 0:
        ch.bank_msb = d2;
        ch.bank_valid = true;
        break;
      case 32:
        ch.bank_lsb = d2;
        ch.bank_valid = true;
        break;
      case 6:
        ch.data_msb = d2;
        ch.data_msb_valid = true;
        return {};
      case 38:
        if (ch.data_msb_valid && ch.sel_msb_valid && ch.sel_lsb_valid &&
            !(ch.sel_msb == 0x7F && ch.sel_lsb == 0x7F)) {
          const uint32_t v =
              midi::scale_cc_14_to_32(static_cast<uint16_t>((ch.data_msb << 7u) | d2));
          return {ch.nrpn ? midi::make_midi2_assignable_controller(ump.group, channel, ch.sel_msb,
                                                                   ch.sel_lsb, v)
                          : midi::make_midi2_registered_controller(ump.group, channel, ch.sel_msb,
                                                                   ch.sel_lsb, v)};
        }
        return {};
      case 98:
      case 99:
      case 100:
      case 101: {
        const bool nrpn = d1 <= 99;
        if (ch.nrpn != nrpn) {
          ch.nrpn = nrpn;
          ch.sel_msb_valid = ch.sel_lsb_valid = false;
        }
        (d1 == 99 || d1 == 101 ? ch.sel_msb : ch.sel_lsb) = d2;
        (d1 == 99 || d1 == 101 ? ch.sel_msb_valid : ch.sel_lsb_valid) = true;
        ch.data_msb_valid = false;
        return {};
      }
      default:
        break;
    }
    return {midi::midi1_to_midi2(ump)};
  }
};

std::vector<Ump> convert(const Performance& perf, Route route, size_t step,
                         Midi1ToMidi2Translator& a, CoreMidiTranslator& b) {
  std::vector<Ump> out;
  for (const Ump& u : perf[step].events) {
    if (route == Route::kMidi1) {
      out.push_back(u);
    } else if (route == Route::kDefaultTranslation) {
      const auto list = a.translate(u);
      for (uint8_t i = 0; i < list.count; ++i) out.push_back(list.messages[i]);
    } else {
      for (const Ump& m : b.translate(u)) out.push_back(m);
    }
  }
  return out;
}

/// Renders @p perf on a prepared synth, sending it in the shape of @p route.
/// @p mutate, when set, rewrites the converted MIDI 2.0 messages.
template <typename Synth>
Stereo render(Synth& synth, const Performance& perf, Route route, void (*mutate)(Ump&) = nullptr) {
  Midi1ToMidi2Translator a;
  CoreMidiTranslator b;
  Stereo out;
  for (size_t i = 0; i < perf.size(); ++i) {
    for (Ump u : convert(perf, route, i, a, b)) {
      if (mutate != nullptr) mutate(u);
      synth.on_event(0, event(u));
    }
    const auto r = render_stereo(synth, perf[i].frames);
    out.left.insert(out.left.end(), r.left.begin(), r.left.end());
    out.right.insert(out.right.end(), r.right.begin(), r.right.end());
  }
  return out;
}

/// Empty when identical; otherwise names the first differing sample and values.
std::string first_difference(const Stereo& expected, const Stereo& actual) {
  std::ostringstream os;
  if (expected.left.size() != actual.left.size()) {
    os << "length " << expected.left.size() << " vs " << actual.left.size();
    return os.str();
  }
  for (size_t i = 0; i < expected.left.size(); ++i) {
    if (expected.left[i] != actual.left[i]) {
      os << "left[" << i << "] " << expected.left[i] << " vs " << actual.left[i];
      return os.str();
    }
    if (expected.right[i] != actual.right[i]) {
      os << "right[" << i << "] " << expected.right[i] << " vs " << actual.right[i];
      return os.str();
    }
  }
  return {};
}

bool silent(const Stereo& s) {
  for (float v : s.left) {
    if (v != 0.0f) return false;
  }
  return true;
}

/// Renders @p perf on a fresh synth per route and requires the three renders to be
/// identical. Returns the number of routes compared against the MIDI 1.0 render.
template <typename Make>
int check_invariance(const std::string& label, const Performance& perf, Make make) {
  auto reference_synth = make();
  const Stereo reference = render(*reference_synth, perf, Route::kMidi1);
  INFO(label);
  CHECK_FALSE(silent(reference));
  int compared = 0;
  for (const Route route : {Route::kDefaultTranslation, Route::kCoreMidi}) {
    auto synth = make();
    const Stereo converted = render(*synth, perf, route);
    const std::string diff = first_difference(reference, converted);
    INFO((route == Route::kDefaultTranslation ? "default translation: " : "coremidi: ") << diff);
    CHECK(diff.empty());
    ++compared;
  }
  return compared;
}

// ---- Synth factories --------------------------------------------------------

std::unique_ptr<NativeSynth> make_native() {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 8;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  auto synth = std::make_unique<NativeSynth>(cfg);
  synth->prepare(kRate, kBlock);
  return synth;
}

std::shared_ptr<Sf2File> load_fixture() {
  static std::shared_ptr<Sf2File> cached;
  if (!cached) {
    std::ifstream in(kSf2Path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    auto sf2 = std::make_shared<Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    cached = sf2;
  }
  return cached;
}

Sf2PlayerConfig sf2_config() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.polyphony = 8;
  cfg.synth_fallback = true;
  cfg.prefer_model_for_modeled_families = false;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  return cfg;
}

/// @p soundfont false gives the data-free player, which plays the GM fallback bank.
std::unique_ptr<Sf2Player> make_sf2(bool soundfont, bool gs_reset) {
  auto synth = std::make_unique<Sf2Player>(sf2_config());
  if (soundfont) synth->set_soundfont(load_fixture());
  synth->prepare(kRate, kBlock);
  if (gs_reset) {
    const std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                   0x00, 0x7F, 0x00, 0x41, 0xF7};
    synth->handle_sysex(msg.data(), msg.size());
  }
  return synth;
}

std::unique_ptr<BuiltinSynth> make_builtin(SynthWaveform wave) {
  BuiltinSynthConfig cfg;
  cfg.waveform = wave;
  cfg.gain = 0.2f;
  cfg.attack_ms = 5.0f;
  cfg.decay_ms = 60.0f;
  cfg.sustain = 0.7f;
  cfg.release_ms = 120.0f;
  cfg.polyphony = 8;
  auto synth = std::make_unique<BuiltinSynth>(cfg);
  synth->prepare(kRate, kBlock);
  return synth;
}

// ---- Case sweeps -------------------------------------------------------------

/// Programs checked in the untagged cases: one per engine family plus the ends.
constexpr uint8_t kSampleNativePrograms[] = {0, 4, 11, 19, 24, 40, 56, 71, 73, 107, 127};
constexpr uint8_t kSampleFallbackPrograms[] = {0, 25, 48, 80, 110};

int sweep_native(const std::vector<uint8_t>& programs, const std::vector<uint8_t>& kits) {
  int compared = 0;
  for (const uint8_t p : programs) {
    compared += check_invariance("native program " + std::to_string(p),
                                 make_performance(kMelodicChannel, kNote, 0, 0, p), make_native);
  }
  for (const uint8_t kit : kits) {
    compared += check_invariance("native kit " + std::to_string(kit),
                                 make_performance(kDrumChannel, kDrumNote, 0, 0, kit), make_native);
  }
  return compared;
}

int sweep_sf2_presets() {
  int compared = 0;
  const auto file = load_fixture();
  REQUIRE_FALSE(file->presets().empty());
  for (const auto& preset : file->presets()) {
    const bool drums = preset.bank >= 128;
    const uint8_t ch = drums ? kDrumChannel : kMelodicChannel;
    const uint8_t note = drums ? kDrumNote : kNote;
    const uint8_t bank = drums ? 0 : static_cast<uint8_t>(preset.bank);
    compared +=
        check_invariance("sf2 preset bank " + std::to_string(preset.bank) + " program " +
                             std::to_string(preset.program),
                         make_performance(ch, note, bank, 0, static_cast<uint8_t>(preset.program)),
                         [] { return make_sf2(true, false); });
  }
  return compared;
}

int sweep_sf2_gs() {
  int compared = 0;
  for (const uint8_t bank : {0, 8, 16}) {
    compared += check_invariance("sf2 gs mode bank " + std::to_string(bank),
                                 make_performance(kMelodicChannel, kNote, bank, 0, 1),
                                 [] { return make_sf2(true, true); });
  }
  compared +=
      check_invariance("sf2 gs mode kit", make_performance(kDrumChannel, kDrumNote, 0, 0, 8),
                       [] { return make_sf2(true, true); });
  compared += check_invariance("sf2 gs mode fallback bank",
                               make_performance(kMelodicChannel, kNote, 8, 0, 24),
                               [] { return make_sf2(false, true); });
  return compared;
}

int sweep_sf2_fallback(const std::vector<uint8_t>& programs) {
  int compared = 0;
  for (const uint8_t p : programs) {
    compared += check_invariance("sf2 fallback program " + std::to_string(p),
                                 make_performance(kMelodicChannel, kNote, 0, 0, p),
                                 [] { return make_sf2(false, false); });
  }
  return compared;
}

int sweep_builtin() {
  int compared = 0;
  const std::pair<SynthWaveform, const char*> waves[] = {{SynthWaveform::kSine, "sine"},
                                                         {SynthWaveform::kSaw, "saw"},
                                                         {SynthWaveform::kSquare, "square"},
                                                         {SynthWaveform::kTriangle, "triangle"}};
  for (const auto& w : waves) {
    const SynthWaveform wave = w.first;
    compared += check_invariance(std::string("builtin ") + w.second,
                                 make_performance(kMelodicChannel, kNote, 0, 0, 5),
                                 [wave] { return make_builtin(wave); });
  }
  return compared;
}

std::vector<uint8_t> all_programs() {
  std::vector<uint8_t> v;
  for (int p = 0; p < 128; ++p) v.push_back(static_cast<uint8_t>(p));
  return v;
}

std::vector<uint8_t> all_kit_programs() {
  std::vector<uint8_t> v;
  for (const auto& kit : sonare::midi::synth::kGsDrumKits) v.push_back(kit.program);
  return v;
}

// ---- Positive control mutations ----------------------------------------------

bool is_midi2(const Ump& u) { return u.message_type() == UmpMessageType::kMidi2ChannelVoice; }

/// Adds one LSB to a MIDI 2.0 note-on velocity.
void bump_velocity(Ump& u) {
  if (is_midi2(u) && u.status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOn)) {
    u.words[1] += 1u << 16;
  }
}

/// Adds one LSB to a MIDI 2.0 CC 7 value.
void bump_volume(Ump& u) {
  if (is_midi2(u) && u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange) &&
      ((u.words[0] >> 8) & 0xFFu) == 7u) {
    u.words[1] += 1u;
  }
}

/// Adds one LSB to a MIDI 2.0 pitch bend.
void bump_bend(Ump& u) {
  if (is_midi2(u) && u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    u.words[1] += 1u;
  }
}

}  // namespace

TEST_CASE("midi2 invariance: NativeSynth programs and kits", "[midi][midi2]") {
  const int compared = sweep_native(
      {std::begin(kSampleNativePrograms), std::end(kSampleNativePrograms)}, {0, 8, 25, 127, 26});
  CHECK(compared == 2 * (11 + 5));
}

TEST_CASE("midi2 invariance: Sf2Player presets, fallback and GS mode", "[midi][midi2]") {
  const int presets = sweep_sf2_presets();
  CHECK(presets >= 2);
  CHECK(sweep_sf2_fallback(
            {std::begin(kSampleFallbackPrograms), std::end(kSampleFallbackPrograms)}) == 2 * 5);
  CHECK(sweep_sf2_gs() == 2 * 5);
}

TEST_CASE("midi2 invariance: BuiltinSynth waveforms", "[midi][midi2]") {
  CHECK(sweep_builtin() == 2 * 4);
}

TEST_CASE("midi2 invariance: a one-LSB MIDI 2.0 difference changes the output", "[midi][midi2]") {
  struct Mutation {
    const char* label;
    void (*fn)(Ump&);
  };
  const Mutation mutations[] = {
      {"velocity", &bump_velocity}, {"cc7", &bump_volume}, {"bend", &bump_bend}};
  const Performance perf = make_performance(kMelodicChannel, kNote, 0, 0, 0);
  auto reference_synth = make_native();
  const Stereo reference = render(*reference_synth, perf, Route::kDefaultTranslation);
  int changed = 0;
  for (const Mutation& m : mutations) {
    auto synth = make_native();
    const Stereo mutated = render(*synth, perf, Route::kDefaultTranslation, m.fn);
    const bool differs = !first_difference(reference, mutated).empty();
    INFO(m.label << " differs: " << differs);
    changed += differs ? 1 : 0;
  }
  CHECK(changed >= 1);
}

TEST_CASE("midi2 invariance: full NativeSynth and Sf2Player fallback sweep",
          "[.][slow][midi][midi2]") {
  CHECK(sweep_native(all_programs(), all_kit_programs()) == 2 * (128 + 26));
  CHECK(sweep_sf2_fallback(all_programs()) == 2 * 128);
}
