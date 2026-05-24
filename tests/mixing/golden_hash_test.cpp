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

#include "mixing/channel_strip.h"

namespace mixing = sonare::mixing;

namespace {

constexpr int kSampleRate = 24000;
constexpr int kSamples = 4096;
constexpr float kPi = 3.14159265358979323846f;

struct Scenario {
  std::string name;
  float fader_db = 0.0f;
  float pan = 0.0f;
  mixing::PanMode pan_mode = mixing::PanMode::Balance;
  float width = 1.0f;
};

std::pair<std::vector<float>, std::vector<float>> make_signal(const std::string& name) {
  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  for (int i = 0; i < kSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    if (name == "tone") {
      left[static_cast<size_t>(i)] = 0.4f * std::sin(2.0f * kPi * 220.0f * t);
      right[static_cast<size_t>(i)] = 0.2f * std::sin(2.0f * kPi * 330.0f * t);
    } else if (name == "transient") {
      const int local = i % 512;
      const float hit = local < 64 ? 1.0f - static_cast<float>(local) / 64.0f : 0.0f;
      left[static_cast<size_t>(i)] = 0.6f * hit + 0.1f * std::sin(2.0f * kPi * 110.0f * t);
      right[static_cast<size_t>(i)] = 0.4f * hit;
    } else {
      uint32_t x = static_cast<uint32_t>(i * 1664525u + 1013904223u);
      x ^= x >> 13;
      x *= 1274126177u;
      const float noise = static_cast<float>(static_cast<int>(x & 0xffffu) - 32768) / 32768.0f;
      left[static_cast<size_t>(i)] = 0.12f * noise + 0.18f * std::sin(2.0f * kPi * 440.0f * t);
      right[static_cast<size_t>(i)] = -0.08f * noise + 0.16f * std::sin(2.0f * kPi * 660.0f * t);
    }
  }
  return {left, right};
}

uint64_t hash_stereo(const std::vector<float>& left, const std::vector<float>& right) {
  uint64_t hash = 1469598103934665603ull;
  auto add_sample = [&](float sample) {
    const int32_t q =
        static_cast<int32_t>(std::lrint(std::clamp(sample, -2.0f, 2.0f) * 1000000.0f));
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= static_cast<uint8_t>((static_cast<uint32_t>(q) >> (byte * 8)) & 0xffu);
      hash *= 1099511628211ull;
    }
  };
  for (size_t i = 0; i < left.size(); ++i) {
    add_sample(left[i]);
    add_sample(right[i]);
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
      {"unity-balance", 0.0f, 0.0f, mixing::PanMode::Balance, 1.0f},
      {"panned-fader", -4.5f, 0.35f, mixing::PanMode::Balance, 1.0f},
      {"stereo-pan-wide", -2.0f, -0.4f, mixing::PanMode::StereoPan, 1.4f},
  };
}

std::vector<std::tuple<std::string, std::string, std::string>> compute_rows() {
  const std::array<std::string, 3> signals{"tone", "transient", "dense"};
  std::vector<std::tuple<std::string, std::string, std::string>> rows;
  for (const auto& scenario : scenarios()) {
    for (const auto& signal_name : signals) {
      auto [left, right] = make_signal(signal_name);
      mixing::ChannelStrip strip;
      strip.prepare(kSampleRate, kSamples);
      strip.set_fader_db(scenario.fader_db);
      strip.set_pan_mode(scenario.pan_mode);
      strip.set_pan(scenario.pan);
      strip.set_width(scenario.width);
      float* channels[] = {left.data(), right.data()};
      strip.process(channels, 2, kSamples);
      rows.emplace_back(scenario.name, signal_name, hex64(hash_stereo(left, right)));
    }
  }
  return rows;
}

std::map<std::string, std::string> load_manifest(const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::string> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string scenario;
    std::string signal;
    std::string hash;
    std::getline(stream, scenario, '\t');
    std::getline(stream, signal, '\t');
    std::getline(stream, hash, '\t');
    out[scenario + "/" + signal] = hash;
  }
  return out;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# scenario\tsignal\tfnv1a_quantized_stereo_hash\n";
  for (const auto& [scenario, signal, hash] : compute_rows()) {
    file << scenario << '\t' << signal << '\t' << hash << '\n';
  }
}

}  // namespace

TEST_CASE("built-in mixing strip golden hashes stay stable", "[.][mixing][golden]") {
  const std::filesystem::path manifest = "tests/mixing/golden/strip_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_MIXING_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == 9);
  REQUIRE(expected.size() == rows.size());

  for (const auto& [scenario, signal, hash] : rows) {
    const std::string key = scenario + "/" + signal;
    CAPTURE(key);
    REQUIRE(expected.at(key) == hash);
  }
}
