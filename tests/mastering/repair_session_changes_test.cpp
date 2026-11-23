#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mastering/assistant/repair_session.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/json.h"

namespace assistant = sonare::mastering::assistant;
namespace json = sonare::util::json;

namespace {

constexpr int kSampleRate = 48000;
constexpr std::size_t kLength = 4096;

std::vector<float> tone(double phase = 0.0) {
  std::vector<float> samples(kLength);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double time = static_cast<double>(i) / static_cast<double>(kSampleRate);
    const double angle = sonare::constants::kTwoPiD * sonare::constants::kA4Hz * time + phase;
    samples[i] = static_cast<float>(0.2 * std::sin(angle) + 0.003 * std::sin(17.0 * angle) +
                                    0.001 * std::sin(37.0 * angle));
  }
  return samples;
}

struct Applied {
  std::vector<std::vector<float>> channels;
  json::Value reports;
  bool completed = false;
};

Applied run_apply(const std::vector<std::vector<float>>& input, const std::string& stages,
                  const assistant::RepairCancelCallback& cancel = {}) {
  Applied result;
  result.channels.assign(input.size(), std::vector<float>(input.front().size(), -7.0f));
  std::vector<const float*> input_channels;
  std::vector<float*> output_channels;
  input_channels.reserve(input.size());
  output_channels.reserve(input.size());
  for (const auto& channel : input) input_channels.push_back(channel.data());
  for (auto& channel : result.channels) output_channels.push_back(channel.data());
  std::string reports_json;
  result.completed = assistant::repair_apply_json(
      input_channels.data(), input.size(), input.front().size(), kSampleRate, stages,
      output_channels.data(), &reports_json, {}, cancel);
  if (result.completed) result.reports = json::parse(reports_json);
  return result;
}

bool has_null_mean(const std::string& path) {
  const auto& means = assistant::repair_apply_null_means();
  const std::pair<std::string, std::string> expected{path, "-Infinity"};
  return std::find(means.begin(), means.end(), expected) != means.end();
}

}  // namespace

TEST_CASE("repair apply reports zero change for a clean declick input",
          "[mastering][assistant][changes]") {
  const std::vector<std::vector<float>> input = {tone(), tone(0.2)};
  const Applied applied = run_apply(input, R"([{"stage":"declick"}])");

  REQUIRE(applied.completed);
  REQUIRE(applied.reports.size() == 1);
  const json::Value& entry = applied.reports[0];
  CHECK(entry["scope"].as_string() == "channel");
  REQUIRE(entry["changes"].size() == input.size());
  for (std::size_t channel = 0; channel < input.size(); ++channel) {
    CHECK(applied.channels[channel] == input[channel]);
    CHECK(entry["changes"][channel]["changedSamples"].as_int() == 0);
    CHECK(entry["changes"][channel]["changeLevelDb"].is_null());
  }
  CHECK(has_null_mean("[].changes[].changeLevelDb"));
}

TEST_CASE("repair apply measures declick sample differences and RMS level",
          "[mastering][assistant][changes]") {
  std::vector<float> input_channel(kLength, 0.0f);
  input_channel[kLength / 2] = 0.95f;
  const std::vector<std::vector<float>> input = {input_channel};
  const Applied applied = run_apply(input, R"([{"stage":"declick"}])");

  REQUIRE(applied.completed);
  const json::Value& entry = applied.reports[0];
  const json::Value& change = entry["changes"][0];
  std::size_t changed_samples = 0;
  double squared_difference_sum = 0.0;
  for (std::size_t sample = 0; sample < input_channel.size(); ++sample) {
    if (applied.channels[0][sample] != input_channel[sample]) ++changed_samples;
    const double difference = static_cast<double>(applied.channels[0][sample]) -
                              static_cast<double>(input_channel[sample]);
    squared_difference_sum += difference * difference;
  }
  REQUIRE(changed_samples > 0);
  CHECK(change["changedSamples"].as_number() == static_cast<double>(changed_samples));
  CHECK(entry["reports"][0]["repairedSamples"].as_int() > 0);

  const double mean_squared_difference =
      squared_difference_sum / static_cast<double>(input_channel.size());
  const double expected_level_db = 10.0 * std::log10(mean_squared_difference);
  CHECK(std::abs(change["changeLevelDb"].as_number() - expected_level_db) < 1.0e-12);
}

TEST_CASE("repair apply measures a declick at the finite sample limit",
          "[mastering][assistant][changes]") {
  constexpr float kMaxSample = std::numeric_limits<float>::max();
  const Applied applied = run_apply({{-kMaxSample, kMaxSample, kMaxSample}},
                                    R"([{"stage":"declick","lpcOrder":0,"neighborRatio":0.5}])");

  REQUIRE(applied.completed);
  const std::vector<float> expected = {-kMaxSample, 0.0f, kMaxSample};
  CHECK(applied.channels.at(0) == expected);
  const json::Value& change = applied.reports[0]["changes"][0];
  CHECK(change["changedSamples"].as_int() == 1);
  REQUIRE(change["changeLevelDb"].is_number());
  const double level = change["changeLevelDb"].as_number();
  const double peak = static_cast<double>(kMaxSample);
  CHECK(std::isfinite(level));
  CHECK(std::abs(level - 10.0 * std::log10(peak * peak / 3.0)) < 1.0e-12);
}

TEST_CASE("repair apply changes contain one row per input channel for linked reports",
          "[mastering][assistant][changes]") {
  const std::vector<std::vector<float>> input = {tone(), tone(0.2), tone(0.4)};
  const Applied applied = run_apply(input, R"([{"stage":"denoise"}])");

  REQUIRE(applied.completed);
  REQUIRE(applied.reports.size() == 1);
  const json::Value& entry = applied.reports[0];
  CHECK(entry["scope"].as_string() == "linked");
  REQUIRE(entry["reports"].size() == 1);
  REQUIRE(entry["changes"].size() == input.size());
  for (const json::Value& change : entry["changes"].as_array()) {
    CHECK(change.contains("changedSamples"));
    CHECK(change.contains("changeLevelDb"));
  }
}

TEST_CASE("repair apply schema paths match every report field", "[mastering][assistant][schema]") {
  const std::vector<std::vector<float>> input = {tone()};
  const Applied applied =
      run_apply(input, R"([{"stage":"dereverb","wpeEnabled":true},{"stage":"denoise"},
                  {"stage":"dehum"},{"stage":"decrackle"},{"stage":"declick"},
                  {"stage":"declip"}])");

  REQUIRE(applied.completed);
  const std::set<std::string> actual = sonare::test::schema_paths_of(json::dump(applied.reports));
  const auto& paths = assistant::repair_apply_schema_paths();
  CHECK(actual == std::set<std::string>(paths.begin(), paths.end()));
  CHECK(has_null_mean("[].reports[].detected.floorDbfs"));
  CHECK(has_null_mean("[].reports[].detected.bandFloorDbfs[]"));
  CHECK(has_null_mean("[].reports[].detected.harmonicDbfs[]"));
}

TEST_CASE("repair apply preserves output and reports when cancelled",
          "[mastering][assistant][changes]") {
  const std::vector<std::vector<float>> input = {tone()};
  std::vector<float> output(kLength, -7.0f);
  const float* input_channels[] = {input[0].data()};
  float* output_channels[] = {output.data()};
  std::string reports_json = "untouched";

  const bool completed = assistant::repair_apply_json(input_channels, 1, kLength, kSampleRate,
                                                      R"([{"stage":"declick"}])", output_channels,
                                                      &reports_json, {}, [] { return true; });

  CHECK_FALSE(completed);
  CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == -7.0f; }));
  CHECK(reports_json == "untouched");
}
