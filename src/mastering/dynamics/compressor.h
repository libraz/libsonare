#pragma once

/// @file compressor.h
/// @brief Feed-forward compressor with soft knee and makeup gain.

#include <memory>
#include <vector>

#include "mastering/dynamics/channel_limits.h"
#include "rt/envelope_follower.h"
#include "rt/processor_base.h"
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

class Compressor : public rt::ProcessorBase {
 public:
  explicit Compressor(CompressorConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process on the same instance:
  ///          the configuration is validated and stored in a lock-free snapshot
  ///          (see @c rt::RtPublisher), and the audio thread atomically adopts
  ///          it at the start of the next block from inside @ref process.
  ///          Derived coefficients (RMS / sidechain HPF / PDR / envelope
  ///          follower) are recomputed on the audio thread when the snapshot is
  ///          adopted, so no per-channel state member is ever written
  ///          concurrently with sample processing. May allocate (the snapshot
  ///          @c shared_ptr) and is therefore NOT realtime-safe itself; call
  ///          from the configuration thread only. Two threads MUST NOT call
  ///          @ref set_config concurrently with each other (single-producer
  ///          hand-off). Throws @c std::invalid_argument with the same rules as
  ///          the constructor; on throw the published configuration is
  ///          unchanged (validation happens before publish, never partway).
  void set_config(const CompressorConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const CompressorConfig& config() const { return config_; }
  float last_gain_reduction_db() const override { return last_gain_reduction_db_; }

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = threshold_db
  //   1 = ratio (clamped to >= 1)
  //   2 = attack_ms (clamped to >= 0)
  //   3 = release_ms (clamped to >= 0)
  //   4 = makeup_gain_db
  //
  // set_parameter mutates the control-thread mirror (config_) directly and is
  // declared RT-safe. It MUST NOT be called concurrently with set_config(); the
  // single-producer hand-off contract of RtPublisher covers either path
  // individually, not both at once.
  bool set_parameter(unsigned int param_id, float value) override;

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
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, recomputes derived coefficients. Returns a
  ///        pointer to the configuration the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (unreachable after prepare() runs because prepare()
  ///        always publishes an initial snapshot).
  const CompressorConfig* adopt_snapshot_for_block() noexcept;

  CompressorConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_->current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block. Held by @c unique_ptr so
  ///        Compressor itself remains move-constructible (RtPublisher deletes
  ///        its copy and move operations to keep the SPSC ring indices
  ///        position-stable); the indirection is touched once per block from
  ///        the audio thread and never per-sample.
  std::unique_ptr<rt::RtPublisher<CompressorConfig>> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        derived coefficients. When @c config_publisher_.current() differs
  ///        from this, the audio thread re-runs @ref update_coefficients
  ///        before processing.
  const CompressorConfig* applied_snapshot_ = nullptr;
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
  // Log-domain attack/release smoothing on the gain-reduction signal (in dB).
  sonare::rt::EnvelopeFollower reduction_smoother_;
  float last_gain_reduction_db_ = 0.0f;
};

}  // namespace sonare::mastering::dynamics
