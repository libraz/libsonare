/// @file room_mono_regression_test.cpp
/// @brief Release-recorded digests of the mono room-reverb outputs.
///
/// Pins the mono renders of room_morph(), RoomReverb, ConvolutionReverb with a
/// unit-energy IR, and synthesize_rir() so a change to the shared room engine
/// can show that its mono behaviour did not move. Each row digests the samples
/// and carries the lengths and diagnostics in the row's meta column.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "core/audio.h"
#include "effects/acoustic/room_morph.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/room_reverb.h"
#include "support/golden_hash.h"

namespace {

using sonare::Audio;
using sonare::acoustic::SourceListener;
using sonare::acoustic::synthesize_rir;
using sonare::acoustic::uniform_shoebox;
using sonare::effects::acoustic::room_morph;
using sonare::effects::acoustic::RoomMorphConfig;
using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;
using sonare::effects::reverb::RoomReverb;
using sonare::effects::reverb::RoomReverbConfig;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 256;
constexpr float kAbsorption = 0.15f;
constexpr float kRirSeconds = 0.5f;

struct Row {
  std::string key;
  std::string hash;
  std::string meta;
};

std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

/// Seeded noise burst of `burst` samples followed by silence up to `total`.
std::vector<float> burst_then_silence(std::size_t burst, std::size_t total, std::uint32_t seed) {
  std::vector<float> samples(total, 0.0f);
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < burst; ++i) {
    state = state * 1664525u + 1013904223u;
    std::uint32_t x = state;
    x ^= x >> 13;
    x *= 1274126177u;
    const float noise = static_cast<float>(static_cast<int>(x & 0xffffu) - 32768) / 32768.0f;
    const float fade = 1.0f - static_cast<float>(i) / static_cast<float>(burst);
    samples[i] = 0.4f * noise * fade;
  }
  return samples;
}

std::string diagnostic_codes(const std::vector<sonare::Diagnostic>& diagnostics) {
  std::string out;
  for (const auto& d : diagnostics) {
    if (!out.empty()) out += ',';
    out += d.code;
  }
  return out.empty() ? "-" : out;
}

SourceListener placement() { return {{2.0f, 3.0f, 1.2f}, {5.0f, 4.0f, 1.7f}}; }

sonare::acoustic::ShoeboxRoom room() {
  return uniform_shoebox(sonare::RoomDimensions{8.0f, 6.0f, 3.5f}, kAbsorption);
}

Row row_room_morph() {
  const Audio input =
      Audio::from_vector(burst_then_silence(kSampleRate / 4, kSampleRate / 2, 11u), kSampleRate);
  RoomMorphConfig config;
  config.target = room();
  config.placement = placement();
  config.wet = 0.5f;
  config.source_tail_suppression = 0.5f;
  config.seed = 1u;
  config.max_seconds = kRirSeconds;
  const auto result = room_morph(input, config);
  const std::vector<float> samples(result.audio.data(), result.audio.data() + result.audio.size());
  return {
      "room_morph/mono", hex64(sonare::test::fnv1a_quantized(samples)),
      "len=" + std::to_string(samples.size()) + " diag=" + diagnostic_codes(result.diagnostics)};
}

/// Runs a processor 1- or 2-channel in whole blocks over burst + trailing zeros.
template <typename Processor>
std::vector<std::vector<float>> render(Processor& processor, int channels, std::size_t total) {
  std::vector<std::vector<float>> buffers;
  for (int c = 0; c < channels; ++c) {
    buffers.push_back(burst_then_silence(kSampleRate / 8, total, 21u + static_cast<unsigned>(c)));
  }
  for (std::size_t offset = 0; offset + kBlock <= total; offset += kBlock) {
    std::vector<float*> ptrs;
    for (auto& b : buffers) ptrs.push_back(b.data() + offset);
    processor.process(ptrs.data(), channels, kBlock);
  }
  return buffers;
}

std::uint64_t digest(const std::vector<std::vector<float>>& buffers) {
  if (buffers.size() == 1) return sonare::test::fnv1a_quantized(buffers[0]);
  return sonare::test::fnv1a_quantized_stereo(buffers[0], buffers[1]);
}

Row row_room_reverb() {
  RoomReverbConfig config;
  config.dims = {8.0f, 6.0f, 3.5f};
  config.source = {2.0f, 3.0f, 1.2f};
  config.listener = {5.0f, 4.0f, 1.7f};
  config.absorption = kAbsorption;
  config.max_seconds = kRirSeconds;
  config.dry_wet = 0.5f;
  RoomReverb reverb(config);
  reverb.prepare(kSampleRate, kBlock);
  const std::size_t aligned = static_cast<std::size_t>(kSampleRate) / kBlock * kBlock;
  const auto out = render(reverb, 1, aligned);
  return {"room_reverb/mono", hex64(digest(out)),
          "len=" + std::to_string(aligned) + " tail=" + std::to_string(reverb.tail_samples()) +
              " latency=" + std::to_string(reverb.latency_samples())};
}

Row row_convolution(int channels) {
  const auto rir = synthesize_rir(room(), placement(), kSampleRate, [] {
    sonare::acoustic::RirSynthConfig c;
    c.max_seconds = kRirSeconds;
    return c;
  }());
  ConvolutionReverbConfig config;
  config.dry_wet = 0.5f;
  ConvolutionReverb reverb(config);
  reverb.load_ir_unit_energy(rir.rir.data(), static_cast<int>(rir.rir.size()));
  reverb.prepare(kSampleRate, kBlock);
  const std::size_t aligned = static_cast<std::size_t>(kSampleRate) / kBlock * kBlock;
  const auto out = render(reverb, channels, aligned);
  return {"convolution_unit_energy/" + std::to_string(channels) + "ch", hex64(digest(out)),
          "len=" + std::to_string(aligned) + " ir=" + std::to_string(reverb.ir_size()) +
              " tail=" + std::to_string(reverb.tail_samples()) +
              " latency=" + std::to_string(reverb.latency_samples())};
}

Row row_synthesize_rir() {
  sonare::acoustic::RirSynthConfig config;
  config.max_seconds = kRirSeconds;
  const auto result = synthesize_rir(room(), placement(), kSampleRate, config);
  const std::vector<float> samples(result.rir.data(), result.rir.data() + result.rir.size());
  return {
      "synthesize_rir/mono", hex64(sonare::test::fnv1a_quantized(samples)),
      "len=" + std::to_string(samples.size()) + " diag=" + diagnostic_codes(result.diagnostics)};
}

std::vector<Row> compute_rows() {
  return {row_room_morph(), row_room_reverb(), row_convolution(1), row_convolution(2),
          row_synthesize_rir()};
}

void write_manifest(const std::filesystem::path& path, const std::vector<Row>& rows) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# key\tfnv1a_quantized_hash\tmeta\n";
  for (const auto& r : rows) file << r.key << '\t' << r.hash << '\t' << r.meta << '\n';
}

std::map<std::string, std::pair<std::string, std::string>> load_manifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::pair<std::string, std::string>> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string key, hash, meta;
    std::getline(stream, key, '\t');
    std::getline(stream, hash, '\t');
    std::getline(stream, meta, '\t');
    out[key] = {hash, meta};
  }
  return out;
}

}  // namespace

TEST_CASE("mono room outputs match the recorded digests", "[.][golden][room_mono_regression]") {
  INFO(sonare::test::kGoldenDigestProvenance);
  const std::filesystem::path manifest = "tests/effects/golden/room_mono_regression.tsv";
  const auto rows = compute_rows();
  if (std::getenv("SONARE_UPDATE_ROOM_MONO_GOLDEN") != nullptr) {
    write_manifest(manifest, rows);
  }
  const auto expected = load_manifest(manifest);
  REQUIRE(expected.size() == rows.size());
  for (const auto& r : rows) {
    CAPTURE(r.key);
    const auto it = expected.find(r.key);
    REQUIRE(it != expected.end());
    CHECK(it->second.first == r.hash);
    CHECK(it->second.second == r.meta);
  }
}

TEST_CASE("mono room outputs are deterministic across runs", "[.][golden][room_mono_regression]") {
  INFO(sonare::test::kGoldenDigestProvenance);
  const auto first = compute_rows();
  const auto second = compute_rows();
  REQUIRE(first.size() == second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CAPTURE(first[i].key);
    CHECK(first[i].hash == second[i].hash);
    CHECK(first[i].meta == second[i].meta);
  }
}
