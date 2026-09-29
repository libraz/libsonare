/// @file midi1_control_golden_test.cpp
/// @brief Golden hashes for MIDI 1.0 channel-voice controller rendering on
///        NativeSynth, Sf2Player and BuiltinSynth.
///
/// Every gesture is sent as MIDI 1.0 UMP (MT 0x2) around a held note, so the
/// manifest states what the 7-bit velocity and controller path renders today and
/// a change that widens that path can be held to bit identity for MIDI 1.0 input.
/// The gesture list is one script per row: events before note-on, events at the
/// head of each held chunk (on_event ignores the frame, so an event reaches the
/// voice at the next process() call), events at note-off and after it. The `none`
/// row of each program runs the same chunks with no gesture. Each row gets a
/// fresh synth with a pinned config.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/controller_profile.h"
#include "midi/mpe.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::BuiltinSynth;
using sonare::midi::BuiltinSynthConfig;
using sonare::midi::ControllerAxis;
using sonare::midi::ControllerInput;
using sonare::midi::ControllerProfile;
using sonare::midi::SynthWaveform;
using sonare::midi::Ump;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::fnv1a_quantized_stereo;
using sonare::test::render_stereo;

namespace midi = sonare::midi;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kSteps = 8;
constexpr int kStepFrames = 1200;
constexpr int kReleaseChunks = 2;
constexpr int kReleaseFrames = 2400;
constexpr uint8_t kNote = 60;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kDrumNote = 38;
constexpr const char* kManifest = "tests/midi/golden/midi1_control_hashes.tsv";
constexpr const char* kSf2Path = "tests/fixtures/sf2/minimal_gs.sf2";

struct NoteSpec {
  uint8_t channel;
  uint8_t note;
  uint8_t velocity;
};

using Sysex = std::vector<uint8_t>;

/// One row's input. held[i] is sent before held chunk i; at_off before the
/// note-offs; release[j] before release chunk j.
struct Script {
  std::vector<NoteSpec> notes;
  std::vector<Ump> pre;
  std::vector<Sysex> pre_sysex;
  std::vector<Ump> held[kSteps];
  std::vector<Ump> at_off;
  std::vector<Ump> release[kReleaseChunks];
};

Ump cc(uint8_t ch, uint8_t num, uint8_t value) {
  return midi::make_midi1_control_change(0, ch, num, value);
}

Sysex dt1(uint8_t mid, uint8_t lo, uint8_t value) {
  Sysex msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo, value};
  const int sum = 0x40 + mid + lo + value;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Where the gesture puts values in the held chunks.
void sweep_cc(Script& s, uint8_t ch, uint8_t num) {
  s.pre.push_back(cc(ch, num, 32));
  s.held[2].push_back(cc(ch, num, 0));
  s.held[4].push_back(cc(ch, num, 64));
  s.held[6].push_back(cc(ch, num, 127));
}

void pedal(Script& s, uint8_t ch, uint8_t num) {
  s.held[1].push_back(cc(ch, num, 127));
  s.release[1].push_back(cc(ch, num, 0));
}

void bend_steps(Script& s, uint8_t ch) {
  s.held[1].push_back(midi::make_midi1_pitch_bend(0, ch, 0x0000));
  s.held[3].push_back(midi::make_midi1_pitch_bend(0, ch, 0x2000));
  s.held[5].push_back(midi::make_midi1_pitch_bend(0, ch, 0x3FFF));
  s.held[7].push_back(midi::make_midi1_pitch_bend(0, ch, 0x3000));
}

void pressure_steps(Script& s, uint8_t ch) {
  s.held[1].push_back(midi::make_midi1_channel_pressure(0, ch, 0));
  s.held[3].push_back(midi::make_midi1_channel_pressure(0, ch, 64));
  s.held[5].push_back(midi::make_midi1_channel_pressure(0, ch, 127));
}

struct GestureRow {
  const char* label;
  void (*build)(Script&, uint8_t ch, uint8_t note);
};

void g_none(Script&, uint8_t, uint8_t) {}
void g_cc7(Script& s, uint8_t ch, uint8_t) { sweep_cc(s, ch, 7); }
void g_cc10(Script& s, uint8_t ch, uint8_t) { sweep_cc(s, ch, 10); }
void g_cc11(Script& s, uint8_t ch, uint8_t) { sweep_cc(s, ch, 11); }
void g_cc64(Script& s, uint8_t ch, uint8_t) { pedal(s, ch, 64); }
void g_cc66(Script& s, uint8_t ch, uint8_t) { pedal(s, ch, 66); }
void g_cc67(Script& s, uint8_t ch, uint8_t) { pedal(s, ch, 67); }
void g_bend(Script& s, uint8_t ch, uint8_t) { bend_steps(s, ch); }
void g_pressure(Script& s, uint8_t ch, uint8_t) { pressure_steps(s, ch); }
void g_poly_pressure(Script& s, uint8_t ch, uint8_t note) {
  s.held[1].push_back(midi::make_midi1_poly_pressure(0, ch, note, 0));
  s.held[3].push_back(midi::make_midi1_poly_pressure(0, ch, note, 64));
  s.held[5].push_back(midi::make_midi1_poly_pressure(0, ch, note, 127));
}
void g_rpn_bend_range(Script& s, uint8_t ch, uint8_t) {
  s.pre.push_back(cc(ch, 101, 0));
  s.pre.push_back(cc(ch, 100, 0));
  s.pre.push_back(cc(ch, 6, 12));
  s.pre.push_back(cc(ch, 38, 0));
  s.held[1].push_back(midi::make_midi1_pitch_bend(0, ch, 0x3FFF));
  s.held[4].push_back(midi::make_midi1_pitch_bend(0, ch, 0x0000));
}
void g_vel1(Script& s, uint8_t, uint8_t) { s.notes[0].velocity = 1; }
void g_vel64(Script& s, uint8_t, uint8_t) { s.notes[0].velocity = 64; }
void g_vel100(Script& s, uint8_t, uint8_t) { s.notes[0].velocity = 100; }
void g_vel127(Script& s, uint8_t, uint8_t) { s.notes[0].velocity = 127; }

constexpr GestureRow kCommonGestures[] = {
    {"none", &g_none},
    {"vel001", &g_vel1},
    {"vel064", &g_vel64},
    {"vel100", &g_vel100},
    {"vel127", &g_vel127},
    {"cc07", &g_cc7},
    {"cc10", &g_cc10},
    {"cc11", &g_cc11},
    {"cc64", &g_cc64},
    {"cc66", &g_cc66},
    {"cc67", &g_cc67},
    {"bend", &g_bend},
    {"chan-pressure", &g_pressure},
    {"poly-pressure", &g_poly_pressure},
    {"rpn-bend-range", &g_rpn_bend_range},
};

/// Bank select and a program change to @p program ahead of the note. Builtin
/// takes no program, and the row shows whether it reads the bank controllers.
Script bank_program_script(uint8_t ch, uint8_t program) {
  Script s;
  s.notes = {{ch, kNote, 100}};
  s.pre.push_back(cc(ch, 0, 0));
  s.pre.push_back(cc(ch, 32, 0));
  s.pre.push_back(midi::make_midi1_program_change(0, ch, program));
  return s;
}

Script base_script(uint8_t ch, uint8_t note) {
  Script s;
  s.notes = {{ch, note, 100}};
  return s;
}

template <typename Synth>
void send_sysex(Synth&, const Sysex&) {}
void send_sysex(Sf2Player& synth, const Sysex& msg) { synth.handle_sysex(msg.data(), msg.size()); }

template <typename Synth>
void send_all(Synth& synth, const std::vector<Ump>& umps) {
  for (const Ump& u : umps) synth.on_event(0, event(u));
}

/// Renders @p s on a synth that is already prepared.
template <typename Synth>
std::string render_script(Synth& synth, const Script& s) {
  for (const Sysex& msg : s.pre_sysex) send_sysex(synth, msg);
  send_all(synth, s.pre);
  for (const NoteSpec& n : s.notes) {
    synth.on_event(0, event(midi::make_midi1_note_on(0, n.channel, n.note, n.velocity)));
  }
  std::vector<float> left;
  std::vector<float> right;
  auto append = [&](int frames) {
    const sonare::test::StereoRender r = render_stereo(synth, frames);
    left.insert(left.end(), r.left.begin(), r.left.end());
    right.insert(right.end(), r.right.begin(), r.right.end());
  };
  for (int i = 0; i < kSteps; ++i) {
    send_all(synth, s.held[i]);
    append(kStepFrames);
  }
  send_all(synth, s.at_off);
  for (const NoteSpec& n : s.notes) {
    synth.on_event(0, event(midi::make_midi1_note_off(0, n.channel, n.note, 0)));
  }
  for (int j = 0; j < kReleaseChunks; ++j) {
    send_all(synth, s.release[j]);
    append(kReleaseFrames);
  }
  return std::to_string(fnv1a_quantized_stereo(left, right));
}

using Rows = std::vector<std::pair<std::string, std::string>>;

// ---- NativeSynth -----------------------------------------------------------

NativeSynthConfig native_config() {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 8;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  return cfg;
}

struct NativeProgram {
  int program;
  SynthEngineMode engine;
  const char* label;
};

constexpr NativeProgram kNativePrograms[] = {
    {0, SynthEngineMode::kPiano, "piano"},
    {4, SynthEngineMode::kFm, "fm-epiano"},
    {11, SynthEngineMode::kModal, "vibraphone"},
    {19, SynthEngineMode::kPipeOrgan, "organ"},
    {24, SynthEngineMode::kKarplusStrong, "nylon-guitar"},
    {107, SynthEngineMode::kPluckedString, "koto"},
    {40, SynthEngineMode::kBowedString, "violin"},
    {56, SynthEngineMode::kBrass, "trumpet"},
    {71, SynthEngineMode::kReed, "clarinet"},
    {73, SynthEngineMode::kFlute, "flute"},
};

/// The bindings that make pressure, poly pressure, CC67, CC74 and velocity reach
/// an axis; without a profile the synth ignores all five.
ControllerProfile bound_profile() {
  ControllerProfile profile;
  REQUIRE(profile.bind({ControllerInput::kChannelPressure, 0, ControllerAxis::kLoudness, 0.3f, 1.0f}));
  REQUIRE(profile.bind({ControllerInput::kPolyPressure, 0, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  REQUIRE(profile.bind({ControllerInput::kControlChange, 67, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  REQUIRE(profile.bind({ControllerInput::kControlChange, 74, ControllerAxis::kBrightness, 0.0f, 1.0f}));
  REQUIRE(profile.bind({ControllerInput::kVelocity, 0, ControllerAxis::kExcitation, 0.0f, 1.0f}));
  return profile;
}

std::string native_row(const Script& s, int program, bool bound = false) {
  NativeSynth synth(native_config());
  if (bound) synth.set_controller_profile(bound_profile());
  synth.prepare(kRate, kBlock);
  for (const NoteSpec& n : s.notes) {
    synth.on_event(
        0, event(midi::make_midi1_program_change(0, n.channel, static_cast<uint8_t>(program))));
  }
  return render_script(synth, s);
}

std::string key(const std::string& synth, const std::string& program, const char* gesture) {
  return synth + "/" + program + "/" + gesture;
}

Rows native_rows() {
  Rows rows;
  for (const NativeProgram& p : kNativePrograms) {
    const std::string prog = std::string("p") + (p.program < 10 ? "00" : p.program < 100 ? "0" : "") +
                             std::to_string(p.program) + "-" + p.label;
    for (const GestureRow& g : kCommonGestures) {
      Script s = base_script(0, kNote);
      g.build(s, 0, kNote);
      rows.emplace_back(key("native", prog, g.label), native_row(s, p.program));
    }
    Script bp = bank_program_script(0, static_cast<uint8_t>((p.program + 40) % 128));
    rows.emplace_back(key("native", prog, "bank-program"), native_row(bp, p.program));
    for (const GestureRow& g : kCommonGestures) {
      const std::string label = g.label;
      if (label != "none" && label != "vel001" && label != "vel127" && label != "cc67" &&
          label != "chan-pressure" && label != "poly-pressure") {
        continue;
      }
      Script s = base_script(0, kNote);
      g.build(s, 0, kNote);
      rows.emplace_back(key("native", prog + "+profile", g.label), native_row(s, p.program, true));
    }
  }
  for (const GestureRow& g : kCommonGestures) {
    Script s = base_script(kDrumChannel, kDrumNote);
    g.build(s, kDrumChannel, kDrumNote);
    rows.emplace_back(key("native", "kit000", g.label), native_row(s, 0));
  }
  // MPE lower zone: manager channel 0 with 7 members, one note per member.
  for (const int program : {0, 40, 56}) {
    for (const char* label : {"none", "bend", "pressure", "timbre"}) {
      Script s;
      s.notes = {{1, 60, 100}, {2, 67, 100}};
      s.pre.push_back(cc(0, 101, 0));
      s.pre.push_back(cc(0, 100, 6));
      s.pre.push_back(cc(0, 6, 7));
      const std::string l = label;
      if (l == "bend") {
        s.held[1].push_back(midi::make_midi1_pitch_bend(0, 1, 0x3000));
        s.held[3].push_back(midi::make_midi1_pitch_bend(0, 2, 0x0800));
        s.held[5].push_back(midi::make_midi1_pitch_bend(0, 1, 0x1000));
        s.held[6].push_back(midi::make_midi1_pitch_bend(0, 0, 0x3000));
      } else if (l == "pressure") {
        s.held[1].push_back(midi::make_midi1_channel_pressure(0, 1, 100));
        s.held[3].push_back(midi::make_midi1_channel_pressure(0, 2, 30));
        s.held[5].push_back(midi::make_midi1_channel_pressure(0, 1, 10));
      } else if (l == "timbre") {
        s.held[1].push_back(cc(1, midi::kMpeTimbreCc, 100));
        s.held[3].push_back(cc(2, midi::kMpeTimbreCc, 20));
        s.held[5].push_back(cc(1, midi::kMpeTimbreCc, 0));
      }
      NativeSynth synth(native_config());
      synth.set_controller_profile(bound_profile());
      synth.prepare(kRate, kBlock);
      s.pre.insert(s.pre.begin(),
                   midi::make_midi1_program_change(0, 1, static_cast<uint8_t>(program)));
      s.pre.insert(s.pre.begin() + 1,
                   midi::make_midi1_program_change(0, 2, static_cast<uint8_t>(program)));
      rows.emplace_back(key("native", "mpe-p" + std::to_string(program), label),
                        render_script(synth, s));
    }
  }
  return rows;
}

// ---- Sf2Player -------------------------------------------------------------

std::shared_ptr<Sf2File> load_fixture() {
  std::ifstream in(kSf2Path, std::ios::binary);
  REQUIRE(in.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
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

std::string sf2_row(const Script& s, int program) {
  Sf2Player synth(sf2_config());
  synth.set_soundfont(load_fixture());
  synth.prepare(kRate, kBlock);
  for (const NoteSpec& n : s.notes) {
    if (n.channel == kDrumChannel) continue;
    synth.on_event(
        0, event(midi::make_midi1_program_change(0, n.channel, static_cast<uint8_t>(program))));
  }
  return render_script(synth, s);
}

Rows sf2_rows() {
  Rows rows;
  for (const int program : {0, 1}) {
    const std::string prog = "p00" + std::to_string(program);
    for (const GestureRow& g : kCommonGestures) {
      Script s = base_script(0, kNote);
      g.build(s, 0, kNote);
      rows.emplace_back(key("sf2", prog, g.label), sf2_row(s, program));
    }
    Script bp = bank_program_script(0, static_cast<uint8_t>(1 - program));
    rows.emplace_back(key("sf2", prog, "bank-program"), sf2_row(bp, program));
  }
  for (const GestureRow& g : kCommonGestures) {
    Script s = base_script(kDrumChannel, kDrumNote);
    g.build(s, kDrumChannel, kDrumNote);
    rows.emplace_back(key("sf2", "kit000", g.label), sf2_row(s, 0));
  }
  // GS velocity sense: depth 0x1A and offset 0x1B of part 1 (channel 0).
  constexpr uint8_t kPartBlock = 0x11;
  struct Sense {
    const char* label;
    uint8_t depth;
    uint8_t offset;
    uint8_t velocity;
  };
  const Sense senses[] = {
      {"gs-sense-default", 0x40, 0x40, 100}, {"gs-sense-depth-low", 0x20, 0x40, 100},
      {"gs-sense-depth-high", 0x7F, 0x40, 100}, {"gs-sense-depth-zero", 0x00, 0x40, 100},
      {"gs-sense-offset-up", 0x40, 0x60, 60},   {"gs-sense-offset-down", 0x40, 0x20, 60},
      {"gs-sense-both", 0x60, 0x50, 30},
  };
  for (const Sense& se : senses) {
    Script s = base_script(0, kNote);
    s.notes[0].velocity = se.velocity;
    s.pre_sysex.push_back(dt1(kPartBlock, 0x1A, se.depth));
    s.pre_sysex.push_back(dt1(kPartBlock, 0x1B, se.offset));
    rows.emplace_back(key("sf2", "p000", se.label), sf2_row(s, 0));
  }
  return rows;
}

// ---- BuiltinSynth ----------------------------------------------------------

BuiltinSynthConfig builtin_config(SynthWaveform wave) {
  BuiltinSynthConfig cfg;
  cfg.waveform = wave;
  cfg.gain = 0.2f;
  cfg.attack_ms = 5.0f;
  cfg.decay_ms = 60.0f;
  cfg.sustain = 0.7f;
  cfg.release_ms = 120.0f;
  cfg.polyphony = 8;
  return cfg;
}

Rows builtin_rows() {
  Rows rows;
  const std::pair<SynthWaveform, const char*> waves[] = {{SynthWaveform::kSine, "sine"},
                                                         {SynthWaveform::kSaw, "saw"}};
  for (const auto& w : waves) {
    auto run = [&](const Script& s) {
      BuiltinSynth synth(builtin_config(w.first));
      synth.prepare(kRate, kBlock);
      return render_script(synth, s);
    };
    for (const GestureRow& g : kCommonGestures) {
      Script s = base_script(0, kNote);
      g.build(s, 0, kNote);
      rows.emplace_back(key("builtin", w.second, g.label), run(s));
    }
    rows.emplace_back(key("builtin", w.second, "bank-program"), run(bank_program_script(0, 5)));
  }
  return rows;
}

// ---- Manifest --------------------------------------------------------------

const Rows& cached(const std::string& synth) {
  static std::map<std::string, Rows> cache;
  auto it = cache.find(synth);
  if (it == cache.end()) {
    Rows rows = synth == "native" ? native_rows() : synth == "sf2" ? sf2_rows() : builtin_rows();
    it = cache.emplace(synth, std::move(rows)).first;
  }
  return it->second;
}

std::map<std::string, std::string> load_manifest() {
  std::map<std::string, std::string> out;
  std::ifstream file(kManifest);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;
    out[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return out;
}

void write_manifest() {
  std::ofstream file(kManifest);
  file << "# synth/program/gesture\tfnv1a_quantized_interleaved_hash\n";
  for (const char* synth : {"native", "sf2", "builtin"}) {
    for (const auto& row : cached(synth)) file << row.first << '\t' << row.second << '\n';
  }
}

void check_synth(const std::string& synth) {
  INFO(sonare::test::kGoldenDigestProvenance);
  if (std::getenv("SONARE_UPDATE_MIDI1_CONTROL_GOLDEN") != nullptr) {
    static bool written = false;
    if (!written) write_manifest();
    written = true;
  }
  const auto expected = load_manifest();
  const Rows& rows = cached(synth);
  for (const auto& row : rows) {
    const auto it = expected.find(row.first);
    INFO(row.first);
    CHECK(it != expected.end());
    if (it != expected.end()) CHECK(row.second == it->second);
  }
  size_t listed = 0;
  for (const auto& e : expected) {
    if (e.first.rfind(synth + "/", 0) == 0) ++listed;
  }
  CHECK(listed == rows.size());
}

}  // namespace

TEST_CASE("midi1 control: NativeSynth engines stay bit-identical", "[.][midi][golden]") {
  for (const NativeProgram& p : kNativePrograms) {
    INFO(p.label);
    CHECK(gm_fallback_patch(0, static_cast<uint8_t>(p.program)).mode == p.engine);
  }
  check_synth("native");
}

TEST_CASE("midi1 control: Sf2Player stays bit-identical", "[.][midi][golden]") {
  check_synth("sf2");
}

TEST_CASE("midi1 control: BuiltinSynth stays bit-identical", "[.][midi][golden]") {
  check_synth("builtin");
}
