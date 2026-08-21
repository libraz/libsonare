#include "feature/onset.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

// librosa power_to_db amin floor (1e-10); equals the generic epsilon value.
constexpr float kAmin = constants::kEpsilon;

}  // namespace

std::vector<float> compute_onset_strength(const MelSpectrogram& mel_spec,
                                          const OnsetConfig& config) {
  SONARE_CHECK(!mel_spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(config.lag >= 1, ErrorCode::InvalidParameter);

  int n_mels = mel_spec.n_mels();
  int n_frames = mel_spec.n_frames();
  const float* power = mel_spec.power_data();

  /// Map power data to Eigen matrix [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> power_map(
      power, n_mels, n_frames);

  /// Convert power to decibels before differencing.
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> log_power =
      power_map.array().max(kAmin).log() * constants::kPowerToDbScale;
  float max_db = log_power.maxCoeff();
  log_power = (log_power.array() - max_db).max(-constants::kDefaultTopDb);

  /// @details Compute first-order difference and half-wave rectification.
  /// diff = log_power[:, lag:] - log_power[:, :-lag]
  std::vector<float> onset_env(n_frames, 0.0f);

  if (n_frames > config.lag) {
    int diff_frames = n_frames - config.lag;

    // Compute difference matrix
    Eigen::MatrixXf diff = log_power.rightCols(diff_frames) - log_power.leftCols(diff_frames);

    // Half-wave rectification and column mean
    Eigen::Map<Eigen::VectorXf> env_map(onset_env.data() + config.lag, diff_frames);
    env_map = diff.array().max(0.0f).colwise().mean();
  }

  /// Detrend: remove mean. We compute the mean over the valid range
  /// ``[lag, n_frames)`` only — the leading ``lag`` samples are zero-pad from
  /// the difference operation and must not bias the mean. Subtracting from the
  /// same valid range keeps the padded prefix at 0, so a subsequent
  /// center-frame shift (applied by the Audio overload) does not pull negative
  /// values into the output. librosa applies a DC-blocker IIR filter at this
  /// point; mean-subtraction is the intentional libsonare-equivalent
  /// approximation (full IIR parity is deliberately not implemented here), and
  /// excluding the prefix matches librosa's behavior of running the filter on
  /// the post-aggregation, post-padding envelope without leading-zero bias.
  if (config.detrend && n_frames > config.lag) {
    const int valid_len = n_frames - config.lag;
    Eigen::Map<Eigen::ArrayXf> valid_view(onset_env.data() + config.lag, valid_len);
    valid_view -= valid_view.mean();
  }

  return onset_env;
}

std::vector<float> center_onset_strength(std::vector<float> onset_env, int n_fft, int hop_length,
                                         bool center) {
  if (!center || onset_env.empty() || hop_length <= 0) return onset_env;

  // Frames to right-shift so onset spikes line up with centered STFT frame
  // times. Integer division deliberately matches librosa's floor division.
  const int frame_offset = n_fft / (2 * hop_length);
  if (frame_offset <= 0) return onset_env;

  // Keep the envelope length equal to the Mel frame count: downstream
  // beat/tempo/meter analyzers rely on that fixed size, so terminal frames
  // shifted past the end are intentionally discarded.
  std::vector<float> shifted(onset_env.size(), 0.0f);
  for (size_t i = 0; i < onset_env.size(); ++i) {
    const size_t shifted_index = i + static_cast<size_t>(frame_offset);
    if (shifted_index < shifted.size()) shifted[shifted_index] = onset_env[i];
  }
  return shifted;
}

std::vector<float> compute_onset_strength(const Audio& audio, const MelConfig& mel_config,
                                          const OnsetConfig& onset_config) {
  MelConfig aligned_mel_config = mel_config;
  aligned_mel_config.center = onset_config.center;
  MelSpectrogram mel_spec = MelSpectrogram::compute(audio, aligned_mel_config);
  std::vector<float> onset_env = compute_onset_strength(mel_spec, onset_config);

  return center_onset_strength(std::move(onset_env), aligned_mel_config.n_fft,
                               aligned_mel_config.hop_length, onset_config.center);
}

std::vector<float> onset_strength_multi(const MelSpectrogram& mel_spec, int n_bands,
                                        const OnsetConfig& config) {
  SONARE_CHECK(!mel_spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bands > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.lag >= 1, ErrorCode::InvalidParameter);

  int n_mels = mel_spec.n_mels();
  int n_frames = mel_spec.n_frames();
  // More bands than Mel bins would give zero-width bands whose column mean is
  // NaN; require at least one Mel bin per band.
  SONARE_CHECK(n_bands <= n_mels, ErrorCode::InvalidParameter);
  const float* power = mel_spec.power_data();

  // Map power data to Eigen matrix [n_mels x n_frames] (row-major)
  Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> power_map(
      power, n_mels, n_frames);

  // Compute log power using Eigen
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> log_power =
      power_map.array().max(kAmin).log() * constants::kPowerToDbScale;
  float max_db = log_power.maxCoeff();
  log_power = (log_power.array() - max_db).max(-constants::kDefaultTopDb);

  // Divide Mel bands into n_bands groups
  int mels_per_band = n_mels / n_bands;

  // Output: [n_bands x n_frames]
  std::vector<float> onset_multi(n_bands * n_frames, 0.0f);

  if (n_frames > config.lag) {
    int diff_frames = n_frames - config.lag;

    for (int b = 0; b < n_bands; ++b) {
      int mel_start = b * mels_per_band;
      int mel_end = (b == n_bands - 1) ? n_mels : (b + 1) * mels_per_band;
      int band_size = mel_end - mel_start;

      // Extract band and compute difference
      Eigen::MatrixXf band_diff = log_power.block(mel_start, config.lag, band_size, diff_frames) -
                                  log_power.block(mel_start, 0, band_size, diff_frames);

      // Half-wave rectification and column mean
      Eigen::Map<Eigen::VectorXf> out_map(onset_multi.data() + b * n_frames + config.lag,
                                          diff_frames);
      out_map = band_diff.array().max(0.0f).colwise().mean();
    }
  }

  // Apply detrend per band, excluding the leading ``lag`` zero prefix so it
  // does not bias the per-band mean. Subtraction stays inside the valid range
  // to keep the padded prefix at 0 (see ``compute_onset_strength`` above).
  if (config.detrend && n_frames > config.lag) {
    const int valid_len = n_frames - config.lag;
    for (int b = 0; b < n_bands; ++b) {
      Eigen::Map<Eigen::ArrayXf> valid_row(onset_multi.data() + b * n_frames + config.lag,
                                           valid_len);
      valid_row -= valid_row.mean();
    }
  }

  return onset_multi;
}

std::vector<int> onset_backtrack(const std::vector<int>& events, const std::vector<float>& energy) {
  std::vector<int> out;
  out.reserve(events.size());
  if (energy.empty()) {
    return out;
  }
  const int n = static_cast<int>(energy.size());

  // librosa.onset.onset_backtrack builds the set of local minima first and then
  // matches each event to the nearest one at or before it. An interior index is
  // a minimum when the curve is non-increasing into it AND strictly rising out
  // of it; index 0 is always a candidate so an event with no preceding minimum
  // still resolves.
  //
  // Both halves of that test matter. Walking backwards while the previous
  // sample is merely not larger stops only where the curve rises to the left,
  // which is the LEFT edge of a flat run rather than its right edge. The two
  // rules agree on a strictly monotone curve and disagree on every plateau: for
  // energy {0, 0, 0, 0, 0.1, 0.5, 1.0} and event 6, librosa answers 3 and the
  // leftward walk answers 0. On a one-shot preceded by silence that is the
  // difference between the real attack and t = 0.
  std::vector<int> minima;
  minima.reserve(static_cast<size_t>(n));
  minima.push_back(0);
  for (int i = 1; i + 1 < n; ++i) {
    if (energy[static_cast<size_t>(i)] <= energy[static_cast<size_t>(i - 1)] &&
        energy[static_cast<size_t>(i)] < energy[static_cast<size_t>(i + 1)]) {
      minima.push_back(i);
    }
  }

  for (int e : events) {
    const int clamped = std::min(std::max(e, 0), n - 1);
    // The last minimum at or before the event. minima is ascending and always
    // starts at 0, so the search never comes back empty.
    const auto after = std::upper_bound(minima.begin(), minima.end(), clamped);
    out.push_back(*(after - 1));
  }
  return out;
}

std::vector<float> spectral_flux(const Spectrogram& spec, int lag) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(lag >= 1, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames = spec.n_frames();
  const std::vector<float>& magnitude = spec.magnitude();

  std::vector<float> flux(n_frames, 0.0f);

  if (n_frames > lag) {
    int diff_frames = n_frames - lag;
    // Sum of |mag[:, f+lag] - mag[:, f]| over bins (auto-vectorized, on par with Eigen).
    // magnitude is row-major [n_bins x n_frames].
    for (int f = 0; f < diff_frames; ++f) {
      float sum = 0.0f;
      for (int b = 0; b < n_bins; ++b) {
        const float d = magnitude[b * n_frames + (f + lag)] - magnitude[b * n_frames + f];
        sum += std::abs(d);
      }
      flux[f + lag] = sum;
    }
  }

  return flux;
}

}  // namespace sonare
