#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "filters/iir.h"
#include "util/constants.h"

namespace sonare::acoustic_detail {

using sonare::constants::kSqrt2;

// Energy-domain floor (1e-20), intentionally smaller than the generic
// sonare::constants::kEpsilon (1e-10); used to guard log/division of energies.
constexpr float kEnergyEpsilon = 1e-20f;

float nan_value();

struct LinearFit {
  float slope = 0.0f;
  float r2 = 0.0f;
};

struct BlindRt60Estimate {
  float rt60 = nan_value();
  float confidence = 0.0f;
  double energy = 0.0;
  float center_hz = 0.0f;
};

struct DecayEstimateCandidate {
  BlindRt60Estimate estimate;
  float score = 0.0f;
  bool from_ml = false;
};

struct FrequencyRtModel {
  bool valid = false;
  float m0 = 0.0f;
  float b = 0.0f;
  float confidence = 0.0f;
  double energy = 0.0;
  std::vector<float> centers;
};

struct FrameEnergy {
  std::vector<float> times;
  std::vector<float> energy;
  std::vector<float> db;
};

struct DecayRegion {
  size_t first = 0;
  size_t last = 0;
};

struct SampleDecayRegion {
  size_t first = 0;
  size_t last = 0;
};

std::vector<double> squared_energy(const float* samples, size_t size);
std::optional<LinearFit> fit_line(const std::vector<float>& x, const std::vector<float>& y,
                                  size_t first, size_t last);
FrameEnergy compute_frame_energy(const float* samples, size_t size, int sample_rate);
float percentile(std::vector<float> values, float q);
float percentile_nth_element(float* data, size_t count, float q);
std::vector<float> suppress_stationary_noise_spectral(const float* samples, size_t size,
                                                      int sample_rate);
float estimate_noise_floor_db(const FrameEnergy& frames);

std::vector<DecayRegion> detect_free_decay_regions(const FrameEnergy& frames, float min_decay_db,
                                                   float noise_floor_margin_db);
std::vector<SampleDecayRegion> detect_lollmann_subframe_regions(const float* samples, size_t size,
                                                                int sample_rate);
std::vector<DecayRegion> map_sample_regions_to_frames(const std::vector<SampleDecayRegion>& regions,
                                                      const FrameEnergy& frames, int sample_rate);
std::optional<BlindRt60Estimate> estimate_exponential_decay_ml(const FrameEnergy& frames,
                                                               size_t first, size_t last,
                                                               float seed_rt60);
BlindRt60Estimate aggregate_decay_candidates(std::vector<DecayEstimateCandidate> candidates);
BlindRt60Estimate estimate_blind_rt60_from_decay(const float* samples, size_t size, int sample_rate,
                                                 float min_decay_db, float noise_floor_margin_db,
                                                 bool suppress_stationary_noise = true);

double sum_range(const std::vector<double>& energy, size_t first, size_t last);
size_t direct_sound_index(const std::vector<double>& energy);

/// @brief Schroeder decay curve anchored at the direct sound and truncated at the noise floor.
///
/// Every room-acoustic parameter is defined against the direct-sound arrival:
/// the backward energy integration starts there, and the ISO 3382 early/late
/// split points (50 ms, 80 ms) are measured from it. `from_band` is the only
/// way to obtain an instance, and it locates the origin and the Lundeby
/// truncation itself, so a call site cannot compute a metric against a curve
/// that is still referenced to the container's first sample: the leading
/// silence every measured impulse response carries (converter round trip,
/// sweep deconvolution pre-delay, microphone distance) is removed once, at
/// construction, for all metrics at once.
///
/// The analysis window is the half-open range [origin, end). Every metric is
/// measured inside it, so the early and late intervals are always
/// complementary sub-intervals of one window.
class AnchoredDecay {
 public:
  /// @brief Anchors one (optionally band-filtered) signal at its direct sound.
  static AnchoredDecay from_band(const float* samples, size_t size, int sample_rate);

  /// @brief Least-squares decay time in seconds between two levels of the anchored curve.
  /// @return Seconds, or NaN when the level range holds fewer than two usable points.
  float decay_time(float upper_db, float lower_db) const;

  /// @brief Clarity in dB: early-to-late energy ratio split `boundary_sec` after the origin.
  /// @return dB, or NaN when either side of the split is unmeasurable.
  float clarity_db(float boundary_sec) const;

  /// @brief Definition D50: the fraction of window energy arriving in the first 50 ms.
  /// @return A value in [0, 1], or NaN when the window does not reach past the split.
  float definition_d50() const;

  /// @brief Direct-sound arrival index into the analysed signal.
  size_t origin() const { return origin_; }

  /// @brief Exclusive end of the analysis window; greater than `origin()` for non-empty input.
  size_t end() const { return end_; }

 private:
  AnchoredDecay() = default;

  /// @brief Early/late split index, clamped into the analysis window.
  size_t split_index(float boundary_sec) const;

  std::vector<double> energy_;  ///< Sample-wise squared energy of the analysed signal.
  std::vector<float> edc_db_;   ///< Schroeder curve in dB; index 0 is `origin_`, where it is 0 dB.
  size_t origin_ = 0;
  size_t end_ = 0;
  int sample_rate_ = 0;
};

float estimate_confidence(float rt60, float edt, float min_decay_db);
AcousticParameters analyze_band(const float* samples, size_t size, int sample_rate,
                                float min_decay_db);
std::vector<float> filter_octave_band(const Audio& ir, float center_hz);
std::vector<float> apply_biquad_dc_removed(const float* input, size_t size, float dc_offset,
                                           const BiquadCoeffs& coeffs);
std::vector<float> filter_third_octave_band(const Audio& audio, float center_hz);

double mean_square_energy(const std::vector<float>& samples);
std::vector<float> third_octave_centers(int count, int sample_rate);
std::vector<BlindRt60Estimate> estimate_third_octave_rt60(const Audio& audio,
                                                          const AcousticConfig& config);
BlindRt60Estimate weighted_subband_average(const std::vector<BlindRt60Estimate>& estimates,
                                           float min_hz, float max_hz);
float frequency_model_value(float subband_index, float m0, float b);
FrequencyRtModel fit_frequency_dependent_rt_model(const std::vector<BlindRt60Estimate>& estimates,
                                                  float min_fit_hz, float max_fit_hz);
BlindRt60Estimate estimate_from_frequency_model(const FrequencyRtModel& model, float center_hz);
BlindRt60Estimate extrapolate_low_frequency_rt60(const BlindRt60Estimate& high_band,
                                                 float center_hz);
bool looks_like_impulse_response(const Audio& audio);

}  // namespace sonare::acoustic_detail
