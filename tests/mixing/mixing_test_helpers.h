/// @file mixing_test_helpers.h
/// @brief Shared helpers for mixing tests.

#pragma once

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/maximizer/maximizer.h"
#include "metering/lufs.h"
#include "mixing/meter.h"
#include "mixing/mixing.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPi;

namespace {
using sonare::test::rms_tail;

[[maybe_unused]] float energy(float left, float right) { return left * left + right * right; }

[[maybe_unused]] double lagrange3_magnitude_db(double fractional_delay,
                                               double normalized_to_nyquist) {
  const double mu = fractional_delay;
  const std::array<double, 4> coeffs{
      -mu * (mu - 1.0) * (mu - 2.0) / 6.0,
      (mu + 1.0) * (mu - 1.0) * (mu - 2.0) / 2.0,
      -(mu + 1.0) * mu * (mu - 2.0) / 2.0,
      (mu + 1.0) * mu * (mu - 1.0) / 6.0,
  };
  const double omega = normalized_to_nyquist * sonare::constants::kPiD;
  std::complex<double> response{0.0, 0.0};
  for (size_t tap = 0; tap < coeffs.size(); ++tap) {
    response += coeffs[tap] *
                std::exp(std::complex<double>{0.0, -omega * (static_cast<double>(tap) - 1.0)});
  }
  return 20.0 * std::log10(std::abs(response));
}

}  // namespace

class TestLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TestLatencyProcessor(int latency) : latency_(latency) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
};

class TestQ8LatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TestQ8LatencyProcessor(int latency_q8) : latency_q8_(latency_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples_q8() const noexcept override { return latency_q8_; }

 private:
  int latency_q8_ = 0;
};

class ScaleProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit ScaleProcessor(float scale) : scale_(scale) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= scale_;
      }
    }
  }
  void reset() override {}

  // Param 0 = linear scale, so insert automation can drive this processor.
  bool set_parameter(unsigned int param_id, float value) override {
    if (param_id == 0) {
      scale_ = value;
      return true;
    }
    return false;
  }

 private:
  float scale_ = 1.0f;
};

class AddProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit AddProcessor(float offset) : offset_(offset) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += offset_;
      }
    }
  }
  void reset() override {}

 private:
  float offset_ = 0.0f;
};

// Reports a gain reduction proportional to the loudest sample seen in the most
// recent process() call (GR = -10 * max|sample|). Used to verify the segmented
// process path aggregates the block-representative (most-negative) GR across
// segments rather than only reflecting the final segment's value.
class PeakGainReductionProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    float peak = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        peak = std::max(peak, std::abs(channels[ch][i]));
      }
    }
    last_gr_db_ = -10.0f * peak;
  }
  void reset() override { last_gr_db_ = 0.0f; }
  float last_gain_reduction_db() const override { return last_gr_db_; }

 private:
  float last_gr_db_ = 0.0f;
};

namespace {

// returns the final snapshot. The buffer is also captured for an optional offline check.
[[maybe_unused]] sonare::mixing::MeterSnapshot drive_meter_sine(
    sonare::mixing::MeterProcessor& meter, float amplitude, double sample_rate, double duration_sec,
    int block_size, std::vector<float>* interleaved = nullptr) {
  const int total = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> left(static_cast<size_t>(block_size));
  std::vector<float> right(static_cast<size_t>(block_size));
  float* channels[] = {left.data(), right.data()};
  if (interleaved != nullptr) {
    interleaved->clear();
    interleaved->reserve(static_cast<size_t>(total) * 2);
  }

  int n = 0;
  while (n < total) {
    const int block = std::min(block_size, total - n);
    for (int i = 0; i < block; ++i) {
      const float s =
          amplitude * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n + i) /
                               static_cast<float>(sample_rate));
      left[static_cast<size_t>(i)] = s;
      right[static_cast<size_t>(i)] = s;
      if (interleaved != nullptr) {
        interleaved->push_back(s);
        interleaved->push_back(s);
      }
    }
    meter.process(channels, 2, block);
    n += block;
  }
  return meter.snapshot();
}

}  // namespace
