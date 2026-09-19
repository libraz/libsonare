#pragma once

/// @file audio_fixtures.h
/// @brief Shared audio sample generators and spectral measurements for tests.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/fft.h"
#include "rt/processor_base.h"
#include "util/constants.h"

namespace sonare::test {

inline std::vector<float> generate_sine(int samples, float frequency_hz, int sample_rate,
                                        float amplitude = 1.0f) {
  std::vector<float> result(static_cast<std::size_t>(std::max(0, samples)));
  for (int i = 0; i < samples; ++i) {
    result[static_cast<std::size_t>(i)] =
        amplitude * static_cast<float>(std::sin(constants::kTwoPiD * frequency_hz *
                                                static_cast<double>(i) / sample_rate));
  }
  return result;
}

inline std::vector<float> generate_sine_samples(float frequency_hz, int sample_rate, int samples,
                                                float amplitude = 1.0f) {
  return generate_sine(samples, frequency_hz, sample_rate, amplitude);
}

inline Audio generate_sine_audio(float frequency_hz, int sample_rate = 22050,
                                 float duration_sec = 0.5f, float amplitude = 1.0f) {
  const int samples = static_cast<int>(static_cast<float>(sample_rate) * duration_sec);
  return Audio::from_vector(generate_sine(samples, frequency_hz, sample_rate, amplitude),
                            sample_rate);
}

inline Audio generate_sine(float frequency_hz, float duration_sec, int sample_rate = 22050,
                           float amplitude = 1.0f) {
  return generate_sine_audio(frequency_hz, sample_rate, duration_sec, amplitude);
}

/// Unit-impulse buffer: sample 0 is 1, all others 0 (empty when @p n <= 0).
inline std::vector<float> generate_impulse(int n) {
  std::vector<float> buf(static_cast<std::size_t>(std::max(0, n)), 0.0f);
  if (n > 0) buf[0] = 1.0f;
  return buf;
}

inline float peak_abs(const std::vector<float>& samples, std::size_t skip = 0) {
  float peak = 0.0f;
  for (std::size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

inline float rms(const float* samples, std::size_t size) {
  if (size == 0) return 0.0f;
  double sum = 0.0;
  for (std::size_t i = 0; i < size; ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
  }
  return static_cast<float>(std::sqrt(sum / static_cast<double>(size)));
}

inline float rms(const std::vector<float>& samples, std::size_t skip = 0) {
  const std::size_t start = std::min(skip, samples.size());
  if (start == samples.size()) return 0.0f;
  return rms(samples.data() + start, samples.size() - start);
}

inline float rms(const Audio& audio, std::size_t skip = 0) {
  const std::size_t start = std::min(skip, audio.size());
  if (start == audio.size()) return 0.0f;
  return rms(audio.data() + start, audio.size() - start);
}

inline float rms_tail(const std::vector<float>& samples, std::size_t skip) {
  return rms(samples, skip);
}

inline float max_abs_difference(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  const std::size_t count = std::min(lhs.size(), rhs.size());
  float peak = 0.0f;
  for (std::size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::abs(lhs[i] - rhs[i]));
  }
  return peak;
}

inline void process(rt::ProcessorBase& processor, std::vector<float>& mono) {
  float* channels[] = {mono.data()};
  processor.process(channels, 1, static_cast<int>(mono.size()));
}

inline void process_stereo(rt::ProcessorBase& processor, std::vector<float>& left,
                           std::vector<float>& right) {
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(std::min(left.size(), right.size())));
}

/// Default analysis sample rate shared by the spectral voice/effect tests.
constexpr double kRate = 48000.0;

/// Default FFT length for the spectral voice/effect tests. The piano test
/// overrides this locally (32768) for finer partial resolution.
constexpr int kFft = 8192;

/// Window coefficient: the 15-digit pi truncation historically baked into every
/// per-file Hann-window spectrum helper. Kept verbatim (NOT constants::kPiD, the
/// full-precision value) so this shared helper is bit-identical to the copies it
/// replaces and no golden or threshold assertion shifts.
constexpr double kHannWindowPi = 3.14159265358979;

/// Hann-windowed power spectrum (magnitude squared) of @p buf starting at
/// @p from over @p fft samples. Reads past the end of @p buf are zero-padded, so
/// a partial final window is safe (the Hann taper is ~0 at the edges, so the
/// missing tail contributes negligibly).
inline std::vector<double> power_spectrum(const std::vector<float>& buf, std::size_t from,
                                          int fft = kFft) {
  std::vector<float> windowed(static_cast<std::size_t>(fft), 0.0f);
  for (int i = 0; i < fft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kHannWindowPi * i / (fft - 1));
    const std::size_t idx = from + static_cast<std::size_t>(i);
    const float sample = idx < buf.size() ? buf[idx] : 0.0f;
    windowed[static_cast<std::size_t>(i)] = sample * static_cast<float>(w);
  }
  sonare::FFT plan(fft);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(windowed.data(), spectrum.data());
  std::vector<double> power(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) power[i] = std::norm(spectrum[i]);
  return power;
}

/// Hann-windowed magnitude spectrum (|X|) of @p buf, zero-padded past the end
/// like power_spectrum. Distinct from power_spectrum which returns |X|^2.
inline std::vector<float> spectrum_mag(const std::vector<float>& buf, std::size_t from,
                                       int fft = kFft) {
  std::vector<float> windowed(static_cast<std::size_t>(fft), 0.0f);
  for (int i = 0; i < fft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kHannWindowPi * i / (fft - 1));
    const std::size_t idx = from + static_cast<std::size_t>(i);
    const float sample = idx < buf.size() ? buf[idx] : 0.0f;
    windowed[static_cast<std::size_t>(i)] = sample * static_cast<float>(w);
  }
  sonare::FFT plan(fft);
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(plan.n_bins()));
  plan.forward(windowed.data(), spectrum.data());
  std::vector<float> mag(spectrum.size());
  for (std::size_t i = 0; i < spectrum.size(); ++i) mag[i] = std::abs(spectrum[i]);
  return mag;
}

/// Fundamental of @p buf from @p from, as the parabolic interpolation of the
/// log-power peak within +-50% of @p f0_hint. The hint is what makes this usable
/// on a voice whose partials outweigh its fundamental.
///
/// The 1e-30 guard is not kEpsilon or kSpectrumEpsilon: it floors a raw |X|^2
/// before a log, where either named epsilon would sit above real bin energy.
inline double fft_fundamental(const std::vector<float>& buf, std::size_t from, double f0_hint) {
  const std::vector<double> ps = power_spectrum(buf, from);
  const int lo = std::max(1, static_cast<int>(0.5 * f0_hint / kRate * kFft));
  const int hi =
      std::min(static_cast<int>(ps.size()) - 2, static_cast<int>(1.5 * f0_hint / kRate * kFft));
  int pk = lo;
  for (int b = lo; b <= hi; ++b)
    if (ps[static_cast<std::size_t>(b)] > ps[static_cast<std::size_t>(pk)]) pk = b;
  const double lm = std::log(ps[static_cast<std::size_t>(pk - 1)] + 1e-30);
  const double l0 = std::log(ps[static_cast<std::size_t>(pk)] + 1e-30);
  const double lp = std::log(ps[static_cast<std::size_t>(pk + 1)] + 1e-30);
  const double denom = lm - 2.0 * l0 + lp;
  const double delta = denom != 0.0 ? 0.5 * (lm - lp) / denom : 0.0;
  return (static_cast<double>(pk) + delta) * kRate / kFft;
}

/// Energy of the @p k-th harmonic of @p f0 in an already-computed @p power
/// spectrum, summed over the peak bin +-2 so a slightly mistuned partial is
/// still counted whole.
inline double harmonic_power(const std::vector<double>& power, double f0, int k) {
  const int centre = static_cast<int>(std::lround(k * f0 / kRate * kFft));
  double acc = 0.0;
  for (int b = centre - 2; b <= centre + 2; ++b) {
    if (b > 0 && b < static_cast<int>(power.size())) acc += power[static_cast<std::size_t>(b)];
  }
  return acc;
}

/// Power-weighted mean frequency of @p buf from @p from, in Hz. Bin 0 is left
/// out so a DC offset cannot pull the centroid down.
inline double spectral_centroid(const std::vector<float>& buf, std::size_t from) {
  const std::vector<double> ps = power_spectrum(buf, from);
  double num = 0.0;
  double den = 0.0;
  for (std::size_t b = 1; b < ps.size(); ++b) {
    num += static_cast<double>(b) * kRate / kFft * ps[b];
    den += ps[b];
  }
  return den > 0.0 ? num / den : 0.0;
}

}  // namespace sonare::test
