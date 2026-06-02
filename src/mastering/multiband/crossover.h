#pragma once

/// @file crossover.h
/// @brief Stateful multiband crossover built from matched low/high-pass filter pairs.

#include <vector>

#include "rt/biquad_design.h"
#include "util/constants.h"

namespace sonare::mastering::multiband {

enum class CrossoverSlope {
  LR2,
  LR4,
  LR8,
};

enum class CrossoverMode {
  LinkwitzRiley,
  Butterworth,
  Bessel,
  FirLinearPhase,
};

struct CrossoverConfig {
  std::vector<float> cutoffs_hz{120.0f, 2000.0f};
  CrossoverSlope slope = CrossoverSlope::LR4;
  CrossoverMode mode = CrossoverMode::LinkwitzRiley;
  int fir_kernel_size = 513;

  bool operator==(const CrossoverConfig& other) const {
    return cutoffs_hz == other.cutoffs_hz && slope == other.slope && mode == other.mode &&
           fir_kernel_size == other.fir_kernel_size;
  }
  bool operator!=(const CrossoverConfig& other) const { return !(*this == other); }
};

struct CrossoverOutput {
  std::vector<std::vector<std::vector<float>>> bands;

  int num_bands() const { return static_cast<int>(bands.size()); }
  int num_channels() const { return bands.empty() ? 0 : static_cast<int>(bands[0].size()); }
  int num_samples() const {
    return bands.empty() || bands[0].empty() ? 0 : static_cast<int>(bands[0][0].size());
  }
};

/// @brief Caller-owned scratch storage for the allocation-free split path.
///
/// Pre-allocated once (via Crossover::prepare_scratch) so that
/// Crossover::split_into() can run on the audio thread without heap traffic.
/// `bands[band][channel][sample]` holds the per-band signal; `band_channels`
/// exposes contiguous `float*` arrays per band for forwarding to sub-processors.
struct CrossoverScratch {
  std::vector<std::vector<std::vector<float>>> bands;
  std::vector<std::vector<float*>> band_channels;

  int num_bands() const { return static_cast<int>(bands.size()); }
  int num_channels() const { return bands.empty() ? 0 : static_cast<int>(bands[0].size()); }
  int capacity_samples() const {
    return bands.empty() || bands[0].empty() ? 0 : static_cast<int>(bands[0][0].size());
  }
};

class Crossover {
 public:
  explicit Crossover(CrossoverConfig config = {});

  void prepare(double sample_rate, int max_block_size);

  /// Allocating convenience path (offline/tests): returns freshly sized bands.
  CrossoverOutput split(float* const* channels, int num_channels, int num_samples);

  /// Size @p scratch for the current configuration so split_into() is
  /// allocation-free. Must be called after prepare() and whenever the channel
  /// count or band count changes. @p max_samples defaults to the prepared block
  /// size; pass a larger value to support oversized offline blocks.
  void prepare_scratch(CrossoverScratch& scratch, int num_channels, int max_samples = -1) const;

  /// Grow @p scratch in place if it cannot already hold @p num_channels /
  /// @p num_samples for the current band layout. Returns true if a reallocation
  /// occurred (i.e. the call was NOT real-time safe). When the scratch is
  /// already large enough this is allocation-free and returns false.
  bool ensure_scratch(CrossoverScratch& scratch, int num_channels, int num_samples) const;

  /// Allocation-free, lock-free split into caller-owned @p scratch. The scratch
  /// must have been sized by prepare_scratch() for at least @p num_channels and
  /// the current block size; @p num_samples must not exceed its capacity.
  void split_into(float* const* channels, int num_channels, int num_samples,
                  CrossoverScratch& scratch);

  void reset();

  void set_config(const CrossoverConfig& config);
  const CrossoverConfig& config() const { return config_; }
  int num_bands() const { return static_cast<int>(config_.cutoffs_hz.size()) + 1; }

  /// @brief Processing latency introduced by the crossover, in samples.
  /// @details Only the linear-phase FIR mode adds latency: each band is delayed
  /// by `fir_kernel_size / 2` samples so the bands stay phase-aligned. All IIR
  /// modes (Linkwitz-Riley/Butterworth/Bessel) are zero-latency and return 0.
  int latency_samples() const {
    return config_.mode == CrossoverMode::FirLinearPhase ? config_.fir_kernel_size / 2 : 0;
  }

 private:
  using Biquad = rt::BiquadState;

  static void validate_config(const CrossoverConfig& config, double sample_rate = 0.0);
  static int filter_order(CrossoverSlope slope, CrossoverMode mode);
  struct FilterSection {
    double lowpass_frequency_scale = 1.0;
    double highpass_frequency_scale = 1.0;
    double q = sonare::constants::kButterworthQD;
  };
  static std::vector<FilterSection> filter_sections(CrossoverSlope slope, CrossoverMode mode);
  void rebuild_state(int num_channels);
  void install_coefficients();
  // Core block processing into a caller-provided band buffer. Returns nothing;
  // writes bands[band][channel][0..num_samples). Allocation-free given that
  // filter/FIR state already matches num_channels.
  void process_block_iir(float* const* channels, int num_channels, int num_samples,
                         std::vector<std::vector<std::vector<float>>>& out_bands);
  void process_block_fir(float* const* channels, int num_channels, int num_samples,
                         std::vector<std::vector<std::vector<float>>>& out_bands);
  void rebuild_fir_state(int num_channels);
  void rebuild_fir_kernels();
  float process_fir_lowpass(float sample, int split_index, int channel);
  float process_fir_delay(float sample, int channel);
  float lowpass(float sample, int split_index, int channel);
  float highpass(float sample, int split_index, int channel);
  float allpass(float sample, int band_index, int split_index, int channel);

  struct SplitChannelState {
    std::vector<Biquad> lowpass;
    std::vector<Biquad> highpass;
  };

  struct CompensationChannelState {
    std::vector<std::vector<Biquad>> allpass_by_split;
  };

  CrossoverConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  // Set when coefficients must be recomputed (config/sample-rate change). The
  // per-block split path only reinstalls coefficients when this is set or when
  // the channel count changed, so steady-state processing is allocation-free.
  bool coeffs_dirty_ = true;
  // Cached per-split biquad stage count. filter_sections() builds temporary
  // vectors, so it is evaluated only on a config/sample-rate change (when
  // coeffs_dirty_ is set) and reused on every steady-state block to keep the
  // audio-thread split path allocation-free.
  size_t cached_section_count_ = 0;
  std::vector<std::vector<SplitChannelState>> states_;
  std::vector<std::vector<CompensationChannelState>> compensation_states_;
  std::vector<std::vector<float>> fir_kernels_;
  std::vector<std::vector<std::vector<float>>> fir_history_;
  std::vector<std::vector<size_t>> fir_history_index_;
  std::vector<std::vector<float>> fir_delay_history_;
  std::vector<size_t> fir_delay_index_;
};

}  // namespace sonare::mastering::multiband
