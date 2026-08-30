/// @file golden_hash_test.cpp
/// @brief Built-in mastering presets hashed per preset and signal.
///
/// The four presets enabling denoise or dereverb run through the STFT, so their
/// hashes are the ones an FFT rounding change moves. PFFFT renders differently
/// depending on the optimization level it was compiled at, which would let a
/// Debug and a Release build disagree here with neither being wrong; the pin in
/// src/CMakeLists.txt is what keeps them together, and removing it fails these
/// twelve alone.

#include "support/golden_hash.h"

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

#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "util/constants.h"

namespace api = sonare::mastering::api;

namespace {

constexpr int kSampleRate = 24000;
using sonare::constants::kPi;

std::vector<float> make_signal(const std::string& name) {
  constexpr float seconds = 0.75f;
  std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate), 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    if (name == "tone") {
      samples[i] =
          0.35f * std::sin(2.0f * kPi * 220.0f * t) + 0.18f * std::sin(2.0f * kPi * 880.0f * t);
    } else if (name == "transient") {
      samples[i] = 0.18f * std::sin(2.0f * kPi * 110.0f * t);
      const int period = kSampleRate / 4;
      const int local = static_cast<int>(i) % period;
      if (local < 96) {
        samples[i] += 0.75f * (1.0f - static_cast<float>(local) / 96.0f);
      }
    } else {
      uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u);
      x ^= x >> 13;
      x *= 1274126177u;
      const float noise = static_cast<float>(static_cast<int>(x & 0xffffu) - 32768) / 32768.0f;
      samples[i] = 0.15f * noise + 0.18f * std::sin(2.0f * kPi * 330.0f * t);
    }
  }
  return samples;
}

std::string hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

// A mastering chain assembled explicitly through the named-processor entry
// points, run in the fixed compressor -> transformer -> limiter order,
// covering the transformer's saturation stage that no built-in preset enables.
std::vector<float> run_explicit_transformer_chain(const std::vector<float>& samples) {
  const auto compressed = api::apply_named_processor(
      "dynamics.compressor", samples.data(), samples.size(), kSampleRate,
      {{"thresholdDb", -18.0}, {"ratio", 4.0}, {"attackMs", 5.0}, {"releaseMs", 80.0}});
  const auto driven = api::apply_named_processor(
      "saturation.transformer", compressed.samples.data(), compressed.samples.size(), kSampleRate,
      {{"driveDb", 10.0}, {"asymmetry", 0.3}, {"mix", 1.0}});
  const auto limited =
      api::apply_named_processor("maximizer.truePeakLimiter", driven.samples.data(),
                                 driven.samples.size(), kSampleRate, {{"ceilingDb", -0.3}});
  return limited.samples;
}

std::map<std::string, std::string> load_manifest(const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::string> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string preset;
    std::string signal;
    std::string hash;
    std::getline(stream, preset, '\t');
    std::getline(stream, signal, '\t');
    std::getline(stream, hash, '\t');
    out[preset + "/" + signal] = hash;
  }
  return out;
}

std::vector<std::tuple<std::string, std::string, std::string>> compute_rows() {
  const std::vector<std::string> signals = {"tone", "transient", "dense"};
  std::vector<std::tuple<std::string, std::string, std::string>> rows;
  for (const auto& preset_name : api::preset_names()) {
    for (const auto& signal : signals) {
      const auto samples = make_signal(signal);
      const auto result = api::master_audio_mono(api::preset_from_string(preset_name),
                                                 samples.data(), samples.size(), kSampleRate);
      rows.emplace_back(preset_name, signal, hex64(sonare::test::fnv1a_quantized(result.samples)));
    }
  }
  for (const auto& signal : signals) {
    const auto samples = make_signal(signal);
    const auto chained = run_explicit_transformer_chain(samples);
    rows.emplace_back("explicit-transformer-chain", signal,
                      hex64(sonare::test::fnv1a_quantized(chained)));
  }
  return rows;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# preset\tsignal\tfnv1a_quantized_hash\n";
  for (const auto& [preset, signal, hash] : compute_rows()) {
    file << preset << '\t' << signal << '\t' << hash << '\n';
  }
}

}  // namespace

TEST_CASE("built-in mastering preset golden hashes stay stable", "[.][mastering][preset][golden]") {
  const std::filesystem::path manifest = "tests/mastering/golden/preset_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_MASTERING_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == 78);
  REQUIRE(expected.size() == rows.size());

  // CHECK, not REQUIRE: a REQUIRE aborts the case on the first drifted row, so
  // a chain-wide change reads as one stale preset and the other rows are never
  // compared. Reporting every drifted row is what tells a stale golden (one
  // whole stage's presets move together) apart from a real regression.
  for (const auto& [preset, signal, hash] : rows) {
    const std::string key = preset + "/" + signal;
    CAPTURE(key);
    CHECK(expected.at(key) == hash);
  }
}
