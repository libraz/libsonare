/// @file assistant_golden_test.cpp
/// @brief Golden hashes over the scenes the mixing assistant suggests.
///
/// @details What is hashed is the suggestion, not audio. The assistant's output
///          is a scene, so a golden over the serialised scene is what pins its
///          decisions; hashing a render would instead be re-testing the mixer.
///
/// @details Excluded from the default run. The suggestion depends on measured
///          loudness and spectral figures, which move with the analysis code
///          underneath it, so these rows go stale for reasons that have nothing
///          to do with a mistake. Before blaming an edit, confirm the row was
///          not already drifted. Regenerate with
///          `SONARE_UPDATE_MIXING_ASSISTANT_GOLDEN=1`, and land the regenerated
///          manifest in the same change as the behaviour that justifies it.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "mix_eval.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/suggester.h"
#include "support/golden_hash.h"

namespace {

using sonare::mixing::assistant::MixAssistantConfig;

struct Scenario {
  std::string name;
  MixAssistantConfig config;
};

std::vector<Scenario> scenarios() {
  std::vector<Scenario> out;

  out.push_back({"default", MixAssistantConfig{}});

  MixAssistantConfig half;
  half.suggestion_strength = 0.5f;
  out.push_back({"half-strength", half});

  MixAssistantConfig no_eq;
  no_eq.enable_eq = false;
  out.push_back({"no-eq", no_eq});

  MixAssistantConfig no_image;
  no_image.enable_image = false;
  out.push_back({"no-image", no_image});

  MixAssistantConfig quiet_target;
  quiet_target.target_track_lufs = -23.0f;
  out.push_back({"broadcast-target", quiet_target});

  return out;
}

std::string hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << value;
  return out.str();
}

std::uint64_t hash_text(const std::string& text) {
  std::uint64_t hash = sonare::test::kFnvOffsetBasis;
  for (unsigned char byte : text) {
    hash ^= byte;
    hash *= sonare::test::kFnvPrime;
  }
  return hash;
}

std::vector<std::tuple<std::string, std::string, std::string>> compute_rows() {
  const auto fixture = sonare::mixing::assistant::test::make_demo_tracks();
  const auto tracks = fixture.inputs();
  std::vector<std::tuple<std::string, std::string, std::string>> rows;
  for (const auto& scenario : scenarios()) {
    const auto result = sonare::mixing::assistant::suggest_scene(tracks, scenario.config);
    rows.emplace_back(scenario.name, "scene",
                      hex64(hash_text(sonare::mixing::api::scene_to_json(result.scene))));

    // The explanation is hashed separately from the scene: a change that moves
    // only the wording is a documentation change, and a change that moves only
    // the scene is a behaviour change. One combined hash could not tell them
    // apart.
    std::string explanation;
    for (const auto& line : result.explanation) {
      explanation += line;
      explanation += '\n';
    }
    rows.emplace_back(scenario.name, "explanation", hex64(hash_text(explanation)));
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
    std::string field;
    std::string hash;
    std::getline(stream, scenario, '\t');
    std::getline(stream, field, '\t');
    std::getline(stream, hash, '\t');
    out[scenario + "/" + field] = hash;
  }
  return out;
}

void write_manifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# scenario\tfield\tfnv1a_text_hash\n";
  for (const auto& [scenario, field, hash] : compute_rows()) {
    file << scenario << '\t' << field << '\t' << hash << '\n';
  }
}

}  // namespace

TEST_CASE("mixing assistant suggestion golden hashes stay stable",
          "[.][mixing][assistant][golden]") {
  const std::filesystem::path manifest = "tests/mixing/golden/assistant_scene_hashes.tsv";
  if (std::getenv("SONARE_UPDATE_MIXING_ASSISTANT_GOLDEN") != nullptr) {
    write_manifest(manifest);
  }

  const auto expected = load_manifest(manifest);
  const auto rows = compute_rows();
  REQUIRE(rows.size() == scenarios().size() * 2);
  REQUIRE(expected.size() == rows.size());

  // CHECK, not REQUIRE: a REQUIRE aborts on the first drifted row and hides how
  // far the drift spreads, which is the signal that separates a stale golden
  // from a real regression.
  for (const auto& [scenario, field, hash] : rows) {
    const std::string key = scenario + "/" + field;
    CAPTURE(key);
    CHECK(expected.at(key) == hash);
  }
}
