#pragma once

/// @file sidechain_router.h
/// @brief Sidechain ducking processor with optional external detector input.

#include <vector>

#include "mastering/common/envelope_follower.h"
#include "mastering/common/lookahead_buffer.h"
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
  float lookahead_ms = 0.0f;
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
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override;
  void clear_sidechain() override;

  void set_config(const SidechainRouterConfig& config);
  const SidechainRouterConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = range_db (clamped to >= 0)
  // lookahead_ms and the sidechain HPF settings are omitted because they resize
  // buffers or are gated by mode switches.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const SidechainRouterConfig& config);
  static float gain_reduction_db(float input_db, const SidechainRouterConfig& config);
  void ensure_followers(int num_channels);
  void ensure_lookahead(int num_channels);
  void ensure_hpf_state(int num_channels);
  float detector_sample(float* const* channels, int channel, int sample);

  SidechainRouterConfig config_{};
  double sample_rate_ = 48000.0;
  int lookahead_samples_ = 0;
  bool prepared_ = false;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
  std::vector<common::EnvelopeFollower> followers_;
  std::vector<common::LookaheadBuffer> lookahead_;
  std::vector<common::LookaheadBuffer> gain_lookahead_;
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
  float hpf_coeff_ = 0.0f;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
