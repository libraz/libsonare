/// @file window.cpp
/// @brief Implementation of window functions.

#include "core/window.h"

#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#include "util/math_utils.h"

namespace sonare {

namespace {

/// @brief Maximum number of cached windows per thread.
constexpr size_t kMaxWindowCacheSize = 16;

/// @brief Thread-local cache for window functions.
/// @details Key includes the periodic flag so symmetric and periodic windows of
///          the same (type, length) do not collide.
thread_local std::map<std::tuple<WindowType, int, bool>, std::vector<float>> g_window_cache;

/// @brief Cosine-window divisor: `length` for periodic, `length - 1` for symmetric.
inline float window_divisor(int length, bool periodic) {
  return static_cast<float>(periodic ? length : (length - 1));
}
}  // namespace

std::vector<float> create_window(WindowType type, int length, bool periodic) {
  switch (type) {
    case WindowType::Hann:
      return hann_window(length, periodic);
    case WindowType::Hamming:
      return hamming_window(length, periodic);
    case WindowType::Blackman:
      return blackman_window(length, periodic);
    case WindowType::Rectangular:
      return rectangular_window(length);
  }
  return hann_window(length, periodic);  // default
}

const std::vector<float>& get_window_cached(WindowType type, int length, bool periodic) {
  auto key = std::make_tuple(type, length, periodic);
  auto it = g_window_cache.find(key);
  if (it != g_window_cache.end()) {
    return it->second;
  }

  /// Clear cache if it exceeds the size limit
  if (g_window_cache.size() >= kMaxWindowCacheSize) {
    g_window_cache.clear();
  }

  // Create and cache the window
  auto result = g_window_cache.emplace(key, create_window(type, length, periodic));
  return result.first->second;
}

std::vector<float> hann_window(int length, bool periodic) {
  if (length <= 1) {
    return std::vector<float>(length, 1.0f);
  }
  std::vector<float> window(length);
  for (int i = 0; i < length; ++i) {
    window[i] = hann_value(i, length, periodic);
  }
  return window;
}

float hann_value(int index, int length, bool periodic) {
  if (length <= 1) {
    return 1.0f;
  }
  // periodic=true (default): divisor `length`, matching librosa/scipy
  // get_window(fftbins=True) for STFT (preserves COLA at 50% overlap).
  // periodic=false: divisor `length - 1`, a symmetric window for linear-phase
  // FIR filter design (unity DC gain, symmetric impulse response).
  return 0.5f * (1.0f - std::cos(kTwoPi * index / window_divisor(length, periodic)));
}

std::vector<float> hamming_window(int length, bool periodic) {
  if (length <= 1) {
    return std::vector<float>(length, 1.0f);
  }
  std::vector<float> window(length);
  const float divisor = window_divisor(length, periodic);
  for (int i = 0; i < length; ++i) {
    window[i] = 0.54f - 0.46f * std::cos(kTwoPi * i / divisor);
  }
  return window;
}

std::vector<float> blackman_window(int length, bool periodic) {
  if (length <= 1) {
    return std::vector<float>(length, 1.0f);
  }
  std::vector<float> window(length);
  constexpr float a0 = 0.42f;
  constexpr float a1 = 0.5f;
  constexpr float a2 = 0.08f;
  const float divisor = window_divisor(length, periodic);
  for (int i = 0; i < length; ++i) {
    float t = static_cast<float>(i) / divisor;
    window[i] = a0 - a1 * std::cos(kTwoPi * t) + a2 * std::cos(2.0f * kTwoPi * t);
  }
  return window;
}

std::vector<float> rectangular_window(int length) { return std::vector<float>(length, 1.0f); }

}  // namespace sonare
