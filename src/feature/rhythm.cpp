/// @file rhythm.cpp
/// @brief Implementation of tempogram and fourier_tempogram.

#include "feature/rhythm.h"

#include <algorithm>
#include <cmath>

#include "core/fft.h"
#include "core/window.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

namespace {

/// @brief Center-pad the onset envelope by win_length/2 with reflect-equivalent
///        zero edges (librosa uses reflect padding; we use constant zeros which
///        is adequate for tempogram parity within tolerance).
std::vector<float> pad_envelope(const std::vector<float>& env, int pad, bool center) {
  if (!center || pad <= 0) return env;
  std::vector<float> padded(env.size() + 2 * static_cast<std::size_t>(pad), 0.0f);
  std::copy(env.begin(), env.end(), padded.begin() + pad);
  return padded;
}

}  // namespace

std::vector<float> tempogram(const std::vector<float>& onset_envelope, int sr,
                             const TempogramConfig& config) {
  // The autocorrelation tempogram operates purely on the onset envelope and its
  // hop/window config; sr is kept only for API symmetry with the BPM-axis
  // helpers (tempogram_ratio, plp) that do need it.
  (void)sr;
  if (config.win_length <= 1) {
    throw SonareException(ErrorCode::InvalidParameter, "tempogram: win_length must be > 1");
  }
  if (config.hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "tempogram: hop_length must be > 0");
  }
  if (onset_envelope.empty()) return {};

  const int win = config.win_length;
  const int half = win / 2;
  const auto padded = pad_envelope(onset_envelope, half, config.center);
  const int n_frames = static_cast<int>(onset_envelope.size());
  const auto window = create_window(config.window, win);

  std::vector<float> tg(static_cast<std::size_t>(win) * static_cast<std::size_t>(n_frames), 0.0f);

  std::vector<double> frame(win);
  std::vector<double> ac(win, 0.0);
  for (int t = 0; t < n_frames; ++t) {
    const int start = config.center ? t : std::max(0, t - half);
    // Copy windowed onset slice.
    for (int i = 0; i < win; ++i) {
      const int idx = start + i;
      const double v = (idx >= 0 && idx < static_cast<int>(padded.size())) ? padded[idx] : 0.0;
      frame[i] = v * window[i];
    }
    // Compute similarity up to lag = win - 1.
    std::fill(ac.begin(), ac.end(), 0.0);
    for (int lag = 0; lag < win; ++lag) {
      double s = 0.0;
      double lhs_sq = 0.0;
      double rhs_sq = 0.0;
      for (int i = 0; i + lag < win; ++i) {
        const double lhs = frame[i];
        const double rhs = frame[i + lag];
        s += lhs * rhs;
        if (config.mode == TempogramMode::kCosine) {
          lhs_sq += lhs * lhs;
          rhs_sq += rhs * rhs;
        }
      }
      if (config.mode == TempogramMode::kCosine) {
        const double denom = std::sqrt(lhs_sq * rhs_sq);
        ac[lag] = denom > static_cast<double>(constants::kEpsilon) ? s / denom : 0.0;
      } else {
        ac[lag] = s;
      }
    }
    // Optional L2 normalize per column.
    if (config.norm) {
      double sum_sq = 0.0;
      for (int lag = 0; lag < win; ++lag) sum_sq += ac[lag] * ac[lag];
      const double n2 = std::sqrt(sum_sq);
      if (n2 > 0.0) {
        const double inv = 1.0 / n2;
        for (int lag = 0; lag < win; ++lag) ac[lag] *= inv;
      }
    }
    for (int lag = 0; lag < win; ++lag) {
      tg[static_cast<std::size_t>(lag) * static_cast<std::size_t>(n_frames) +
         static_cast<std::size_t>(t)] = static_cast<float>(ac[lag]);
    }
  }
  return tg;
}

std::vector<float> tempogram(const Audio& audio, const TempogramConfig& config) {
  MelConfig mel_cfg;
  mel_cfg.hop_length = config.hop_length;
  OnsetConfig onset_cfg;
  onset_cfg.center = config.center;
  const auto env = compute_onset_strength(audio, mel_cfg, onset_cfg);
  return tempogram(env, audio.sample_rate(), config);
}

std::vector<float> fourier_tempogram(const std::vector<float>& onset_envelope, int /*sr*/,
                                     const TempogramConfig& config) {
  if (config.win_length <= 1) {
    throw SonareException(ErrorCode::InvalidParameter, "fourier_tempogram: win_length must be > 1");
  }
  if (config.hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "fourier_tempogram: hop_length must be > 0");
  }
  if (onset_envelope.empty()) return {};

  const int win = config.win_length;
  const int n_bins = win / 2 + 1;
  const int half = win / 2;
  const auto padded = pad_envelope(onset_envelope, half, config.center);
  const int n_frames = static_cast<int>(onset_envelope.size());
  const auto window = create_window(config.window, win);

  // Use kissfft via core/fft.h.
  FFT fft(win);
  std::vector<float> spec_mag(static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames),
                              0.0f);
  std::vector<float> buf(win);
  std::vector<std::complex<float>> out(n_bins);
  for (int t = 0; t < n_frames; ++t) {
    const int start = config.center ? t : std::max(0, t - half);
    for (int i = 0; i < win; ++i) {
      const int idx = start + i;
      const float v = (idx >= 0 && idx < static_cast<int>(padded.size())) ? padded[idx] : 0.0f;
      buf[i] = v * window[i];
    }
    fft.forward(buf.data(), out.data());
    for (int b = 0; b < n_bins; ++b) {
      spec_mag[static_cast<std::size_t>(b) * static_cast<std::size_t>(n_frames) +
               static_cast<std::size_t>(t)] = std::abs(out[b]);
    }
  }
  return spec_mag;
}

std::vector<float> fourier_tempogram(const Audio& audio, const TempogramConfig& config) {
  MelConfig mel_cfg;
  mel_cfg.hop_length = config.hop_length;
  OnsetConfig onset_cfg;
  onset_cfg.center = config.center;
  const auto env = compute_onset_strength(audio, mel_cfg, onset_cfg);
  return fourier_tempogram(env, audio.sample_rate(), config);
}

std::vector<float> cyclic_tempogram(const std::vector<float>& onset_envelope, int sr,
                                    const TempogramConfig& config, float bpm_min, int n_bins) {
  if (sr <= 0 || config.hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cyclic_tempogram: sr and hop_length must be > 0");
  }
  if (bpm_min <= 0.0f || n_bins <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "cyclic_tempogram: bpm_min and n_bins must be > 0");
  }
  if (onset_envelope.empty()) return {};

  const std::vector<float> ft = fourier_tempogram(onset_envelope, sr, config);
  const int n_frames = static_cast<int>(onset_envelope.size());
  const int fourier_bins = config.win_length / 2 + 1;
  std::vector<float> cyclic(static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames),
                            0.0f);

  const double octave_width = std::log(2.0);
  for (int bin = 1; bin < fourier_bins; ++bin) {
    const double bpm = static_cast<double>(bin) / static_cast<double>(config.win_length) * 60.0 *
                       static_cast<double>(sr) / static_cast<double>(config.hop_length);
    if (bpm <= 0.0) continue;
    double phase = std::fmod(std::log(bpm / static_cast<double>(bpm_min)) / octave_width, 1.0);
    if (phase < 0.0) phase += 1.0;
    const int cyclic_bin = std::clamp(
        static_cast<int>(std::round(phase * static_cast<double>(n_bins))) % n_bins, 0, n_bins - 1);
    for (int frame = 0; frame < n_frames; ++frame) {
      cyclic[static_cast<std::size_t>(cyclic_bin) * static_cast<std::size_t>(n_frames) +
             static_cast<std::size_t>(frame)] +=
          ft[static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames) +
             static_cast<std::size_t>(frame)];
    }
  }

  return cyclic;
}

std::vector<float> cyclic_tempogram(const Audio& audio, const TempogramConfig& config,
                                    float bpm_min, int n_bins) {
  MelConfig mel_cfg;
  mel_cfg.hop_length = config.hop_length;
  OnsetConfig onset_cfg;
  onset_cfg.center = config.center;
  const auto env = compute_onset_strength(audio, mel_cfg, onset_cfg);
  return cyclic_tempogram(env, audio.sample_rate(), config, bpm_min, n_bins);
}

std::vector<float> plp(const std::vector<float>& onset_envelope, const PlpConfig& config) {
  if (config.win_length <= 1) {
    throw SonareException(ErrorCode::InvalidParameter, "plp: win_length must be > 1");
  }
  if (config.hop_length <= 0 || config.sr <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "plp: sr and hop_length must be > 0");
  }
  const int n = static_cast<int>(onset_envelope.size());
  if (n == 0) return {};

  // librosa.beat.plp picks the dominant tempo per frame from a Fourier
  // tempogram restricted to [tempo_min, tempo_max].
  TempogramConfig tcfg;
  tcfg.hop_length = config.hop_length;
  tcfg.win_length = config.win_length;
  tcfg.window = WindowType::Hann;
  tcfg.center = true;
  tcfg.norm = false;
  const std::vector<float> ft = fourier_tempogram(onset_envelope, config.sr, tcfg);
  const int n_bins = config.win_length / 2 + 1;
  const int n_frames = n;

  // Tempo per Fourier bin = bin_freq * 60 * sr / hop_length (in BPM).
  auto bin_to_bpm = [&](int b) {
    const double freq = static_cast<double>(b) / static_cast<double>(config.win_length);
    return freq * 60.0 * static_cast<double>(config.sr) / static_cast<double>(config.hop_length);
  };

  // Restrict to tempo band, pick argmax per frame to define mask.
  std::vector<int> argmax(n_frames, 0);
  for (int t = 0; t < n_frames; ++t) {
    float best = -1.0f;
    int best_b = 0;
    for (int b = 1; b < n_bins; ++b) {
      double bpm = bin_to_bpm(b);
      if (bpm < config.tempo_min || bpm > config.tempo_max) continue;
      float v = ft[b * n_frames + t];
      if (v > best) {
        best = v;
        best_b = b;
      }
    }
    argmax[t] = best_b;
  }

  // PLP: place a sinusoid with frame-local tempo and per-frame magnitude back
  // into the onset envelope time domain (additive overlap-add, no FFT inverse).
  std::vector<double> pulse(n, 0.0);
  std::vector<double> window_kernel(config.win_length);
  for (int i = 0; i < config.win_length; ++i) {
    window_kernel[i] =
        0.5 * (1.0 - std::cos(constants::kTwoPiD * i / static_cast<double>(config.win_length)));
  }
  const int half = config.win_length / 2;
  for (int t = 0; t < n_frames; ++t) {
    int b = argmax[t];
    if (b == 0) continue;
    double omega =
        constants::kTwoPiD * static_cast<double>(b) / static_cast<double>(config.win_length);
    float mag = ft[b * n_frames + t];
    for (int i = 0; i < config.win_length; ++i) {
      int idx = t + i - half;
      if (idx < 0 || idx >= n) continue;
      pulse[idx] += mag * window_kernel[i] * std::cos(omega * (i - half));
    }
  }
  // Half-wave rectification + L-inf normalize (mirrors librosa).
  std::vector<float> out(n, 0.0f);
  double maxv = 0.0;
  for (int i = 0; i < n; ++i) {
    double v = std::max(0.0, pulse[i]);
    out[i] = static_cast<float>(v);
    if (v > maxv) maxv = v;
  }
  if (maxv > 0.0) {
    for (int i = 0; i < n; ++i) out[i] = static_cast<float>(out[i] / maxv);
  }
  return out;
}

std::vector<float> plp(const Audio& audio, const PlpConfig& config) {
  MelConfig mel_cfg;
  mel_cfg.hop_length = config.hop_length;
  OnsetConfig onset_cfg;
  onset_cfg.center = true;
  const auto env = compute_onset_strength(audio, mel_cfg, onset_cfg);
  PlpConfig adjusted = config;
  adjusted.sr = audio.sample_rate();
  return plp(env, adjusted);
}

std::vector<float> tempogram_ratio(const std::vector<float>& tempogram_data, int win_length, int sr,
                                   int hop_length, const std::vector<float>& factors) {
  if (win_length <= 0 || hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "tempogram_ratio: win_length and hop_length must be > 0");
  }
  std::vector<float> out(factors.size(), 0.0f);
  if (tempogram_data.empty() || factors.empty()) return out;
  const int n_frames =
      static_cast<int>(tempogram_data.size() / static_cast<std::size_t>(win_length));
  if (n_frames <= 0) return out;

  // Estimate a reference tempo: pick the lag with the strongest mean energy.
  std::vector<double> lag_mean(win_length, 0.0);
  for (int lag = 0; lag < win_length; ++lag) {
    double s = 0.0;
    for (int t = 0; t < n_frames; ++t) {
      s += tempogram_data[static_cast<std::size_t>(lag) * static_cast<std::size_t>(n_frames) +
                          static_cast<std::size_t>(t)];
    }
    lag_mean[lag] = s / static_cast<double>(n_frames);
  }
  // Skip lag 0 (DC autocorrelation peak) when picking the reference.
  int best_lag = 1;
  double best_val = -1.0;
  for (int lag = 1; lag < win_length; ++lag) {
    if (lag_mean[lag] > best_val) {
      best_val = lag_mean[lag];
      best_lag = lag;
    }
  }

  // Reference tempo in BPM is 60 * sr / (best_lag * hop_length); ratios scale lag.
  for (std::size_t k = 0; k < factors.size(); ++k) {
    const float f = factors[k];
    if (f <= 0.0f) continue;
    // Tempo scales inversely with lag: factor 2 (tempo*2) => half lag.
    const int lag = static_cast<int>(std::round(best_lag / f));
    if (lag < 0 || lag >= win_length) continue;
    out[k] = static_cast<float>(lag_mean[lag]);
  }
  (void)sr;  // sr currently unused; kept for future tempo-axis scaling.
  return out;
}

}  // namespace sonare
