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

uint64_t hash_samples(const std::vector<float>& samples) {
  uint64_t hash = 1469598103934665603ull;
  for (float sample : samples) {
    const int32_t q =
        static_cast<int32_t>(std::lrint(std::clamp(sample, -2.0f, 2.0f) * 1000000.0f));
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
      rows.emplace_back(preset_name, signal, hex64(hash_samples(result.samples)));
    }
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
  REQUIRE(rows.size() == 75);
  REQUIRE(expected.size() == rows.size());

  for (const auto& [preset, signal, hash] : rows) {
    const std::string key = preset + "/" + signal;
    CAPTURE(key);
    REQUIRE(expected.at(key) == hash);
  }
}
