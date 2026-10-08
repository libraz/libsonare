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

/// @brief Largest sample difference between two copies of a processor fed the same
///        stereo signal block by block, where @p touch is applied to the second
///        copy halfway through.
/// @details @p make returns a prepared processor; @p touch is what must leave its
///          running state alone, so a correct processor returns exactly 0.
template <typename Make, typename Touch>
float divergence_after(Make&& make, Touch&& touch, int sample_rate = 48000, int block = 256,
                       int blocks = 40) {
  auto control = make();
  auto touched = make();
  float worst = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    if (b == blocks / 2) touch(*touched);
    std::vector<float> l1(static_cast<size_t>(block));
    std::vector<float> r1(static_cast<size_t>(block));
    for (int i = 0; i < block; ++i) {
      const double t = static_cast<double>(b * block + i) / sample_rate;
      l1[static_cast<size_t>(i)] =
          static_cast<float>(0.4 * std::sin(constants::kTwoPiD * 110.0 * t) +
                             0.3 * std::sin(constants::kTwoPiD * 2300.0 * t));
      r1[static_cast<size_t>(i)] =
          static_cast<float>(0.3 * std::sin(constants::kTwoPiD * 330.0 * t) +
                             0.3 * std::sin(constants::kTwoPiD * 7100.0 * t));
    }
    std::vector<float> l2 = l1;
    std::vector<float> r2 = r1;
    process_stereo(*control, l1, r1);
    process_stereo(*touched, l2, r2);
    worst = std::max(worst, std::max(max_abs_difference(l1, l2), max_abs_difference(r1, r2)));
  }
  return worst;
}

/// @brief How far a processor's other planes move when its detector-excluded plane
///        goes from silence to full scale, over a 6-plane block stream.
/// @details Two copies built by @p make get plane 3 excluded; one hears a silent
///          plane 3, the other a full-scale one. Returns the largest difference
///          across the remaining planes' output and the reported gain reduction,
///          which a processor honouring the exclusion keeps at exactly 0.
template <typename Make>
float excluded_plane_influence(Make&& make, int sample_rate = 48000, int block = 256,
                               int blocks = 40) {
  constexpr int kPlanes = 6;
  constexpr int kExcluded = 3;
  auto quiet = make();
  auto loud = make();
  quiet->set_detector_excluded_channel(kExcluded);
  loud->set_detector_excluded_channel(kExcluded);
  float worst = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    std::vector<std::vector<float>> a(kPlanes, std::vector<float>(static_cast<size_t>(block)));
    for (int ch = 0; ch < kPlanes; ++ch) {
      for (int i = 0; i < block; ++i) {
        const double t = static_cast<double>(b * block + i) / sample_rate;
        a[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
            ch == kExcluded ? 0.0f
                            : static_cast<float>(
                                  0.05 * std::sin(constants::kTwoPiD * (220.0 + 110.0 * ch) * t));
      }
    }
    auto c = a;
    for (int i = 0; i < block; ++i) {
      const double t = static_cast<double>(b * block + i) / sample_rate;
      c[kExcluded][static_cast<size_t>(i)] =
          static_cast<float>(0.95 * std::sin(constants::kTwoPiD * 60.0 * t));
    }
    float* pa[kPlanes];
    float* pc[kPlanes];
    for (int ch = 0; ch < kPlanes; ++ch) {
      pa[ch] = a[static_cast<size_t>(ch)].data();
      pc[ch] = c[static_cast<size_t>(ch)].data();
    }
    quiet->process(pa, kPlanes, block);
    loud->process(pc, kPlanes, block);
    for (int ch = 0; ch < kPlanes; ++ch) {
      if (ch == kExcluded) continue;
      worst = std::max(worst,
                       max_abs_difference(a[static_cast<size_t>(ch)], c[static_cast<size_t>(ch)]));
    }
    worst =
        std::max(worst, std::abs(quiet->last_gain_reduction_db() - loud->last_gain_reduction_db()));
  }
  return worst;
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
