#pragma once

/// @file metrics.h
/// @brief Reference-free measurements: f0, formants, spectral distance, aliasing, latency, clicks.

#include <array>
#include <cstddef>
#include <vector>

namespace sonare::voicelab {

/// @brief Per-hop f0 track in Hz; `voiced` (optional) receives the voicing decision per hop.
std::vector<float> estimate_f0_track(const float* x, std::size_t n, double sr, double hop_s,
                                     std::vector<bool>* voiced = nullptr);

/// @return Estimated F1..F3 in Hz.
std::array<float, 3> estimate_formants(const float* x, std::size_t n, double sr);

/// @brief RMS dB distance, level removed, of `test`'s harmonic levels (0-5 kHz) from
///        `reference`'s all-pole envelope stretched in frequency by `reference_warp`.
float log_spectral_distance_db(const float* reference, const float* test, std::size_t n, double sr,
                               float reference_warp);

/// @brief Energy in dB (relative to the total) that lies off the integer multiples of `f0_hz`.
float inharmonic_energy_db(const float* x, std::size_t n, double sr, float f0_hz);

/// @brief Delay of `out` against `in` in samples, by waveform or envelope cross-correlation.
int estimate_latency_samples(const float* in, const float* out, std::size_t n, int max_lag,
                             bool use_envelope);

/// @return Number of click samples.
int count_clicks(const float* x, std::size_t n, double sr);

}  // namespace sonare::voicelab
