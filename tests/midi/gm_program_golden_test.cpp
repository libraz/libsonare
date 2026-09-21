/// @file gm_program_golden_test.cpp
/// @brief Golden hashes for every GM melodic program rendered through
///        NativeSynth's fallback bank.
///
/// This is the only check in the tree that can state a NativeSynth render is
/// unchanged. voice-gate compares a rendered voice against recorded bounds, so
/// it catches a voice that moved audibly but cannot separate "identical" from
/// "moved within the bounds" -- which is what a refactor claiming bit-identity
/// has to demonstrate.
///
/// Each program gets its own NativeSynth. Sweeping one instance would need
/// CC120 to silence the pool between programs, and CC120 is a control change --
/// a baseline for "renders with no controller input" cannot be built out of
/// controller input. A fresh instance also ties the per-voice seeded constants
/// to the program rather than to how many programs preceded it.
///
/// The config is pinned field by field rather than left to the defaults, so a
/// default that moves shows up as an edit here instead of as 128 unexplained
/// rows.
///
/// Four programs share a hash with another: the rig is a per-part stage the
/// SoundFont path applies (see docs/voicing.md), and NativeSynth alone renders
/// the instrument at the pickup, where an overdriven and a clean electric
/// guitar are the same instrument. What distinguishes them is not in scope for
/// this file.
///
/// The second case is the first baseline's complement: the controller paths the
/// first one holds out of its inputs. Its unit is the engine rather than the
/// program, because what a gesture moves is engine code and a channel layer
/// above it, and the per-program identity of an unplayed note is what the rows
/// above already pin. Where a patch field gates a branch inside an engine the
/// unit is the branch instead, since one program covers only the side it sits
/// on; kPathSelectors names those fields and an engine outside it claims its
/// programs differ only in values the same code reads. The fresh-instance rule
/// carries over -- one instance per (row, gesture) -- so a row carries the
/// controller input its name says and nothing another row sent.
///
/// Held frames are rendered in equal chunks there, because on_event ignores the
/// frame it is given and an event reaches the voice at the head of the next
/// process() call. The `none` row runs the same chunks and is required to equal
/// its program's row above, which is what keeps the two manifests on one
/// harness and what makes the other rows' difference the gesture rather than
/// the subdivision.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "midi/articulation_mode.h"
#include "midi/controller_profile.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::ArticulationMode;
using sonare::midi::ControllerAxis;
using sonare::midi::ControllerBinding;
using sonare::midi::ControllerInput;
using sonare::midi::ControllerProfile;
using sonare::midi::synth::engine_axis_capability;
using sonare::midi::synth::EngineAxisCapability;
using sonare::midi::synth::gm_fallback_patch;
using sonare::midi::synth::kAxisBrightness;
using sonare::midi::synth::kAxisForce;
using sonare::midi::synth::kSynthEngineModeMax;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;
using sonare::test::event;
using sonare::test::fnv1a_quantized_stereo;
using sonare::test::render_stereo;

constexpr double kGoldenRate = 48000.0;
constexpr int kGoldenBlock = 256;
constexpr int kGoldenNote = 60;
constexpr int kGoldenVelocity = 100;

/// Frames rendered while the key is held, then again after note-off. The tail
/// covers the slowest fallback release, so a program whose ring-down changes is
/// caught rather than trimmed away.
constexpr int kHeldFrames = 12000;
constexpr int kReleaseFrames = 12000;

constexpr int kProgramCount = 128;

NativeSynthConfig golden_config() {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;
  return cfg;
}

std::string render_program(int program) {
  const NativeSynthConfig cfg = golden_config();

  NativeSynth synth(cfg);
  synth.prepare(kGoldenRate, kGoldenBlock);
  synth.on_event(
      0, event(sonare::midi::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, kGoldenNote, kGoldenVelocity)));

  sonare::test::StereoRender held = render_stereo(synth, kHeldFrames);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, kGoldenNote, 0)));
  const sonare::test::StereoRender tail = render_stereo(synth, kReleaseFrames);

  held.left.insert(held.left.end(), tail.left.begin(), tail.left.end());
  held.right.insert(held.right.end(), tail.right.begin(), tail.right.end());
  return std::to_string(fnv1a_quantized_stereo(held.left, held.right));
}

std::vector<std::pair<int, std::string>> compute_rows() {
  std::vector<std::pair<int, std::string>> rows;
  rows.reserve(kProgramCount);
  for (int program = 0; program < kProgramCount; ++program) {
    rows.emplace_back(program, render_program(program));
  }
  return rows;
}

/// Chunks the held frames are rendered in, so a controller message lands inside
/// the note instead of only at its start.
constexpr int kGestureSteps = 8;
static_assert(kHeldFrames % kGestureSteps == 0, "a ramp step must be a whole number of frames");

/// The note the legato row slurs to, and the bend row's two depths in cents.
constexpr uint8_t kSlurNote = 67;
constexpr double kBendOnsetCents = 50.0;
constexpr double kBendHeldCents = 200.0;
/// The bend row's RPN range, off the two-semitone power-on value so a broken
/// RPN parse moves the row rather than leaving it on a coincidence.
constexpr uint8_t kBendRangeSemitones = 12;

enum class Gesture {
  kNone,
  kBend,
  kModWheel,
  kBreath,
  kBrightness,
  kPressure,
  kPolyPressure,
  kLegato,
};

struct GestureRow {
  Gesture gesture;
  const char* label;
};

constexpr GestureRow kGestures[] = {
    {Gesture::kNone, "none"},
    {Gesture::kBend, "bend"},
    {Gesture::kModWheel, "mod-wheel"},
    {Gesture::kBreath, "breath"},
    {Gesture::kBrightness, "brightness"},
    {Gesture::kPressure, "pressure"},
    {Gesture::kPolyPressure, "poly-pressure"},
    {Gesture::kLegato, "legato"},
};

/// One name per engine. No `default:` label: a mode added without a name here
/// is a compile error rather than a row that never appears.
const char* engine_label(SynthEngineMode mode) noexcept {
  switch (mode) {
    case SynthEngineMode::kSubtractive:
      return "subtractive";
    case SynthEngineMode::kFm:
      return "fm";
    case SynthEngineMode::kKarplusStrong:
      return "karplus-strong";
    case SynthEngineMode::kModal:
      return "modal";
    case SynthEngineMode::kAdditive:
      return "additive";
    case SynthEngineMode::kPercussion:
      return "percussion";
    case SynthEngineMode::kPiano:
      return "piano";
    case SynthEngineMode::kPipeOrgan:
      return "pipe-organ";
    case SynthEngineMode::kBowedString:
      return "bowed-string";
    case SynthEngineMode::kReed:
      return "reed";
    case SynthEngineMode::kBrass:
      return "brass";
    case SynthEngineMode::kFlute:
      return "flute";
    case SynthEngineMode::kPluckedString:
      return "plucked-string";
    case SynthEngineMode::kVocal:
      return "vocal";
    case SynthEngineMode::kFreeReed:
      return "free-reed";
    case SynthEngineMode::kHarpsichord:
      return "harpsichord";
    case SynthEngineMode::kSample:
      return "sample";
  }
  return "";
}

bool reed_is_beating(const NativeSynthPatch& patch) noexcept {
  return patch.reed.closing_pressure > 0.0f;
}

/// A patch field that gates a code path inside an engine. One representative per
/// engine covers whichever side its lowest program happens to sit on, so an
/// engine listed here takes two -- `reed.closing_pressure` chooses the Bernoulli
/// valve over the linearised table, and the lowest reed program is on the table
/// side, so a change to the valve would have moved no gesture row at all. An
/// engine absent from here claims its programs differ only in values the same
/// code reads; a gate added without a row goes unmeasured under every gesture.
struct PathSelector {
  SynthEngineMode mode;
  /// Suffixed onto the engine's own label to name the row.
  const char* label;
  bool (*selects)(const NativeSynthPatch&) noexcept;
};

const PathSelector kPathSelectors[] = {
    {SynthEngineMode::kReed, "beating", &reed_is_beating},
};

struct EngineRow {
  SynthEngineMode mode;
  int program;
  std::string label;
};

/// The lowest program on each side of every gate, read from the fallback map
/// rather than written down, so a re-voicing moves the `program` column instead
/// of silently leaving a row on an engine -- or a branch -- it no longer reaches.
std::vector<EngineRow> engine_representatives() {
  std::vector<EngineRow> out;
  for (int ordinal = 0; ordinal <= kSynthEngineModeMax; ++ordinal) {
    const auto mode = static_cast<SynthEngineMode>(ordinal);
    const PathSelector* selector = nullptr;
    for (const PathSelector& candidate : kPathSelectors) {
      if (candidate.mode == mode) selector = &candidate;
    }
    int base = -1;
    int gated = -1;
    for (int program = 0; program < kProgramCount; ++program) {
      const NativeSynthPatch& patch = gm_fallback_patch(0, static_cast<uint8_t>(program));
      if (patch.mode != mode) continue;
      const bool gates = selector != nullptr && selector->selects(patch);
      if (gates) {
        if (gated < 0) gated = program;
      } else if (base < 0) {
        base = program;
      }
      if (base >= 0 && (selector == nullptr || gated >= 0)) break;
    }
    if (base >= 0) out.push_back({mode, base, engine_label(mode)});
    if (gated >= 0) {
      out.push_back({mode, gated, std::string(engine_label(mode)) + '-' + selector->label});
    }
  }
  return out;
}

/// Whether the engine reads the axis the gesture carries, from the library's own
/// accept set. A gesture an engine declines renders the bare note, so its hash
/// equals the `none` row and the reach check below has nothing to assert.
bool gesture_reaches(SynthEngineMode mode, Gesture gesture) noexcept {
  const EngineAxisCapability cap = engine_axis_capability(mode);
  switch (gesture) {
    case Gesture::kNone:
      return false;
    // Pitch and voice allocation are the channel layer's, above every engine.
    case Gesture::kBend:
    case Gesture::kModWheel:
    case Gesture::kLegato:
      return true;
    case Gesture::kBreath:
    case Gesture::kPressure:
    case Gesture::kPolyPressure:
      return cap.continuous && (cap.mask & kAxisForce) != 0u;
    case Gesture::kBrightness:
      return cap.continuous && (cap.mask & kAxisBrightness) != 0u;
  }
  return false;
}

uint16_t bend_wheel(double cents) {
  const double span = kBendRangeSemitones * 100.0;
  int wheel = 8192 + static_cast<int>(std::lround(8192.0 * cents / span));
  if (wheel < 0) wheel = 0;
  if (wheel > 16383) wheel = 16383;
  return static_cast<uint16_t>(wheel);
}

uint8_t ramp_value(int step) { return static_cast<uint8_t>(step * 127 / (kGestureSteps - 1)); }

void send_step(NativeSynth& synth, Gesture gesture, int step) {
  namespace m = sonare::midi;
  const int midpoint = kGestureSteps / 2;
  switch (gesture) {
    case Gesture::kNone:
      break;
    case Gesture::kBend:
      if (step == midpoint) {
        synth.on_event(0, event(m::make_midi1_pitch_bend(0, 0, bend_wheel(kBendHeldCents))));
      }
      break;
    case Gesture::kModWheel:
      synth.on_event(0, event(m::make_midi1_control_change(0, 0, 1, ramp_value(step))));
      break;
    case Gesture::kBreath:
      synth.on_event(0, event(m::make_midi1_control_change(0, 0, 2, ramp_value(step))));
      break;
    case Gesture::kBrightness:
      synth.on_event(0, event(m::make_midi1_control_change(0, 0, 74, ramp_value(step))));
      break;
    case Gesture::kPressure:
      synth.on_event(0, event(m::make_midi1_channel_pressure(0, 0, ramp_value(step))));
      break;
    case Gesture::kPolyPressure:
      synth.on_event(0, event(m::make_midi1_poly_pressure(0, 0, kGoldenNote, ramp_value(step))));
      break;
    case Gesture::kLegato:
      if (step == midpoint) {
        synth.on_event(0, event(m::make_midi1_note_on(0, 0, kSlurNote, kGoldenVelocity)));
      }
      break;
  }
}

std::string render_gesture(int program, Gesture gesture) {
  namespace m = sonare::midi;
  const NativeSynthConfig cfg = golden_config();

  NativeSynth synth(cfg);
  synth.prepare(kGoldenRate, kGoldenBlock);
  // Both pressure gestures are routed deliberately, because an axis an engine
  // accepts is not by itself a path a message travels. Unbound, poly pressure
  // reaches only the matrix's aftertouch source, which no fallback patch routes
  // to an exciter, so its rows landed on `none` on every engine that reads
  // force -- the reach check below found that, and binding it is the answer.
  if (gesture == Gesture::kPressure) {
    ControllerProfile profile;
    REQUIRE(ControllerProfile::preset("breath-aftertouch", &profile));
    REQUIRE(synth.set_controller_profile(profile));
  }
  if (gesture == Gesture::kPolyPressure) {
    ControllerProfile profile;
    ControllerBinding binding;
    binding.input = ControllerInput::kPolyPressure;
    binding.axis = ControllerAxis::kExcitation;
    REQUIRE(profile.bind(binding));
    REQUIRE(synth.set_controller_profile(profile));
  }
  if (gesture == Gesture::kLegato) {
    REQUIRE(synth.set_articulation(0, ArticulationMode::kMonoLegato));
  }
  synth.on_event(0, event(m::make_midi1_program_change(0, 0, static_cast<uint8_t>(program))));
  if (gesture == Gesture::kBend) {
    synth.on_event(0, event(m::make_midi1_control_change(0, 0, 101, 0)));
    synth.on_event(0, event(m::make_midi1_control_change(0, 0, 100, 0)));
    synth.on_event(0, event(m::make_midi1_control_change(0, 0, 6, kBendRangeSemitones)));
    // Bent before the note starts, so the row covers a voice that never sounded
    // unbent as well as the per-sample retune the mid-note move drives.
    synth.on_event(0, event(m::make_midi1_pitch_bend(0, 0, bend_wheel(kBendOnsetCents))));
  }
  synth.on_event(0, event(m::make_midi1_note_on(0, 0, kGoldenNote, kGoldenVelocity)));

  sonare::test::StereoRender out;
  for (int step = 0; step < kGestureSteps; ++step) {
    send_step(synth, gesture, step);
    const sonare::test::StereoRender chunk = render_stereo(synth, kHeldFrames / kGestureSteps);
    out.left.insert(out.left.end(), chunk.left.begin(), chunk.left.end());
    out.right.insert(out.right.end(), chunk.right.begin(), chunk.right.end());
  }
  synth.on_event(0, event(m::make_midi1_note_off(0, 0, kGoldenNote, 0)));
  if (gesture == Gesture::kLegato) {
    synth.on_event(0, event(m::make_midi1_note_off(0, 0, kSlurNote, 0)));
  }
  const sonare::test::StereoRender tail = render_stereo(synth, kReleaseFrames);
  out.left.insert(out.left.end(), tail.left.begin(), tail.left.end());
  out.right.insert(out.right.end(), tail.right.begin(), tail.right.end());
  return std::to_string(fnv1a_quantized_stereo(out.left, out.right));
}

struct GestureResult {
  std::string key;
  int program;
  std::string hash;
};

std::vector<GestureResult> compute_gesture_rows() {
  std::vector<GestureResult> rows;
  for (const EngineRow& engine : engine_representatives()) {
    for (const GestureRow& row : kGestures) {
      rows.push_back({engine.label + '/' + row.label, engine.program,
                      render_gesture(engine.program, row.gesture)});
    }
  }
  return rows;
}

std::map<int, std::string> load_manifest(const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<int, std::string> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string program;
    std::string hash;
    std::getline(stream, program, '\t');
    std::getline(stream, hash, '\t');
    out[std::stoi(program)] = hash;
  }
  return out;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# gm_program\tfnv1a_quantized_interleaved_hash\n";
  for (const auto& [program, hash] : compute_rows()) {
    file << program << '\t' << hash << '\n';
  }
}

std::map<std::string, std::string> load_gesture_manifest(const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::string> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string key;
    std::string program;
    std::string hash;
    std::getline(stream, key, '\t');
    std::getline(stream, program, '\t');
    std::getline(stream, hash, '\t');
    out[key] = hash;
  }
  return out;
}

void write_gesture_manifest(const std::filesystem::path& path,
                            const std::vector<GestureResult>& rows) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# engine/gesture\tprogram\tfnv1a_quantized_interleaved_hash\n";
  for (const GestureResult& row : rows) {
    file << row.key << '\t' << row.program << '\t' << row.hash << '\n';
  }
}

}  // namespace

TEST_CASE("NativeSynth GM program renders stay bit-identical", "[.][midi][synth][golden]") {
  INFO(sonare::test::kGoldenDigestProvenance);
  const std::filesystem::path manifest = "tests/midi/golden/gm_program_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_SYNTH_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == kProgramCount);
  REQUIRE(expected.size() == rows.size());

  // Checked rather than required, as in the mixing golden family: how far a
  // drift spread -- one program or all of them -- is what separates a stale
  // manifest from a regression, and a required row hides that at the first one.
  for (const auto& [program, hash] : rows) {
    CAPTURE(program);
    CHECK(expected.at(program) == hash);
  }
}

TEST_CASE("NativeSynth engine renders under a controller gesture stay bit-identical",
          "[.][midi][synth][golden]") {
  INFO(sonare::test::kGoldenDigestProvenance);
  const std::filesystem::path manifest = "tests/midi/golden/gm_gesture_hashes.tsv";
  const std::vector<GestureResult> rows = compute_gesture_rows();
  if (std::getenv("SONARE_UPDATE_SYNTH_GOLDEN") != nullptr) {
    write_gesture_manifest(manifest, rows);
  }

  const auto expected = load_gesture_manifest(manifest);
  const auto engines = engine_representatives();
  // Un-reach is a failure rather than a clean run: a walk that stopped finding
  // engines reports exactly what a passing tree reports. Every mode but the
  // sampler answers a program and every gated engine answers on both sides of
  // its gate, so the count is stated from the selector table rather than
  // written down, and the one absence is named rather than subtracted -- an
  // engine dropping out of the bank moves the count, a gate whose two sides
  // collapsed onto one program moves it too, and a program reaching the sampler
  // trips the loop even where the count would still agree.
  INFO("engines walked: " << engines.size());
  REQUIRE(engines.size() == static_cast<size_t>(kSynthEngineModeMax) + std::size(kPathSelectors));
  for (const EngineRow& engine : engines) {
    CAPTURE(engine.label, engine.program);
    REQUIRE(engine.mode != SynthEngineMode::kSample);
  }
  REQUIRE(rows.size() == engines.size() * std::size(kGestures));
  REQUIRE(expected.size() == rows.size());

  for (const GestureResult& row : rows) {
    CAPTURE(row.key, row.program);
    CHECK(expected.at(row.key) == row.hash);
  }

  const std::string none_suffix = "/none";
  auto is_none = [&none_suffix](const std::string& key) {
    return key.size() >= none_suffix.size() &&
           key.compare(key.size() - none_suffix.size(), none_suffix.size(), none_suffix) == 0;
  };

  // The bridge to the manifest above: the same program through the same config,
  // differing only in the subdivision the other rows need. A mismatch says the
  // two manifests have drifted onto different harnesses, or that where a block
  // boundary falls is audible.
  const auto by_program = load_manifest("tests/midi/golden/gm_program_hashes.tsv");
  std::map<std::string, std::string> none_by_engine;
  for (const GestureResult& row : rows) {
    if (!is_none(row.key)) continue;
    CAPTURE(row.key, row.program);
    CHECK(by_program.at(row.program) == row.hash);
    none_by_engine[row.key.substr(0, row.key.size() - none_suffix.size())] = row.hash;
  }
  REQUIRE(none_by_engine.size() == engines.size());

  // Reach, and the standing proof that these rows can still go red: a gesture an
  // engine declines renders the bare note and lands on that engine's `none`
  // hash, so where the library's own accept set says the axis is read, the two
  // must differ. A message that stops being delivered collapses onto `none`.
  size_t reached = 0;
  for (const EngineRow& engine : engines) {
    for (const GestureRow& gesture : kGestures) {
      if (!gesture_reaches(engine.mode, gesture.gesture)) continue;
      const std::string key = engine.label + '/' + gesture.label;
      const auto found = std::find_if(rows.begin(), rows.end(),
                                      [&key](const GestureResult& r) { return r.key == key; });
      REQUIRE(found != rows.end());
      CAPTURE(key);
      CHECK(found->hash != none_by_engine.at(engine.label));
      ++reached;
    }
  }
  INFO("gestures asserted reachable: " << reached);
  REQUIRE(reached > 0);
}
