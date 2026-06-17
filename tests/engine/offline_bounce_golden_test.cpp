#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "util/constants.h"

using sonare::constants::kPi;

namespace {

constexpr int kSourceSampleRate = 48000;
constexpr int kTargetSampleRate = 24000;
constexpr int kFrames = 48000;
constexpr int kBlock = 128;

struct Scenario {
  std::string name;
  int normalize_lufs = 0;
  float target_lufs = -18.0f;
  int dither = 0;
  int dither_bits = 16;
  uint32_t dither_seed = 0;
};

std::pair<std::vector<float>, std::vector<float>> make_clip(const std::string& name) {
  std::vector<float> left(kFrames, 0.0f);
  std::vector<float> right(kFrames, 0.0f);
  for (int i = 0; i < kFrames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSourceSampleRate);
    if (name == "tone") {
      left[static_cast<size_t>(i)] = 0.25f * std::sin(2.0f * sonare::constants::kPi * 220.0f * t);
      right[static_cast<size_t>(i)] = 0.18f * std::sin(2.0f * sonare::constants::kPi * 330.0f * t);
    } else {
      const int local = i % 6000;
      const float transient = local < 128 ? 1.0f - static_cast<float>(local) / 128.0f : 0.0f;
      left[static_cast<size_t>(i)] =
          0.12f * std::sin(2.0f * sonare::constants::kPi * 110.0f * t) + 0.32f * transient;
      right[static_cast<size_t>(i)] =
          -0.10f * std::sin(2.0f * sonare::constants::kPi * 165.0f * t) - 0.22f * transient;
    }
  }
  return {left, right};
}

uint64_t hash_samples(const float* samples, size_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < count; ++i) {
    const int32_t q =
        static_cast<int32_t>(std::lrint(std::clamp(samples[i], -2.0f, 2.0f) * 1000000.0f));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= static_cast<uint8_t>((static_cast<uint32_t>(q) >> (byte * 8)) & 0xffu);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

std::string hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

std::vector<Scenario> scenarios() {
  return {
      {"src-only", 0, -18.0f, 0, 16, 0},
      {"src-lufs-tpdf", 1, -18.0f, 2, 16, 0x12345678u},
      {"src-lufs-noiseshaped", 1, -20.0f, 3, 16, 0x87654321u},
  };
}

std::tuple<std::string, int64_t, int, int, float> run_bounce(const std::string& signal,
                                                             const Scenario& scenario) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSourceSampleRate, kBlock, 256, 256) == SONARE_OK);

  auto [left, right] = make_clip(signal);
  const float* clip_channels[] = {left.data(), right.data()};
  SonareEngineClip clip{};
  clip.id = 1;
  clip.channels = clip_channels;
  clip.num_channels = 2;
  clip.num_samples = kFrames;
  clip.start_ppq = 0.0;
  clip.length_samples = kFrames;
  clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  REQUIRE(sonare_engine_seek_sample(engine, 0, -1) == SONARE_OK);

  SonareEngineBounceOptions options{};
  options.total_frames = kFrames;
  options.block_size = kBlock;
  options.num_channels = 2;
  options.source_sample_rate = kSourceSampleRate;
  options.target_sample_rate = kTargetSampleRate;
  options.normalize_lufs = scenario.normalize_lufs;
  options.target_lufs = scenario.target_lufs;
  options.dither = scenario.dither;
  options.dither_bits = scenario.dither_bits;
  options.dither_seed = scenario.dither_seed;
  SonareEngineBounceResult result{};
  REQUIRE(sonare_engine_bounce_offline(engine, &options, &result) == SONARE_OK);
  REQUIRE(result.interleaved != nullptr);
  REQUIRE(result.frames == kTargetSampleRate);
  REQUIRE(result.num_channels == 2);
  REQUIRE(result.sample_rate == kTargetSampleRate);
  REQUIRE(result.sample_count == static_cast<size_t>(kTargetSampleRate * 2));
  if (scenario.normalize_lufs) {
    REQUIRE(std::abs(result.integrated_lufs - scenario.target_lufs) < 0.35f);
  } else {
    REQUIRE(std::isfinite(result.integrated_lufs));
  }
  const std::string hash = hex64(hash_samples(result.interleaved, result.sample_count));
  const int64_t frames = result.frames;
  const int channels = result.num_channels;
  const int sample_rate = result.sample_rate;
  const float integrated_lufs = result.integrated_lufs;
  sonare_free_floats(result.interleaved);
  sonare_engine_destroy(engine);
  return {hash, frames, channels, sample_rate, integrated_lufs};
}

std::vector<std::tuple<std::string, std::string, std::string, int64_t, int, int>> compute_rows() {
  const std::array<std::string, 2> signals{"tone", "transient"};
  std::vector<std::tuple<std::string, std::string, std::string, int64_t, int, int>> rows;
  for (const auto& scenario : scenarios()) {
    for (const auto& signal : signals) {
      auto [hash, frames, channels, sample_rate, _lufs] = run_bounce(signal, scenario);
      rows.emplace_back(scenario.name, signal, hash, frames, channels, sample_rate);
    }
  }
  return rows;
}

std::map<std::string, std::tuple<std::string, int64_t, int, int>> load_manifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::tuple<std::string, int64_t, int, int>> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string scenario;
    std::string signal;
    std::string hash;
    std::string frames;
    std::string channels;
    std::string sample_rate;
    std::getline(stream, scenario, '\t');
    std::getline(stream, signal, '\t');
    std::getline(stream, hash, '\t');
    std::getline(stream, frames, '\t');
    std::getline(stream, channels, '\t');
    std::getline(stream, sample_rate, '\t');
    out[scenario + "/" + signal] =
        std::make_tuple(hash, std::stoll(frames), std::stoi(channels), std::stoi(sample_rate));
  }
  return out;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# scenario\tsignal\tfnv1a_quantized_interleaved_hash\tframes\tchannels\tsample_rate\n";
  for (const auto& [scenario, signal, hash, frames, channels, sample_rate] : compute_rows()) {
    file << scenario << '\t' << signal << '\t' << hash << '\t' << frames << '\t' << channels << '\t'
         << sample_rate << '\n';
  }
}

}  // namespace

TEST_CASE("realtime engine offline bounce golden hashes stay stable",
          "[.][engine][offline][bounce][golden]") {
  const std::filesystem::path manifest = "tests/engine/golden/offline_bounce_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_ENGINE_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == 6);
  REQUIRE(expected.size() == rows.size());

  for (const auto& [scenario, signal, hash, frames, channels, sample_rate] : rows) {
    const std::string key = scenario + "/" + signal;
    CAPTURE(key);
    const auto [expected_hash, expected_frames, expected_channels, expected_sample_rate] =
        expected.at(key);
    REQUIRE(expected_hash == hash);
    REQUIRE(expected_frames == frames);
    REQUIRE(expected_channels == channels);
    REQUIRE(expected_sample_rate == sample_rate);
  }
}
