#pragma once

/// @file dynamic_eq.h
/// @brief Level-dependent dynamic equalizer.

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "mastering/eq/parametric.h"
#include "rt/processor_base.h"

namespace sonare::mastering::eq {

struct DynamicEqBand {
  EqBandType type = EqBandType::Peak;
  float frequency_hz = 1000.0f;
  float static_gain_db = 0.0f;
  float q = 1.0f;
  float threshold_db = -24.0f;
  float ratio = 2.0f;
  float range_db = -6.0f;
  bool enabled = false;
  float sidechain_q = 1.0f;
  float sidechain_freq_hz = -1.0f;
  float attack_ms = 5.0f;
  float release_ms = 50.0f;
  float lookahead_ms = 0.0f;
};

class DynamicEq : public rt::ProcessorBase {
 public:
  static constexpr size_t kMaxBands = 8;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Number of automatable scalar parameters per band (the per-band stride used
  // by set_parameter). See set_parameter for the field order.
  static constexpr unsigned int kParamsPerBand = 11;

  void set_band(size_t index, const DynamicEqBand& band);
  void clear_band(size_t index);
  void clear();

  // Automatable parameters (RT-safe: writes the band field then recomputes the
  // affected band's gain and biquad coefficients in place via the underlying
  // ParametricEq, preserving filter and envelope/smoothing state). Parameters
  // are laid out in per-band blocks of kParamsPerBand (= 11), so band `b`
  // occupies ids `kParamsPerBand*b .. kParamsPerBand*b + 10`:
  //   +0  = frequency_hz       (clamped to (0, Nyquist))
  //   +1  = static_gain_db
  //   +2  = q                  (clamped to > 0)
  //   +3  = threshold_db
  //   +4  = ratio              (clamped to >= 1)
  //   +5  = range_db           (signed: < 0 cuts, > 0 boosts)
  //   +6  = sidechain_q        (clamped to > 0)
  //   +7  = sidechain_freq_hz  (> 0 to set; -1 follows band frequency_hz)
  //   +8  = attack_ms          (clamped to >= 0)
  //   +9  = release_ms         (clamped to >= 0)
  //   +10 = lookahead_ms       (clamped to >= 0)
  // Band type and the enabled flag are not automatable. A param change on a
  // DISABLED band updates config only and has no audible effect until the band
  // is enabled (via set_band). Ids for b >= kMaxBands are rejected (false).
  bool set_parameter(unsigned int param_id, float value) override;
  /// Borrows sidechain buffers until the next set/clear/process call.
  void set_sidechain(const float* const* channels, int num_channels, int num_samples) override;
  void clear_sidechain() override;

  const DynamicEqBand& band(size_t index) const;
  float last_detector_db() const { return last_detector_db_; }
  float last_band_detector_db(size_t index) const;
  float last_applied_gain_db(size_t index) const;

 private:
  // Granularity (in samples) at which the per-band biquad coefficients are
  // recomputed/applied during process(). The gain is smoothed at SAMPLE rate
  // (one exponential one-pole step per sample); the biquad coefficients are
  // re-derived from the smoothed gain every kCoeffUpdateInterval samples. This
  // removes the previous once-per-block gain step (audible staircase /
  // "zipper" modulation on multi-dB jumps) while keeping biquad recomputation
  // affordable: a 32-sample interval bounds the residual gain step the filter
  // sees to a fraction of the per-sample increment.
  static constexpr int kCoeffUpdateInterval = 32;
  // Skip re-applying a band's coefficients when the smoothed gain has not moved
  // by at least this many dB since the last applied value (steady state).
  static constexpr float kGainEpsilonDb = 1.0e-3f;

  // Persistent per-(band, channel) sidechain detector state. Kept across blocks
  // so the bandpass filter, envelope follower and lookahead delay all evolve
  // continuously — the detection horizon therefore crosses block boundaries
  // correctly (mirrors how dynamics/limiter.cpp persists its lookahead).
  struct DetectorBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    double process(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
    void reset() { z1 = z2 = 0.0; }
  };
  struct DetectorChannel {
    DetectorBiquad filter_a;
    DetectorBiquad filter_b;
    double envelope = 0.0;
    // Lookahead delay ring of pre-filter input samples. The detector consumes a
    // sample that is `lookahead_samples` ahead of the audio position, drawing
    // the tail from the previous block so the horizon is continuous.
    std::vector<float> look_ring;
    size_t look_pos = 0;
    void reset() {
      filter_a.reset();
      filter_b.reset();
      envelope = 0.0;
      std::fill(look_ring.begin(), look_ring.end(), 0.0f);
      look_pos = 0;
    }
  };
  struct DetectorState {
    std::vector<DetectorChannel> channels;  // one per audio/sidechain channel
    int lookahead_samples = 0;
    // Cached design inputs so coefficients/ring are only rebuilt on change.
    float frequency_hz = -1.0f;
    float sidechain_freq_hz = -2.0f;
    float sidechain_q = -1.0f;
    float attack_ms = -1.0f;
    float release_ms = -1.0f;
    float lookahead_ms = -1.0f;
    DetectorBiquad prototype;
    double attack = 1.0;
    double release = 1.0;
  };

  static void validate_index(size_t index);
  static void validate_band(const DynamicEqBand& band);
  static float detector_db(const float* const* channels, int num_channels, int num_samples);
  // Updates band `index`'s persistent detector against the supplied sidechain
  // and returns the block's detector level in dB. Stateful: advances the
  // bandpass filters, envelope and lookahead ring so the result is continuous
  // across blocks.
  float band_detector_db(const float* const* channels, int num_channels, int num_samples,
                         size_t index);
  void ensure_detector(size_t index, int num_channels);
  static float dynamic_gain_delta(const DynamicEqBand& band, float detector_db);
  void rebuild(int num_samples = 0);
  void apply_band_gain(size_t index, float gain_db);
  void validate_sidechain(int expected_samples) const;

  ParametricEq eq_;
  double sample_rate_ = 48000.0;
  bool prepared_ = false;
  std::array<DynamicEqBand, kMaxBands> bands_{};
  std::array<float, kMaxBands> last_band_detector_db_{};
  std::array<float, kMaxBands> last_applied_gain_db_{};
  std::array<float, kMaxBands> smoothed_gain_db_{};
  std::array<float, kMaxBands> target_gain_db_{};
  // Gain last committed to the underlying biquad coefficients (steady-state
  // skip reference). Seeded to NaN so the first apply always programs the band.
  std::array<float, kMaxBands> last_applied_coeff_gain_db_{};
  std::array<DetectorState, kMaxBands> detectors_{};
  std::vector<float*> sub_channels_;
  float last_detector_db_ = sonare::constants::kFloorDb;
  const float* const* sidechain_channels_ = nullptr;
  int sidechain_num_channels_ = 0;
  int sidechain_num_samples_ = 0;
};

}  // namespace sonare::mastering::eq
