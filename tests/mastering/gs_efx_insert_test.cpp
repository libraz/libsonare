/// @file gs_efx_insert_test.cpp
/// @brief The GS EFX unit as the `effects.gsEfx` audio insert: catalog, construction, automation.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/processor_params.h"
#include "support/audio_fixtures.h"
#include "util/exception.h"
#if defined(SONARE_WITH_MIXING)
#include "mixing/channel_strip.h"
#endif

namespace {

using sonare::SonareException;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_info_json;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::make_insert_from_params;
using sonare::mastering::api::Param;

constexpr const char* kName = "effects.gsEfx";
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 256;

bool contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<float> input_signal(std::size_t size) {
  std::vector<float> input(size, 0.0f);
  for (std::size_t i = 0; i < size; ++i) {
    input[i] = static_cast<float>(0.15 * std::sin(0.021 * static_cast<double>(i)) +
                                  0.04 * std::cos(0.071 * static_cast<double>(i)));
  }
  input[0] += 0.5f;
  return input;
}

std::vector<float> render_prepared(const std::string& json) {
  auto processor = make_insert(kName, json);
  REQUIRE(processor != nullptr);
  processor->prepare(kSampleRate, kBlockSize);
  std::vector<float> output = input_signal(4096);
  for (std::size_t offset = 0; offset < output.size(); offset += kBlockSize) {
    const int count = static_cast<int>(std::min<std::size_t>(kBlockSize, output.size() - offset));
    float* channels[] = {output.data() + offset};
    processor->process(channels, 1, count);
  }
  return output;
}

float max_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float difference = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference = std::max(difference, std::fabs(a[i] - b[i]));
  }
  return difference;
}

void require_invalid(const std::vector<Param>& params) {
  REQUIRE_THROWS_AS(make_insert_from_params(kName, params), SonareException);
}

TEST_CASE("GS EFX audio insert is listed and publishes its complete construction surface",
          "[mastering][insert_factory][gs][efx]") {
  REQUIRE(contains(insert_factory_names(), kName));

  const auto names = insert_param_names(kName);
  REQUIRE(contains(names, "typeMsb"));
  REQUIRE(contains(names, "typeLsb"));
  REQUIRE(contains(names, "realization"));
  for (int byte = 0; byte < 20; ++byte) {
    REQUIRE(contains(names, "byte" + std::to_string(byte)));
  }

  const std::string metadata = insert_param_info_json(kName);
  REQUIRE(metadata.find(R"("name":"typeMsb","id":null)") != std::string::npos);
  REQUIRE(metadata.find(R"("name":"typeLsb","id":null)") != std::string::npos);
  REQUIRE(metadata.find(R"("name":"realization","id":null)") != std::string::npos);
  // The default Thru state cannot change type, so none of its bytes can be heard.
  INFO(metadata);
  REQUIRE(metadata.find(R"("name":"byte0","id":0,"rtSafe":false)") != std::string::npos);
  REQUIRE(metadata.find(R"("name":"byte19","id":19,"rtSafe":false)") != std::string::npos);
}

TEST_CASE("GS EFX factory admits the Thru default and modern/classic/parallel types",
          "[mastering][insert_factory][gs][efx]") {
  // Missing selectors are the power-on Thru state and must remain a useful
  // dry processor for catalog probing and scenes that omit optional params.
  const std::vector<float> input = input_signal(4096);
  const std::vector<float> thru = render_prepared("{}");
  REQUIRE(max_difference(input, thru) == 0.0f);

  const std::array<const char*, 3> types = {
      R"({"typeMsb":1,"typeLsb":0,"realization":0,"byte1":127})",
      R"({"typeMsb":1,"typeLsb":0,"realization":1,"byte1":127})",
      R"({"typeMsb":17,"typeLsb":0,"realization":0,"byte1":127})",
  };
  for (const char* json : types) {
    auto processor = make_insert(kName, json);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, kBlockSize);
    std::vector<float> output = input;
    for (std::size_t offset = 0; offset < output.size(); offset += kBlockSize) {
      float* channels[] = {output.data() + offset};
      processor->process(channels, 1, kBlockSize);
    }
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float sample) { return std::isfinite(sample); }));
    REQUIRE(max_difference(input, output) > 1.0e-6f);
  }
}

TEST_CASE("GS EFX audio insert matches the public classic construction example",
          "[mastering][insert_factory][gs][efx]") {
  auto processor = make_insert(kName, R"({"typeMsb":1,"typeLsb":16,"realization":1,"byte0":80})");
  REQUIRE(processor != nullptr);
  processor->prepare(48000.0, 256);
  std::vector<float> output = input_signal(4096);
  for (std::size_t offset = 0; offset < output.size(); offset += 256) {
    float* channels[] = {output.data() + offset};
    processor->process(channels, 1, 256);
  }
  REQUIRE(std::all_of(output.begin(), output.end(),
                      [](float sample) { return std::isfinite(sample); }));
  REQUIRE(max_difference(input_signal(4096), output) > 1.0e-6f);
}

TEST_CASE("GS EFX JSON and Param-list construction have identical audio behavior",
          "[mastering][insert_factory][gs][efx]") {
  const std::string json = R"({"typeMsb":1,"typeLsb":0,"realization":0,"byte1":127,"byte19":120})";
  const std::vector<Param> params = {
      {"typeMsb", 1.0}, {"typeLsb", 0.0}, {"realization", 0.0}, {"byte1", 127.0}, {"byte19", 120.0},
  };

  auto from_json = make_insert(kName, json);
  auto from_params = make_insert_from_params(kName, params);
  REQUIRE(from_json != nullptr);
  REQUIRE(from_params != nullptr);
  from_json->prepare(kSampleRate, kBlockSize);
  from_params->prepare(kSampleRate, kBlockSize);

  std::vector<float> left = input_signal(4096);
  std::vector<float> right = left;
  for (std::size_t offset = 0; offset < left.size(); offset += kBlockSize) {
    float* json_channels[] = {left.data() + offset};
    float* param_channels[] = {right.data() + offset};
    from_json->process(json_channels, 1, kBlockSize);
    from_params->process(param_channels, 1, kBlockSize);
  }
  REQUIRE(max_difference(left, right) < 1.0e-6f);
}

TEST_CASE("GS EFX factory rejects incomplete selectors and non-integral or out-of-range bytes",
          "[mastering][insert_factory][gs][efx]") {
  require_invalid({{"typeMsb", 1.0}});
  require_invalid({{"typeLsb", 0.0}});
  require_invalid({{"realization", 2.0}});
  require_invalid({{"typeMsb", 1.5}, {"typeLsb", 0.0}});
  require_invalid({{"typeMsb", std::numeric_limits<double>::quiet_NaN()}, {"typeLsb", 0.0}});
  require_invalid({{"typeMsb", 1.0}, {"typeLsb", 0.0}, {"byte0", 127.5}});
  require_invalid({{"typeMsb", 1.0}, {"typeLsb", 0.0}, {"byte0", -1.0}});
  require_invalid({{"typeMsb", 1.0}, {"typeLsb", 0.0}, {"byte0", 128.0}});
  // 0x0100 byte0 is a two-state selector; values beyond its printed state
  // list are ignored by the MIDI write path and must be rejected by a
  // standalone construction API rather than silently producing a different rig.
  require_invalid({{"typeMsb", 1.0}, {"typeLsb", 0.0}, {"byte0", 127.0}});
  require_invalid({{"typeMsb", 127.0}, {"typeLsb", 127.0}});
}

TEST_CASE("GS EFX explicit raw bytes overlay type defaults before prepare",
          "[mastering][insert_factory][gs][efx]") {
  auto processor = make_insert(kName, R"({"typeMsb":1,"typeLsb":0,"byte3":127})");
  REQUIRE(processor != nullptr);
  unsigned int byte3_id = 0;
  unsigned int byte1_id = 0;
  for (const auto& descriptor : processor->parameter_descriptors()) {
    if (descriptor.key == "byte3") byte3_id = descriptor.id;
    if (descriptor.key == "byte1") byte1_id = descriptor.id;
  }
  float byte3 = 0.0f;
  float byte1 = 0.0f;
  REQUIRE(processor->constructed_parameter_value(byte3_id, &byte3));
  REQUIRE(processor->constructed_parameter_value(byte1_id, &byte1));
  REQUIRE(byte3 == 127.0f);
  // Type 0x0100's measured power-on byte1 is 69. This catches the common
  // implementation error where the factory starts every omitted slot at 0
  // instead of loading the selected GS type defaults first.
  REQUIRE(byte1 == 69.0f);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("GS EFX factory insert runs through ChannelStrip and preserves surround rear planes",
          "[mastering][mixing][insert_factory][gs][efx]") {
  const std::array<const char*, 2> realizations = {
      R"({"typeMsb":1,"typeLsb":0,"realization":0,"byte1":127})",
      R"({"typeMsb":1,"typeLsb":16,"realization":1,"byte0":80})",
  };
  const auto policy = sonare::mastering::api::channel_policy(kName);
  REQUIRE(policy == sonare::mastering::api::ChannelPolicy::StereoPairOnly);

  for (const char* json : realizations) {
    auto processor = make_insert(kName, json);
    REQUIRE(processor != nullptr);

    sonare::mixing::ChannelStrip strip;
    strip.set_prepared_channels(4);
    strip.add_pre_insert(std::move(processor),
                         policy == sonare::mastering::api::ChannelPolicy::StereoPairOnly);
    strip.prepare(kSampleRate, kBlockSize);

    const bool classic = std::string(json).find(R"("realization":1)") != std::string::npos;
    if (classic) {
      // Classic units expose a fixed ten-second host rendering allowance at
      // 48 kHz. This is a metadata contract; rendering that many samples is
      // unnecessary for proving the live strip path below.
      REQUIRE(strip.tail_samples() == 480000);
    } else {
      REQUIRE(strip.tail_samples() >= 0);
    }

    std::array<std::vector<float>, 4> planes;
    for (std::size_t channel = 0; channel < planes.size(); ++channel) {
      planes[channel] = input_signal(4096);
      for (float& sample : planes[channel]) {
        sample += static_cast<float>(channel + 1) * 0.01f;
      }
    }
    const std::vector<float> front_left = planes[0];
    const std::vector<float> front_right = planes[1];
    const std::vector<float> rear_left = planes[2];
    const std::vector<float> rear_right = planes[3];
    for (std::size_t offset = 0; offset < planes[0].size(); offset += kBlockSize) {
      float* channels[] = {planes[0].data() + offset, planes[1].data() + offset,
                           planes[2].data() + offset, planes[3].data() + offset};
      strip.process(channels, 4, kBlockSize);
    }

    REQUIRE(planes[2] == rear_left);
    REQUIRE(planes[3] == rear_right);
    REQUIRE(max_difference(front_left, planes[0]) > 1.0e-6f);
    REQUIRE(max_difference(front_right, planes[1]) > 1.0e-6f);
    REQUIRE(std::all_of(planes[0].begin(), planes[0].end(),
                        [](float sample) { return std::isfinite(sample); }));
    REQUIRE(std::all_of(planes[1].begin(), planes[1].end(),
                        [](float sample) { return std::isfinite(sample); }));
  }
}
#endif

}  // namespace
