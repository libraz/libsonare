/// @file sf2_file_test.cpp
/// @brief SF2 parser (midi/synth/sf2_file): preset/instrument/zone/sample
///        enumeration from a synthetic in-memory SoundFont, generator and
///        loop-point decoding, bank/program lookup, and malformed/truncated
///        input safety (parse fails cleanly, never crashes).

#include "midi/synth/sf2_file.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "support/sf2_builder.h"

namespace {

using Catch::Approx;
using sonare::midi::synth::Sf2File;
using sonare::test::Sf2Builder;

// A small two-sample, two-preset fixture: a looped "sine" melodic preset with
// two velocity layers and a one-shot "noise" drum preset on bank 128.
std::vector<uint8_t> build_fixture() {
  Sf2Builder b;

  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 32.0);
  }
  const int sine_id = b.add_sample("sine", sine, 32000, 60, 32, 96);

  std::vector<float> noise(64);
  for (size_t i = 0; i < noise.size(); ++i) noise[i] = ((i * 37) % 64) / 32.0f - 1.0f;
  const int noise_id = b.add_sample("noise", noise, 44100, 60, 0, 64);

  Sf2Builder::ZoneSpec soft;
  soft.vel_lo = 0;
  soft.vel_hi = 63;
  soft.gens.push_back({54 /*sampleModes*/, 1 /*continuous loop*/});
  soft.gens.push_back({48 /*initialAttenuation*/, 100});
  soft.target = sine_id;

  Sf2Builder::ZoneSpec loud;
  loud.vel_lo = 64;
  loud.vel_hi = 127;
  loud.gens.push_back({54, 1});
  loud.target = sine_id;

  const int melodic = b.add_instrument("melodic", {soft, loud});

  Sf2Builder::ZoneSpec drum_zone;
  drum_zone.key_lo = 35;
  drum_zone.key_hi = 45;
  drum_zone.target = noise_id;
  const int drums = b.add_instrument("drums", {drum_zone});

  Sf2Builder::ZoneSpec preset_zone;
  preset_zone.target = melodic;
  b.add_preset("Melodic", 0, 0, {preset_zone});

  Sf2Builder::ZoneSpec drum_preset_zone;
  drum_preset_zone.target = drums;
  b.add_preset("Drums", 128, 0, {drum_preset_zone});

  return b.build();
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value & 0xFF);
  bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

bool fourcc_at(const std::vector<uint8_t>& bytes, size_t offset, const char* id) {
  return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, id, 4) == 0;
}

std::vector<uint8_t> build_fixture_with_empty_pdta_chunk(const char* target_id) {
  std::vector<uint8_t> bytes = build_fixture();
  for (size_t chunk = 12; chunk + 12 <= bytes.size();) {
    const uint32_t chunk_size = read_u32(bytes, chunk + 4);
    const size_t next = chunk + 8 + chunk_size + (chunk_size & 1u);
    if (fourcc_at(bytes, chunk, "LIST") && fourcc_at(bytes, chunk + 8, "pdta")) {
      for (size_t sub = chunk + 12; sub + 8 <= chunk + 8 + chunk_size;) {
        const uint32_t sub_size = read_u32(bytes, sub + 4);
        const size_t sub_body = sub + 8;
        const size_t sub_next = sub_body + sub_size + (sub_size & 1u);
        if (fourcc_at(bytes, sub, target_id)) {
          const size_t erase_count = sub_next - sub_body;
          write_u32(bytes, sub + 4, 0);
          bytes.erase(bytes.begin() + static_cast<ptrdiff_t>(sub_body),
                      bytes.begin() + static_cast<ptrdiff_t>(sub_next));
          write_u32(bytes, chunk + 4, chunk_size - static_cast<uint32_t>(erase_count));
          write_u32(bytes, 4, read_u32(bytes, 4) - static_cast<uint32_t>(erase_count));
          return bytes;
        }
        sub = sub_next;
      }
    }
    chunk = next;
  }
  return bytes;
}

}  // namespace

TEST_CASE("Sf2File parses presets, instruments, zones and samples", "[midi][sf2]") {
  const std::vector<uint8_t> bytes = build_fixture();
  Sf2File sf2;
  std::string error;
  REQUIRE(sf2.parse(bytes.data(), bytes.size(), &error));
  INFO(error);

  REQUIRE(sf2.presets().size() == 2);
  REQUIRE(sf2.instruments().size() == 2);
  REQUIRE(sf2.samples().size() == 2);

  // Preset lookup by (bank, program).
  REQUIRE(sf2.find_preset(0, 0) == 0);
  REQUIRE(sf2.find_preset(128, 0) == 1);
  REQUIRE(sf2.find_preset(0, 1) == -1);
  REQUIRE(sf2.presets()[0].name == "Melodic");
  REQUIRE(sf2.presets()[1].bank == 128);

  // Preset zone -> instrument link.
  REQUIRE(sf2.presets()[0].zones.size() == 1);
  REQUIRE(sf2.presets()[0].zones[0].instrument == 0);

  // Instrument velocity layers decode their ranges and generators.
  const auto& melodic = sf2.instruments()[0];
  REQUIRE(melodic.name == "melodic");
  REQUIRE(melodic.zones.size() == 2);
  REQUIRE(melodic.zones[0].vel_lo == 0);
  REQUIRE(melodic.zones[0].vel_hi == 63);
  REQUIRE(melodic.zones[1].vel_lo == 64);
  REQUIRE(melodic.zones[1].vel_hi == 127);
  REQUIRE(melodic.zones[0].sample == 0);
  const auto* atten = melodic.zones[0].find_gen(sonare::midi::synth::kGenInitialAttenuation);
  REQUIRE(atten != nullptr);
  REQUIRE(atten->amount == 100);
  const auto* modes = melodic.zones[0].find_gen(sonare::midi::synth::kGenSampleModes);
  REQUIRE(modes != nullptr);
  REQUIRE(modes->amount == 1);

  // Drum instrument key range.
  const auto& drums = sf2.instruments()[1];
  REQUIRE(drums.zones.size() == 1);
  REQUIRE(drums.zones[0].key_lo == 35);
  REQUIRE(drums.zones[0].key_hi == 45);
  REQUIRE(drums.zones[0].matches(40, 100));
  REQUIRE_FALSE(drums.zones[0].matches(60, 100));

  // Sample headers index the pool; loop points sit inside the sample.
  const auto& sine = sf2.samples()[0];
  REQUIRE(sine.name == "sine");
  REQUIRE(sine.sample_rate == 32000);
  REQUIRE(sine.end - sine.start == 96);
  REQUIRE(sine.loop_start == sine.start + 32);
  REQUIRE(sine.loop_end == sine.start + 96);

  // PCM round-trips through the 16-bit conversion into the float pool.
  const auto& pool = sf2.sample_pool();
  REQUIRE(pool.size() >= sine.end);
  REQUIRE(pool[sine.start] == Approx(0.0f).margin(1e-4));
  REQUIRE(pool[sine.start + 8] == Approx(1.0f).margin(1e-3));  // sin(pi/2)
}

TEST_CASE("Sf2File velocity/key zone matching helper", "[midi][sf2]") {
  const std::vector<uint8_t> bytes = build_fixture();
  Sf2File sf2;
  REQUIRE(sf2.parse(bytes.data(), bytes.size(), nullptr));
  const auto& melodic = sf2.instruments()[0];
  REQUIRE(melodic.zones[0].matches(60, 30));
  REQUIRE_FALSE(melodic.zones[0].matches(60, 100));
  REQUIRE(melodic.zones[1].matches(60, 100));
}

TEST_CASE("Sf2File rejects malformed input without crashing", "[midi][sf2]") {
  const std::vector<uint8_t> bytes = build_fixture();
  Sf2File sf2;
  std::string error;

  SECTION("empty / tiny input") {
    REQUIRE_FALSE(sf2.parse(nullptr, 0, &error));
    REQUIRE_FALSE(error.empty());
    const uint8_t tiny[4] = {'R', 'I', 'F', 'F'};
    REQUIRE_FALSE(sf2.parse(tiny, sizeof(tiny), &error));
  }

  SECTION("wrong magic") {
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    REQUIRE_FALSE(sf2.parse(bad.data(), bad.size(), &error));
    bad = bytes;
    bad[8] = 'x';  // form type
    REQUIRE_FALSE(sf2.parse(bad.data(), bad.size(), &error));
  }

  SECTION("every truncation point parses without crash") {
    // Truncation may legitimately succeed once all required chunks are
    // present; the guarantee is "no crash, and failure reports an error".
    for (size_t len = 0; len < bytes.size(); len += 7) {
      Sf2File partial;
      std::string err;
      const bool parsed = partial.parse(bytes.data(), len, &err);
      if (!parsed) REQUIRE_FALSE(err.empty());
    }
  }

  SECTION("corrupted bag indices fail cleanly") {
    // Flip bytes throughout the pdta region; parse must never crash and the
    // object must stay consistent (empty on failure).
    for (size_t flip = bytes.size() / 2; flip < bytes.size(); flip += 11) {
      std::vector<uint8_t> bad = bytes;
      bad[flip] ^= 0xFF;
      Sf2File f;
      std::string err;
      if (!f.parse(bad.data(), bad.size(), &err)) {
        REQUIRE(f.presets().empty());
      }
    }
  }

  SECTION("empty bag tables fail cleanly") {
    {
      std::vector<uint8_t> bad = build_fixture_with_empty_pdta_chunk("ibag");
      Sf2File f;
      std::string err;
      REQUIRE_FALSE(f.parse(bad.data(), bad.size(), &err));
      REQUIRE(err == "sf2: missing ibag");
      REQUIRE(f.presets().empty());
      REQUIRE(f.instruments().empty());
    }
    {
      std::vector<uint8_t> bad = build_fixture_with_empty_pdta_chunk("pbag");
      Sf2File f;
      std::string err;
      REQUIRE_FALSE(f.parse(bad.data(), bad.size(), &err));
      REQUIRE(err == "sf2: missing pbag");
      REQUIRE(f.presets().empty());
      REQUIRE(f.instruments().empty());
    }
  }
}

TEST_CASE("Sf2File global zones supply defaults without becoming layers", "[midi][sf2]") {
  Sf2Builder b;
  std::vector<float> pcm(32, 0.5f);
  const int sid = b.add_sample("s", pcm, 22050, 60, 0, 32);

  // Global zone (no terminal gen) carrying a filter cutoff, then a local zone.
  Sf2Builder::ZoneSpec global_zone;
  global_zone.gens.push_back({8 /*initialFilterFc*/, 9500});
  global_zone.target = -1;
  Sf2Builder::ZoneSpec local;
  local.target = sid;
  const int inst = b.add_instrument("withglobal", {global_zone, local});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("P", 0, 5, {pz});

  const auto bytes = b.build();
  Sf2File sf2;
  std::string error;
  REQUIRE(sf2.parse(bytes.data(), bytes.size(), &error));
  REQUIRE(sf2.instruments().size() == 1);
  const auto& zones = sf2.instruments()[0].zones;
  REQUIRE(zones.size() == 2);
  REQUIRE(zones[0].is_global());
  REQUIRE(zones[0].find_gen(sonare::midi::synth::kGenInitialFilterFc) != nullptr);
  REQUIRE_FALSE(zones[1].is_global());
  REQUIRE(sf2.find_preset(0, 5) == 0);
}
