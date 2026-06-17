#pragma once

/// @file gain.h
/// @brief Realtime-safe channel gain processor with VCA offset support.

#include <atomic>

#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

struct GainConfig {
  float gain_db = 0.0f;
  float smoothing_ms = 5.0f;
};

class GainProcessor : public rt::ProcessorBase {
 public:
  explicit GainProcessor(GainConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// Snaps the gain smoother to its current target so the next block starts at
  /// the steady-state gain instead of ramping in. Used by the engine's
  /// settle-before-offline-render pass to keep a bounce deterministic.
  void settle() noexcept;

  void set_gain_db(float gain_db) noexcept;
  float gain_db() const noexcept { return gain_db_.load(std::memory_order_relaxed); }

  // The applied VCA offset is the sum of two independent components:
  //   * a manual trim, set absolutely here (direct user/host control), and
  //   * an accumulated VCA-group contribution, mutated only by deltas.
  // Keeping them separate means a direct set_vca_offset_db() no longer stomps
  // the contributions of any VCA groups the strip belongs to, and vice versa.
  void set_vca_offset_db(float offset_db) noexcept;
  void add_vca_group_offset_db(float delta_db) noexcept;
  // Effective offset (manual trim + accumulated group contribution).
  float vca_offset_db() const noexcept;
  float vca_trim_offset_db() const noexcept;
  float vca_group_offset_db() const noexcept;

 private:
  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  rt::ParamSmoother smoother_{1.0f, 5.0f, 48000.0};
  std::atomic<float> gain_db_{0.0f};
  std::atomic<float> vca_trim_offset_db_{0.0f};
  std::atomic<float> vca_group_offset_db_{0.0f};
};

}  // namespace sonare::mixing
