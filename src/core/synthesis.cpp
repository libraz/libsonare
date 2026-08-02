/// @file synthesis.cpp
/// @brief Implementation of synthetic audio generators.

#include "core/synthesis.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/constants.h"
#include "util/exception.h"
#include "util/resource_limits.h"

namespace sonare {

using constants::kTwoPiD;

namespace {

size_t checked_sample_count(float duration, int sample_rate, const char* name) {
  const double samples = static_cast<double>(duration) * sample_rate;
  if (!std::isfinite(duration) || duration < 0.0f || samples < 0.0 ||
      samples > static_cast<double>(resource::kMaxOfflineAudioSamples)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(name) + ": duration exceeds the offline audio limit");
  }
  return static_cast<size_t>(samples);
}

}  // namespace

Audio tone(float frequency, int sr, float duration, float phi, float amplitude) {
  if (sr <= 0) throw SonareException(ErrorCode::InvalidParameter, "tone: sr must be positive");
  // Reject non-finite duration explicitly: `duration < 0` is false for NaN, so a
  // NaN (or +Inf) would slip past the sign check and cast to a huge/garbage size_t,
  // triggering a runaway allocation.
  if (!std::isfinite(duration) || duration < 0.0f)
    throw SonareException(ErrorCode::InvalidParameter,
                          "tone: duration must be finite and non-negative");

  const size_t n = checked_sample_count(duration, sr, "tone");
  std::vector<float> y(n);
  const double inv_sr = 1.0 / static_cast<double>(sr);
  const double w = kTwoPiD * static_cast<double>(frequency);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) * inv_sr;
    y[i] = static_cast<float>(static_cast<double>(amplitude) * std::sin(w * t + phi));
  }
  return Audio::from_vector(std::move(y), sr);
}

Audio chirp(float fmin, float fmax, int sr, float duration, bool linear) {
  if (sr <= 0) throw SonareException(ErrorCode::InvalidParameter, "chirp: sr must be positive");
  // See tone(): NaN/Inf duration must be rejected before the size_t cast.
  if (!std::isfinite(duration) || duration < 0.0f)
    throw SonareException(ErrorCode::InvalidParameter,
                          "chirp: duration must be finite and non-negative");

  const size_t n = checked_sample_count(duration, sr, "chirp");
  std::vector<float> y(n);
  if (n == 0) return Audio::from_vector(std::move(y), sr);

  const double inv_sr = 1.0 / static_cast<double>(sr);
  const double d = static_cast<double>(duration);
  const double f0 = static_cast<double>(fmin);
  const double f1 = static_cast<double>(fmax);

  if (linear) {
    // f(t) = f0 + beta * t, phase(t) = 2*pi*(f0 * t + 0.5 * beta * t^2)
    const double beta = (f1 - f0) / d;
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) * inv_sr;
      const double phase = kTwoPiD * (f0 * t + 0.5 * beta * t * t);
      y[i] = static_cast<float>(std::sin(phase));
    }
  } else {
    // Exponential / logarithmic sweep:
    //   f(t) = f0 * (f1/f0) ** (t/d)
    //   phase(t) = 2*pi * f0 * d / ln(f1/f0) * ((f1/f0)**(t/d) - 1)
    if (!(f0 > 0.0) || !(f1 > 0.0)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "chirp: fmin and fmax must be > 0 for exponential sweep");
    }
    const double ratio = f1 / f0;
    if (std::abs(ratio - 1.0) < 1e-12) {
      // Degenerate: pure tone at f0.
      const double w = kTwoPiD * f0;
      for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) * inv_sr;
        y[i] = static_cast<float>(std::sin(w * t));
      }
      return Audio::from_vector(std::move(y), sr);
    }
    const double log_ratio = std::log(ratio);
    const double k = f0 * d / log_ratio;
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) * inv_sr;
      const double phase = kTwoPiD * k * (std::pow(ratio, t / d) - 1.0);
      y[i] = static_cast<float>(std::sin(phase));
    }
  }
  return Audio::from_vector(std::move(y), sr);
}

Audio clicks(const std::vector<float>& times, int sr, int length, float frequency,
             float click_duration) {
  if (sr <= 0) throw SonareException(ErrorCode::InvalidParameter, "clicks: sr must be positive");
  if (!std::isfinite(click_duration) || !(click_duration > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "clicks: click_duration must be > 0");
  }
  if (!std::isfinite(frequency) || !(frequency > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "clicks: frequency must be > 0");
  }
  if (length < 0 || static_cast<size_t>(length) > resource::kMaxOfflineAudioSamples)
    throw SonareException(ErrorCode::InvalidParameter, "clicks: length must be non-negative");

  // Build the click waveform: 2**(0 .. -10) * sin(2*pi*f*n/sr)
  const size_t click_count = checked_sample_count(click_duration, sr, "clicks");
  if (click_count > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter, "clicks: click duration is too long");
  }
  const int click_n = static_cast<int>(click_count);
  if (click_n <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "clicks: click_duration too short for sr (rounds to 0 samples)");
  }
  std::vector<float> click(click_n);
  const double w = kTwoPiD * static_cast<double>(frequency) / static_cast<double>(sr);
  // logspace(0, -10, num=click_n, base=2)
  // Matches numpy.logspace: 10**linspace(start*log10(base), stop*log10(base))
  if (click_n == 1) {
    click[0] = static_cast<float>(std::sin(0.0));  // = 0
  } else if (click_n > 1) {
    const double start_exp = 0.0;
    const double stop_exp = -10.0;
    const double step = (stop_exp - start_exp) / static_cast<double>(click_n - 1);
    for (int i = 0; i < click_n; ++i) {
      const double exponent = start_exp + step * static_cast<double>(i);
      const double envelope = std::pow(2.0, exponent);
      const double s = std::sin(w * static_cast<double>(i));
      click[i] = static_cast<float>(envelope * s);
    }
  }

  // Convert times to sample positions.
  std::vector<int> positions;
  positions.reserve(times.size());
  for (float t : times) {
    if (!std::isfinite(t)) {
      throw SonareException(ErrorCode::InvalidParameter, "clicks: times must be finite");
    }
    if (t < 0.0f) continue;
    const double rounded = std::round(static_cast<double>(t) * sr);
    if (rounded > static_cast<double>(resource::kMaxOfflineAudioSamples) ||
        rounded > static_cast<double>(std::numeric_limits<int>::max())) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "clicks: time exceeds the offline audio limit");
    }
    positions.push_back(static_cast<int>(rounded));
  }

  // Determine output length.
  int out_len = length;
  if (out_len == 0) {
    int max_pos = 0;
    for (int p : positions) {
      if (p > max_pos) max_pos = p;
    }
    if (static_cast<size_t>(max_pos) > resource::kMaxOfflineAudioSamples - click_count) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "clicks: output exceeds the offline audio limit");
    }
    out_len = static_cast<int>(static_cast<size_t>(max_pos) + click_count);
  } else {
    // Filter positions past the boundary (librosa keeps positions < length).
    std::vector<int> filtered;
    filtered.reserve(positions.size());
    for (int p : positions) {
      if (p < out_len) filtered.push_back(p);
    }
    positions = std::move(filtered);
  }

  std::vector<float> out(static_cast<size_t>(std::max(out_len, 0)), 0.0f);

  for (int start : positions) {
    int end = start + click_n;
    int copy_n = click_n;
    if (end > out_len) copy_n = out_len - start;
    if (copy_n <= 0) continue;
    for (int i = 0; i < copy_n; ++i) {
      out[static_cast<size_t>(start + i)] += click[static_cast<size_t>(i)];
    }
  }

  return Audio::from_vector(std::move(out), sr);
}

}  // namespace sonare
