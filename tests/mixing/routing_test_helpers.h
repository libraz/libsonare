/// @file routing_test_helpers.h
/// @brief Shared helpers for routed mixer tests.

#pragma once

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "graph/graph.h"
#include "mastering/eq/linear_phase.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/meter.h"
#include "rt/processor_base.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPi;

namespace {

// Integer-latency pass-through that buffers its input by `latency` samples.
// Copied from graph_test.cpp's FixedLatencyProcessor so this file is
// self-contained; used to give a ChannelStrip pre-insert a known latency that
// the routing graph must compensate.
class FixedLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit FixedLatencyProcessor(int latency) : latency_(latency) {}

  void prepare(double, int) override {
    delay_.assign(static_cast<size_t>(2 * latency_), 0.0f);
    write_index_.fill(0);
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    if (latency_ == 0) {
      return;
    }
    for (int ch = 0; ch < num_channels; ++ch) {
      const int row = std::min(ch, 1);
      float* channel_delay = delay_.data() + static_cast<size_t>(row * latency_);
      for (int i = 0; i < num_samples; ++i) {
        const float input = channels[ch][i];
        channels[ch][i] = channel_delay[static_cast<size_t>(write_index_[row])];
        channel_delay[static_cast<size_t>(write_index_[row])] = input;
        write_index_[row] = (write_index_[row] + 1) % latency_;
      }
    }
  }

  void reset() override {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    write_index_.fill(0);
  }

  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  std::array<int, 2> write_index_{};
  std::vector<float> delay_;
};

class TestStripNode final : public sonare::rt::ProcessorBase {
 public:
  TestStripNode(sonare::mixing::ChannelStrip* strip, int num_sends)
      : strip_(strip), num_sends_(num_sends) {}

  void prepare(double, int) override {}
  void reset() override { sample_pos_ = 0; }

  void process(float* const* channels, int, int num_samples) override {
    strip_->process_at(channels, 2, num_samples, sample_pos_);
    for (int s = 0; s < num_sends_; ++s) {
      float* send[2] = {channels[2 + 2 * s], channels[3 + 2 * s]};
      std::fill(send[0], send[0] + num_samples, 0.0f);
      std::fill(send[1], send[1] + num_samples, 0.0f);
      strip_->mix_send_at(static_cast<size_t>(s), send, 2, num_samples, sample_pos_);
    }
    sample_pos_ += num_samples;
  }

  int latency_samples_q8() const noexcept override { return strip_->latency_samples_q8(); }
  int output_latency_samples_q8(int output_port) const noexcept override {
    if (output_port >= 2) {
      return strip_->send_latency_samples_q8(static_cast<size_t>((output_port - 2) / 2));
    }
    return strip_->post_fader_latency_samples_q8();
  }

 private:
  sonare::mixing::ChannelStrip* strip_ = nullptr;
  int num_sends_ = 0;
  int64_t sample_pos_ = 0;
};

// Total energy across both stereo channels of a processed block.
[[maybe_unused]] double block_energy(const std::vector<float>& left,
                                     const std::vector<float>& right) {
  double sum = 0.0;
  for (size_t i = 0; i < left.size(); ++i) {
    sum += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
  }
  return sum;
}

}  // namespace

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH
