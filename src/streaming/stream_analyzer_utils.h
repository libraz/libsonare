#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "util/constants.h"

namespace sonare::streaming_detail {

// librosa power_to_db amin floor (1e-10); equals the generic epsilon value.
constexpr float kLogAmin = sonare::constants::kEpsilon;
constexpr float kBpmMin = 60.0f;
constexpr float kBpmMax = 200.0f;
constexpr int kMinOnsetFrames = 100;

std::vector<float> compute_bin_frequencies(int n_bins, int sr, int n_fft);
float compute_centroid_frame(const float* magnitude, int n_bins, const float* frequencies);
float compute_flatness_frame(const float* magnitude, int n_bins);
float compute_rms_frame(const float* samples, int n_fft);
float lag_to_bpm(int lag, int sr, int hop_length);
int bpm_to_lag(float bpm, int sr, int hop_length);
std::vector<float> compute_autocorrelation_streaming(const std::vector<float>& signal, int max_lag);
std::pair<float, float> find_best_tempo(const std::vector<float>& autocorr, int sr, int hop_length,
                                        float bpm_min, float bpm_max);
uint8_t quantize_to_u8(float value, float min_val, float max_val);
int16_t quantize_to_i16(float value, float min_val, float max_val);
float single_power_to_db(float power_val, float ref = 1.0f,
                         float amin = sonare::constants::kEpsilon);
int count_shared_notes(int root1, int quality1, int root2, int quality2);
bool are_chords_confusable(int root1, int quality1, int root2, int quality2);
std::array<float, 12> compute_median_chroma(const std::deque<std::array<float, 12>>& history);

}  // namespace sonare::streaming_detail
