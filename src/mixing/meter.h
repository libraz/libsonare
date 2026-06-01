#pragma once

/// @file meter.h
/// @brief Lightweight realtime meter snapshot processor.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "rt/biquad_design.h"
#include "rt/processor_base.h"
#include "rt/true_peak_filter.h"
#include "util/constants.h"

namespace sonare::mixing {

struct MeterSnapshot {
  std::array<float, 2> peak_db{{constants::kFloorDb, constants::kFloorDb}};
  std::array<float, 2> rms_db{{constants::kFloorDb, constants::kFloorDb}};
  float correlation = 0.0f;
  float mono_compat_width = 0.0f;
  float mono_compat_peak = 0.0f;
  float mono_compat_side_rms = 0.0f;
  bool likely_mono_compatible = true;
  float momentary_lufs = constants::kFloorDb;
  float short_term_lufs = constants::kFloorDb;
  float integrated_lufs = constants::kFloorDb;
  float gain_reduction_db = 0.0f;
  std::array<float, 2> true_peak_db{{constants::kFloorDb, constants::kFloorDb}};
  float max_true_peak_db = constants::kFloorDb;
  uint64_t seq = 0;
};

/// @brief Optional configuration for the meter processor.
struct MeterConfig {
  /// @brief When false, all LUFS work is skipped and LUFS fields stay at the floor.
  bool measure_lufs = true;
  bool measure_true_peak = false;
  int true_peak_oversample = 4;
  float mono_compat_correlation_threshold = 0.0f;
};

class MeterProcessor : public rt::ProcessorBase {
 public:
  MeterProcessor() = default;
  explicit MeterProcessor(MeterConfig config) : config_(config) {}

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Clear only the integrated-loudness histogram; momentary/short-term state is kept.
  void reset_integrated() noexcept;

  /// @brief Lock-free seqlock read of the most recent snapshot (UI thread).
  MeterSnapshot snapshot() const noexcept;
  void set_gain_reduction_db(float db) noexcept;

 private:
  double filter_sample(int channel, double x) noexcept;
  float energy_to_lufs(double energy) const noexcept;
  void publish(const MeterSnapshot& next) noexcept;

  MeterConfig config_{};
  std::atomic<float> gain_reduction_db_{0.0f};
  rt::TruePeakFilter true_peak_filter_{2, 4};

  // Seqlock: odd guard_ means a write is in progress; readers retry until it is even and stable.
  alignas(64) std::atomic<uint32_t> guard_{0};
  MeterSnapshot snapshot_{};
  uint64_t seq_ = 0;

  // LUFS streaming state. The two ITU-R BS.1770 K-weighting sections are
  // evaluated in Direct Form II transposed (double precision) per channel.
  double sample_rate_ = 0.0;
  std::array<rt::BiquadStateD, 2> k_state_pre_{};
  std::array<rt::BiquadStateD, 2> k_state_rlb_{};

  // Ring buffer of per-sample combined K-weighted squared energy (sized to the short-term window).
  std::vector<double> energy_ring_;
  size_t ring_pos_ = 0;
  size_t momentary_len_ = 0;
  size_t short_term_len_ = 0;
  size_t filled_ = 0;  // samples written since reset (saturates at ring size)
  double momentary_sum_ = 0.0;
  double short_term_sum_ = 0.0;

  // Integrated gating-block accumulation.
  size_t gate_hop_ = 0;                  // 100 ms hop in samples
  size_t gate_hop_counter_ = 0;          // samples since last gating block was taken
  static constexpr int kHistBins = 750;  // -70.0 .. +5.0 LU in 0.1 LU steps
  static constexpr double kHistLowLufs = -70.0;
  static constexpr double kHistBinLu = 0.1;
  std::array<uint64_t, kHistBins> hist_count_{};
  std::array<double, kHistBins> hist_energy_{};
};

}  // namespace sonare::mixing
