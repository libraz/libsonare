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

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
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

std::string render_program(int program) {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 4;
  cfg.bus_drive = 0.0f;
  cfg.dc_block = true;

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

}  // namespace

TEST_CASE("NativeSynth GM program renders stay bit-identical", "[.][midi][synth][golden]") {
  const std::filesystem::path manifest = "tests/midi/golden/gm_program_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_SYNTH_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == kProgramCount);
  REQUIRE(expected.size() == rows.size());

  for (const auto& [program, hash] : rows) {
    CAPTURE(program);
    REQUIRE(expected.at(program) == hash);
  }
}
