#pragma once

/// @file sidechain_router.h
/// @brief Sidechain ducking processor with optional external detector input.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/processor_base.h"

namespace sonare::mastering::dynamics {

struct SidechainRouterConfig {
  float threshold_db = -24.0f;
  float ratio = 4.0f;
  float attack_ms = 5.0f;
  float release_ms = 100.0f;
  float range_db = 18.0f;
  bool sidechain_hpf_enabled = false;
  float sidechain_hpf_hz = 90.0f;
  bool mono_summing = false;
  bool key_listen = false;
};

class SidechainRouter : public common::ProcessorBase {
 public:
  explicit SidechainRouter(SidechainRouterConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Borrows channel pointers until the next set_sidechain(), clear_sidechain(),
  // or process() call that consumes them. The caller owns the buffers and must
  // keep them alive and unchanged for that interval.
  void set_sidechain(const float* const* channels, int num_channels, int num_samples);
  void clear_sidechain();

  void set_config(const SidechainRouterConfig& config);
  const SidechainRouterConfig& config() const { return config_; }
  float last_gain_reduction_db() const { return last_gain_reduction_db_; }

 private:
  static void validate_config(const SidechainRouterConfig& config);
  static float gain_reduction_db(float input_db, const SidechainRouterConfig& config);
  void ensure_followers(int num_channels);
  float detector_sample(float* const* channels, int channel, int sample);

  SidechainRouterConfig config_{};
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
  std::vector<common::EnvelopeFollower> followers_;
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
  float hpf_coeff_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
