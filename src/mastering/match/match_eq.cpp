#include "mastering/match/match_eq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "mastering/common/partitioned_convolver.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::match {
namespace {

using sonare::constants::kPiD;

float interpolate_db(const ReferenceSpectrum& spectrum, float frequency_hz) {
  if (spectrum.frequencies.empty() || spectrum.db.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrum must not be empty");
  }
  if (spectrum.frequencies.size() != spectrum.db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrum size mismatch");
  }
  if (frequency_hz <= spectrum.frequencies.front()) {
    return spectrum.db.front();
  }
  if (frequency_hz >= spectrum.frequencies.back()) {
    return spectrum.db.back();
  }

  const auto upper =
      std::upper_bound(spectrum.frequencies.begin(), spectrum.frequencies.end(), frequency_hz);
  const size_t index = static_cast<size_t>(upper - spectrum.frequencies.begin());
  const float f0 = spectrum.frequencies[index - 1];
  const float f1 = spectrum.frequencies[index];
  // Interpolate dB linearly against log-frequency so spacing matches the
  // perceptual (octave) scale rather than raw Hz.
  float t;
  if (!(f0 > 0.0f) || f1 == f0) {
    t = (frequency_hz - f0) / std::max(f1 - f0, 1.0f);
  } else {
    t = static_cast<float>(std::log(static_cast<double>(frequency_hz) / f0) /
                           std::log(static_cast<double>(f1) / f0));
  }
  return spectrum.db[index - 1] + (spectrum.db[index] - spectrum.db[index - 1]) * t;
}

bool is_power_of_two(int value) { return value > 0 && (value & (value - 1)) == 0; }

float db_to_gain(float db) { return db_to_linear(db); }

std::vector<float> smooth_log_frequency(const std::vector<float>& frequencies,
                                        const std::vector<float>& gain_db, int smoothing_bins) {
  if (smoothing_bins <= 0 || gain_db.size() <= 2) return gain_db;

  std::vector<float> smoothed(gain_db.size(), 0.0f);
  for (size_t i = 0; i < gain_db.size(); ++i) {
    const size_t begin =
        i > static_cast<size_t>(smoothing_bins) ? i - static_cast<size_t>(smoothing_bins) : 0;
    const size_t end = std::min(gain_db.size(), i + static_cast<size_t>(smoothing_bins) + 1);
    const double center = std::log(std::max(frequencies[i], 1.0f));
    const double radius =
        std::max(std::abs(std::log(std::max(frequencies[end - 1], 1.0f)) - center),
                 std::abs(center - std::log(std::max(frequencies[begin], 1.0f))));
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (size_t j = begin; j < end; ++j) {
      const double distance =
          radius > 0.0 ? std::abs(std::log(std::max(frequencies[j], 1.0f)) - center) / radius : 0.0;
      const double weight = 0.5 + 0.5 * std::cos(std::min(distance, 1.0) * kPiD);
      weighted_sum += static_cast<double>(gain_db[j]) * weight;
      weight_sum += weight;
    }
    smoothed[i] = static_cast<float>(weight_sum > 0.0 ? weighted_sum / weight_sum : gain_db[i]);
  }
  return smoothed;
}

std::vector<float> minimum_phase_kernel(const std::vector<float>& magnitude_bins, int kernel_size) {
  const size_t n_bins = magnitude_bins.size();
  const size_t n_fft = (n_bins - 1) * 2;
  FFT fft(static_cast<int>(n_fft));

  // Hermitian-symmetric, zero-phase log-magnitude spectrum (n_bins = n_fft/2+1).
  std::vector<std::complex<float>> log_magnitude_bins(n_bins);
  for (size_t k = 0; k < n_bins; ++k) {
    log_magnitude_bins[k] = {
        static_cast<float>(std::log(std::max(static_cast<double>(magnitude_bins[k]), 1e-9))), 0.0f};
  }

  // Real cepstrum: inverse of the real, symmetric log spectrum. FFT::inverse
  // applies the 1/n_fft scaling, matching the previous naive inverse DFT.
  std::vector<float> cepstrum(n_fft, 0.0f);
  fft.inverse(log_magnitude_bins.data(), cepstrum.data());

  // Fold the cepstrum so the reconstructed spectrum becomes minimum phase.
  std::vector<std::complex<float>> folded(n_fft, {0.0f, 0.0f});
  folded[0] = {cepstrum[0], 0.0f};
  for (size_t n = 1; n < n_fft / 2; ++n) {
    folded[n] = {cepstrum[n] * 2.0f, 0.0f};
  }
  folded[n_fft / 2] = {cepstrum[n_fft / 2], 0.0f};

  // Forward (unnormalized) transform of the real folded cepstrum, matching the
  // previous naive forward DFT, yields the complex (minimum-phase) log spectrum.
  std::vector<std::complex<float>> minimum_log_spectrum(n_fft);
  fft.forward_complex(folded.data(), minimum_log_spectrum.data());

  // Exponentiate. The folded cepstrum is real, so its spectrum is Hermitian
  // symmetric and the exponential preserves that symmetry; the inverse below
  // therefore produces a real impulse response.
  std::vector<std::complex<float>> minimum_spectrum_bins(n_bins);
  for (size_t k = 0; k < n_bins; ++k) {
    minimum_spectrum_bins[k] = std::exp(minimum_log_spectrum[k]);
  }
  std::vector<float> impulse(n_fft, 0.0f);
  fft.inverse(minimum_spectrum_bins.data(), impulse.data());

  std::vector<float> kernel(static_cast<size_t>(kernel_size), 0.0f);
  // Apply a decaying (half-Hann) tail taper. A minimum-phase impulse is causal
  // with its energy front-loaded, so a symmetric window (as used in the
  // zero-phase branch) would wrongly attenuate the leading energy. Tapering only
  // the tail toward zero suppresses the truncation discontinuity / ripple while
  // preserving the early response.
  for (int i = 0; i < kernel_size; ++i) {
    // Second half of a length-(2*kernel_size-1) Hann window: 1.0 at i=0 falling
    // to ~0 at i=kernel_size-1.
    const double phase = kPiD * static_cast<double>(i) / static_cast<double>(kernel_size);
    const float taper = static_cast<float>(0.5 * (1.0 + std::cos(phase)));
    kernel[static_cast<size_t>(i)] = impulse[static_cast<size_t>(i)] * taper;
  }
  return kernel;
}

int next_power_of_two(size_t value) {
  size_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return static_cast<int>(result);
}

std::vector<float> apply_fir_partitioned(const Audio& audio, const std::vector<float>& kernel,
                                         int partition_size, int latency_compensation) {
  const int block_size = partition_size > 0 ? partition_size : 256;
  common::PartitionedConvolver convolver({block_size});
  convolver.set_impulse_response(kernel);

  const size_t padded_size = ((audio.size() + kernel.size() + static_cast<size_t>(block_size) - 1) /
                              static_cast<size_t>(block_size)) *
                             static_cast<size_t>(block_size);
  std::vector<float> input(padded_size, 0.0f);
  std::copy(audio.begin(), audio.end(), input.begin());
  std::vector<float> convolved(padded_size, 0.0f);
  for (size_t offset = 0; offset < padded_size; offset += static_cast<size_t>(block_size)) {
    convolver.process_block(input.data() + offset, convolved.data() + offset);
  }

  std::vector<float> output(audio.size(), 0.0f);
  for (size_t i = 0; i < output.size(); ++i) {
    const size_t source = i + static_cast<size_t>(std::max(latency_compensation, 0));
    output[i] = source < convolved.size() ? convolved[source] : 0.0f;
  }
  return output;
}

}  // namespace

MatchEqCurve match_eq_curve(const ReferenceSpectrum& source, const ReferenceSpectrum& reference,
                            const MatchEqConfig& config) {
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f) ||
      config.smoothing_bins < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
  if (source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }
  if (source.frequencies.empty() || source.frequencies.size() != source.db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid source spectrum");
  }

  MatchEqCurve curve;
  curve.frequencies.reserve(source.frequencies.size());
  curve.gain_db.reserve(source.frequencies.size());
  for (float frequency : source.frequencies) {
    if (frequency < config.min_frequency_hz || frequency > config.max_frequency_hz) continue;
    curve.frequencies.push_back(frequency);
    curve.gain_db.push_back(
        std::clamp(interpolate_db(reference, frequency) - interpolate_db(source, frequency),
                   -config.max_gain_db, config.max_gain_db));
  }

  curve.gain_db = smooth_log_frequency(curve.frequencies, curve.gain_db, config.smoothing_bins);
  return curve;
}

std::vector<float> match_eq_fir_kernel(const MatchEqCurve& curve, int sample_rate,
                                       const MatchEqFirConfig& config) {
  if (curve.frequencies.empty() || curve.frequencies.size() != curve.gain_db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ curve");
  }
  if (sample_rate <= 0 || !is_power_of_two(config.fft_size) || config.kernel_size <= 0 ||
      config.kernel_size > config.fft_size || (config.kernel_size % 2) == 0 ||
      config.partition_size < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ FIR configuration");
  }

  FFT fft(config.fft_size);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(fft.n_bins()));
  std::vector<float> magnitude(static_cast<size_t>(fft.n_bins()), 1.0f);
  const ReferenceSpectrum curve_spectrum{curve.frequencies, curve.gain_db, sample_rate};
  for (int bin = 0; bin < fft.n_bins(); ++bin) {
    const float frequency = static_cast<float>(bin) * static_cast<float>(sample_rate) /
                            static_cast<float>(config.fft_size);
    magnitude[static_cast<size_t>(bin)] = db_to_gain(interpolate_db(curve_spectrum, frequency));
    spectrum[static_cast<size_t>(bin)] = {magnitude[static_cast<size_t>(bin)], 0.0f};
  }

  if (config.phase == MatchEqFirPhase::MinimumPhase) {
    return minimum_phase_kernel(magnitude, config.kernel_size);
  }

  std::vector<float> zero_phase(static_cast<size_t>(config.fft_size), 0.0f);
  fft.inverse(spectrum.data(), zero_phase.data());

  const size_t kernel_size = static_cast<size_t>(config.kernel_size);
  std::vector<float> kernel(kernel_size, 0.0f);
  const int half = config.kernel_size / 2;
  for (int i = 0; i < config.kernel_size; ++i) {
    const int source = (i - half + config.fft_size) % config.fft_size;
    // Symmetric Hann (periodic=false): FIR match-EQ taps need a symmetric window
    // for linear phase and unity DC gain.
    kernel[static_cast<size_t>(i)] =
        zero_phase[static_cast<size_t>(source)] * hann_value(i, config.kernel_size, false);
  }

  return kernel;
}

Audio apply_match_eq(const Audio& audio, const ReferenceSpectrum& source,
                     const ReferenceSpectrum& reference, const MatchEqConfig& match_config,
                     const MatchEqFirConfig& fir_config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (audio.sample_rate() != source.sample_rate || source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, match_config);
  const std::vector<float> kernel = match_eq_fir_kernel(curve, audio.sample_rate(), fir_config);
  const int latency_compensation =
      fir_config.phase == MatchEqFirPhase::LinearPhase ? static_cast<int>(kernel.size() / 2) : 0;
  std::vector<float> output =
      apply_fir_partitioned(audio, kernel, fir_config.partition_size, latency_compensation);
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

float estimate_reference_delay_samples(const Audio& source, const Audio& reference,
                                       int max_abs_delay) {
  if (source.empty() || reference.empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (source.sample_rate() != reference.sample_rate()) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }
  if (max_abs_delay < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "max_abs_delay must be non-negative");
  }

  const size_t length = std::min(source.size(), reference.size());
  const int clamped_max_delay = static_cast<int>(
      std::min<size_t>(static_cast<size_t>(max_abs_delay), length == 0 ? 0 : length - 1));
  if (clamped_max_delay <= 0) {
    return 0.0f;
  }

  // FFT-based cross-correlation (O(N log N)) replacing the previous
  // O(N * max_delay) brute force. With s, r zero-padded to a common power-of-two
  // size, ifft(conj(FFT(s)) * FFT(r))[m] equals sum_i s[i] * r[i + m] (mod n),
  // i.e. the unnormalized cross-correlation. We map circular lags to signed lags
  // and normalize by the global signal energies so the score is comparable to
  // the previous Pearson-style correlation.
  const int n_fft = next_power_of_two(length + static_cast<size_t>(clamped_max_delay) + 1);
  FFT fft(n_fft);

  std::vector<float> source_padded(static_cast<size_t>(n_fft), 0.0f);
  std::vector<float> reference_padded(static_cast<size_t>(n_fft), 0.0f);
  double source_energy = 0.0;
  double reference_energy = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const float s = source[i];
    const float r = reference[i];
    source_padded[i] = s;
    reference_padded[i] = r;
    source_energy += static_cast<double>(s) * s;
    reference_energy += static_cast<double>(r) * r;
  }
  if (source_energy <= 0.0 || reference_energy <= 0.0) {
    return 0.0f;
  }

  const int n_bins = fft.n_bins();
  std::vector<std::complex<float>> source_spectrum(static_cast<size_t>(n_bins));
  std::vector<std::complex<float>> reference_spectrum(static_cast<size_t>(n_bins));
  fft.forward(source_padded.data(), source_spectrum.data());
  fft.forward(reference_padded.data(), reference_spectrum.data());

  std::vector<std::complex<float>> product(static_cast<size_t>(n_bins));
  for (int bin = 0; bin < n_bins; ++bin) {
    product[static_cast<size_t>(bin)] = std::conj(source_spectrum[static_cast<size_t>(bin)]) *
                                        reference_spectrum[static_cast<size_t>(bin)];
  }
  std::vector<float> correlation(static_cast<size_t>(n_fft), 0.0f);
  fft.inverse(product.data(), correlation.data());

  const float inv_norm = static_cast<float>(1.0 / std::sqrt(source_energy * reference_energy));
  int best_lag = 0;
  float best_score = -1.0f;
  for (int lag = -clamped_max_delay; lag <= clamped_max_delay; ++lag) {
    // Positive lag => reference leads (index n_fft + ... wraps for negatives).
    const int index = lag >= 0 ? lag : n_fft + lag;
    const float score = correlation[static_cast<size_t>(index)] * inv_norm;
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  return static_cast<float>(best_lag);
}

Audio align_reference_to_source(const Audio& source, const Audio& reference, int max_abs_delay) {
  const int delay = static_cast<int>(
      std::round(estimate_reference_delay_samples(source, reference, max_abs_delay)));
  std::vector<float> aligned(source.size(), 0.0f);
  for (size_t i = 0; i < aligned.size(); ++i) {
    const int reference_index = static_cast<int>(i) + delay;
    if (reference_index >= 0 && reference_index < static_cast<int>(reference.size())) {
      aligned[i] = reference[static_cast<size_t>(reference_index)];
    }
  }
  return Audio::from_vector(std::move(aligned), source.sample_rate());
}

std::vector<eq::EqBand> match_eq_bands(const ReferenceSpectrum& source,
                                       const ReferenceSpectrum& reference,
                                       const MatchEqConfig& config) {
  if (config.max_bands == 0 || config.max_bands > eq::ParametricEq::kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ band count");
  }
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
  if (source.sample_rate != reference.sample_rate) {
    throw SonareException(ErrorCode::InvalidParameter, "sample rates must match");
  }

  const MatchEqCurve curve = match_eq_curve(source, reference, config);
  return match_eq_bands_from_curve(curve, config);
}

std::vector<eq::EqBand> match_eq_bands_from_curve(const MatchEqCurve& curve,
                                                  const MatchEqConfig& config) {
  if (config.max_bands == 0 || config.max_bands > eq::EqualizerProcessor::kMaxBands) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ band count");
  }
  if (!(config.max_gain_db >= 0.0f) || !(config.min_frequency_hz > 0.0f) ||
      !(config.max_frequency_hz > config.min_frequency_hz) || !(config.q > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ configuration");
  }
  if (curve.frequencies.size() != curve.gain_db.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid match EQ curve");
  }
  if (curve.frequencies.empty()) {
    return {};
  }

  struct Candidate {
    size_t index = 0;
    float strength = 0.0f;
    bool extrema = false;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(curve.frequencies.size());
  for (size_t i = 0; i < curve.frequencies.size(); ++i) {
    const float frequency = curve.frequencies[i];
    if (frequency < config.min_frequency_hz || frequency > config.max_frequency_hz) {
      continue;
    }
    const float gain = curve.gain_db[i];
    bool extrema = i == 0 || i + 1 == curve.gain_db.size();
    if (i > 0 && i + 1 < curve.gain_db.size()) {
      const float prev = curve.gain_db[i - 1];
      const float next = curve.gain_db[i + 1];
      extrema = (gain >= prev && gain > next) || (gain > prev && gain >= next) ||
                (gain <= prev && gain < next) || (gain < prev && gain <= next);
    }
    candidates.push_back({i, std::abs(gain), extrema});
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     if (a.extrema != b.extrema) return a.extrema > b.extrema;
                     return a.strength > b.strength;
                   });

  std::vector<size_t> selected;
  selected.reserve(config.max_bands);
  const float min_spacing_octaves =
      std::max(0.0f, std::log2(config.max_frequency_hz / config.min_frequency_hz) /
                         static_cast<float>(std::max<size_t>(config.max_bands, 1)) * 0.35f);
  for (int pass = 0; pass < 2 && selected.size() < config.max_bands; ++pass) {
    for (const auto& candidate : candidates) {
      if (selected.size() >= config.max_bands) {
        break;
      }
      if (pass == 0 && !candidate.extrema) {
        continue;
      }
      if (candidate.strength < 0.05f) {
        continue;
      }
      const float frequency = curve.frequencies[candidate.index];
      bool too_close = false;
      for (size_t existing : selected) {
        const float existing_frequency = curve.frequencies[existing];
        too_close =
            too_close || std::abs(std::log2(frequency / existing_frequency)) < min_spacing_octaves;
      }
      if (!too_close) {
        selected.push_back(candidate.index);
      }
    }
  }
  if (selected.empty()) {
    return {};
  }

  std::sort(selected.begin(), selected.end(),
            [&](size_t a, size_t b) { return curve.frequencies[a] < curve.frequencies[b]; });

  std::vector<eq::EqBand> bands;
  bands.reserve(selected.size());
  for (size_t index : selected) {
    bands.push_back({eq::EqBandType::Peak, curve.frequencies[index],
                     std::clamp(curve.gain_db[index], -config.max_gain_db, config.max_gain_db),
                     config.q, true});
  }
  return bands;
}

void configure_equalizer_from_match(eq::EqualizerProcessor& equalizer,
                                    const ReferenceSpectrum& source,
                                    const ReferenceSpectrum& reference,
                                    const MatchEqConfig& config) {
  const auto bands = match_eq_bands(source, reference, config);
  equalizer.clear();
  for (size_t i = 0; i < bands.size(); ++i) {
    equalizer.set_band(i, bands[i]);
  }
}

}  // namespace sonare::mastering::match
