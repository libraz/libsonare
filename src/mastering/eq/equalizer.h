#pragma once

/// @file equalizer.h
/// @brief Unified equalizer processor entry point.

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mastering/eq/dynamic_detector.h"
#include "mastering/eq/equalizer_spectrum.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/spectrum_engine.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::mastering::eq {

struct EqualizerProcessorConfig {
  /// Widest block process() accepts. Linear-phase bands process at most two
  /// channels, so with any of them a wider block is refused.
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
  int tail_samples() const noexcept override;
  /// @brief Sets a per-band parameter addressed by an encoded @p param_id.
  /// @details The id encodes both the band and the parameter:
  ///   - `band_index = param_id / 3`
  ///   - `param_id % 3` selects the parameter:
  ///       - `0` -> frequency_hz
  ///       - `1` -> gain_db
  ///       - `2` -> Q
  /// Band type, placement (channel/mid-side), phase, enabled, soloed/bypassed
  /// and any `dynamic.*` parameters are NOT addressable through this method.
  /// Bands on the FIR backend (linear phase, or a brickwall cut) are not
  /// realtime-safe to mutate; see parameter_is_realtime_safe().
  /// @param param_id Encoded band/parameter selector (see above).
  /// @param value New parameter value.
  /// @return true if the id maps to a valid band/parameter and was applied.
  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  // Automatable parameters per band b (ids 3*b + field), keyed as
  // "band<b>.<key>": field 0=frequencyHz, 1=gainDb, 2=q.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  void set_phase_mode(PhaseMode mode);
  PhaseMode phase_mode() const noexcept { return phase_mode_; }

  void set_band(size_t index, const EqBand& band);
  /// @brief Throws exactly where set_band() would refuse @p band, changing nothing.
  void validate_band(const EqBand& band) const;
  void clear_band(size_t index);
  void clear();
  /// Borrows sidechain buffers until the next process/clear call. Dynamic bands
  /// opt into this key via DynamicParams::external_sidechain. A key with channels
  /// but fewer samples than the block (including zero) stays bound and reads as
  /// silence past its end; only zero channels unbinds it.
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

  /// @brief Composite magnitude of a band set at each requested frequency, in dB.
  /// @details The curve a caller draws. Built from the same normalization, the
  /// same tilt expansion, the same cut-slope cascade and the same coefficient
  /// design the audio path uses, so it says what the equalizer does rather than
  /// what its settings look like. Disabled, bypassed and — when anything is
  /// soloed — unsoloed bands drop out, and a soloed band is drawn as the band
  /// pass it is heard as. Linear-phase bands are included: the phase mode
  /// changes the phase and not the magnitude.
  ///
  /// @p placement selects which signal path the curve is for. A `Stereo` band is
  /// on every path, since the stereo backend runs on both channels ahead of the
  /// per-channel and mid/side ones — and being identical on left and right, it
  /// commutes with the mid/side matrix, so it is exact on those two curves as
  /// well. Bands placed on a path other than the requested one are left out; a
  /// mid band has no per-channel magnitude to fold into a left or right curve.
  ///
  /// Frequencies are clamped to [0 Hz, Nyquist]. @p out_db must have room for
  /// @p frequency_count entries.
  /// @throws SonareException on a non-positive sample rate, a null array with a
  ///         non-zero count, or a band whose own frequency is outside
  ///         (0 Hz, Nyquist) — the same bound set_band() enforces.
  static void magnitude_response_db(const EqBand* bands, size_t band_count, double sample_rate,
                                    PhaseMode global_phase, StereoPlacement placement,
                                    const float* frequencies_hz, size_t frequency_count,
                                    float* out_db);

  /// @brief The same curve for the bands this processor is holding.
  /// @details Adds what only the live processor knows: its phase mode, its gain
  /// scale, and the gain each dynamic band is currently applying — so the curve
  /// moves with the audio the way the display of one does. Static gains alone
  /// come out of the overload above.
  void magnitude_response_db(StereoPlacement placement, const float* frequencies_hz,
                             size_t frequency_count, float* out_db) const;

 private:
  static void magnitude_response_impl(const EqBand* bands, size_t band_count, double sample_rate,
                                      PhaseMode global_phase, float gain_scale,
                                      const float* dynamic_gain_db, StereoPlacement placement,
                                      const float* frequencies_hz, size_t frequency_count,
                                      float* out_db);
  static void validate_process_args(float* const* channels, int num_channels, int num_samples);
  static void validate_band_index(size_t index);
  // set_band without the refusal of a frequency the current rate cannot carry.
  void install_band(size_t index, const EqBand& band);
  static void validate_supported_band(const EqBand& band, PhaseMode global_phase);
  /// Whether @p band runs on the FIR backend: linear phase, or a brickwall cut.
  /// Selects the kernel-rebuilding set_parameter path and answers
  /// parameter_is_realtime_safe().
  static bool uses_fir_backend(const EqBand& band, PhaseMode global_phase) noexcept;
  static void validate_backend_capacity(const std::array<EqBand, kMaxBands>& bands,
                                        PhaseMode global_phase);
  static void validate_dynamic_params(const DynamicParams& dyn);
  static float dynamic_gain_delta(const EqBand& band, float detector_db, float threshold_db);
  /// Adds @p count samples (starting @p offset into the block) to the interval's detector and
  /// auto-gain input sums; must run before the filters overwrite the block.
  void accumulate_control_input(const float* const* channels, int num_channels, int offset,
                                int count);
  // @p excluded_channel (-1 for none) contributes to neither sum nor count.
  void accumulate_band_detector(size_t band_index, const float* const* channels, int num_channels,
                                int count, int excluded_channel, const EqBand& band,
                                int readable_samples);
  void clear_control_accumulators() noexcept;
  /// Turns the finished interval's sums into detector readings, thresholds, dynamic gains and the
  /// auto-gain. @return true when a non-finite cell was discarded.
  bool settle_control();
  /// Runs the filters and the output stage over one chunk of at most one control interval.
  void process_chunk(float* const* channels, int num_channels, int num_samples);
  void validate_sidechain(int expected_samples) const;
  // Applies the recursive-cell rule to the detector and gain state that survives
  // a block. O(bands x channels) and independent of block size, so the
  // per-sample input scan the realtime contract avoids is not reintroduced.
  /// @return true when any detector or gain cell was returned to its post-reset
  ///         value.
  bool discard_non_finite_dynamic_state() noexcept;
  void update_iir_bands_preserving_state(int num_samples = 0);
  void rebuild_iir(int num_samples = 0);
  /// Returns band @p index's applied dynamic gain: smoothed over @p num_samples while the band
  /// is running, held on a non-process rebuild, seeded at @p target_gain when it has no state.
  float settle_dynamic_gain(size_t index, float target_gain, int num_samples);
  static EqBand backend_band(EqBand band, PhaseMode global_phase, float gain_scale);
  void process_mono_backend(ParametricEq& backend, float* samples, int num_samples);
  void process_mono_fir(LinearPhaseEq& backend, float* samples, int num_samples);
  void apply_auto_gain(float* const* channels, int num_channels, int num_samples) noexcept;
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
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  int max_block_size_ = 0;
  // Detector-delay ring capacity (samples) preallocated in prepare().
  int max_detector_delay_samples_ = 0;
  bool prepared_ = false;
  bool has_mid_side_bands_ = false;
  bool has_dynamic_bands_ = false;
  std::array<EqBand, kMaxBands> bands_{};
  std::array<float, kMaxBands> last_band_detector_db_{};
  std::array<std::vector<BandDetectorChannel>, kMaxBands> detector_states_{};
  std::array<float, kMaxBands> last_applied_gain_db_{};
  std::array<float, kMaxBands> smoothed_gain_db_{};
  // True while smoothed_gain_db_ describes a running dynamic band.
  std::array<bool, kMaxBands> dynamic_gain_live_{};
  // False until a dynamic band has its first detector reading; that reading seeds the glide.
  std::array<bool, kMaxBands> dynamic_reading_seen_{};
  std::array<float, kMaxBands> auto_threshold_db_{};
  float last_detector_db_ = sonare::constants::kFloorDb;
  bool auto_gain_enabled_ = false;
  bool has_linear_bands_ = false;
  bool has_lr_linear_bands_ = false;
  bool has_mid_side_linear_bands_ = false;
  float last_auto_gain_db_ = 0.0f;
  float smoothed_auto_gain_db_ = 0.0f;
  // Linear gain the auto-gain applies until the next control update.
  float auto_gain_linear_ = 1.0f;
  int control_phase_ = 0;
  double detector_sum_ = 0.0;
  double detector_count_ = 0.0;
  std::array<double, kMaxBands> band_detector_sum_{};
  std::array<double, kMaxBands> band_detector_count_{};
  double auto_gain_input_sum_ = 0.0;
  double auto_gain_output_sum_ = 0.0;
  double auto_gain_count_ = 0.0;
  std::vector<float*> chunk_channels_;
  std::vector<const float*> chunk_sidechain_;
  float gain_scale_ = 1.0f;
  float output_gain_db_ = 0.0f;
  float output_pan_ = 0.0f;
  mutable std::atomic<uint32_t> spectrum_guard_{0};
  EqualizerSpectrumSnapshot spectrum_snapshot_{};
  uint64_t spectrum_seq_ = 0;
  // Produces the snapshot's frequency-domain magnitude profile. Allocates only
  // in prepare(); see EqSpectrumAnalyzer for the realtime contract.
  EqSpectrumAnalyzer spectrum_analyzer_;
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
