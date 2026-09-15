#include "mastering/repair/dereverb_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_profile.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

constexpr double kRegularization = static_cast<double>(sonare::constants::kSpectrumEpsilon);

/// @brief Power a bin must exceed to take part in a ratio.
constexpr double kPowerFloor = 1e-18;

/// @brief Largest predictor norm the WPE stage applies.
/// @details Past unity the prediction grows rather than cancels, so the loop is
/// held just inside it.
constexpr double kWpePredictorNormCap = 0.98;

// The lower quartile of the per-cell lag ratio. The median sits at 0 dB because
// most cells are steady, and the upper tail is onsets; a decay shows in the
// lower quartile, which separated dry speech from the same speech reverberated
// by 8.7 dB on the evaluation corpus while the median separated them by 1.0.
constexpr double kLateDecayPercentile = 0.25;

StftConfig analysis_config(const DereverbClassicalConfig& config) {
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  stft_config.window = WindowType::Hann;
  stft_config.center = true;
  return stft_config;
}

/// @brief Pads a buffer up to one analysis frame, as the repair path does.
Audio padded_for_analysis(const Audio& audio, int n_fft) {
  if (static_cast<int>(audio.size()) >= n_fft) return audio;
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  samples.resize(static_cast<size_t>(n_fft), 0.0f);
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

int late_delay_frames(const DereverbClassicalConfig& config, int sample_rate) {
  return std::max(1, static_cast<int>(std::round(config.late_delay_ms * 0.001f *
                                                 static_cast<float>(sample_rate) /
                                                 static_cast<float>(config.hop_length))));
}

/// @brief Energy a tail is assumed to have lost across the late-reverb lag.
double late_decay(const DereverbClassicalConfig& config, int delay_frames, int sample_rate) {
  const float delay_sec =
      static_cast<float>(delay_frames * config.hop_length) / static_cast<float>(sample_rate);
  return std::exp(-2.0 * static_cast<double>(delay_sec) * 6.0 * std::log(10.0) /
                  static_cast<double>(config.t60_sec));
}

/// @brief Solves matrix * x = rhs by Gauss-Jordan elimination with partial pivoting.
/// @details Both arguments are working storage and are left in an arbitrary state; the caller
///          refills them before the next use.
std::vector<std::complex<float>> solve_linear_system(
    std::vector<std::vector<std::complex<double>>>& matrix,
    std::vector<std::complex<double>>& rhs) {
  const size_t n = rhs.size();
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    double best = std::abs(matrix[col][col]);
    for (size_t row = col + 1; row < n; ++row) {
      const double candidate = std::abs(matrix[row][col]);
      if (candidate > best) {
        best = candidate;
        pivot = row;
      }
    }
    if (best < 1.0e-18) {
      // Near-singular column: no usable pivot. Force this predictor entry to
      // zero instead of leaving rhs[col] at its raw cross-correlation value,
      // which would otherwise yield an arbitrary (often huge) predictor tap.
      rhs[col] = {0.0, 0.0};
      continue;
    }
    if (pivot != col) {
      std::swap(matrix[pivot], matrix[col]);
      std::swap(rhs[pivot], rhs[col]);
    }
    const auto diagonal = matrix[col][col];
    for (size_t k = col; k < n; ++k) matrix[col][k] /= diagonal;
    rhs[col] /= diagonal;
    for (size_t row = 0; row < n; ++row) {
      if (row == col) continue;
      const auto factor = matrix[row][col];
      if (std::abs(factor) < 1.0e-18) continue;
      for (size_t k = col; k < n; ++k) matrix[row][k] -= factor * matrix[col][k];
      rhs[row] -= factor * rhs[col];
    }
  }

  std::vector<std::complex<float>> solution(n);
  for (size_t i = 0; i < n; ++i) solution[i] = static_cast<std::complex<float>>(rhs[i]);
  return solution;
}

/// @brief Mean predictor norm a WPE pass produced, before and after the cap.
struct PredictorNorms {
  double before_cap = 0.0;
  double after_cap = 0.0;
};

/// @brief Fits one WPE predictor per bin over every channel at once.
/// @details Accumulating the covariance over all channels and applying the
///   solution to all of them keeps the stage identical across channels, which is
///   what makes it safe to link. @p channels is left untouched when
///   @p subtract is false, so the detector shares the fit with the repair.
PredictorNorms fit_wpe(std::vector<std::vector<std::complex<float>>>& channels, int bins,
                       int frames, int delay_frames, int taps, float strength, bool subtract) {
  PredictorNorms norms;
  const int first_predictable = delay_frames + taps - 1;
  std::vector<std::vector<std::complex<double>>> covariance(
      static_cast<size_t>(taps),
      std::vector<std::complex<double>>(static_cast<size_t>(taps), {0.0, 0.0}));
  std::vector<std::complex<double>> cross(static_cast<size_t>(taps), {0.0, 0.0});
  std::vector<std::vector<std::complex<float>>> next;
  if (subtract) next = channels;

  for (int b = 0; b < bins; ++b) {
    for (auto& row : covariance) row.assign(static_cast<size_t>(taps), {0.0, 0.0});
    cross.assign(static_cast<size_t>(taps), {0.0, 0.0});
    for (const auto& channel : channels) {
      for (int t = first_predictable; t < frames; ++t) {
        const auto current =
            static_cast<std::complex<double>>(channel[static_cast<size_t>(b * frames + t)]);
        for (int i = 0; i < taps; ++i) {
          const auto xi = static_cast<std::complex<double>>(
              channel[static_cast<size_t>(b * frames + t - delay_frames - i)]);
          cross[static_cast<size_t>(i)] += current * std::conj(xi);
          for (int j = 0; j < taps; ++j) {
            const auto xj = static_cast<std::complex<double>>(
                channel[static_cast<size_t>(b * frames + t - delay_frames - j)]);
            covariance[static_cast<size_t>(i)][static_cast<size_t>(j)] += xi * std::conj(xj);
          }
        }
      }
    }
    for (int i = 0; i < taps; ++i) {
      covariance[static_cast<size_t>(i)][static_cast<size_t>(i)] +=
          std::complex<double>{kRegularization, 0.0};
    }
    auto predictors = solve_linear_system(covariance, cross);
    double predictor_norm = 0.0;
    for (const auto& predictor : predictors) predictor_norm += std::abs(predictor);
    norms.before_cap += predictor_norm;
    if (predictor_norm > kWpePredictorNormCap) {
      const float scale = static_cast<float>(kWpePredictorNormCap / predictor_norm);
      for (auto& predictor : predictors) predictor *= scale;
      predictor_norm = kWpePredictorNormCap;
    }
    norms.after_cap += predictor_norm;
    if (!subtract) continue;

    for (size_t c = 0; c < channels.size(); ++c) {
      for (int t = 0; t < frames; ++t) {
        const size_t idx = static_cast<size_t>(b * frames + t);
        if (t < first_predictable) {
          next[c][idx] = channels[c][idx];
          continue;
        }
        std::complex<float> predicted{0.0f, 0.0f};
        for (int tap = 0; tap < taps; ++tap) {
          predicted += predictors[static_cast<size_t>(tap)] *
                       channels[c][static_cast<size_t>(b * frames + t - delay_frames - tap)];
        }
        next[c][idx] = channels[c][idx] - strength * predicted;
      }
    }
  }

  if (bins > 0) {
    norms.before_cap /= static_cast<double>(bins);
    norms.after_cap /= static_cast<double>(bins);
  }
  if (subtract) channels.swap(next);
  return norms;
}

/// @brief The late-reverb subtraction mask, one real gain per (bin, frame).
std::vector<double> subtraction_gains(const float* power, int bins, int frames, int delay_frames,
                                      double decay, const DereverbClassicalConfig& config,
                                      size_t* suppressed_out) {
  std::vector<double> gains(static_cast<size_t>(bins * frames), 1.0);
  const double threshold = static_cast<double>(config.threshold);
  const double attenuation = static_cast<double>(config.attenuation);
  const double over_subtraction = static_cast<double>(config.over_subtraction);
  const double spectral_floor = static_cast<double>(config.spectral_floor);
  size_t suppressed = 0;

  for (int b = 0; b < bins; ++b) {
    for (int t = 0; t < frames; ++t) {
      const size_t idx = static_cast<size_t>(b * frames + t);
      const double current_power = std::max(static_cast<double>(power[idx]), kPowerFloor);
      const int late_frame = t - delay_frames;
      const double late_psd =
          late_frame >= 0
              ? static_cast<double>(power[static_cast<size_t>(b * frames + late_frame)]) * decay
              : 0.0;
      if (!(late_psd > threshold * current_power)) continue;
      ++suppressed;
      const double clean_power =
          std::max(current_power - over_subtraction * late_psd, spectral_floor * current_power);
      const double full = std::sqrt(clean_power / current_power);
      // Written so attenuation == 1 leaves `full` bit for bit rather than
      // round-tripping it through 1 - (1 - full).
      gains[idx] = full + (1.0 - attenuation) * (1.0 - full);
    }
  }
  if (suppressed_out != nullptr) *suppressed_out = suppressed;
  return gains;
}

/// @brief Lower-quartile decay across the module's own late-reverb lag, in dB.
float late_decay_ratio_db(const float* power, int bins, int frames, int delay_frames) {
  if (frames <= delay_frames) return 0.0f;
  std::vector<double> ratios;
  ratios.reserve(static_cast<size_t>(bins) * static_cast<size_t>(frames - delay_frames));
  for (int b = 0; b < bins; ++b) {
    const float* row = power + static_cast<size_t>(b) * static_cast<size_t>(frames);
    for (int t = delay_frames; t < frames; ++t) {
      const double current = static_cast<double>(row[t]);
      const double lagged = static_cast<double>(row[t - delay_frames]);
      if (current <= kPowerFloor || lagged <= kPowerFloor) continue;
      ratios.push_back(10.0 * std::log10(current / lagged));
    }
  }
  if (ratios.empty()) return 0.0f;
  const size_t rank =
      std::min(ratios.size() - 1,
               static_cast<size_t>(kLateDecayPercentile * static_cast<double>(ratios.size())));
  std::nth_element(ratios.begin(), ratios.begin() + static_cast<ptrdiff_t>(rank), ratios.end());
  return static_cast<float>(ratios[rank]);
}

float mean_reduction_db(const std::vector<double>& gains) {
  if (gains.empty()) return 0.0f;
  double sum = 0.0;
  for (const double gain : gains) sum += -20.0 * std::log10(std::max(gain, kPowerFloor));
  return static_cast<float>(sum / static_cast<double>(gains.size()));
}

}  // namespace

void validate_config(const DereverbClassicalConfig& config) {
  if (!std::isfinite(config.threshold) || config.threshold < 0.0f || config.threshold > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb threshold must be finite and in [0, 1]");
  }
  if (!std::isfinite(config.attenuation) || config.attenuation < 0.0f ||
      config.attenuation > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb attenuation must be finite and in [0, 1]");
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb n_fft must be a positive power of two");
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    throw SonareException(ErrorCode::InvalidParameter, "dereverb hop_length must be in (0, n_fft]");
  }
  if (!std::isfinite(config.t60_sec) || config.t60_sec <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb t60_sec must be finite and positive");
  }
  if (!std::isfinite(config.late_delay_ms) || config.late_delay_ms < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb late_delay_ms must be finite and non-negative");
  }
  if (!std::isfinite(config.over_subtraction) || config.over_subtraction < 0.0f ||
      config.over_subtraction > 16.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb over_subtraction must be finite and in [0, 16]");
  }
  if (!std::isfinite(config.spectral_floor) || config.spectral_floor < 0.0f ||
      config.spectral_floor > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb spectral_floor must be finite and in [0, 1]");
  }
  if (config.wpe_iterations < 1 || config.wpe_iterations > kDereverbMaxWpeIterations) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb wpe_iterations must be in [1, kDereverbMaxWpeIterations]");
  }
  if (config.wpe_taps < 1 || config.wpe_taps > kDereverbMaxWpeTaps) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb wpe_taps must be in [1, kDereverbMaxWpeTaps]");
  }
  if (!std::isfinite(config.wpe_strength) || config.wpe_strength < 0.0f ||
      config.wpe_strength > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "dereverb wpe_strength must be finite and in [0, 1]");
  }
}

ReverbDetection detect_reverb(const float* samples, std::size_t size, int sample_rate,
                              const DereverbClassicalConfig& config) {
  const auto validated = Validated<DereverbClassicalConfig>::make(config);
  if (samples == nullptr || size == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }

  const Audio audio = Audio::from_buffer(samples, size, sample_rate);
  const Audio analysis = padded_for_analysis(audio, validated->n_fft);
  const Spectrogram spec = Spectrogram::compute(analysis, analysis_config(validated.get()));
  if (spec.empty()) return ReverbDetection{};

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const int delay_frames = late_delay_frames(validated.get(), sample_rate);

  ReverbDetection detection;
  detection.late_decay_ratio_db =
      late_decay_ratio_db(spec.power().data(), bins, frames, delay_frames);
  if (validated->wpe_enabled) {
    const size_t cells = static_cast<size_t>(bins) * static_cast<size_t>(frames);
    std::vector<std::vector<std::complex<float>>> channels(1);
    channels[0].assign(spec.complex_data(), spec.complex_data() + cells);
    // One pass is the whole fit: without subtraction the next iteration sees the
    // same data and solves the same system.
    const PredictorNorms norms =
        fit_wpe(channels, bins, frames, delay_frames, validated->wpe_taps, validated->wpe_strength,
                /*subtract=*/false);
    detection.late_predictability = static_cast<float>(norms.before_cap);
  }
  return detection;
}

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config) {
  return dereverb_classical(audio, config, nullptr);
}

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config,
                         DereverbReport* report) {
  if (report != nullptr) *report = DereverbReport{};
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DereverbClassicalConfig>::make(config);

  // A short block must use the same STFT dereverberation path as a full clip.
  // Pad only the analysis input and trim reconstruction back to the caller's
  // original length; do not silently switch to the former tail-attenuation DSP.
  const Audio analysis = padded_for_analysis(audio, validated->n_fft);
  const Spectrogram spec = Spectrogram::compute(analysis, analysis_config(validated.get()));
  if (spec.empty()) return audio;

  const int bins = spec.n_bins();
  const int frames = spec.n_frames();
  const size_t cells = static_cast<size_t>(bins) * static_cast<size_t>(frames);
  const int delay_frames = late_delay_frames(validated.get(), audio.sample_rate());
  const double decay = late_decay(validated.get(), delay_frames, audio.sample_rate());
  size_t suppressed = 0;
  const auto gains = subtraction_gains(spec.power().data(), bins, frames, delay_frames, decay,
                                       validated.get(), report != nullptr ? &suppressed : nullptr);

  const auto* complex_data = spec.complex_data();
  std::vector<std::vector<std::complex<float>>> channels(1);
  channels[0].resize(cells);
  for (size_t i = 0; i < cells; ++i) {
    channels[0][i] = {static_cast<float>(complex_data[i].real() * gains[i]),
                      static_cast<float>(complex_data[i].imag() * gains[i])};
  }

  double before_cap = 0.0;
  double after_cap = 0.0;
  if (validated->wpe_enabled) {
    for (int iteration = 0; iteration < validated->wpe_iterations; ++iteration) {
      const PredictorNorms norms =
          fit_wpe(channels, bins, frames, delay_frames, validated->wpe_taps,
                  validated->wpe_strength, /*subtract=*/true);
      before_cap += norms.before_cap;
      after_cap += norms.after_cap;
    }
  }

  // Off the sample path entirely, so asking for it cannot move the output. The
  // lag statistic is a second pass over the power, which is why it is skipped
  // rather than computed and thrown away.
  if (report != nullptr) {
    report->detected.late_decay_ratio_db =
        late_decay_ratio_db(spec.power().data(), bins, frames, delay_frames);
    report->mean_reduction_db = mean_reduction_db(gains);
    report->suppressed_fraction = gains.empty()
                                      ? 0.0f
                                      : static_cast<float>(static_cast<double>(suppressed) /
                                                           static_cast<double>(gains.size()));
    if (validated->wpe_enabled) {
      const double passes = static_cast<double>(validated->wpe_iterations);
      report->detected.late_predictability = static_cast<float>(before_cap / passes);
      report->wpe_predictor_norm = static_cast<float>(after_cap / passes);
    }
  }

  const Spectrogram clean = Spectrogram::from_complex(
      channels[0].data(), bins, frames, spec.n_fft(), spec.hop_length(), spec.sample_rate(),
      spec.window(), spec.center(), spec.win_length());
  return clean.to_audio(static_cast<int>(audio.size()));
}

DereverbReport dereverb_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                         std::vector<Audio>* out,
                                         const DereverbClassicalConfig& config) {
  const auto validated = Validated<DereverbClassicalConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "dereverb output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }

  std::vector<Audio> analysis;
  std::vector<const Audio*> analysis_pointers;
  analysis.reserve(channel_count);
  analysis_pointers.reserve(channel_count);
  for (size_t c = 0; c < channel_count; ++c) {
    if (channels[c] == nullptr) {
      throw SonareException(ErrorCode::InvalidParameter, "dereverb channel must not be null");
    }
    analysis.push_back(padded_for_analysis(*channels[c], validated->n_fft));
  }
  for (const Audio& channel : analysis) analysis_pointers.push_back(&channel);

  const common::LinkedSpectra linked = common::LinkedSpectra::compute(
      analysis_pointers.data(), channel_count, analysis_config(validated.get()));
  DereverbReport report;
  if (linked.empty()) {
    out->clear();
    for (size_t c = 0; c < channel_count; ++c) out->push_back(*channels[c]);
    return report;
  }

  const int bins = linked.n_bins();
  const int frames = linked.n_frames();
  const int sample_rate = channels[0]->sample_rate();
  const int delay_frames = late_delay_frames(validated.get(), sample_rate);
  const double decay = late_decay(validated.get(), delay_frames, sample_rate);

  size_t suppressed = 0;
  const auto gains = subtraction_gains(linked.summed_power().data(), bins, frames, delay_frames,
                                       decay, validated.get(), &suppressed);
  auto masked = linked.masked(gains.data());

  report.detected.late_decay_ratio_db =
      late_decay_ratio_db(linked.summed_power().data(), bins, frames, delay_frames);
  if (validated->wpe_enabled) {
    double before = 0.0;
    double after = 0.0;
    for (int iteration = 0; iteration < validated->wpe_iterations; ++iteration) {
      const PredictorNorms norms =
          fit_wpe(masked, bins, frames, delay_frames, validated->wpe_taps, validated->wpe_strength,
                  /*subtract=*/true);
      before += norms.before_cap;
      after += norms.after_cap;
    }
    const double passes = static_cast<double>(validated->wpe_iterations);
    report.detected.late_predictability = static_cast<float>(before / passes);
    report.wpe_predictor_norm = static_cast<float>(after / passes);
  }

  *out = linked.resynthesize(masked, static_cast<int>(channels[0]->size()));
  report.mean_reduction_db = mean_reduction_db(gains);
  report.suppressed_fraction =
      gains.empty()
          ? 0.0f
          : static_cast<float>(static_cast<double>(suppressed) / static_cast<double>(gains.size()));
  return report;
}

DereverbStereoResult dereverb_classical_stereo(const Audio& left, const Audio& right,
                                               const DereverbClassicalConfig& config) {
  const Audio* channels[2] = {&left, &right};
  std::vector<Audio> out;
  DereverbStereoResult result;
  result.report = dereverb_classical_linked(channels, 2, &out, config);
  result.left = std::move(out[0]);
  result.right = std::move(out[1]);
  return result;
}

void apply_room_measurement(DereverbClassicalConfig& config, float rt60_mid_sec,
                            float volume_m3) noexcept {
  if (std::isfinite(rt60_mid_sec) && rt60_mid_sec > 0.0f) config.t60_sec = rt60_mid_sec;
  if (std::isfinite(volume_m3) && volume_m3 > 0.0f) {
    // Polack: the response stops being separable reflections and becomes a
    // diffuse tail at roughly sqrt(V) milliseconds. The ceiling is a second,
    // past which no room mixes later; the floor is a guard on an absurd volume
    // rather than a mixing time, reached only under 1 m^3.
    config.late_delay_ms = std::clamp(std::sqrt(volume_m3), 1.0f, 1000.0f);
  }
}

}  // namespace sonare::mastering::repair
