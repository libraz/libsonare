/// @file streaming_flush_tail_test.cpp
/// @brief Finite-tail requirements for streaming mastering flush.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

#include "mastering/api/chain.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::mastering::api {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr int kFlushBlockSize = 32;
constexpr int kMaxFlushCalls = 4096;
constexpr float kSentinel = -123.0f;

using Planes = std::vector<std::vector<float>>;

Planes make_signal(int num_channels, int offset, int num_samples) {
  Planes result(static_cast<size_t>(num_channels), std::vector<float>(num_samples));
  for (int channel = 0; channel < num_channels; ++channel) {
    for (int i = 0; i < num_samples; ++i) {
      const double phase = static_cast<double>(offset + i) / kSampleRate;
      result[static_cast<size_t>(channel)][static_cast<size_t>(i)] =
          0.55f * static_cast<float>(std::sin(2.0 * constants::kPiD * 440.0 * phase +
                                              static_cast<double>(channel) * 0.37));
    }
  }
  return result;
}

void process_block(StreamingMasteringChain& chain, Planes& block) {
  std::vector<float*> channels;
  channels.reserve(block.size());
  for (auto& plane : block) channels.push_back(plane.data());
  chain.process_block(channels.data(), static_cast<int>(channels.size()),
                      static_cast<int>(block.front().size()));
}

void warm_chain(StreamingMasteringChain& chain, int num_channels) {
  bool emitted_audio = false;
  for (int block_index = 0; block_index < 4; ++block_index) {
    auto block = make_signal(num_channels, block_index * kBlockSize, kBlockSize);
    process_block(chain, block);
    for (const auto& plane : block) {
      REQUIRE(std::all_of(plane.begin(), plane.end(),
                          [](float sample) { return std::isfinite(sample); }));
      emitted_audio |= std::any_of(plane.begin(), plane.end(),
                                   [](float sample) { return std::abs(sample) > 1.0e-5f; });
    }
  }
  REQUIRE(emitted_audio);
}

Planes drain_chain(StreamingMasteringChain& chain, int num_channels) {
  Planes output(static_cast<size_t>(num_channels));
  for (int call = 0; call < kMaxFlushCalls; ++call) {
    Planes block(static_cast<size_t>(num_channels), std::vector<float>(kFlushBlockSize, kSentinel));
    std::vector<float*> channels;
    channels.reserve(block.size());
    for (auto& plane : block) channels.push_back(plane.data());
    const int written = chain.flush(channels.data(), num_channels, kFlushBlockSize);
    REQUIRE(written >= 0);
    REQUIRE(written <= kFlushBlockSize);
    if (written == 0) return output;
    for (int channel = 0; channel < num_channels; ++channel) {
      const auto& plane = block[static_cast<size_t>(channel)];
      output[static_cast<size_t>(channel)].insert(output[static_cast<size_t>(channel)].end(),
                                                  plane.begin(), plane.begin() + written);
    }
  }
  throw std::runtime_error(
      "StreamingMasteringChain flush did not terminate within its finite bound");
}

bool same_audio(const Planes& left, const Planes& right) {
  if (left.size() != right.size()) return false;
  for (size_t channel = 0; channel < left.size(); ++channel) {
    if (left[channel].size() != right[channel].size()) return false;
    for (size_t sample = 0; sample < left[channel].size(); ++sample) {
      if (!std::isfinite(left[channel][sample]) || !std::isfinite(right[channel][sample])) {
        return false;
      }
      if (std::abs(left[channel][sample] - right[channel][sample]) > 1.0e-6f) return false;
    }
  }
  return true;
}

MasteringChainConfig tape_config(float bias) {
  MasteringChainConfig config;
  config.saturation.tape.enabled = true;
  config.saturation.tape.config.bias = bias;
  return config;
}

void require_unsupported_flush(StreamingMasteringChain& chain, int num_channels,
                               Planes& destination, int capacity) {
  std::vector<float*> channels;
  channels.reserve(destination.size());
  for (auto& plane : destination) channels.push_back(plane.data());
  bool refused = false;
  try {
    (void)chain.flush(channels.data(), num_channels, capacity);
  } catch (const SonareException& error) {
    refused = true;
    CHECK(error.code() == ErrorCode::NotImplemented);
  }
  REQUIRE(refused);
}

using CChain = std::unique_ptr<SonareStreamingMasteringChain,
                               decltype(&sonare_streaming_mastering_chain_destroy)>;

CChain make_c_chain(int num_channels) {
  const SonareMasteringParam params[] = {
      {"saturation.tape.enabled", 1.0},
      {"saturation.tape.bias", 0.1},
  };
  CChain chain(sonare_streaming_mastering_chain_create(params, 2),
               &sonare_streaming_mastering_chain_destroy);
  REQUIRE(chain != nullptr);
  REQUIRE(sonare_streaming_mastering_chain_prepare(chain.get(), static_cast<int>(kSampleRate),
                                                   kBlockSize, num_channels) == SONARE_OK);
  return chain;
}

void warm_c_chain(SonareStreamingMasteringChain* chain, int num_channels) {
  bool emitted_audio = false;
  for (int block_index = 0; block_index < 4; ++block_index) {
    auto block = make_signal(num_channels, block_index * kBlockSize, kBlockSize);
    SonareError result = SONARE_OK;
    if (num_channels == 1) {
      result =
          sonare_streaming_mastering_chain_process_mono(chain, block[0].data(), block[0].size());
    } else {
      result = sonare_streaming_mastering_chain_process_stereo(chain, block[0].data(),
                                                               block[1].data(), block[0].size());
    }
    REQUIRE(result == SONARE_OK);
    for (const auto& plane : block) {
      REQUIRE(std::all_of(plane.begin(), plane.end(),
                          [](float sample) { return std::isfinite(sample); }));
      emitted_audio |= std::any_of(plane.begin(), plane.end(),
                                   [](float sample) { return std::abs(sample) > 1.0e-5f; });
    }
  }
  REQUIRE(emitted_audio);
}

Planes drain_c_chain(SonareStreamingMasteringChain* chain, int num_channels) {
  Planes output(static_cast<size_t>(num_channels));
  for (int call = 0; call < kMaxFlushCalls; ++call) {
    Planes block(static_cast<size_t>(num_channels), std::vector<float>(kFlushBlockSize, kSentinel));
    size_t written = static_cast<size_t>(-1);
    SonareError result = SONARE_OK;
    if (num_channels == 1) {
      result = sonare_streaming_mastering_chain_flush_mono(chain, block[0].data(), block[0].size(),
                                                           &written);
    } else {
      result = sonare_streaming_mastering_chain_flush_stereo(
          chain, block[0].data(), block[1].data(), block[0].size(), &written);
    }
    REQUIRE(result == SONARE_OK);
    REQUIRE(written <= static_cast<size_t>(kFlushBlockSize));
    if (written == 0) return output;
    for (int channel = 0; channel < num_channels; ++channel) {
      const auto& plane = block[static_cast<size_t>(channel)];
      output[static_cast<size_t>(channel)].insert(
          output[static_cast<size_t>(channel)].end(), plane.begin(),
          plane.begin() + static_cast<std::ptrdiff_t>(written));
    }
  }
  throw std::runtime_error(
      "C API streaming mastering flush did not terminate within its finite bound");
}

}  // namespace
}  // namespace sonare::mastering::api

TEST_CASE("StreamingMasteringChain refuses an unbounded automatic flush without changing state",
          "[streaming-flush-tail][mastering][chain][streaming]") {
  using namespace sonare::mastering::api;

  const int num_channels = GENERATE(1, 2);
  StreamingMasteringChain actual(tape_config(0.1f));
  StreamingMasteringChain control(tape_config(0.1f));
  actual.prepare(kSampleRate, kBlockSize, num_channels);
  control.prepare(kSampleRate, kBlockSize, num_channels);
  warm_chain(actual, num_channels);
  warm_chain(control, num_channels);

  Planes empty_capacity(static_cast<size_t>(num_channels), std::vector<float>(8, kSentinel));
  std::vector<float*> empty_capacity_channels;
  for (auto& plane : empty_capacity) empty_capacity_channels.push_back(plane.data());
  CHECK(actual.flush(empty_capacity_channels.data(), num_channels, 0) == 0);
  for (const auto& plane : empty_capacity) {
    CHECK(
        std::all_of(plane.begin(), plane.end(), [](float sample) { return sample == kSentinel; }));
  }

  Planes refused_output(static_cast<size_t>(num_channels),
                        std::vector<float>(kFlushBlockSize, kSentinel));
  require_unsupported_flush(actual, num_channels, refused_output, kFlushBlockSize);
  for (const auto& plane : refused_output) {
    CHECK(
        std::all_of(plane.begin(), plane.end(), [](float sample) { return sample == kSentinel; }));
  }

  actual.set_parameter("saturation.tape.bias", 0.0);
  control.set_parameter("saturation.tape.bias", 0.0);
  const auto actual_tail = drain_chain(actual, num_channels);
  const auto control_tail = drain_chain(control, num_channels);
  for (const auto& plane : actual_tail) REQUIRE_FALSE(plane.empty());
  CHECK(same_audio(actual_tail, control_tail));
}

TEST_CASE("StreamingMasteringChain preserves a finite flush budget when an unbounded tail appears",
          "[streaming-flush-tail][mastering][chain][streaming]") {
  using namespace sonare::mastering::api;

  constexpr int num_channels = 1;
  StreamingMasteringChain actual(tape_config(0.0f));
  StreamingMasteringChain control(tape_config(0.0f));
  actual.prepare(kSampleRate, kBlockSize, num_channels);
  control.prepare(kSampleRate, kBlockSize, num_channels);
  warm_chain(actual, num_channels);
  warm_chain(control, num_channels);

  Planes first_tail(1, std::vector<float>(kFlushBlockSize, kSentinel));
  Planes control_first_tail(1, std::vector<float>(kFlushBlockSize, kSentinel));
  std::vector<float*> first_channels{first_tail[0].data()};
  std::vector<float*> control_channels{control_first_tail[0].data()};
  const int first_written = actual.flush(first_channels.data(), 1, kFlushBlockSize);
  const int control_first_written = control.flush(control_channels.data(), 1, kFlushBlockSize);
  REQUIRE(first_written == kFlushBlockSize);
  REQUIRE(control_first_written == first_written);
  CHECK(same_audio(first_tail, control_first_tail));

  actual.set_parameter("saturation.tape.bias", 0.1);
  Planes refused_output(1, std::vector<float>(kFlushBlockSize, kSentinel));
  require_unsupported_flush(actual, 1, refused_output, kFlushBlockSize);
  CHECK(std::all_of(refused_output[0].begin(), refused_output[0].end(),
                    [](float sample) { return sample == kSentinel; }));

  actual.set_parameter("saturation.tape.bias", 0.0);
  const auto actual_tail = drain_chain(actual, 1);
  const auto control_tail = drain_chain(control, 1);
  REQUIRE_FALSE(actual_tail[0].empty());
  CHECK(same_audio(actual_tail, control_tail));
}

TEST_CASE("Streaming mastering C API refuses unbounded mono and stereo flushes safely",
          "[streaming-flush-tail][c_api][mastering][streaming]") {
  using namespace sonare::mastering::api;

  const int num_channels = GENERATE(1, 2);
  auto actual = make_c_chain(num_channels);
  auto control = make_c_chain(num_channels);
  warm_c_chain(actual.get(), num_channels);
  warm_c_chain(control.get(), num_channels);

  Planes empty_capacity(static_cast<size_t>(num_channels), std::vector<float>(8, kSentinel));
  size_t samples_written = static_cast<size_t>(-1);
  const SonareError empty_result =
      num_channels == 1 ? sonare_streaming_mastering_chain_flush_mono(
                              actual.get(), empty_capacity[0].data(), 0, &samples_written)
                        : sonare_streaming_mastering_chain_flush_stereo(
                              actual.get(), empty_capacity[0].data(), empty_capacity[1].data(), 0,
                              &samples_written);
  CHECK(empty_result == SONARE_OK);
  CHECK(samples_written == 0);
  for (const auto& plane : empty_capacity) {
    CHECK(
        std::all_of(plane.begin(), plane.end(), [](float sample) { return sample == kSentinel; }));
  }

  Planes refused_output(static_cast<size_t>(num_channels),
                        std::vector<float>(kFlushBlockSize, kSentinel));
  samples_written = static_cast<size_t>(-1);
  const SonareError flush_result =
      num_channels == 1
          ? sonare_streaming_mastering_chain_flush_mono(actual.get(), refused_output[0].data(),
                                                        refused_output[0].size(), &samples_written)
          : sonare_streaming_mastering_chain_flush_stereo(
                actual.get(), refused_output[0].data(), refused_output[1].data(),
                refused_output[0].size(), &samples_written);
  REQUIRE(flush_result == SONARE_ERROR_NOT_SUPPORTED);
  CHECK(samples_written == 0);
  for (const auto& plane : refused_output) {
    CHECK(
        std::all_of(plane.begin(), plane.end(), [](float sample) { return sample == kSentinel; }));
  }

  REQUIRE(sonare_streaming_mastering_chain_set_parameter(actual.get(), "saturation.tape.bias",
                                                         0.0) == SONARE_OK);
  REQUIRE(sonare_streaming_mastering_chain_set_parameter(control.get(), "saturation.tape.bias",
                                                         0.0) == SONARE_OK);
  const auto actual_tail = drain_c_chain(actual.get(), num_channels);
  const auto control_tail = drain_c_chain(control.get(), num_channels);
  for (const auto& plane : actual_tail) REQUIRE_FALSE(plane.empty());
  CHECK(same_audio(actual_tail, control_tail));
}
