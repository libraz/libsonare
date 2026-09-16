#include "mastering/repair/denoise_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_profile.h"
#include "mastering/common/noise_tracker.h"
#include "mastering/common/stft_stream.h"
#include "mastering/repair/denoise_internal.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/non_finite_state.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

using sonare::discard_run_if_non_finite;

namespace detail {

MedianGainSmoother::MedianGainSmoother(int bins)
    : bins_(std::max(bins, 0)),
      raw_(3, std::vector<double>(static_cast<size_t>(std::max(bins, 0)), 1.0)),
      out_(static_cast<size_t>(std::max(bins, 0)), 1.0) {
  window_.reserve(9);
}

double* MedianGainSmoother::slot(int frame) { return raw_[static_cast<size_t>(frame % 3)].data(); }

const double* MedianGainSmoother::push(const double* raw_frame) {
  std::copy(raw_frame, raw_frame + bins_, raw_[static_cast<size_t>(pushed_ % 3)].begin());
  ++pushed_;
  // The median spans the frame after the one it answers for, so the first push
  // only fills the ring.
  if (pushed_ < 2) return nullptr;
  return emit(pushed_ - 2, /*has_next=*/true);
}

const double* MedianGainSmoother::flush() {
  if (pushed_ == 0) return nullptr;
  return emit(pushed_ - 1, /*has_next=*/false);
}

bool MedianGainSmoother::discard_non_finite_state() noexcept {
  // Unity, not zero: these cells are gains, and a discarded frame that silences
  // its band is a louder defect than the one it recovers from.
  bool discarded = false;
  for (auto& frame : raw_) {
    discarded |= discard_run_if_non_finite(frame.begin(), frame.end(), 1.0);
  }
  discarded |= discard_run_if_non_finite(out_.begin(), out_.end(), 1.0);
  return discarded;
}

const double* MedianGainSmoother::emit(int target, bool has_next) {
  const int last = has_next ? target + 1 : target;
  for (int b = 0; b < bins_; ++b) {
    window_.clear();
    for (int db = -1; db <= 1; ++db) {
      const int bb = b + db;
      if (bb < 0 || bb >= bins_) continue;
      for (int tt = target - 1; tt <= last; ++tt) {
        if (tt < 0) continue;
        window_.push_back(slot(tt)[bb]);
      }
    }
    std::nth_element(window_.begin(), window_.begin() + window_.size() / 2, window_.end());
    out_[static_cast<size_t>(b)] = window_[window_.size() / 2];
  }
  return out_.data();
}

common::NoiseTracker::Mode tracker_mode_for(DenoiseNoiseEstimator estimator) {
  switch (estimator) {
    case DenoiseNoiseEstimator::Quantile:
      throw SonareException(ErrorCode::InvalidState,
                            "quantile noise estimation does not run through NoiseTracker");
    case DenoiseNoiseEstimator::Mcra:
      return common::NoiseTracker::Mode::Mcra;
    case DenoiseNoiseEstimator::Imcra:
      return common::NoiseTracker::Mode::Imcra;
    case DenoiseNoiseEstimator::Spp:
      return common::NoiseTracker::Mode::Spp;
  }
  throw SonareException(ErrorCode::InvalidParameter, "invalid denoise noise estimator");
}

void frame_powers(const std::complex<float>* frame, int bins, float* power_f, double* power_d) {
  for (int b = 0; b < bins; ++b) {
    const float re = frame[b].real();
    const float im = frame[b].imag();
    power_f[b] = re * re + im * im;
    const double magnitude = std::abs(frame[b]);
    power_d[b] = magnitude * magnitude;
  }
}

}  // namespace detail

namespace {

using detail::frame_powers;
using detail::tracker_mode_for;
using sonare::constants::kPiD;

/// @brief Gain below which an attenuation is reported as the mask's floor.
constexpr double kReportedGainEpsilon = 1e-12;

// The knob is the suppression depth in dB; the mask needs it as a linear floor.
double gain_floor_of(const DenoiseClassicalConfig& config) {
  return db_to_linear(-static_cast<double>(config.reduction_db));
}

StftConfig analysis_config(const DenoiseClassicalConfig& config) {
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  return stft_config;
}

/// Whether @p mode names one of the gain functions this module implements.
/// @details Exhaustive and without a `default`, for the reason on its estimator
///   sibling below. The trailing `false` is for an integer cast from outside the
///   enumeration, which no case can catch.
bool is_known_denoise_mode(DenoiseMode mode) {
  switch (mode) {
    case DenoiseMode::LogMmse:
    case DenoiseMode::MmseStsa:
    case DenoiseMode::SpectralSubtraction:
      return true;
  }
  return false;
}

/// Whether @p estimator names one of the estimators this module implements.
/// @details Exhaustive and without a `default`, so adding an estimator is a build
///   error here rather than a value the validator quietly rejects. The trailing
///   `false` is for an integer cast from outside the enumeration.
bool is_known_noise_estimator(DenoiseNoiseEstimator estimator) {
  switch (estimator) {
    case DenoiseNoiseEstimator::Quantile:
    case DenoiseNoiseEstimator::Mcra:
    case DenoiseNoiseEstimator::Imcra:
    case DenoiseNoiseEstimator::Spp:
      return true;
  }
  return false;
}

// Exponential integral E1(x) = integral from x to infinity of (e^-t / t) dt.
// Approximated via Abramowitz & Stegun 5.1.53 (series) for x <= 1, and
// 5.1.56 (rational asymptotic) for x > 1. Accuracy ~1e-7 over (0, inf).
double exponential_integral_e1(double x) {
  if (x <= 0.0) return 0.0;
  if (x <= 1.0) {
    // A&S 5.1.53: E1(x) = -gamma - ln(x) + sum_{n=1..inf} (-1)^(n+1) x^n / (n * n!)
    constexpr double kGamma = 0.5772156649015329;
    double sum = -kGamma - std::log(x);
    double term = 1.0;
    for (int n = 1; n < 20; ++n) {
      term *= -x / static_cast<double>(n);
      sum -= term / static_cast<double>(n);
    }
    return sum;
  }
  // A&S 5.1.56: rational approximation for x > 1.
  const double a0 = 8.5733287401, a1 = 18.0590169730, a2 = 8.6347608925, a3 = 0.2677737343;
  const double b0 = 9.5733223454, b1 = 25.6329561486, b2 = 21.0996530827, b3 = 3.9584969228;
  const double num = x * x * x * x + a3 * x * x * x + a2 * x * x + a1 * x + a0;
  const double den = x * x * x * x + b3 * x * x * x + b2 * x * x + b1 * x + b0;
  return std::exp(-x) / x * (num / den);
}

float gain_logmmse(double ksi, double gamma_post) {
  const double nu = ksi * gamma_post / (1.0 + ksi);
  const double e1 = exponential_integral_e1(nu);
  return static_cast<float>((ksi / (1.0 + ksi)) * std::exp(0.5 * e1));
}

float gain_mmse_stsa(double ksi, double gamma_post) {
  // Closed-form using modified Bessel functions of orders 0 and 1 (Ephraim-Malah 1984).
  // G_STSA = sqrt(pi)/2 * sqrt(nu)/gamma * exp(-nu/2) * ((1+nu)*I0(nu/2) + nu*I1(nu/2))
  const double nu = ksi * gamma_post / (1.0 + ksi);
  if (nu < 1e-12) return 0.0f;
  const double half_nu = 0.5 * nu;
  if (half_nu > 30.0) {
    const double envelope = (1.0 + 4.0 * half_nu) / std::sqrt(2.0 * kPiD * half_nu);
    const double prefactor = std::sqrt(kPiD) * 0.5 * std::sqrt(nu) / gamma_post;
    const double gain = prefactor * envelope;
    return static_cast<float>(std::isfinite(gain) ? gain : 1.0);
  }
  // Series for I0(z) and I1(z) at z = nu/2 = half_nu.
  // I0(z) = sum_k (z/2)^(2k) / (k!)^2, I1(z) = sum_k (z/2)^(2k+1) / (k! (k+1)!).
  // The successive-term ratio therefore carries (z/2)^2 = (half_nu/2)^2.
  const double z_half_sq = 0.25 * half_nu * half_nu;
  double i0 = 1.0;
  double i1 = 0.0;
  double term0 = 1.0;
  double term1 = 0.5 * half_nu;
  i1 = term1;
  for (int k = 1; k < 25; ++k) {
    term0 *= z_half_sq / static_cast<double>(k * k);
    i0 += term0;
    if (k >= 1) {
      term1 *= z_half_sq / static_cast<double>(k * (k + 1));
      i1 += term1;
    }
  }
  const double envelope = std::exp(-half_nu) * ((1.0 + nu) * i0 + nu * i1);
  const double prefactor = std::sqrt(kPiD) * 0.5 * std::sqrt(nu) / gamma_post;
  const double gain = prefactor * envelope;
  return static_cast<float>(std::isfinite(gain) ? gain : 1.0);
}

double speech_presence_probability(double ksi, double gamma_post) {
  constexpr double q = 0.05;  // Cohen OM-LSA default speech-absence prior.
  const double v = ksi * gamma_post / (1.0 + ksi);
  const double odds = (q / (1.0 - q)) * (1.0 + ksi) * std::exp(-v);
  const double p = 1.0 / (1.0 + odds);
  return std::clamp(p, 0.05, 1.0);
}

/// Whether @p mode takes the log-spectral gain rather than the amplitude one.
/// @details Exhaustive and without a `default`, so a new mode is a build error
///   rather than inheriting whichever branch an `else` names. SpectralSubtraction
///   throws: gains_berouti answers it, this gain function never does.
bool uses_log_spectral_gain(DenoiseMode mode) {
  switch (mode) {
    case DenoiseMode::LogMmse:
      return true;
    case DenoiseMode::MmseStsa:
      return false;
    case DenoiseMode::SpectralSubtraction:
      throw SonareException(ErrorCode::InvalidState,
                            "spectral subtraction does not use the Ephraim-Malah gain");
  }
  throw SonareException(ErrorCode::InvalidParameter, "invalid denoise mode");
}

/// Whether @p mode is answered by Berouti's subtraction rather than Ephraim-Malah.
/// @details Exhaustive and without a `default`, so a new mode is a build error
///   here rather than inheriting whichever side a plain comparison leaves it on.
bool uses_spectral_subtraction(DenoiseMode mode) {
  switch (mode) {
    case DenoiseMode::SpectralSubtraction:
      return true;
    case DenoiseMode::LogMmse:
    case DenoiseMode::MmseStsa:
      return false;
  }
  throw SonareException(ErrorCode::InvalidParameter, "invalid denoise mode");
}

double mean_square(const float* samples, size_t size) {
  double sum = 0.0;
  for (size_t i = 0; i < size; ++i) {
    sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
  }
  return size == 0 ? 0.0 : sum / static_cast<double>(size);
}

NoiseDetection to_detection(const common::NoiseFloorDbfs& levels) {
  NoiseDetection detection;
  detection.floor_dbfs = levels.broadband;
  for (size_t k = 0; k < kRepairNoiseBandCount; ++k) detection.band_floor_dbfs[k] = levels.bands[k];
  return detection;
}

/// @brief DenoiseReport's mask summary, accumulated one gain frame at a time.
/// @details The mean is carried as one partial sum per bin rather than one running
///   total, because the whole-plane loop it replaces walked bin-major: summing the
///   frames of a bin and then the bins reassociates nothing, while a single
///   running total over frames would.
class MaskSummary {
 public:
  MaskSummary(int bins, int frames, const DenoiseClassicalConfig& config)
      : frames_(frames),
        floor_gain_(gain_floor_of(config)),
        has_gain_floor_(!uses_spectral_subtraction(config.mode)),
        per_bin_(static_cast<size_t>(std::max(bins, 0)), 0.0) {}

  void add(const double* gains, int bins) {
    for (int b = 0; b < bins; ++b) {
      const double reduction = -20.0 * std::log10(std::max(gains[b], kReportedGainEpsilon));
      per_bin_[static_cast<size_t>(b)] += reduction;
      max_db_ = std::max(max_db_, reduction);
      if (has_gain_floor_ && gains[b] <= floor_gain_) ++at_floor_;
    }
  }

  void write(DenoiseReport* report) const {
    const size_t cells = per_bin_.size() * static_cast<size_t>(std::max(frames_, 0));
    if (cells == 0) return;
    double sum_db = 0.0;
    for (const double value : per_bin_) sum_db += value;
    report->mean_reduction_db = static_cast<float>(sum_db / static_cast<double>(cells));
    report->max_reduction_db = static_cast<float>(max_db_);
    report->floor_limited_fraction =
        static_cast<float>(static_cast<double>(at_floor_) / static_cast<double>(cells));
  }

 private:
  int frames_;
  double floor_gain_;
  bool has_gain_floor_;
  double max_db_ = 0.0;
  size_t at_floor_ = 0;
  std::vector<double> per_bin_;
};

/// @brief One analysis grid over a channel set, read one frame at a time.
/// @details Holds the per-channel spectra of the frame it last read plus the two
///   channel-summed power spectra the estimator and the gain recursion each want.
///   A single channel sums to itself, so the mono entry point is this with one
///   channel rather than a second traversal beside it.
class LinkedFrameReader {
 public:
  LinkedFrameReader(const Audio* const* channels, std::size_t channel_count,
                    const StftConfig& config) {
    readers_.reserve(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
      readers_.push_back(std::make_unique<common::StftFrameReader>(
          channels[c]->data(), channels[c]->size(), channels[c]->sample_rate(), config));
    }
    bins_ = readers_.empty() ? 0 : readers_.front()->n_bins();
    frames_ = readers_.empty() ? 0 : readers_.front()->n_frames();
    const std::size_t bins = static_cast<std::size_t>(std::max(bins_, 0));
    spectra_.assign(bins * channel_count, std::complex<float>{});
    power_f_.assign(bins, 0.0f);
    power_d_.assign(bins, 0.0);
    channel_f_.assign(bins, 0.0f);
    channel_d_.assign(bins, 0.0);
  }

  int n_bins() const { return bins_; }
  int n_frames() const { return frames_; }
  std::size_t channel_count() const { return readers_.size(); }

  /// @brief Reads frame @p index of every channel and sums its power.
  void read(int index) {
    std::fill(power_f_.begin(), power_f_.end(), 0.0f);
    std::fill(power_d_.begin(), power_d_.end(), 0.0);
    for (std::size_t c = 0; c < readers_.size(); ++c) {
      const std::complex<float>* spectrum = readers_[c]->frame(index);
      std::copy(
          spectrum, spectrum + bins_,
          spectra_.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(bins_) * c));
      frame_powers(spectrum, bins_, channel_f_.data(), channel_d_.data());
      for (int b = 0; b < bins_; ++b) {
        power_f_[static_cast<std::size_t>(b)] += channel_f_[static_cast<std::size_t>(b)];
        power_d_[static_cast<std::size_t>(b)] += channel_d_[static_cast<std::size_t>(b)];
      }
    }
  }

  /// Spectra of the frame last read, channel-major and each @ref n_bins long.
  const std::complex<float>* spectra() const { return spectra_.data(); }
  /// Channel-summed re^2 + im^2, which is what the estimator reads.
  const float* power_f() const { return power_f_.data(); }
  /// Channel-summed |z|^2, which is what the gain recursion reads.
  const double* power_d() const { return power_d_.data(); }

 private:
  int bins_ = 0;
  int frames_ = 0;
  std::vector<std::unique_ptr<common::StftFrameReader>> readers_;
  std::vector<std::complex<float>> spectra_;
  std::vector<float> power_f_;
  std::vector<double> power_d_;
  std::vector<float> channel_f_;
  std::vector<double> channel_d_;
};

/// @brief The stationary noise PSD from the quietest frames, without a plane.
/// @details Two walks over the reader: one ranking the frames by energy, one over
///   the selected frames in that ranking's order. The second walks by rank rather
///   than by time because what it builds is a sum of floats, so the order is part
///   of the result and not an implementation detail.
std::vector<double> quantile_noise_psd(LinkedFrameReader& reader, float quantile) {
  const int bins = reader.n_bins();
  const int frames = reader.n_frames();
  std::vector<double> noise(static_cast<size_t>(std::max(bins, 0)), 0.0);
  if (frames == 0 || bins == 0) return noise;

  std::vector<std::pair<double, int>> frame_energies(static_cast<size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    reader.read(t);
    const float* power = reader.power_f();
    double energy = 0.0;
    for (int b = 0; b < bins; ++b) energy += power[b];
    frame_energies[static_cast<size_t>(t)] = {energy, t};
  }
  std::sort(frame_energies.begin(), frame_energies.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  const int noise_frames =
      std::max(1, static_cast<int>(std::round(static_cast<float>(frames) * quantile)));
  for (int i = 0; i < noise_frames; ++i) {
    reader.read(frame_energies[static_cast<size_t>(i)].second);
    const float* power = reader.power_f();
    for (int b = 0; b < bins; ++b) noise[static_cast<size_t>(b)] += power[b];
  }
  const double scale = 1.0 / static_cast<double>(noise_frames);
  for (double& value : noise) value *= scale;
  return noise;
}

/// @brief Supplies each frame's noise PSD, whichever estimator was configured.
/// @details Quantile answers with one spectrum measured up front and repeated;
///   the three tracker modes are recursive in time and answer from the frame they
///   are being fed. Both look the same to the caller, which is what lets the
///   masking walk carry no branch on the estimator.
class NoisePsdSource {
 public:
  NoisePsdSource(LinkedFrameReader& reader, int sample_rate, const DenoiseClassicalConfig& config)
      : bins_(reader.n_bins()), frame_(static_cast<size_t>(std::max(bins_, 0)), 0.0) {
    if (config.noise_estimator == DenoiseNoiseEstimator::Quantile) {
      frame_ = quantile_noise_psd(reader, config.noise_estimation_quantile);
      return;
    }
    tracker_ = std::make_unique<common::NoiseTracker>(
        bins_, sample_rate, tracker_mode_for(config.noise_estimator), config.hop_length);
    tracker_input_.assign(static_cast<size_t>(std::max(bins_, 0)), 0.0f);
  }

  /// @brief Advances to the next frame and returns its per-bin noise PSD.
  const double* update(const float* power_f) {
    if (tracker_ == nullptr) return frame_.data();
    for (int b = 0; b < bins_; ++b) {
      tracker_input_[static_cast<size_t>(b)] = std::max(power_f[b], 0.0f);
    }
    tracker_->update(tracker_input_.data());
    const float* tracked = tracker_->noise_psd();
    for (int b = 0; b < bins_; ++b) frame_[static_cast<size_t>(b)] = tracked[b];
    return frame_.data();
  }

 private:
  int bins_;
  std::vector<double> frame_;
  std::vector<float> tracker_input_;
  std::unique_ptr<common::NoiseTracker> tracker_;
};

/// @brief The masking walk both entry points run, once the channels are bound.
/// @details Fills @p out with one denoised Audio per channel and, when @p report
///   is non-null, the floor and mask summary measured on the same walk. Kept as
///   one function because the mono and linked entries differ only in how many
///   channels they hand over and in what they do with the result.
void denoise_channels(LinkedFrameReader& reader, int sample_rate, std::size_t length,
                      const StftConfig& stft_config, const DenoiseClassicalConfig& config,
                      double signal_mean_square, std::vector<Audio>* out, DenoiseReport* report) {
  const int bins = reader.n_bins();
  const int frames = reader.n_frames();
  const std::size_t channels = reader.channel_count();

  NoisePsdSource noise(reader, sample_rate, config);
  detail::GainStage stage(bins, config);
  MaskSummary summary(bins, frames, config);
  std::vector<std::unique_ptr<common::IstftAccumulator>> synthesis;
  synthesis.reserve(channels);
  for (std::size_t c = 0; c < channels; ++c) {
    synthesis.push_back(std::make_unique<common::IstftAccumulator>(frames, sample_rate, stft_config,
                                                                   static_cast<int>(length)));
  }

  const int latency = stage.latency();
  const std::size_t cells = static_cast<std::size_t>(bins) * channels;
  std::vector<double> noise_sum(static_cast<size_t>(bins), 0.0);
  std::vector<double> power_sum(static_cast<size_t>(bins), 0.0);
  std::vector<std::complex<float>> held(cells);
  std::vector<std::complex<float>> masked(static_cast<size_t>(bins));

  const auto emit = [&](const std::complex<float>* source, const double* gains) {
    for (std::size_t c = 0; c < channels; ++c) {
      const std::complex<float>* channel = source + static_cast<std::size_t>(bins) * c;
      for (int b = 0; b < bins; ++b) {
        masked[static_cast<size_t>(b)] = {static_cast<float>(channel[b].real() * gains[b]),
                                          static_cast<float>(channel[b].imag() * gains[b])};
      }
      synthesis[c]->push(masked.data());
    }
    summary.add(gains, bins);
  };

  for (int t = 0; t < frames; ++t) {
    reader.read(t);
    const float* power_f = reader.power_f();
    const double* noise_frame = noise.update(power_f);
    for (int b = 0; b < bins; ++b) {
      noise_sum[static_cast<size_t>(b)] += noise_frame[b];
      power_sum[static_cast<size_t>(b)] += static_cast<double>(power_f[b]);
    }
    const double* gains = stage.push(reader.power_d(), noise_frame);
    if (gains != nullptr) emit(latency == 0 ? reader.spectra() : held.data(), gains);
    if (latency != 0) std::copy(reader.spectra(), reader.spectra() + cells, held.begin());
  }
  const double* tail = stage.flush();
  if (tail != nullptr) emit(held.data(), tail);

  out->clear();
  out->reserve(channels);
  for (std::size_t c = 0; c < channels; ++c) out->push_back(synthesis[c]->finish());

  // Off the sample path entirely, so asking for it cannot move the output.
  if (report != nullptr) {
    report->detected = to_detection(common::noise_floor_dbfs_from_sums(
        noise_sum.data(), power_sum.data(), bins, frames, signal_mean_square, sample_rate));
    summary.write(report);
  }
}

}  // namespace

namespace detail {

GainStage::GainStage(int bins, const DenoiseClassicalConfig& config)
    : bins_(bins),
      config_(config),
      floor_gain_(gain_floor_of(config)),
      berouti_(uses_spectral_subtraction(config.mode)),
      log_spectral_(berouti_ ? false : uses_log_spectral_gain(config.mode)),
      prev_clean_power_(static_cast<size_t>(bins), 0.0),
      raw_(static_cast<size_t>(bins), 1.0),
      out_(static_cast<size_t>(bins), 1.0) {
  if (!berouti_ && config.gain_smoothing) {
    smoother_ = std::make_unique<MedianGainSmoother>(bins);
  }
}

int GainStage::latency() const { return smoother_ ? 1 : 0; }

const double* GainStage::push(const double* power_frame, const double* noise_frame) {
  compute_raw(power_frame, noise_frame, raw_.data());
  if (smoother_ == nullptr) return finish(raw_.data());
  const double* smoothed = smoother_->push(raw_.data());
  return smoothed == nullptr ? nullptr : finish(smoothed);
}

const double* GainStage::flush() {
  if (smoother_ == nullptr) return nullptr;
  const double* smoothed = smoother_->flush();
  return smoothed == nullptr ? nullptr : finish(smoothed);
}

bool GainStage::discard_non_finite_state() noexcept {
  bool discarded =
      discard_run_if_non_finite(prev_clean_power_.begin(), prev_clean_power_.end(), 0.0);
  discarded |= discard_run_if_non_finite(raw_.begin(), raw_.end(), 1.0);
  discarded |= discard_run_if_non_finite(out_.begin(), out_.end(), 1.0);
  if (smoother_ != nullptr) discarded |= smoother_->discard_non_finite_state();
  return discarded;
}

void GainStage::compute_raw(const double* power_frame, const double* noise_frame, double* raw) {
  if (berouti_) {
    const double alpha = static_cast<double>(config_.over_subtraction);
    const double beta = static_cast<double>(config_.spectral_floor);
    for (int b = 0; b < bins_; ++b) {
      const double power = power_frame[b];
      const double mag = std::sqrt(power);
      const double noise_pow = std::max(noise_frame[b], 1e-12);
      const double floor_pow = beta * noise_pow;
      const double clean_power = std::max(power - alpha * noise_pow, floor_pow);
      raw[b] = mag > 1e-12 ? std::sqrt(clean_power) / mag : 0.0;
    }
    return;
  }

  const double alpha = config_.dd_alpha;
  for (int b = 0; b < bins_; ++b) {
    const double power = power_frame[b];
    const double noise = std::max(noise_frame[b], 1e-12);

    // a posteriori SNR.
    const double gamma_post = std::max(power / noise, 1e-6);
    // Decision-directed a priori SNR (Ephraim-Malah recursion).
    const double ml_estimate = std::max(gamma_post - 1.0, 0.0);
    const double ksi = std::max(
        alpha * prev_clean_power_[static_cast<size_t>(b)] / noise + (1.0 - alpha) * ml_estimate,
        1e-6);

    double gain = log_spectral_ ? gain_logmmse(ksi, gamma_post) : gain_mmse_stsa(ksi, gamma_post);
    if (config_.speech_presence_gain) {
      const double presence = speech_presence_probability(ksi, gamma_post);
      gain = std::pow(std::max(gain, floor_gain_), presence) *
             std::pow(std::max(floor_gain_, 1.0e-6), 1.0 - presence);
    }
    gain = std::max(gain, floor_gain_);
    gain = std::min(gain, 1.0);

    raw[b] = gain;
    // The recursion feeds on the raw gain: smoothing happens after this frame
    // has already set the next one's prior.
    prev_clean_power_[static_cast<size_t>(b)] = power * gain * gain;
  }
}

/// Rounds the mask to float. Deliberately here rather than at the multiply: the
/// mask is one real number per cell and every channel is scaled by the same one.
const double* GainStage::finish(const double* source) {
  if (berouti_) return source;
  for (int b = 0; b < bins_; ++b) {
    out_[static_cast<size_t>(b)] =
        static_cast<double>(static_cast<float>(std::clamp(source[b], floor_gain_, 1.0)));
  }
  return out_.data();
}

}  // namespace detail

void validate_config(const DenoiseClassicalConfig& config) {
  if (!is_known_denoise_mode(config.mode)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid denoise mode");
  }
  if (!is_known_noise_estimator(config.noise_estimator)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid denoise noise estimator");
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise n_fft must be a positive power of two");
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise hop_length must be in (0, n_fft]");
  }
  if (!std::isfinite(config.dd_alpha) || config.dd_alpha < 0.0f || config.dd_alpha >= 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise dd_alpha must be finite and in [0, 1)");
  }
  // No upper bound: the derived floor is 10^(-reduction_db/20), which already
  // lands in (0, 1] for every finite non-negative depth.
  if (!std::isfinite(config.reduction_db) || config.reduction_db < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise reduction_db must be finite and non-negative");
  }
  if (!std::isfinite(config.over_subtraction) || config.over_subtraction < 0.0f ||
      config.over_subtraction > 16.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise over_subtraction must be finite and in [0, 16]");
  }
  if (!std::isfinite(config.spectral_floor) || config.spectral_floor < 0.0f ||
      config.spectral_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise spectral_floor must be finite and in [0, 1]");
  }
  if (!std::isfinite(config.noise_estimation_quantile) ||
      config.noise_estimation_quantile <= 0.0f || config.noise_estimation_quantile > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise noise_estimation_quantile must be finite and in (0, 1]");
  }
}

NoiseDetection detect_noise_floor(const float* samples, std::size_t size, int sample_rate,
                                  const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (samples == nullptr || size == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (size < static_cast<size_t>(validated->n_fft)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  const Audio audio = Audio::from_buffer(samples, size, sample_rate);
  const Audio* channels[1] = {&audio};
  LinkedFrameReader reader(channels, 1, analysis_config(validated.get()));
  const int bins = reader.n_bins();
  const int frames = reader.n_frames();
  if (bins == 0 || frames == 0) return NoiseDetection{};

  NoisePsdSource noise(reader, sample_rate, validated.get());
  std::vector<double> noise_sum(static_cast<size_t>(bins), 0.0);
  std::vector<double> power_sum(static_cast<size_t>(bins), 0.0);

  for (int t = 0; t < frames; ++t) {
    reader.read(t);
    const float* power_f = reader.power_f();
    const double* noise_frame = noise.update(power_f);
    for (int b = 0; b < bins; ++b) {
      noise_sum[static_cast<size_t>(b)] += noise_frame[b];
      power_sum[static_cast<size_t>(b)] += static_cast<double>(power_f[b]);
    }
  }
  return to_detection(common::noise_floor_dbfs_from_sums(
      noise_sum.data(), power_sum.data(), bins, frames, mean_square(samples, size), sample_rate));
}

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config) {
  return denoise_classical(audio, config, nullptr);
}

Audio denoise_classical(const Audio& audio, const DenoiseClassicalConfig& config,
                        DenoiseReport* report) {
  if (report != nullptr) *report = DenoiseReport{};
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);

  if (static_cast<int>(audio.size()) < validated->n_fft) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  const StftConfig stft_config = analysis_config(validated.get());
  const Audio* channels[1] = {&audio};
  LinkedFrameReader reader(channels, 1, stft_config);
  if (reader.n_bins() == 0 || reader.n_frames() == 0) return audio;

  std::vector<Audio> out;
  denoise_channels(reader, audio.sample_rate(), audio.size(), stft_config, validated.get(),
                   mean_square(audio.data(), audio.size()), &out, report);
  return std::move(out[0]);
}

DenoiseReport denoise_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                       std::vector<Audio>* out,
                                       const DenoiseClassicalConfig& config) {
  const auto validated = Validated<DenoiseClassicalConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "denoise output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  const size_t length = channels[0]->size();
  if (static_cast<int>(length) < validated->n_fft) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "denoise input must contain at least n_fft samples");
  }

  for (size_t c = 1; c < channel_count; ++c) {
    if (channels[c] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channel must not be null");
    }
    if (channels[c]->size() != length) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channels must have the same length");
    }
    if (channels[c]->sample_rate() != channels[0]->sample_rate()) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "linked analysis channels must share one sample rate");
    }
  }

  const StftConfig stft_config = analysis_config(validated.get());
  LinkedFrameReader reader(channels, channel_count, stft_config);

  DenoiseReport report;
  if (reader.n_bins() == 0 || reader.n_frames() == 0) {
    out->clear();
    for (size_t c = 0; c < channel_count; ++c) out->push_back(*channels[c]);
    return report;
  }

  double summed_mean_square = 0.0;
  for (size_t c = 0; c < channel_count; ++c) {
    summed_mean_square += mean_square(channels[c]->data(), channels[c]->size());
  }
  denoise_channels(reader, channels[0]->sample_rate(), length, stft_config, validated.get(),
                   summed_mean_square, out, &report);
  return report;
}

DenoiseStereoResult denoise_classical_stereo(const Audio& left, const Audio& right,
                                             const DenoiseClassicalConfig& config) {
  const Audio* channels[2] = {&left, &right};
  std::vector<Audio> out;
  DenoiseStereoResult result;
  result.report = denoise_classical_linked(channels, 2, &out, config);
  result.left = std::move(out[0]);
  result.right = std::move(out[1]);
  return result;
}

}  // namespace sonare::mastering::repair
