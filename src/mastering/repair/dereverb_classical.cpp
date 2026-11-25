#include "mastering/repair/dereverb_classical.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "mastering/common/noise_profile.h"
#include "mastering/common/stft_stream.h"
#include "mastering/repair/analysis_padding.h"
#include "mastering/repair/dereverb_internal.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/validated.h"

namespace sonare::mastering::repair {

namespace check = sonare::util::check;
using sonare::constants::kLn10D;
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

int late_delay_frames(const DereverbClassicalConfig& config, int sample_rate) {
  return std::max(1, static_cast<int>(std::round(config.late_delay_ms * 0.001f *
                                                 static_cast<float>(sample_rate) /
                                                 static_cast<float>(config.hop_length))));
}

/// @brief Power a tail is assumed to have kept across the late-reverb lag:
///        10^(-6 delay / T60), so a lag of T60 is -60 dB.
double late_decay(const DereverbClassicalConfig& config, int delay_frames, int sample_rate) {
  const float delay_sec =
      static_cast<float>(delay_frames * config.hop_length) / static_cast<float>(sample_rate);
  return std::exp(-6.0 * kLn10D * static_cast<double>(delay_sec) /
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
  // A bin's prediction reads only that bin's own earlier frames, so one row of scratch holds the
  // subtraction until the row is complete, rather than a copy of the whole spectrum.
  std::vector<std::complex<float>> row;
  if (subtract) row.resize(static_cast<size_t>(frames));

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
            covariance[static_cast<size_t>(i)][static_cast<size_t>(j)] += std::conj(xi) * xj;
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
      for (int t = first_predictable; t < frames; ++t) {
        const size_t idx = static_cast<size_t>(b * frames + t);
        std::complex<float> predicted{0.0f, 0.0f};
        for (int tap = 0; tap < taps; ++tap) {
          predicted += predictors[static_cast<size_t>(tap)] *
                       channels[c][static_cast<size_t>(b * frames + t - delay_frames - tap)];
        }
        row[static_cast<size_t>(t)] = channels[c][idx] - strength * predicted;
      }
      for (int t = first_predictable; t < frames; ++t) {
        channels[c][static_cast<size_t>(b * frames + t)] = row[static_cast<size_t>(t)];
      }
    }
  }

  if (bins > 0) {
    norms.before_cap /= static_cast<double>(bins);
    norms.after_cap /= static_cast<double>(bins);
  }
  return norms;
}

/// @brief The late-reverb subtraction gain of one (bin, frame) cell, indexed bin-major.
double subtraction_gain_at(const float* power, int frames, int delay_frames, double decay,
                           const DereverbClassicalConfig& config, size_t idx, bool* suppressed) {
  const int late_frame = static_cast<int>(idx % static_cast<size_t>(frames)) - delay_frames;
  const double late_psd =
      late_frame >= 0 ? static_cast<double>(power[idx - static_cast<size_t>(delay_frames)]) * decay
                      : 0.0;
  return detail::dereverb_subtraction_gain(static_cast<double>(power[idx]), late_psd, config,
                                           suppressed);
}

/// @brief Calls @p visit with each cell's decay across the late-reverb lag, in dB.
template <typename Visit>
void for_each_lag_ratio_db(const float* power, int bins, int frames, int delay_frames,
                           Visit&& visit) {
  for (int b = 0; b < bins; ++b) {
    const float* row = power + static_cast<size_t>(b) * static_cast<size_t>(frames);
    for (int t = delay_frames; t < frames; ++t) {
      const double current = static_cast<double>(row[t]);
      const double lagged = static_cast<double>(row[t - delay_frames]);
      if (current <= kPowerFloor || lagged <= kPowerFloor) continue;
      visit(static_cast<float>(10.0 * std::log10(current / lagged)));
    }
  }
}

/// @brief Unsigned key whose order is the order of finite floats.
uint32_t float_order_key(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return (bits & 0x80000000u) != 0 ? ~bits : (bits | 0x80000000u);
}

float float_from_order_key(uint32_t key) {
  const uint32_t bits = (key & 0x80000000u) != 0 ? (key & 0x7fffffffu) : ~key;
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/// @brief Lower-quartile decay across the module's own late-reverb lag, in dB.
/// @details The order statistic is selected by its key, high half then low half, over two counting
///   passes, so no ratio is stored; the result is the element a full sort would put at the rank.
std::optional<float> late_decay_ratio_db(const float* power, int bins, int frames,
                                         int delay_frames) {
  if (frames <= delay_frames) return std::nullopt;
  constexpr size_t kHalfKeyValues = size_t{1} << 16;
  std::vector<size_t> counts(kHalfKeyValues, 0);
  size_t total = 0;
  for_each_lag_ratio_db(power, bins, frames, delay_frames, [&](float ratio) {
    ++counts[float_order_key(ratio) >> 16];
    ++total;
  });
  if (total == 0) return std::nullopt;
  const size_t rank =
      std::min(total - 1, static_cast<size_t>(kLateDecayPercentile * static_cast<double>(total)));
  size_t below = 0;
  uint32_t high = 0;
  while (below + counts[high] <= rank) below += counts[high++];
  counts.assign(kHalfKeyValues, 0);
  for_each_lag_ratio_db(power, bins, frames, delay_frames, [&](float ratio) {
    const uint32_t key = float_order_key(ratio);
    if ((key >> 16) == high) ++counts[key & 0xffffu];
  });
  uint32_t low = 0;
  while (below + counts[low] <= rank) below += counts[low++];
  return float_from_order_key((high << 16) | low);
}

}  // namespace

namespace detail {

int dereverb_late_delay_frames(const DereverbClassicalConfig& config, int sample_rate) {
  return late_delay_frames(config, sample_rate);
}

double dereverb_late_decay(const DereverbClassicalConfig& config, int delay_frames,
                           int sample_rate) {
  return late_decay(config, delay_frames, sample_rate);
}

double dereverb_subtraction_gain(double current_power, double late_psd,
                                 const DereverbClassicalConfig& config, bool* suppressed) {
  const double current = std::max(current_power, kPowerFloor);
  *suppressed = late_psd > static_cast<double>(config.threshold) * current;
  if (!*suppressed) return 1.0;
  const double clean_power =
      std::max(current - static_cast<double>(config.over_subtraction) * late_psd,
               static_cast<double>(config.spectral_floor) * current);
  const double full = std::sqrt(clean_power / current);
  // Written so attenuation == 1 leaves `full` bit for bit rather than
  // round-tripping it through 1 - (1 - full).
  return full + (1.0 - static_cast<double>(config.attenuation)) * (1.0 - full);
}

}  // namespace detail

void validate_config(const DereverbClassicalConfig& config) {
  check::in_range(config.threshold, 0.0, 1.0, "threshold");
  check::in_range(config.attenuation, 0.0, 1.0, "attenuation");
  check::power_of_two_within(config.n_fft, kDereverbNFftDomain, "n_fft");
  check::integer_in_range(config.hop_length, 1, config.n_fft / 2, "hop_length");
  check::in_interval(config.t60_sec, 0.0, false, kDereverbMaxT60Sec, true, "t60_sec");
  check::in_range(config.late_delay_ms, 0.0, kDereverbMaxLateDelayMs, "late_delay_ms");
  check::in_range(config.over_subtraction, 0.0, 16.0, "over_subtraction");
  check::in_range(config.spectral_floor, 0.0, 1.0, "spectral_floor");
  check::integer_within(config.wpe_iterations, kDereverbWpeIterationsDomain, "wpe_iterations");
  check::integer_within(config.wpe_taps, kDereverbWpeTapsDomain, "wpe_taps");
  check::in_range(config.wpe_strength, 0.0, 1.0, "wpe_strength");
  if (!common::has_reconstructible_hann_geometry(config.n_fft, config.hop_length)) {
    check::refuse_relation({"n_fft", "hop_length"},
                           "{0} and {1} must leave every Hann overlap-add sum above 1e-8");
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

namespace {

/// @brief The linked dereverberation every entry point runs; @p report, when given, receives the
///   measurements, which are off the sample path and skipped when it is null.
void dereverb_linked(const Audio* const* channels, std::size_t channel_count,
                     std::vector<Audio>* out, const DereverbClassicalConfig& config,
                     DereverbReport* report) {
  const auto validated = Validated<DereverbClassicalConfig>::make(config);
  if (out == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "dereverb output must not be null");
  }
  if (channels == nullptr || channel_count == 0 || channels[0] == nullptr || channels[0]->empty()) {
    throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  }

  // Before padding, which would make short channels of different lengths agree.
  common::validate_linked_channels(channels, channel_count);
  // The padded copies are dropped once analysed; every later buffer is spectrum-sized.
  common::LinkedSpectra linked = [&] {
    std::vector<Audio> analysis;
    std::vector<const Audio*> analysis_pointers;
    analysis.reserve(channel_count);
    analysis_pointers.reserve(channel_count);
    for (size_t c = 0; c < channel_count; ++c) {
      analysis.push_back(padded_for_analysis(*channels[c], validated->n_fft));
    }
    for (const Audio& channel : analysis) analysis_pointers.push_back(&channel);
    return common::LinkedSpectra::compute(analysis_pointers.data(), channel_count,
                                          analysis_config(validated.get()));
  }();
  if (linked.empty()) {
    out->clear();
    for (size_t c = 0; c < channel_count; ++c) out->push_back(*channels[c]);
    return;
  }

  const int bins = linked.n_bins();
  const int frames = linked.n_frames();
  const int sample_rate = channels[0]->sample_rate();
  const int delay_frames = late_delay_frames(validated.get(), sample_rate);
  const double decay = late_decay(validated.get(), delay_frames, sample_rate);

  // Read from the summed power before masking releases it.
  if (report != nullptr) {
    report->detected.late_decay_ratio_db =
        late_decay_ratio_db(linked.summed_power().data(), bins, frames, delay_frames);
  }
  // Each gain is applied as it is computed, so no mask is held beside the spectra.
  const float* power = linked.summed_power().data();
  size_t suppressed = 0;
  double reduction_db_sum = 0.0;
  auto masked = linked.take_masked([&](size_t idx) {
    bool cell_suppressed = false;
    const double gain = subtraction_gain_at(power, frames, delay_frames, decay, validated.get(),
                                            idx, &cell_suppressed);
    if (report != nullptr) {
      if (cell_suppressed) ++suppressed;
      reduction_db_sum += -20.0 * std::log10(std::max(gain, kPowerFloor));
    }
    return gain;
  });
  if (report != nullptr) {
    const double cells = static_cast<double>(bins) * static_cast<double>(frames);
    report->mean_reduction_db = static_cast<float>(reduction_db_sum / cells);
    report->suppressed_fraction = static_cast<float>(static_cast<double>(suppressed) / cells);
  }

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
    if (report != nullptr) {
      const double passes = static_cast<double>(validated->wpe_iterations);
      report->detected.late_predictability = static_cast<float>(before / passes);
      report->wpe_predictor_norm = static_cast<float>(after / passes);
    }
  }

  *out = linked.resynthesize(std::move(masked), static_cast<int>(channels[0]->size()));
}

}  // namespace

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config,
                         DereverbReport* report) {
  if (report != nullptr) *report = DereverbReport{};
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  // One channel through the linked path is the mono result: its summed power is that channel's.
  const Audio* channel = &audio;
  std::vector<Audio> out;
  dereverb_linked(&channel, 1, &out, config, report);
  return std::move(out[0]);
}

DereverbReport dereverb_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                         std::vector<Audio>* out,
                                         const DereverbClassicalConfig& config) {
  DereverbReport report;
  dereverb_linked(channels, channel_count, out, config, &report);
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
