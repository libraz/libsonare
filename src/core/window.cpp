/// @file window.cpp
/// @brief Implementation of window functions.

#include "core/window.h"

#include <cmath>

namespace sonare {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
}  // namespace

std::vector<float> create_window(WindowType type, int length) {
  switch (type) {
    case WindowType::Hann:
      return hann_window(length);
    case WindowType::Hamming:
      return hamming_window(length);
    case WindowType::Blackman:
      return blackman_window(length);
    case WindowType::Rectangular:
      return rectangular_window(length);
  }
  return hann_window(length);  // default
}

std::vector<float> hann_window(int length) {
  std::vector<float> window(length);
  for (int i = 0; i < length; ++i) {
    window[i] = 0.5f * (1.0f - std::cos(kTwoPi * i / (length - 1)));
  }
  return window;
}

std::vector<float> hamming_window(int length) {
  std::vector<float> window(length);
  for (int i = 0; i < length; ++i) {
    window[i] = 0.54f - 0.46f * std::cos(kTwoPi * i / (length - 1));
  }
  return window;
}

std::vector<float> blackman_window(int length) {
  std::vector<float> window(length);
  constexpr float a0 = 0.42f;
  constexpr float a1 = 0.5f;
  constexpr float a2 = 0.08f;
  for (int i = 0; i < length; ++i) {
    float t = static_cast<float>(i) / (length - 1);
    window[i] = a0 - a1 * std::cos(kTwoPi * t) + a2 * std::cos(2.0f * kTwoPi * t);
  }
  return window;
}

std::vector<float> rectangular_window(int length) { return std::vector<float>(length, 1.0f); }

}  // namespace sonare
