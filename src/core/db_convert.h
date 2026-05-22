#pragma once

/// @file db_convert.h
/// @brief Standalone dB / amplitude / power conversion functions.
/// @details Mirrors librosa.power_to_db, librosa.amplitude_to_db,
///          librosa.db_to_power, librosa.db_to_amplitude. Distinct from
///          Spectrogram::to_db() which operates on a Spectrogram instance.

#include <cstddef>
#include <vector>

namespace sonare {

/// @brief Convert a power spectrogram (magnitude squared) to dB.
/// @param S Power values (>= 0 expected)
/// @param n Length of S
/// @param ref Reference value. If <= 0, the maximum of |S| is used.
/// @param amin Floor value to avoid log(0) (default 1e-10)
/// @param top_db Threshold below max(dB) to clamp (default 80.0).
///        Pass a negative value to disable clamping.
/// @return dB values, 10 * log10(max(S, amin)) - 10 * log10(max(ref, amin)).
/// @throw std::invalid_argument if amin <= 0, or n > 0 with null S.
std::vector<float> power_to_db(const float* S, std::size_t n, float ref = 1.0f, float amin = 1e-10f,
                               float top_db = 80.0f);
std::vector<float> power_to_db(const std::vector<float>& S, float ref = 1.0f, float amin = 1e-10f,
                               float top_db = 80.0f);

/// @brief Convert amplitude (magnitude) values to dB.
/// @details Internally squares the input and forwards to power_to_db with
///          amin squared, exactly matching librosa.amplitude_to_db.
/// @param amin Amplitude-domain floor (default 1e-5).
std::vector<float> amplitude_to_db(const float* S, std::size_t n, float ref = 1.0f,
                                   float amin = 1e-5f, float top_db = 80.0f);
std::vector<float> amplitude_to_db(const std::vector<float>& S, float ref = 1.0f,
                                   float amin = 1e-5f, float top_db = 80.0f);

/// @brief Inverse of power_to_db. ref * 10^(S_db / 10).
std::vector<float> db_to_power(const float* S_db, std::size_t n, float ref = 1.0f);
std::vector<float> db_to_power(const std::vector<float>& S_db, float ref = 1.0f);

/// @brief Inverse of amplitude_to_db. ref * 10^(S_db / 20).
std::vector<float> db_to_amplitude(const float* S_db, std::size_t n, float ref = 1.0f);
std::vector<float> db_to_amplitude(const std::vector<float>& S_db, float ref = 1.0f);

}  // namespace sonare
