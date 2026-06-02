#pragma once

/// @file equalizer.h
/// @brief Unified equalizer processor entry point.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mastering/eq/linear_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/spectrum_engine.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

struct EqualizerProcessorConfig {
  int max_channels = 2;
  uint64_t spectrum_instance_id = 0;
  LinearPhaseEqConfig linear_phase_config{};
};

class EqualizerProcessor : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = 24;

  explicit EqualizerProcessor(EqualizerProcessorConfig config = {});
  ~EqualizerProcessor() override;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  /// @brief Sets a per-band parameter addressed by an encoded @p param_id.
  /// @details The id encodes both the band and the parameter:
  ///   - `band_index = param_id / 3`
  ///   - `param_id % 3` selects the parameter:
  ///       - `0` -> frequency_hz
  ///       - `1` -> gain_db
  ///       - `2` -> Q
  /// Band type, placement (channel/mid-side), phase, enabled, soloed/bypassed
  /// and any `dynamic.*` parameters are NOT addressable through this method.
  /// Linear-phase bands are not realtime-safe to mutate; see
  /// parameter_is_realtime_safe().
  /// @param param_id Encoded band/parameter selector (see above).
  /// @param value New parameter value.
  /// @return true if the id maps to a valid band/parameter and was applied.
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

  void set_phase_mode(PhaseMode mode);
  PhaseMode phase_mode() const noexcept { return phase_mode_; }

  void set_band(size_t index, const EqBand& band);
  void clear_band(size_t index);
  void clear();
  /// Borrows sidechain buffers until the next process/clear call. Dynamic bands
  /// opt into this key via DynamicParams::external_sidechain.
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override;
  void clear_sidechain() override;

  const EqBand& band(size_t index) const;
  float last_detector_db() const noexcept { return last_detector_db_; }
  float last_band_detector_db(size_t index) const;
  float last_applied_gain_db(size_t index) const;
  void set_auto_gain_enabled(bool enabled) noexcept { auto_gain_enabled_ = enabled; }
  bool auto_gain_enabled() const noexcept { return auto_gain_enabled_; }
  float last_auto_gain_db() const noexcept { return last_auto_gain_db_; }
  void set_gain_scale(float scale);
  float gain_scale() const noexcept { return gain_scale_; }
  void set_output_gain_db(float gain_db);
  float output_gain_db() const noexcept { return output_gain_db_; }
  void set_output_pan(float pan);
  float output_pan() const noexcept { return output_pan_; }
  EqualizerSpectrumSnapshot spectrum_snapshot() const noexcept;

 private:
  static void validate_process_args(float* const* channels, int num_channels, int num_samples);
  static void validate_band_index(size_t index);
  static void validate_supported_band(const EqBand& band, PhaseMode global_phase);
  static void validate_backend_capacity(const std::array<EqBand, kMaxBands>& bands,
                                        PhaseMode global_phase);
  static void validate_dynamic_params(const DynamicParams& dyn);
  static float detector_db(const float* const* channels, int num_channels, int num_samples);
  // Per-band, per-channel detector filter and envelope state that persists across
  // process() blocks. Allocating fresh state per block (the previous behaviour)
  // re-rang the bandpass filter and reset the envelope at every block boundary,
  // making the dynamic-band detector dependent on block size and producing
  // discontinuities. State is preallocated in prepare() and cleared in reset().
  struct DetectorState {
    double filter_a_z1 = 0.0;
    double filter_a_z2 = 0.0;
    double filter_b_z1 = 0.0;
    double filter_b_z2 = 0.0;
    double envelope = 0.0;
    // Lookahead delay ring of pre-filter input samples. Preallocated in prepare()
    // to the maximum supported lookahead; only the first look_size entries form
    // the live FIFO, so the detector consumes a CONTINUOUS stream across blocks
    // instead of repeating the final sample for the last look_size positions of
    // every block (which systematically under-detected at each boundary).
    std::vector<float> look_ring;
    size_t look_size = 0;
    size_t look_pos = 0;
  };
  // Maximum per-band detector lookahead the detector ring is preallocated for.
  // Lookahead past this bound saturates so the audio-thread path never resizes.
  static constexpr float kMaxDetectorLookaheadMs = 20.0f;
  float band_detector_db(size_t band_index, const float* const* channels, int num_channels,
                         int num_samples, double sample_rate, const EqBand& band);
  static float rms_db(const float* const* channels, int num_channels, int num_samples) noexcept;
  static float dynamic_gain_delta(const EqBand& band, float detector_db, float threshold_db);
  void update_dynamic_state(const float* const* channels, int num_channels, int num_samples);
  void validate_sidechain(int expected_samples) const;
  void update_iir_bands_preserving_state(int num_samples = 0);
  void rebuild_iir(int num_samples = 0);
  static EqBand backend_band(EqBand band, PhaseMode global_phase, float gain_scale);
  void process_mono_backend(ParametricEq& backend, float* samples, int num_samples);
  void process_mono_fir(LinearPhaseEq& backend, float* samples, int num_samples);
  void apply_auto_gain(float* const* channels, int num_channels, int num_samples,
                       float input_db) noexcept;
  void apply_output_gain_and_pan(float* const* channels, int num_channels,
                                 int num_samples) noexcept;
  static void capture_stream(const float* const* channels, int num_channels, int num_samples,
                             std::array<SpectrumPoint, kSpectrumStreamCapacity>& stream,
                             size_t& count) noexcept;
  void publish_spectrum_snapshot(const EqualizerSpectrumSnapshot& pre_snapshot,
                                 const float* const* channels, int num_channels,
                                 int num_samples) noexcept;

  EqualizerProcessorConfig config_{};
  PhaseMode phase_mode_ = PhaseMode::ZeroLatency;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  // Detector lookahead ring capacity (samples) preallocated in prepare().
  int max_detector_lookahead_samples_ = 0;
  bool prepared_ = false;
  bool has_mid_side_bands_ = false;
  bool has_dynamic_bands_ = false;
  std::array<EqBand, kMaxBands> bands_{};
  std::array<float, kMaxBands> last_band_detector_db_{};
  std::array<std::vector<DetectorState>, kMaxBands> detector_states_{};
  std::array<float, kMaxBands> last_applied_gain_db_{};
  std::array<float, kMaxBands> smoothed_gain_db_{};
  std::array<float, kMaxBands> auto_threshold_db_{};
  float last_detector_db_ = sonare::constants::kFloorDb;
  bool auto_gain_enabled_ = false;
  bool has_linear_bands_ = false;
  bool has_lr_linear_bands_ = false;
  bool has_mid_side_linear_bands_ = false;
  float last_auto_gain_db_ = 0.0f;
  float smoothed_auto_gain_db_ = 0.0f;
  float gain_scale_ = 1.0f;
  float output_gain_db_ = 0.0f;
  float output_pan_ = 0.0f;
  mutable std::atomic<uint32_t> spectrum_guard_{0};
  EqualizerSpectrumSnapshot spectrum_snapshot_{};
  uint64_t spectrum_seq_ = 0;
  ParametricEq stereo_iir_;
  ParametricEq left_iir_;
  ParametricEq right_iir_;
  ParametricEq mid_iir_;
  ParametricEq side_iir_;
  LinearPhaseEq left_channel_fir_;
  LinearPhaseEq right_channel_fir_;
  LinearPhaseEq mid_fir_;
  LinearPhaseEq side_fir_;
  std::vector<float> mid_buffer_;
  std::vector<float> side_buffer_;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
};

}  // namespace sonare::mastering::eq
