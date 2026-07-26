#pragma once

/// @file compressor.h
/// @brief Feed-forward compressor with soft knee and makeup gain.

#include <array>
#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
#include "rt/rt_config_lifecycle.h"
#include "rt/rt_publisher.h"

namespace sonare::mastering::dynamics {

enum class DetectorMode {
  Peak,
  Rms,
  LogRms,
};

struct CompressorConfig {
  float threshold_db = -18.0f;
  float ratio = 2.0f;
  float attack_ms = 10.0f;
  float release_ms = 100.0f;
  float knee_db = 0.0f;
  float makeup_gain_db = 0.0f;
  bool auto_makeup = false;
  DetectorMode detector = DetectorMode::Rms;
  bool sidechain_hpf_enabled = false;
  float sidechain_hpf_hz = 100.0f;
  float pdr_time_ms = 0.0f;
  float pdr_release_scale = 1.0f;
};

class Compressor : public rt::ProcessorBase,
                   public rt::RtConfigLifecycle<Compressor, CompressorConfig> {
  using ConfigBase = rt::RtConfigLifecycle<Compressor, CompressorConfig>;
  friend ConfigBase;

 public:
  explicit Compressor(CompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // set_config() / config() are provided by RtConfigLifecycle: set_config
  // validates (Compressor::validate_config) before publishing a lock-free
  // snapshot the audio thread adopts at the next block; config() returns the
  // control-thread mirror. Derived coefficients (RMS / sidechain HPF / PDR /
  // envelope follower) are recomputed on the audio thread when the snapshot is
  // adopted, so no per-channel state is written concurrently with processing.
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (control-thread; no audio-state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = makeup_gain_db
  //   5 = knee_db (clamped to >= 0)
  //   6 = auto_makeup (value != 0)
  //   7 = detector (0=Peak, 1=Rms, 2=LogRms)
  //   8 = sidechain_hpf_enabled (value != 0)
  //   9 = sidechain_hpf_hz (clamped to > 0)
  //  10 = pdr_time_ms (clamped to >= 0)
  //  11 = pdr_release_scale (clamped to >= 1)
  //
  // RT-safe: set_parameter updates the audio thread's live working config
  // (active_) in place and re-derives the scalar coefficients without publishing
  // a new snapshot (no allocation), so it is safe to apply from the audio
  // callback. The control-thread mirror (config_) is kept in sync so config()
  // reads back the automated state; only the snapshot publish (the allocation)
  // is dropped. MUST NOT run concurrently with set_config() (single producer).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=thresholdDb, 1=ratio, 2=attackMs, 3=releaseMs,
  // 4=makeupGainDb, 5=kneeDb, 6=autoMakeup, 7=detector, 8=sidechainHpfEnabled,
  // 9=sidechainHpfHz, 10=pdrTimeMs, 11=pdrReleaseScale.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const CompressorConfig& config);
  static float gain_reduction_db(float input_db, const CompressorConfig& config);
  /// @brief Recomputes scalar derived coefficients (RMS / sidechain HPF / PDR /
  ///        envelope follower) from @p config. RT-safe: scalar math only, no
  ///        allocation.
  /// @details Called from prepare() and — via @ref adopt_snapshot_for_block —
  ///          from the audio thread when a new configuration snapshot is
  ///          adopted between blocks.
  void update_coefficients(const CompressorConfig& config);
  void update_release_table(const CompressorConfig& config) noexcept;

  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  // RMS pre-smoothing state (for Rms / LogRms detectors). Rms = 10 ms window,
  // LogRms = 50 ms window for sustained-level estimation.
  float rms_state_ = 0.0f;
  float rms_coeff_ = 0.0f;
  float log_rms_coeff_ = 0.0f;
  // The Rms and LogRms detectors share rms_state_ but use different time
  // constants. When the detector mode changes between blocks the carried-over
  // state belongs to the wrong window, producing a spurious gain transient at
  // the switch. Track the last mode and reseed rms_state_ to the current
  // instantaneous power on a change so steady-state behaviour is unaffected.
  DetectorMode last_detector_mode_ = DetectorMode::Rms;
  bool detector_mode_initialized_ = false;
  float hpf_b0_ = 1.0f;
  float hpf_a1_ = 0.0f;
  // Per-channel sidechain HPF state, sized to the channel count on first
  // process() so stereo channels do not share (and corrupt) filter memory.
  std::vector<float> hpf_x1_;
  std::vector<float> hpf_y1_;
  float pdr_state_db_ = 0.0f;
  float pdr_coeff_ = 0.0f;
  static constexpr size_t kReleaseTableSteps = 256;
  std::array<float, kReleaseTableSteps + 1> release_coeff_table_{};
  // Log-domain attack/release smoothing on the gain-reduction signal (in dB).
  sonare::rt::EnvelopeFollower reduction_smoother_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
