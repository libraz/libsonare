#include "mastering/assistant/repair_session.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

using namespace sonare;
namespace assistant = sonare::mastering::assistant;
namespace repair = sonare::mastering::repair;
namespace json = sonare::util::json;

namespace {

constexpr int kSr = 48000;
constexpr std::size_t kLength = kSr / 4;

/// A tone with a little noise, so every detector has a body to measure against.
std::vector<float> material(double hz, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> noise(0.0f, 0.003f);
  std::vector<float> samples(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.2f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * hz *
                                                    static_cast<double>(i) / kSr)) +
                 noise(rng);
  }
  return samples;
}

void add_clicks(std::vector<float>& samples, std::size_t first, std::size_t stride) {
  for (std::size_t i = first; i < samples.size(); i += stride) samples[i] = 0.95f;
}

std::vector<const float*> pointers(const std::vector<std::vector<float>>& planes) {
  std::vector<const float*> out;
  for (const auto& plane : planes) out.push_back(plane.data());
  return out;
}

struct Applied {
  std::vector<std::vector<float>> channels;
  json::Value reports;
};

Applied run_apply(const std::vector<std::vector<float>>& planes, const std::string& stages,
                  const assistant::RepairProgressCallback& progress = {},
                  const assistant::RepairCancelCallback& cancel = {}, bool* completed = nullptr) {
  Applied out;
  out.channels.assign(planes.size(), std::vector<float>(kLength, -7.0f));
  std::vector<float*> outputs;
  for (auto& plane : out.channels) outputs.push_back(plane.data());
  std::string reports;
  const bool done =
      assistant::repair_apply_json(pointers(planes).data(), planes.size(), kLength, kSr, stages,
                                   outputs.data(), &reports, progress, cancel);
  if (completed != nullptr) *completed = done;
  if (done) out.reports = json::parse(reports);
  return out;
}

std::vector<Audio> audios(const std::vector<std::vector<float>>& planes) {
  std::vector<Audio> out;
  for (const auto& plane : planes) out.push_back(Audio::from_buffer(plane.data(), kLength, kSr));
  return out;
}

bool same(const Audio& audio, const std::vector<float>& samples) {
  return audio.size() == samples.size() && std::equal(samples.begin(), samples.end(), audio.data());
}

}  // namespace

TEST_CASE("the linked declick, declip and dehum forms reproduce the mono and stereo entries",
          "[mastering][repair]") {
  std::vector<std::vector<float>> planes = {material(440.0, 1), material(660.0, 2)};
  add_clicks(planes[0], 1000, 4000);
  add_clicks(planes[1], 3000, 5000);
  for (float& sample : planes[1]) sample = std::clamp(sample * 6.0f, -0.98f, 0.98f);
  const std::vector<Audio> in = audios(planes);
  const Audio* one[1] = {&in[0]};
  const Audio* two[2] = {&in[0], &in[1]};
  std::vector<Audio> out;

  repair::DeclickConfig click;
  const Audio declicked = repair::declick(in[0], click);
  repair::declick_linked(one, 1, &out, click);
  REQUIRE(std::equal(out[0].begin(), out[0].end(), declicked.begin()));
  const auto click_pair = repair::declick_stereo(in[0], in[1], click);
  const auto click_reports = repair::declick_linked(two, 2, &out, click);
  REQUIRE(std::equal(out[1].begin(), out[1].end(), click_pair.right.begin()));
  REQUIRE(click_reports[0].linked_runs == click_pair.left_report.linked_runs);

  repair::DeclipConfig clip;
  const Audio declipped = repair::declip(in[1], clip);
  const Audio* clipped[1] = {&in[1]};
  repair::declip_linked(clipped, 1, &out, clip);
  REQUIRE(std::equal(out[0].begin(), out[0].end(), declipped.begin()));
  const auto clip_pair = repair::declip_stereo(in[0], in[1], clip);
  repair::declip_linked(two, 2, &out, clip);
  REQUIRE(std::equal(out[0].begin(), out[0].end(), clip_pair.left.begin()));
  REQUIRE(std::equal(out[1].begin(), out[1].end(), clip_pair.right.begin()));

  repair::DehumConfig hum;
  hum.adaptive = true;
  const Audio dehummed = repair::dehum(in[0], hum);
  repair::dehum_linked(one, 1, &out, hum);
  REQUIRE(std::equal(out[0].begin(), out[0].end(), dehummed.begin()));
  const auto hum_pair = repair::dehum_stereo(in[0], in[1], hum);
  repair::dehum_linked(two, 2, &out, hum);
  REQUIRE(std::equal(out[0].begin(), out[0].end(), hum_pair.left.begin()));
  REQUIRE(std::equal(out[1].begin(), out[1].end(), hum_pair.right.begin()));
}

TEST_CASE("linked declick repairs the union of every channel's runs", "[mastering][repair]") {
  std::vector<std::vector<float>> planes = {material(440.0, 3), material(550.0, 4),
                                            material(660.0, 5)};
  constexpr std::size_t kClick = 6000;
  planes[2][kClick] = 0.95f;
  const std::vector<Audio> in = audios(planes);
  const Audio* three[3] = {&in[0], &in[1], &in[2]};
  std::vector<Audio> out;
  const auto reports = repair::declick_linked(three, 3, &out, repair::DeclickConfig{});

  REQUIRE(reports.size() == 3);
  CHECK(reports[2].detected.count == 1);
  for (std::size_t c = 0; c < 2; ++c) {
    CHECK(reports[c].detected.count == 0);
    CHECK(reports[c].repaired_runs == 1);
    CHECK(reports[c].linked_runs == 1);
  }
  CHECK(std::abs(out[2][kClick]) < 0.3f);
}

TEST_CASE("repair analysis measures every channel and recommends from the suggester's rules",
          "[mastering][assistant]") {
  std::vector<std::vector<float>> planes = {material(440.0, 6), material(550.0, 7),
                                            material(660.0, 8)};
  add_clicks(planes[1], 2000, 3500);
  const std::string text =
      assistant::repair_analyze_json(pointers(planes).data(), planes.size(), kLength, kSr, "");
  const json::Value result = json::parse(text);

  REQUIRE(result["channels"].as_array().size() == 3);
  CHECK(result["channels"][0]["clickCount"].as_number() == 0.0);
  CHECK(result["channels"][1]["clickCount"].as_number() > 0.0);
  CHECK(result["defects"]["clickCount"].as_number() ==
        result["channels"][1]["clickCount"].as_number());
  CHECK(result["declipThresholdSafe"].is_bool());
  CHECK(std::isfinite(result["integratedLufs"].as_number()));
  const json::Array& recommended = result["recommended"].as_array();
  REQUIRE_FALSE(recommended.empty());
  CHECK(recommended.front()["stage"].as_string() == "declick");
  CHECK(recommended.front()["lpcOrder"].as_number() == 20.0);
  for (const json::Value& stage : recommended) CHECK(stage["stage"].as_string() != "dereverb");

  // The recommendation is a valid stage list as it stands.
  const Applied applied = run_apply(planes, json::dump(result["recommended"]));
  CHECK(applied.reports.as_array().size() == recommended.size());
}

TEST_CASE("the repair analysis schema list matches what the writer emits",
          "[mastering][assistant][schema]") {
  std::vector<std::vector<float>> planes = {material(440.0, 9), material(550.0, 10)};
  add_clicks(planes[0], 2000, 3500);
  std::set<std::string> actual = sonare::test::schema_paths_of(
      assistant::repair_analyze_json(pointers(planes).data(), planes.size(), kLength, kSr, ""));
  // Settings below `recommended[]` depend on the stage, so the list stops at its `stage` key.
  for (auto it = actual.begin(); it != actual.end();) {
    const bool setting = it->rfind("recommended[].", 0) == 0 && *it != "recommended[].stage";
    it = setting ? actual.erase(it) : std::next(it);
  }
  const auto& listed = assistant::repair_analysis_schema_paths();
  REQUIRE(actual == std::set<std::string>(listed.begin(), listed.end()));
}

TEST_CASE("repair apply runs the chain's fixed order whatever order the list gives",
          "[mastering][assistant]") {
  std::vector<std::vector<float>> planes = {material(440.0, 11), material(660.0, 12)};
  add_clicks(planes[0], 2000, 4500);
  for (float& sample : planes[1]) sample = std::clamp(sample * 6.0f, -0.98f, 0.98f);

  const Applied forward = run_apply(planes, R"([{"stage":"declip"},{"stage":"declick"}])");
  const Applied reversed = run_apply(planes, R"([{"stage":"declick"},{"stage":"declip"}])");
  REQUIRE(forward.channels == reversed.channels);
  REQUIRE(json::dump(forward.reports) == json::dump(reversed.reports));
  CHECK(forward.reports[0]["stage"].as_string() == "declip");
  CHECK(forward.reports[1]["stage"].as_string() == "declick");

  // Declip then declick through the per-stage stereo entries is the same pass.
  const std::vector<Audio> in = audios(planes);
  const auto declipped = repair::declip_stereo(in[0], in[1]);
  const auto declicked = repair::declick_stereo(declipped.left, declipped.right);
  CHECK(same(declicked.left, forward.channels[0]));
  CHECK(same(declicked.right, forward.channels[1]));
}

TEST_CASE("repair apply reports per channel for channel stages and once for linked ones",
          "[mastering][assistant]") {
  const std::vector<std::vector<float>> planes = {material(440.0, 13), material(550.0, 14),
                                                  material(660.0, 15)};
  const Applied applied = run_apply(planes, R"([{"stage":"denoise","mode":"logMmse"},
      {"stage":"decrackle"},{"stage":"dereverb"},{"stage":"dehum","adaptive":true}])");
  const json::Array& reports = applied.reports.as_array();
  REQUIRE(reports.size() == 4);
  const std::vector<std::string> order = {"decrackle", "dehum", "denoise", "dereverb"};
  for (std::size_t i = 0; i < reports.size(); ++i) {
    CAPTURE(i);
    CHECK(reports[i]["stage"].as_string() == order[i]);
    const bool linked = order[i] == "denoise" || order[i] == "dereverb";
    CHECK(reports[i]["scope"].as_string() == (linked ? "linked" : "channel"));
    CHECK(reports[i]["reports"].as_array().size() == (linked ? 1u : 3u));
  }
  CHECK(reports[2]["reports"][0]["detected"]["bandFloorDbfs"].as_array().size() ==
        repair::kRepairNoiseBandCount);
}

TEST_CASE("repair apply refuses a repeated stage, an unknown stage and an unknown setting",
          "[mastering][assistant]") {
  const std::vector<std::vector<float>> planes = {material(440.0, 16)};
  const auto refused = [&](const std::string& stages) {
    try {
      run_apply(planes, stages);
    } catch (const SonareException& e) {
      return e.code() == ErrorCode::InvalidParameter;
    }
    return false;
  };
  CHECK(refused(R"([{"stage":"declick"},{"stage":"declick","lpcOrder":8}])"));
  CHECK(refused(R"([{"stage":"trimSilence"}])"));
  CHECK(refused(R"([{"stage":"declick","lpcOdrer":8}])"));
  CHECK(refused(R"([{"stage":"denoise","mode":"bogus"}])"));
  CHECK(refused(R"([{"stage":"declick","lpcOrder":2049}])"));
  CHECK(refused(R"([{"stage":"declick","stage":"declip"}])"));
  CHECK(refused(R"({"stage":"declick"})"));
  CHECK(refused("[{"));
  CHECK_FALSE(refused(R"([{"stage":"denoise","mode":"logMmse","noiseEstimator":1}])"));
}

TEST_CASE("repair apply reports progress per stage and stops when cancelled",
          "[mastering][assistant]") {
  const std::vector<std::vector<float>> planes = {material(440.0, 17), material(550.0, 18)};
  const std::string stages = R"([{"stage":"decrackle"},{"stage":"declick"}])";
  std::vector<std::string> names;
  std::vector<float> fractions;
  const Applied full = run_apply(planes, stages, [&](float done, const char* stage) {
    fractions.push_back(done);
    names.emplace_back(stage);
  });
  CHECK(names == std::vector<std::string>{"repair.declick", "repair.decrackle"});
  CHECK(fractions == std::vector<float>{0.5f, 1.0f});

  bool completed = true;
  const Applied cancelled = run_apply(planes, stages, {}, [] { return true; }, &completed);
  CHECK_FALSE(completed);
  for (const auto& plane : cancelled.channels) {
    CHECK(std::all_of(plane.begin(), plane.end(), [](float s) { return s == -7.0f; }));
  }
}
