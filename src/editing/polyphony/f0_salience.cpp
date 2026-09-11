#include "editing/polyphony/f0_salience.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::polyphony {
namespace {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

/// An axis wide enough for one cent over twenty-seven octaves. Past it a caller
/// is sizing an allocation rather than choosing a resolution, and whether that
/// refuses or merely stalls is the allocator's decision rather than this code's.
constexpr int kMaxAxisBins = 32768;

/// Partial 128 of the lowest F0 any usable axis reaches is already past audio,
/// so this bounds the kernel's tables without bounding a caller.
constexpr int kMaxHarmonics = 128;

/// Bins spanning [ref_hz, max_hz] inclusive at @p cents_per_bin.
int axis_bins(float ref_hz, float max_hz, float cents_per_bin) {
  const double span =
      kCentsPerOctave * std::log2(static_cast<double>(max_hz) / ref_hz) / cents_per_bin;
  SONARE_CHECK(span >= 0.0 && span < static_cast<double>(kMaxAxisBins - 1),
               ErrorCode::InvalidParameter);
  return static_cast<int>(std::ceil(span)) + 1;
}

/// One cent-axis position resolved to the two bins it falls between.
struct BinSplit {
  int lo = 0;
  float frac = 0.0f;
  bool inside = false;
};

/// The shared read and write geometry: outside the axis, and a NaN, read as
/// absent so that a partial over the ceiling contributes and removes nothing.
BinSplit split_at(const CentAxis& axis, float position) {
  BinSplit split;
  const float floor_bin = std::floor(position);
  if (!(floor_bin >= 0.0f) || floor_bin >= static_cast<float>(axis.n_bins - 1)) return split;
  split.lo = static_cast<int>(floor_bin);
  split.frac = position - floor_bin;
  split.inside = true;
  return split;
}

/// Phase residual wrapped to [-pi, pi).
double wrap_phase(double delta) {
  double wrapped = std::fmod(delta + kPiD, kTwoPiD);
  if (wrapped < 0.0) wrapped += kTwoPiD;
  return wrapped - kPiD;
}

void fill_phase(const std::complex<float>* data, int n_bins, int n_frames, int frame,
                std::vector<double>& out) {
  for (int bin = 0; bin < n_bins; ++bin) {
    const std::complex<float>& value = data[static_cast<size_t>(bin) * n_frames + frame];
    out[static_cast<size_t>(bin)] =
        std::atan2(static_cast<double>(value.imag()), static_cast<double>(value.real()));
  }
}

}  // namespace

float CentAxis::hz_at(float bin) const {
  return static_cast<float>(ref_hz *
                            std::exp2(static_cast<double>(bin) * cents_per_bin / kCentsPerOctave));
}

float CentAxis::bin_at(float hz) const {
  return static_cast<float>(kCentsPerOctave * std::log2(static_cast<double>(hz) / ref_hz) /
                            cents_per_bin);
}

std::vector<float> tonality_weights(const float* inst_freq_hz, int n_bins, float bin_hz) {
  SONARE_CHECK(n_bins >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(bin_hz) && bin_hz > 0.0f, ErrorCode::InvalidParameter);

  std::vector<float> weights(static_cast<size_t>(n_bins));
  for (int bin = 0; bin < n_bins; ++bin) {
    float slope = 0.0f;
    if (bin == 0) {
      slope = (inst_freq_hz[1] - inst_freq_hz[0]) / bin_hz;
    } else if (bin == n_bins - 1) {
      slope = (inst_freq_hz[bin] - inst_freq_hz[bin - 1]) / bin_hz;
    } else {
      slope = (inst_freq_hz[bin + 1] - inst_freq_hz[bin - 1]) / (2.0f * bin_hz);
    }
    // Signed: a falling advance reads as steady, which keeps the bins between
    // two unresolved partials in the sum. Taking the absolute value costs a
    // voice in the low register.
    weights[static_cast<size_t>(bin)] = std::clamp(1.0f - slope, 0.0f, 1.0f);
  }
  return weights;
}

const float* CentSpectrum::column(int frame) const {
  return values.data() + static_cast<size_t>(frame) * static_cast<size_t>(axis.n_bins);
}

float* CentSpectrum::column(int frame) {
  return values.data() + static_cast<size_t>(frame) * static_cast<size_t>(axis.n_bins);
}

CentSpectrum compute_cent_spectrum(const Spectrogram& spec, const CentSpectrumConfig& config) {
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.ref_hz) && config.ref_hz > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.cents_per_bin) && config.cents_per_bin >= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_hz) && config.max_hz > config.ref_hz,
               ErrorCode::InvalidParameter);
  // The frequency is a phase advance over one hop read at one bin spacing, so a
  // spectrogram carrying neither is not an input this can read.
  SONARE_CHECK(
      spec.n_bins() >= 2 && spec.n_fft() > 0 && spec.hop_length() > 0 && spec.sample_rate() > 0,
      ErrorCode::InvalidParameter);

  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const double sample_rate = spec.sample_rate();
  const double hop = spec.hop_length();
  const double bin_hz = sample_rate / spec.n_fft();

  CentSpectrum spectrum;
  spectrum.axis.ref_hz = config.ref_hz;
  spectrum.axis.cents_per_bin = config.cents_per_bin;
  spectrum.axis.n_bins = axis_bins(config.ref_hz, config.max_hz, config.cents_per_bin);
  spectrum.values.assign(static_cast<size_t>(n_frames) * spectrum.axis.n_bins, 0.0f);
  spectrum.n_frames = n_frames;
  spectrum.hop_length = spec.hop_length();
  spectrum.sample_rate = spec.sample_rate();

  const std::complex<float>* data = spec.complex_data();
  std::vector<double> previous_phase(static_cast<size_t>(n_bins));
  std::vector<double> current_phase(static_cast<size_t>(n_bins));
  fill_phase(data, n_bins, n_frames, 0, previous_phase);
  fill_phase(data, n_bins, n_frames, 1, current_phase);

  std::vector<float> inst_freq(static_cast<size_t>(n_bins));
  std::vector<float> tonality;
  for (int frame = 0; frame < n_frames; ++frame) {
    // Frame 0 has no predecessor of its own and so reads frame 1's advance.
    if (frame > 1) {
      previous_phase.swap(current_phase);
      fill_phase(data, n_bins, n_frames, frame, current_phase);
    }
    for (int bin = 0; bin < n_bins; ++bin) {
      const double centre_hz = bin * bin_hz;
      const double expected = kTwoPiD * hop * centre_hz / sample_rate;
      const double residual = wrap_phase(current_phase[static_cast<size_t>(bin)] -
                                         previous_phase[static_cast<size_t>(bin)] - expected);
      inst_freq[static_cast<size_t>(bin)] =
          static_cast<float>(centre_hz + residual * sample_rate / (kTwoPiD * hop));
    }
    if (config.use_tonality) {
      tonality = tonality_weights(inst_freq.data(), n_bins, static_cast<float>(bin_hz));
    }

    float* column = spectrum.column(frame);
    for (int bin = 0; bin < n_bins; ++bin) {
      const float freq = inst_freq[static_cast<size_t>(bin)];
      if (!(freq > 0.0f)) continue;
      float magnitude = std::abs(data[static_cast<size_t>(bin) * n_frames + frame]);
      if (config.use_tonality) magnitude *= tonality[static_cast<size_t>(bin)];
      if (!(magnitude > 0.0f)) continue;
      // Split between the two cent bins, so a partial drifting inside one moves
      // smoothly instead of snapping.
      const BinSplit split = split_at(spectrum.axis, spectrum.axis.bin_at(freq));
      if (!split.inside) continue;
      column[split.lo] += magnitude * (1.0f - split.frac);
      column[split.lo + 1] += magnitude * split.frac;
    }
  }

  return spectrum;
}

SalienceKernel::SalienceKernel(const CentAxis& spectrum_axis, const SalienceConfig& config) {
  SONARE_CHECK(spectrum_axis.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(spectrum_axis.ref_hz) && spectrum_axis.ref_hz > 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(spectrum_axis.cents_per_bin) && spectrum_axis.cents_per_bin >= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(spectrum_axis.n_bins <= kMaxAxisBins, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_harmonics > 0 && config.n_harmonics <= kMaxHarmonics,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.f0_min_hz) && std::isfinite(config.f0_max_hz) &&
                   config.f0_max_hz > config.f0_min_hz,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.f0_min_hz > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.beta_hz) && config.beta_hz > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.alpha_hz), ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.inharmonicity) && config.inharmonicity >= 0.0f,
               ErrorCode::InvalidParameter);

  spectrum_axis_ = spectrum_axis;
  // The F0 axis runs from f0_min_hz at the spectrum's own resolution, which is
  // what makes a harmonic a constant shift on both.
  f0_axis_.ref_hz = config.f0_min_hz;
  f0_axis_.cents_per_bin = spectrum_axis.cents_per_bin;
  f0_axis_.n_bins = axis_bins(config.f0_min_hz, config.f0_max_hz, spectrum_axis.cents_per_bin);
  n_harmonics_ = config.n_harmonics;

  const size_t n_entries = static_cast<size_t>(f0_axis_.n_bins) * n_harmonics_;
  partial_bins_.resize(n_entries);
  partial_weights_.resize(n_entries);
  for (int f0_bin = 0; f0_bin < f0_axis_.n_bins; ++f0_bin) {
    const double f0_hz = f0_axis_.hz_at(static_cast<float>(f0_bin));
    for (int harmonic = 1; harmonic <= n_harmonics_; ++harmonic) {
      const double ideal_hz = f0_hz * harmonic;
      const double stretch =
          std::sqrt(1.0 + static_cast<double>(config.inharmonicity) * harmonic * harmonic);
      const size_t index = static_cast<size_t>(f0_bin) * n_harmonics_ + (harmonic - 1);
      partial_bins_[index] = spectrum_axis_.bin_at(static_cast<float>(ideal_hz * stretch));
      // The weight stays on the ideal position; inharmonicity moves where the
      // partial is read, not what it is worth.
      partial_weights_[index] =
          static_cast<float>((f0_hz + config.alpha_hz) / (ideal_hz + config.beta_hz));
    }
  }
}

float SalienceKernel::partial_bin(int f0_bin, int harmonic) const {
  return partial_bins_[static_cast<size_t>(f0_bin) * n_harmonics_ + (harmonic - 1)];
}

float SalienceKernel::partial_weight(int f0_bin, int harmonic) const {
  return partial_weights_[static_cast<size_t>(f0_bin) * n_harmonics_ + (harmonic - 1)];
}

void SalienceKernel::evaluate(const float* column, float* out) const {
  for (int f0_bin = 0; f0_bin < f0_axis_.n_bins; ++f0_bin) {
    const size_t base = static_cast<size_t>(f0_bin) * n_harmonics_;
    float sum = 0.0f;
    for (int harmonic = 0; harmonic < n_harmonics_; ++harmonic) {
      const BinSplit split = split_at(spectrum_axis_, partial_bins_[base + harmonic]);
      if (!split.inside) continue;
      const float value =
          column[split.lo] * (1.0f - split.frac) + column[split.lo + 1] * split.frac;
      sum += partial_weights_[base + harmonic] * value;
    }
    out[f0_bin] = sum;
  }
}

float SalienceKernel::subtract(float* column, int f0_bin, float factor) const {
  double before = 0.0;
  for (int bin = 0; bin < spectrum_axis_.n_bins; ++bin) before += column[bin];

  // Every partial is read before any is written, so two landing in one bin read
  // the column as it stood.
  const size_t base = static_cast<size_t>(f0_bin) * n_harmonics_;
  std::vector<float> amounts(static_cast<size_t>(n_harmonics_), 0.0f);
  for (int harmonic = 0; harmonic < n_harmonics_; ++harmonic) {
    const BinSplit split = split_at(spectrum_axis_, partial_bins_[base + harmonic]);
    if (!split.inside) continue;
    // Divided by the interpolation weights' squared norm, which is what makes a
    // full-factor call remove the whole partial: writing back the value as read
    // returns 2f(1-f) of it, so one halfway between two bins would keep half.
    const float norm = (1.0f - split.frac) * (1.0f - split.frac) + split.frac * split.frac;
    amounts[static_cast<size_t>(harmonic)] =
        (column[split.lo] * (1.0f - split.frac) + column[split.lo + 1] * split.frac) * factor /
        norm;
  }
  for (int harmonic = 0; harmonic < n_harmonics_; ++harmonic) {
    const BinSplit split = split_at(spectrum_axis_, partial_bins_[base + harmonic]);
    if (!split.inside) continue;
    const float amount = amounts[static_cast<size_t>(harmonic)];
    column[split.lo] -= amount * (1.0f - split.frac);
    column[split.lo + 1] -= amount * split.frac;
  }

  double after = 0.0;
  for (int bin = 0; bin < spectrum_axis_.n_bins; ++bin) {
    column[bin] = std::max(0.0f, column[bin]);
    after += column[bin];
  }
  // The column's own drop, so the zero clamp cannot report more taken than the
  // column held.
  return static_cast<float>(std::max(0.0, before - after));
}

const float* SalienceSurface::column(int frame) const {
  return values.data() + static_cast<size_t>(frame) * static_cast<size_t>(axis.n_bins);
}

SalienceSurface compute_salience(const CentSpectrum& spectrum, const SalienceConfig& config) {
  SONARE_CHECK(spectrum.n_frames > 0 && spectrum.axis.n_bins > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(
      spectrum.values.size() == static_cast<size_t>(spectrum.n_frames) * spectrum.axis.n_bins,
      ErrorCode::InvalidParameter);

  const SalienceKernel kernel(spectrum.axis, config);

  SalienceSurface surface;
  surface.axis = kernel.f0_axis();
  surface.values.assign(static_cast<size_t>(spectrum.n_frames) * surface.axis.n_bins, 0.0f);
  surface.n_frames = spectrum.n_frames;
  surface.hop_length = spectrum.hop_length;
  surface.sample_rate = spectrum.sample_rate;

  for (int frame = 0; frame < spectrum.n_frames; ++frame) {
    kernel.evaluate(spectrum.column(frame),
                    surface.values.data() + static_cast<size_t>(frame) * surface.axis.n_bins);
  }
  return surface;
}

}  // namespace sonare::editing::polyphony
