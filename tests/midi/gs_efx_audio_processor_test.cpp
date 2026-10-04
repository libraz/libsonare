/// @file gs_efx_audio_processor_test.cpp
/// @brief Direct audio-path coverage for the shared GS EFX graph facade.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if SONARE_BUILD_FX && defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif
#include "midi/synth/gs_efx.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_graph.h"
#include "support/alloc_guard.h"

namespace {

using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxProcessor;
using sonare::midi::synth::GsEfxRealization;
using sonare::midi::synth::GsEfxStageFactory;
using sonare::midi::synth::sf2_build_efx_unit;
using sonare::midi::synth::sf2_run_efx_unit;
using sonare::midi::synth::Sf2EfxDelayRt;
using sonare::midi::synth::Sf2EfxUnitRt;
using sonare::rt::ParamDescriptor;
using sonare::rt::ProcessorBase;

constexpr double kSampleRate = 48000.0;

struct Probe {
  int prepare_calls = 0;
  int wrong_channel_prepare_calls = 0;
};

/// A deterministic stage used to compare the public facade with the shared
/// Sf2 runner without pulling the mastering factory into this focused test.
/// It deliberately has every generated binding key, so a test exercises the
/// real row-to-stage/descriptor resolver and not a test-only special case.
class ProbeStage final : public ProcessorBase {
 public:
  ProbeStage(std::shared_ptr<Probe> probe, std::string_view name)
      : probe_(std::move(probe)), gain_(name_gain(name)) {}

  void prepare(double, int, int max_channels) override {
    ++probe_->prepare_calls;
    if (max_channels != 2) ++probe_->wrong_channel_prepare_calls;
  }

  void prepare(double sample_rate, int max_block_size) override {
    prepare(sample_rate, max_block_size, 2);
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    const float stage_gain = gain_ * parameter_gain_;
    for (int channel = 0; channel < num_channels; ++channel) {
      for (int sample = 0; sample < num_samples; ++sample) {
        channels[channel][sample] *= stage_gain;
      }
    }
  }

  void reset() override {}

  int tail_samples() const noexcept override { return 13; }

  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id < sonare::midi::synth::kGsEfxRowKeys.size();
  }

  std::vector<ParamDescriptor> parameter_descriptors() const override {
    std::vector<ParamDescriptor> out;
    out.reserve(sonare::midi::synth::kGsEfxRowKeys.size());
    for (unsigned int id = 0; id < sonare::midi::synth::kGsEfxRowKeys.size(); ++id) {
      out.push_back({std::string(sonare::midi::synth::kGsEfxRowKeys[id]), id});
    }
    return out;
  }

 protected:
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (!parameter_is_realtime_safe(param_id)) return false;
    parameter_gain_ = 0.5f + 0.5f * (value / 127.0f);
    return true;
  }

 private:
  static float name_gain(std::string_view name) noexcept {
    unsigned int sum = 0;
    for (const char c : name) sum += static_cast<unsigned char>(c);
    return 0.55f + 0.01f * static_cast<float>(sum % 25u);
  }

  std::shared_ptr<Probe> probe_;
  float gain_ = 1.0f;
  float parameter_gain_ = 1.0f;
};

/// A fixed-delay identity stage makes branch alignment and bypass compensation
/// observable without depending on a mastering processor's impulse shape.
class FixedLatencyStage final : public ProcessorBase {
 public:
  explicit FixedLatencyStage(int latency_q8) : latency_q8_(std::max(0, latency_q8)) {}

  void prepare(double, int, int max_channels) override {
    REQUIRE(max_channels == 2);
    const bool fractional = (latency_q8_ & 0xFF) != 0;
    const int integer_samples = latency_q8_ >> 8;
    const std::size_t size = fractional ? static_cast<std::size_t>(std::max(8, integer_samples + 8))
                                        : static_cast<std::size_t>(std::max(1, integer_samples));
    for (auto& line : lines_) line.assign(size, 0.0f);
    for (auto& line : fractional_lines_) line.assign(fractional ? size : 0u, 0.0f);
    indices_.fill(0);
    fractional_indices_.fill(0);
  }

  void prepare(double sample_rate, int max_block_size) override {
    prepare(sample_rate, max_block_size, 2);
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (latency_q8_ == 0) return;
    if ((latency_q8_ & 0xFF) != 0) {
      for (int channel = 0; channel < num_channels; ++channel) {
        auto& line = fractional_lines_[static_cast<std::size_t>(channel)];
        auto& index = fractional_indices_[static_cast<std::size_t>(channel)];
        for (int sample = 0; sample < num_samples; ++sample) {
          channels[channel][sample] = sonare::rt::lagrange3_fractional_delay(
              line, index, latency_q8_, channels[channel][sample]);
        }
      }
      return;
    }
    for (int channel = 0; channel < num_channels; ++channel) {
      auto& line = lines_[static_cast<std::size_t>(channel)];
      auto& index = indices_[static_cast<std::size_t>(channel)];
      for (int sample = 0; sample < num_samples; ++sample) {
        const float input = channels[channel][sample];
        channels[channel][sample] = line[index];
        line[index] = input;
        index = (index + 1) % line.size();
      }
    }
  }

  void reset() override {
    for (auto& line : lines_) std::fill(line.begin(), line.end(), 0.0f);
    for (auto& line : fractional_lines_) std::fill(line.begin(), line.end(), 0.0f);
    indices_.fill(0);
    fractional_indices_.fill(0);
  }

  int latency_samples() const noexcept override { return latency_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_q8_; }

  bool parameter_is_realtime_safe(unsigned int) const noexcept override { return false; }

  std::vector<ParamDescriptor> parameter_descriptors() const override { return {}; }

 protected:
  bool set_parameter_impl(unsigned int, float) override { return false; }

 private:
  int latency_q8_ = 0;
  std::array<std::vector<float>, 2> lines_{};
  std::array<std::vector<float>, 2> fractional_lines_{};
  std::array<std::size_t, 2> indices_{};
  std::array<std::size_t, 2> fractional_indices_{};
};

class NoAllocLatencyStage final : public ProcessorBase {
 public:
  explicit NoAllocLatencyStage(int latency_q8) : latency_q8_(latency_q8) {}

  void prepare(double, int, int max_channels) override { REQUIRE(max_channels == 2); }

  void prepare(double sample_rate, int max_block_size) override {
    prepare(sample_rate, max_block_size, 2);
  }

  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_q8_; }
  bool parameter_is_realtime_safe(unsigned int) const noexcept override { return false; }
  std::vector<ParamDescriptor> parameter_descriptors() const override { return {}; }

 protected:
  bool set_parameter_impl(unsigned int, float) override { return false; }

 private:
  int latency_q8_ = 0;
};

/// Records the value each stage parameter holds, and can refuse one numbered write.
struct WriteLog {
  using Key = std::pair<const void*, unsigned int>;
  std::map<Key, float> current;
  std::vector<std::pair<Key, float>> accepted;
  int writes = 0;
  int reject_write = -1;
};

class RecordingStage final : public ProcessorBase {
 public:
  explicit RecordingStage(std::shared_ptr<WriteLog> log) : log_(std::move(log)) {}

  void prepare(double, int, int) override {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}

  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override {
    return param_id < sonare::midi::synth::kGsEfxRowKeys.size();
  }

  std::vector<ParamDescriptor> parameter_descriptors() const override {
    std::vector<ParamDescriptor> out;
    for (unsigned int id = 0; id < sonare::midi::synth::kGsEfxRowKeys.size(); ++id) {
      out.push_back({std::string(sonare::midi::synth::kGsEfxRowKeys[id]), id});
    }
    return out;
  }

 protected:
  bool set_parameter_impl(unsigned int param_id, float value) override {
    if (!parameter_is_realtime_safe(param_id)) return false;
    if (log_->writes++ == log_->reject_write) return false;
    log_->current[{this, param_id}] = value;
    log_->accepted.push_back({{this, param_id}, value});
    return true;
  }

 private:
  std::shared_ptr<WriteLog> log_;
};

GsEfxStageFactory probe_factory(const std::shared_ptr<Probe>& probe) {
  return [probe](std::string_view name, std::string_view) {
    return std::make_unique<ProbeStage>(probe, name);
  };
}

GsEfxStageFactory fixed_latency_factory(int chorus_q8, int delay_q8, int amp_q8 = 0) {
  return [chorus_q8, delay_q8, amp_q8](std::string_view name, std::string_view) {
    int latency_q8 = 0;
    if (name == "effects.modulation.chorus") {
      latency_q8 = chorus_q8;
    } else if (name == "effects.delay.stereo") {
      latency_q8 = delay_q8;
    } else if (name == "saturation.ampSim") {
      latency_q8 = amp_q8;
    }
    return std::make_unique<FixedLatencyStage>(latency_q8);
  };
}

GsEfxStageFactory no_alloc_latency_factory(int first_q8, int second_q8) {
  return [first_q8, second_q8](std::string_view name, std::string_view) {
    if (name == "effects.modulation.chorus") {
      return std::make_unique<NoAllocLatencyStage>(first_q8);
    }
    if (name == "utility.gain") {
      return std::make_unique<NoAllocLatencyStage>(second_q8);
    }
    return std::make_unique<NoAllocLatencyStage>(0);
  };
}

GsEfx state_for(uint16_t type) {
  GsEfx state;
  state.type = type;
  state.type_msb = static_cast<uint8_t>(type >> 8);
  state.assigned = type != 0;
  if (const auto* defaults = gs_efx_type_defaults(type); defaults != nullptr) {
    state.params = defaults->params;
  }
  return state;
}

std::vector<float> signal(std::size_t size, float phase) {
  std::vector<float> out(size);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = 0.2f * std::sin(0.017f * static_cast<float>(i) + phase) +
             0.05f * std::cos(0.071f * static_cast<float>(i) + phase);
  }
  out.front() += 0.5f;
  return out;
}

float max_difference(const std::vector<float>& left, const std::vector<float>& right) {
  REQUIRE(left.size() == right.size());
  float result = 0.0f;
  for (std::size_t i = 0; i < left.size(); ++i) {
    result = std::max(result, std::fabs(left[i] - right[i]));
  }
  return result;
}

float reference_fractional_delay_step(std::vector<float>& history, std::size_t& write,
                                      int delay_samples_q8, float input) {
  const auto sample_at_delay = [&](int delay) {
    delay = std::max(0, delay);
    const std::size_t index =
        (write + history.size() - (static_cast<std::size_t>(delay) % history.size())) %
        history.size();
    return history[index];
  };

  history[write] = input;
  const float delay = static_cast<float>(std::max(0, delay_samples_q8)) / 256.0f;
  const int base = static_cast<int>(std::floor(delay));
  const float mu = delay - static_cast<float>(base);
  float y0 = 0.0f;
  float y1 = 0.0f;
  float y2 = 0.0f;
  float y3 = 0.0f;
  float c0 = 0.0f;
  float c1 = 0.0f;
  float c2 = 0.0f;
  float c3 = 0.0f;
  if (base >= 1) {
    y0 = sample_at_delay(base - 1);
    y1 = sample_at_delay(base);
    y2 = sample_at_delay(base + 1);
    y3 = sample_at_delay(base + 2);
    c0 = -mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
    c1 = (mu + 1.0f) * (mu - 1.0f) * (mu - 2.0f) / 2.0f;
    c2 = -(mu + 1.0f) * mu * (mu - 2.0f) / 2.0f;
    c3 = (mu + 1.0f) * mu * (mu - 1.0f) / 6.0f;
  } else {
    y0 = sample_at_delay(0);
    y1 = sample_at_delay(1);
    y2 = sample_at_delay(2);
    y3 = sample_at_delay(3);
    c0 = -(mu - 1.0f) * (mu - 2.0f) * (mu - 3.0f) / 6.0f;
    c1 = mu * (mu - 2.0f) * (mu - 3.0f) / 2.0f;
    c2 = -mu * (mu - 1.0f) * (mu - 3.0f) / 2.0f;
    c3 = mu * (mu - 1.0f) * (mu - 2.0f) / 6.0f;
  }
  write = (write + 1) % history.size();
  return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
}

struct StereoAudio {
  std::vector<float> left;
  std::vector<float> right;
};

StereoAudio run_facade(const GsEfx& state, GsEfxRealization realization,
                       const GsEfxStageFactory& factory, int block_size,
                       const std::vector<float>& input_left,
                       const std::vector<float>& input_right) {
  GsEfxProcessor processor(state, realization, factory);
  processor.prepare(kSampleRate, block_size, 2);
  StereoAudio output{input_left, input_right};
  float* channels[] = {output.left.data(), output.right.data()};
  processor.process(channels, 2, static_cast<int>(output.left.size()));
  return output;
}

StereoAudio run_shared_unit(const GsEfx& state, GsEfxRealization realization,
                            const GsEfxStageFactory& factory, int block_size,
                            const std::vector<float>& input_left,
                            const std::vector<float>& input_right) {
  const std::vector<sonare::midi::synth::GsEfxStage> stages =
      realization == GsEfxRealization::kModern ? gs_efx_insert_chain(state)
                                               : std::vector<sonare::midi::synth::GsEfxStage>{};
  Sf2EfxUnitRt unit =
      sf2_build_efx_unit(state, stages, realization, factory, kSampleRate, block_size);
  StereoAudio output{input_left, input_right};
  for (std::size_t offset = 0; offset < output.left.size(); offset += block_size) {
    const int count = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(block_size), output.left.size() - offset));
    sf2_run_efx_unit(unit, output.left.data() + offset, output.right.data() + offset, count);
  }
  return output;
}

#if SONARE_BUILD_FX && defined(SONARE_WITH_MASTERING)
int add_latency_saturated(int left, int right) {
  const int64_t sum = static_cast<int64_t>(std::max(0, left)) + std::max(0, right);
  return sum >= std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                : static_cast<int>(sum);
}

struct GraphLatency {
  int samples = 0;
  int q8 = 0;
};

GraphLatency actual_factory_graph_latency(
    const GsEfx& state, const std::vector<sonare::midi::synth::GsEfxStage>& stages) {
  int front_q8 = 0;
  int half_a_q8 = 0;
  int half_b_q8 = 0;
  int back_q8 = 0;
  for (const auto& stage : stages) {
    auto processor = sonare::mastering::api::make_insert(stage.name, stage.params_json);
    REQUIRE(processor != nullptr);
    processor->prepare(kSampleRate, 512, 2);
    const int latency_q8 = processor->latency_samples_q8();
    int* branch_latency_q8 = &front_q8;
    if (stage.branch == sonare::midi::synth::kGsEfxBranchHalfA) {
      branch_latency_q8 = &half_a_q8;
    } else if (stage.branch == sonare::midi::synth::kGsEfxBranchHalfB) {
      branch_latency_q8 = &half_b_q8;
    } else if (stage.branch == sonare::midi::synth::kGsEfxBranchBack) {
      branch_latency_q8 = &back_q8;
    }
    *branch_latency_q8 = add_latency_saturated(*branch_latency_q8, latency_q8);
  }
  (void)state;
  const int q8 = add_latency_saturated(
      add_latency_saturated(front_q8, std::max(half_a_q8, half_b_q8)), back_q8);
  return {q8 >> 8, q8};
}

TEST_CASE("GS EFX facade reports latency of the realized modern child graph",
          "[midi][gs][efx][audio][latency]") {
  const auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  for (const uint16_t type :
       {uint16_t{0x0200}, uint16_t{0x0300}, uint16_t{0x0400}, uint16_t{0x1100}}) {
    const GsEfx state = state_for(type);
    const auto stages = gs_efx_insert_chain(state);
    const GraphLatency expected = actual_factory_graph_latency(state, stages);
    GsEfxProcessor processor(state, GsEfxRealization::kModern, factory);
    processor.prepare(kSampleRate, 512, 2);
    CAPTURE(type, expected.samples, expected.q8);
    if (type != 0x1100) CHECK(expected.samples > 0);
    CHECK(processor.latency_samples() == expected.samples);
    CHECK(processor.latency_samples_q8() == expected.q8);
  }
}
#endif  // SONARE_BUILD_FX && SONARE_WITH_MASTERING

TEST_CASE("GS EFX aligns unequal parallel branch latency without audio allocation",
          "[midi][gs][efx][audio][latency]") {
  GsEfxProcessor processor(state_for(0x1100), GsEfxRealization::kModern,
                           fixed_latency_factory(5 * 256, 11 * 256));
  processor.prepare(kSampleRate, 32, 2);
  REQUIRE(processor.latency_samples() == 11);
  REQUIRE(processor.latency_samples_q8() == 11 * 256);

  std::array<float, 32> left{};
  std::array<float, 32> right{};
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    processor.process(channels, 2, static_cast<int>(left.size()));
    allocations = guard.count();
  }
  REQUIRE(allocations == 0);
  REQUIRE(std::fabs(left[5]) < 1.0e-6f);
  REQUIRE(std::fabs(right[5]) < 1.0e-6f);
  REQUIRE(std::fabs(left[11] - 2.0f) < 1.0e-6f);
  REQUIRE(std::fabs(right[11] - 2.0f) < 1.0e-6f);

  processor.reset();
  left.fill(0.0f);
  right.fill(0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  processor.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(std::fabs(left[11] - 2.0f) < 1.0e-6f);
  REQUIRE(std::fabs(right[11] - 2.0f) < 1.0e-6f);

  processor.prepare(96000.0, 17, 2);
  REQUIRE(processor.latency_samples() == 11);
  REQUIRE(processor.latency_samples_q8() == 11 * 256);
  left.fill(0.0f);
  right.fill(0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  processor.process(channels, 2, static_cast<int>(left.size()));
  REQUIRE(std::fabs(left[11] - 2.0f) < 1.0e-6f);
  REQUIRE(std::fabs(right[11] - 2.0f) < 1.0e-6f);
}

TEST_CASE("GS EFX keeps constructed disabled stages on the reported timeline",
          "[midi][gs][efx][audio][latency]") {
  // Four amp stages, one of them enabled, three samples each.
  GsEfxProcessor processor(state_for(0x0400), GsEfxRealization::kModern,
                           fixed_latency_factory(0, 0, 3 * 256));
  processor.prepare(kSampleRate, 32, 2);
  REQUIRE(processor.latency_samples() == 12);

  std::array<float, 32> left{};
  std::array<float, 32> right{};
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
  for (int i = 0; i < 12; ++i) {
    REQUIRE(std::fabs(left[static_cast<std::size_t>(i)]) < 1.0e-6f);
    REQUIRE(std::fabs(right[static_cast<std::size_t>(i)]) < 1.0e-6f);
  }
  REQUIRE(std::fabs(left[12] - 1.0f) < 1.0e-6f);
  REQUIRE(std::fabs(right[12] - 1.0f) < 1.0e-6f);
}

TEST_CASE("GS EFX reports fractional bypass support in the offline tail",
          "[midi][gs][efx][audio][latency]") {
  GsEfxProcessor processor(state_for(0x0400), GsEfxRealization::kModern,
                           fixed_latency_factory(0, 0, 128));
  processor.prepare(kSampleRate, 32, 2);
  // Four half-sample amp stages: two whole samples of latency, and three samples
  // of fractional-delay support each, ten of them past the floor.
  REQUIRE(processor.latency_samples() == 2);
  REQUIRE(processor.tail_samples() == 10);

  std::array<float, 1> left{{1.0f}};
  std::array<float, 1> right{{1.0f}};
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, 1);

  const int drain_samples = processor.latency_samples() + processor.tail_samples();
  REQUIRE(drain_samples == 12);
  std::vector<float> drained_left(static_cast<std::size_t>(drain_samples + 1), 0.0f);
  std::vector<float> drained_right(static_cast<std::size_t>(drain_samples + 1), 0.0f);
  for (int i = 0; i <= drain_samples; ++i) {
    float* drain_channels[] = {drained_left.data() + i, drained_right.data() + i};
    processor.process(drain_channels, 2, 1);
  }
  REQUIRE(std::fabs(drained_left[static_cast<std::size_t>(drain_samples - 1)]) > 1.0e-6f);
  REQUIRE(std::fabs(drained_right[static_cast<std::size_t>(drain_samples - 1)]) > 1.0e-6f);
  REQUIRE(std::fabs(drained_left[static_cast<std::size_t>(drain_samples)]) < 1.0e-6f);
  REQUIRE(std::fabs(drained_right[static_cast<std::size_t>(drain_samples)]) < 1.0e-6f);
}

TEST_CASE("GS EFX rejects a saturated graph before allocating compensation",
          "[midi][gs][efx][audio][latency]") {
  sonare::test::AllocationFailureGuard fail_large_allocations(1u << 20);
  sonare::ErrorCode error = sonare::ErrorCode::Ok;
  try {
    GsEfxProcessor processor(state_for(0x1100), GsEfxRealization::kModern,
                             no_alloc_latency_factory(std::numeric_limits<int>::max(), 1));
  } catch (const sonare::SonareException& exception) {
    error = exception.code();
  }
  REQUIRE(error == sonare::ErrorCode::InvalidParameter);
}

TEST_CASE("GS EFX preserves fractional child latency in Q8 metadata",
          "[midi][gs][efx][audio][latency]") {
  GsEfxProcessor processor(state_for(0x1100), GsEfxRealization::kModern,
                           fixed_latency_factory(5 * 256 + 128, 11 * 256 + 64));
  processor.prepare(kSampleRate, 32, 2);
  REQUIRE(processor.latency_samples() == 11);
  REQUIRE(processor.latency_samples_q8() == 11 * 256 + 64);
}

TEST_CASE("GS EFX fractional delay PCM matches the independent tap reference",
          "[midi][gs][efx][audio][latency]") {
  constexpr int kDelayQ8 = 7 * 256 + 96;
  constexpr std::size_t kSamples = 47;
  const std::vector<float> input_left = signal(kSamples, 0.11f);
  const std::vector<float> input_right = signal(kSamples, 0.73f);

  const auto reference_run = [&](const std::vector<float>& source_left,
                                 const std::vector<float>& source_right,
                                 const std::vector<int>& chunks) {
    std::vector<float> left_history(15, 0.0f);
    std::vector<float> right_history(15, 0.0f);
    std::size_t left_write = 0;
    std::size_t right_write = 0;
    StereoAudio output{source_left, source_right};
    std::size_t offset = 0;
    for (const int chunk : chunks) {
      REQUIRE(chunk >= 0);
      REQUIRE(offset + static_cast<std::size_t>(chunk) <= output.left.size());
      for (int i = 0; i < chunk; ++i) {
        output.left[offset + static_cast<std::size_t>(i)] = reference_fractional_delay_step(
            left_history, left_write, kDelayQ8, output.left[offset + static_cast<std::size_t>(i)]);
        output.right[offset + static_cast<std::size_t>(i)] =
            reference_fractional_delay_step(right_history, right_write, kDelayQ8,
                                            output.right[offset + static_cast<std::size_t>(i)]);
      }
      offset += static_cast<std::size_t>(chunk);
    }
    REQUIRE(offset == output.left.size());
    return output;
  };

  const auto process_run = [](Sf2EfxDelayRt& delay, const std::vector<float>& source_left,
                              const std::vector<float>& source_right,
                              const std::vector<int>& chunks) {
    StereoAudio output{source_left, source_right};
    std::size_t offset = 0;
    for (const int chunk : chunks) {
      REQUIRE(chunk >= 0);
      REQUIRE(offset + static_cast<std::size_t>(chunk) <= output.left.size());
      delay.process(output.left.data() + offset, output.right.data() + offset, chunk);
      offset += static_cast<std::size_t>(chunk);
    }
    REQUIRE(offset == output.left.size());
    return output;
  };

  Sf2EfxDelayRt delay;
  delay.prepare(kDelayQ8);
  const std::vector<int> chunks{5, 11, 7, 24};
  const StereoAudio expected = reference_run(input_left, input_right, chunks);
  const StereoAudio actual = process_run(delay, input_left, input_right, chunks);
  REQUIRE(max_difference(actual.left, expected.left) < 1.0e-6f);
  REQUIRE(max_difference(actual.right, expected.right) < 1.0e-6f);

  delay.reset();
  const StereoAudio reset_actual = process_run(delay, input_left, input_right, {47});
  const StereoAudio reset_expected = reference_run(input_left, input_right, {47});
  REQUIRE(max_difference(reset_actual.left, reset_expected.left) < 1.0e-6f);
  REQUIRE(max_difference(reset_actual.right, reset_expected.right) < 1.0e-6f);
  REQUIRE(max_difference(actual.left, reset_actual.left) < 1.0e-6f);
  REQUIRE(max_difference(actual.right, reset_actual.right) < 1.0e-6f);
}

TEST_CASE("GS EFX preserves a delay beyond the legacy alignment ceiling",
          "[midi][gs][efx][audio][latency]") {
  constexpr int kBeyondLegacyCeilingQ8 = 192000 * 256 + 128;
  GsEfxProcessor processor(state_for(0x1100), GsEfxRealization::kModern,
                           fixed_latency_factory(kBeyondLegacyCeilingQ8, 0));
  processor.prepare(kSampleRate, 32, 2);
  REQUIRE(processor.latency_samples() == (kBeyondLegacyCeilingQ8 >> 8));
  REQUIRE(processor.latency_samples_q8() == kBeyondLegacyCeilingQ8);
}

#if SONARE_BUILD_FX && defined(SONARE_WITH_MASTERING)
TEST_CASE("GS EFX real factory impulse reports its amp-sim latency",
          "[midi][gs][efx][audio][latency]") {
  const auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  const GsEfx state = state_for(0x0200);
  const auto stages = gs_efx_insert_chain(state);
  // Every stage sits on the timeline, the disabled amps included.
  int stage_latency = 0;
  for (const auto& stage : stages) {
    auto proc = factory(stage.name, stage.params_json);
    REQUIRE(proc != nullptr);
    proc->prepare(kSampleRate, 512);
    stage_latency += proc->latency_samples();
  }
  const auto peak_of = [](const std::vector<float>& left, const std::vector<float>& right) {
    int peak_index = 0;
    float peak = 0.0f;
    for (size_t sample = 0; sample < left.size(); ++sample) {
      const float level = std::max(std::fabs(left[sample]), std::fabs(right[sample]));
      if (level > peak) {
        peak = level;
        peak_index = static_cast<int>(sample);
      }
    }
    REQUIRE(peak > 1.0e-5f);
    return peak_index;
  };
  const std::size_t length = static_cast<std::size_t>(stage_latency) + 160;

  GsEfxProcessor processor(state, GsEfxRealization::kModern, factory);
  processor.prepare(kSampleRate, 512, 2);
  std::vector<float> left(length, 0.0f);
  std::vector<float> right(length, 0.0f);
  left[0] = 1.0f;
  right[0] = 1.0f;
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));

  // The enabled stages run directly and each disabled one as its own latency,
  // in chain order: a modulated stage after a delay sees the impulse later.
  std::vector<float> ref_left(length, 0.0f);
  std::vector<float> ref_right(length, 0.0f);
  ref_left[0] = 1.0f;
  ref_right[0] = 1.0f;
  float* ref_channels[] = {ref_left.data(), ref_right.data()};
  for (const auto& stage : stages) {
    auto proc = factory(stage.name, stage.params_json);
    proc->prepare(kSampleRate, static_cast<int>(length), 2);
    if (stage.enabled) {
      proc->process(ref_channels, 2, static_cast<int>(length));
      continue;
    }
    const auto delay = static_cast<std::ptrdiff_t>(proc->latency_samples());
    for (auto* channel : {&ref_left, &ref_right}) {
      std::rotate(channel->rbegin(), channel->rbegin() + delay, channel->rend());
      std::fill(channel->begin(), channel->begin() + delay, 0.0f);
    }
  }
  CAPTURE(stage_latency);
  REQUIRE(stage_latency > 0);
  REQUIRE(processor.latency_samples() == stage_latency);
  REQUIRE(peak_of(left, right) == peak_of(ref_left, ref_right));
}
#endif  // SONARE_BUILD_FX && SONARE_WITH_MASTERING

TEST_CASE("GS EFX facade and Sf2 share serial and parallel modern audio graphs",
          "[midi][gs][efx][audio]") {
  const std::vector<float> input_left = signal(257, 0.0f);
  const std::vector<float> input_right = signal(257, 0.31f);
  for (const uint16_t type : {uint16_t{0x0100}, uint16_t{0x1100}}) {
    const GsEfx state = state_for(type);
    const auto facade_probe = std::make_shared<Probe>();
    const auto shared_probe = std::make_shared<Probe>();
    const StereoAudio facade = run_facade(state, GsEfxRealization::kModern,
                                          probe_factory(facade_probe), 37, input_left, input_right);
    const StereoAudio shared = run_shared_unit(
        state, GsEfxRealization::kModern, probe_factory(shared_probe), 37, input_left, input_right);

    REQUIRE(max_difference(facade.left, shared.left) < 1.0e-6f);
    REQUIRE(max_difference(facade.right, shared.right) < 1.0e-6f);
    REQUIRE(max_difference(facade.left, input_left) > 1.0e-5f);
    REQUIRE(facade_probe->prepare_calls > 0);
    REQUIRE(facade_probe->wrong_channel_prepare_calls == 0);
    REQUIRE(shared_probe->wrong_channel_prepare_calls == 0);
  }
}

TEST_CASE("GS EFX facade supports mono folding, chunking, reset, and RT raw-byte rules",
          "[midi][gs][efx][audio]") {
  const GsEfx state = state_for(0x0100);
  const auto mono_probe = std::make_shared<Probe>();
  GsEfxProcessor mono(state, GsEfxRealization::kModern, probe_factory(mono_probe));
  mono.prepare(96000.0, 31, 1);
  const std::vector<float> input = signal(129, 0.0f);
  std::vector<float> first = input;
  float* mono_channels[] = {first.data()};
  mono.process(mono_channels, 1, static_cast<int>(first.size()));
  // The type carries the effect and the shared output tone stage in series.
  REQUIRE(mono.tail_samples() == 26);

  mono.reset();
  std::vector<float> after_reset = input;
  mono_channels[0] = after_reset.data();
  mono.process(mono_channels, 1, static_cast<int>(after_reset.size()));
  REQUIRE(max_difference(first, after_reset) < 1.0e-6f);

  const auto stereo_probe = std::make_shared<Probe>();
  GsEfxProcessor stereo(state, GsEfxRealization::kModern, probe_factory(stereo_probe));
  stereo.prepare(96000.0, 31, 2);
  std::vector<float> stereo_left = input;
  std::vector<float> stereo_right = input;
  float* stereo_channels[] = {stereo_left.data(), stereo_right.data()};
  stereo.process(stereo_channels, 2, static_cast<int>(input.size()));
  REQUIRE(max_difference(first, stereo_left) < 1.0e-6f);
  REQUIRE(max_difference(first, stereo_right) < 1.0e-6f);
  REQUIRE(mono_probe->wrong_channel_prepare_calls == 0);
  REQUIRE(stereo_probe->wrong_channel_prepare_calls == 0);

  const uint8_t old_selector = mono.state().params[0];
  REQUIRE_FALSE(mono.set_parameter(0, 2.0f));
  REQUIRE(mono.state().params[0] == old_selector);
  REQUIRE_FALSE(mono.set_parameter(1, 1.5f));
  REQUIRE(mono.state().params[1] == 69);
  REQUIRE(mono.parameter_is_realtime_safe(1));
  REQUIRE(mono.set_parameter(1, 127.0f));
  REQUIRE(mono.state().params[1] == 127);
}

TEST_CASE("GS EFX Thru refuses raw bytes in both realizations and passes audio dry",
          "[midi][gs][efx][audio][rt]") {
  for (const GsEfxRealization realization :
       {GsEfxRealization::kModern, GsEfxRealization::kClassic}) {
    GsEfxProcessor processor(state_for(0), realization, probe_factory(std::make_shared<Probe>()));
    processor.prepare(kSampleRate, 64, 2);
    const GsEfx before = processor.state();

    std::array<float, 64> left{};
    std::array<float, 64> right{};
    left.fill(0.25f);
    right.fill(-0.5f);
    const auto before_left = left;
    const auto before_right = right;
    bool byte0_set = true;
    bool byte19_set = true;
    std::size_t allocations = 0;
    {
      sonare::test::AllocationGuard guard;
      byte0_set = processor.set_parameter(0, 42.0f);
      byte19_set = processor.set_parameter(19, 99.0f);
      float* channels[] = {left.data(), right.data()};
      processor.process(channels, 2, static_cast<int>(left.size()));
      allocations = guard.count();
    }

    for (unsigned int slot = 0; slot < 20; ++slot) {
      INFO("slot " << slot);
      REQUIRE_FALSE(processor.parameter_is_realtime_safe(slot));
    }
    REQUIRE_FALSE(byte0_set);
    REQUIRE_FALSE(byte19_set);
    REQUIRE(processor.state().params == before.params);
    REQUIRE(left == before_left);
    REQUIRE(right == before_right);
    REQUIRE(allocations == 0);
  }
}

TEST_CASE("GS EFX enable-only bytes are realtime-safe and retain the fade ramp",
          "[midi][gs][efx][audio]") {
  const GsEfx state = state_for(0x0400);
  GsEfxProcessor processor(state, GsEfxRealization::kModern,
                           probe_factory(std::make_shared<Probe>()));
  processor.prepare(kSampleRate, 64, 2);
  REQUIRE(processor.parameter_is_realtime_safe(3));

  std::vector<float> input(512, 1.0f);
  std::vector<float> enabled_left = input;
  std::vector<float> enabled_right = input;
  float* channels[] = {enabled_left.data(), enabled_right.data()};
  processor.process(channels, 2, static_cast<int>(input.size()));

  REQUIRE(processor.set_parameter(3, 0.0f));
  std::vector<float> fade_out_left = {1.0f};
  std::vector<float> fade_out_right = {1.0f};
  float* first_channels[] = {fade_out_left.data(), fade_out_right.data()};
  processor.process(first_channels, 2, 1);
  std::vector<float> disabled_left = input;
  std::vector<float> disabled_right = input;
  float* disabled_channels[] = {disabled_left.data(), disabled_right.data()};
  processor.process(disabled_channels, 2, static_cast<int>(input.size()));
  REQUIRE(std::fabs(fade_out_left[0] - disabled_left.back()) > 1.0e-4f);

  REQUIRE(processor.set_parameter(3, 1.0f));
  std::vector<float> fade_in_left = {1.0f};
  std::vector<float> fade_in_right = {1.0f};
  float* second_channels[] = {fade_in_left.data(), fade_in_right.data()};
  processor.process(second_channels, 2, 1);
  std::vector<float> enabled_again_left = input;
  std::vector<float> enabled_again_right = input;
  float* enabled_again_channels[] = {enabled_again_left.data(), enabled_again_right.data()};
  processor.process(enabled_again_channels, 2, static_cast<int>(input.size()));
  REQUIRE(std::fabs(fade_in_left[0] - enabled_again_left.back()) > 1.0e-4f);
}

TEST_CASE("GS EFX restores earlier destinations when a later one refuses a byte",
          "[midi][gs][efx][audio]") {
  // Search the binding rows for a byte that reaches several stage controls, so the
  // second write can be refused after the first was taken.
  bool found = false;
  for (const auto& row : sonare::midi::synth::kGsEfxBindingRows) {
    if (found) break;
    if (gs_efx_type_defaults(row.type) == nullptr) continue;
    const auto log = std::make_shared<WriteLog>();
    GsEfxProcessor processor(state_for(row.type), GsEfxRealization::kModern,
                             [log](std::string_view, std::string_view) {
                               return std::make_unique<RecordingStage>(log);
                             });
    processor.prepare(kSampleRate, 64, 2);
    const unsigned int slot = row.slot;
    if (!processor.parameter_is_realtime_safe(slot)) continue;

    // byte_a: a byte written to two or more controls.
    int byte_a = -1;
    for (int byte = 0; byte < 128 && byte_a < 0; ++byte) {
      const int writes_before = log->writes;
      if (processor.set_parameter(slot, static_cast<float>(byte)) &&
          log->writes - writes_before >= 2) {
        byte_a = byte;
      }
    }
    if (byte_a < 0) continue;

    // byte_b: a byte that moves the first control byte_a reaches.
    const auto held_a = log->current;
    int byte_b = -1;
    for (int byte = 0; byte < 128 && byte_b < 0; ++byte) {
      const size_t first = log->accepted.size();
      if (byte == byte_a || !processor.set_parameter(slot, static_cast<float>(byte))) continue;
      const auto& [key, value] = log->accepted[first];
      if (value != held_a.at(key)) byte_b = byte;
      REQUIRE(processor.set_parameter(slot, static_cast<float>(byte_a)));
    }
    if (byte_b < 0) continue;
    REQUIRE(log->current == held_a);

    INFO("type " << row.type << " slot " << slot << " bytes " << byte_a << " -> " << byte_b);
    log->reject_write = log->writes + 1;
    REQUIRE_FALSE(processor.set_parameter(slot, static_cast<float>(byte_b)));
    CHECK(processor.state().params[slot] == byte_a);
    CHECK(log->current == held_a);
    found = true;
  }
  REQUIRE(found);
}

#if SONARE_BUILD_FX
TEST_CASE("GS EFX facade preserves the classic graph parity and validates raw state",
          "[midi][gs][efx][audio]") {
  const GsEfx state = state_for(0x0110);
  const auto facade_probe = std::make_shared<Probe>();
  const auto shared_probe = std::make_shared<Probe>();
  const StereoAudio facade =
      run_facade(state, GsEfxRealization::kClassic, probe_factory(facade_probe), 37,
                 signal(257, 0.0f), signal(257, 0.31f));
  const StereoAudio shared =
      run_shared_unit(state, GsEfxRealization::kClassic, probe_factory(shared_probe), 37,
                      signal(257, 0.0f), signal(257, 0.31f));
  REQUIRE(max_difference(facade.left, shared.left) < 1.0e-5f);
  REQUIRE(max_difference(facade.right, shared.right) < 1.0e-5f);
  REQUIRE(max_difference(facade.left, signal(257, 0.0f)) > 1.0e-5f);

  GsEfx wrong_msb = state;
  wrong_msb.type_msb = 2;
  REQUIRE_THROWS_AS(GsEfxProcessor(wrong_msb, GsEfxRealization::kClassic, {}),
                    sonare::SonareException);
  GsEfx wrong_list = state_for(0x0100);
  wrong_list.params[0] = 2;
  REQUIRE_THROWS_AS(GsEfxProcessor(wrong_list, GsEfxRealization::kModern,
                                   probe_factory(std::make_shared<Probe>())),
                    sonare::SonareException);
}
#endif  // SONARE_BUILD_FX

}  // namespace
