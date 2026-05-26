/// @file audio_profile.cpp
/// @brief Mastering assistant audio profiling implementation.

#include "mastering/assistant/audio_profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "analysis/bpm_analyzer.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "metering/basic.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/constants.h"

namespace sonare::mastering::assistant {
namespace {

constexpr float kMinDb = sonare::constants::kFloorDb;

float mean_finite(const std::vector<float>& values) {
  double sum = 0.0;
  int count = 0;
  for (float value : values) {
    if (std::isfinite(value)) {
      sum += value;
      ++count;
    }
  }
  return count > 0 ? static_cast<float>(sum / count) : 0.0f;
}

float stddev_finite(const std::vector<float>& values) {
  const float mean = mean_finite(values);
  double sum = 0.0;
  int count = 0;
  for (float value : values) {
    if (std::isfinite(value)) {
      const double diff = static_cast<double>(value) - mean;
      sum += diff * diff;
      ++count;
    }
  }
  return count > 1 ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
}

float power_to_db(double power) {
  if (power <= 1.0e-20) return kMinDb;
  return static_cast<float>(10.0 * std::log10(power));
}

float band_rms_db(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft, int sr,
                  float min_hz, float max_hz) {
  double power_sum = 0.0;
  int count = 0;
  for (int bin = 0; bin < n_bins; ++bin) {
    const float hz = static_cast<float>(bin) * static_cast<float>(sr) / static_cast<float>(n_fft);
    if (hz < min_hz || hz >= max_hz) continue;
    for (int frame = 0; frame < n_frames; ++frame) {
      const float mag = magnitude[static_cast<size_t>(bin) * n_frames + frame];
      power_sum += static_cast<double>(mag) * mag;
      ++count;
    }
  }
  if (count == 0) return kMinDb;
  return power_to_db(power_sum / count);
}

float attack_density(const std::vector<float>& onset, float duration_sec) {
  if (onset.size() < 3 || duration_sec <= 0.0f) return 0.0f;
  const float max_value = *std::max_element(onset.begin(), onset.end());
  if (max_value <= sonare::constants::kEpsilon) return 0.0f;
  const float threshold = max_value * 0.30f;
  int peaks = 0;
  for (size_t i = 1; i + 1 < onset.size(); ++i) {
    if (onset[i] > threshold && onset[i] >= onset[i - 1] && onset[i] > onset[i + 1]) {
      ++peaks;
    }
  }
  return static_cast<float>(peaks) / duration_sec;
}

float sustain_ratio(const std::vector<float>& rms) {
  if (rms.empty()) return 0.0f;
  const float max_value = *std::max_element(rms.begin(), rms.end());
  if (max_value <= sonare::constants::kSpectrumEpsilon) return 0.0f;
  int sustained = 0;
  for (float value : rms) {
    if (value >= max_value * 0.35f) ++sustained;
  }
  return static_cast<float>(sustained) / static_cast<float>(rms.size());
}

void add_candidate(std::vector<GenreCandidate>& candidates, std::string name, float score) {
  candidates.push_back(GenreCandidate{std::move(name), std::clamp(score, 0.0f, 1.0f)});
}

std::vector<GenreCandidate> infer_genres(float bpm, const SpectralProfile& spectral,
                                         const DynamicsProfile& dynamics) {
  std::vector<GenreCandidate> out;
  const float low_bias = spectral.low_rms_db - spectral.mid_rms_db;
  const float air_bias = spectral.air_rms_db - spectral.mid_rms_db;
  const float bright = spectral.centroid_hz > 2500.0f ? 1.0f : spectral.centroid_hz / 2500.0f;

  add_candidate(out, "edm",
                (bpm >= 118.0f && bpm <= 150.0f ? 0.45f : 0.10f) +
                    (low_bias > -8.0f ? 0.25f : 0.0f) +
                    (dynamics.attack_density > 1.5f ? 0.20f : 0.0f) + 0.10f * bright);
  add_candidate(out, "hipHop",
                (bpm >= 70.0f && bpm <= 105.0f ? 0.35f : 0.10f) +
                    (low_bias > -6.0f ? 0.35f : 0.0f) +
                    (spectral.centroid_hz < 2500.0f ? 0.15f : 0.0f));
  add_candidate(out, "classical",
                (dynamics.short_term_lufs_std > 4.0f ? 0.35f : 0.05f) +
                    (dynamics.attack_density < 1.2f ? 0.25f : 0.0f) +
                    (spectral.flatness < 0.15f ? 0.20f : 0.0f) +
                    (bpm > 0.0f && bpm < 120.0f ? 0.10f : 0.0f));
  add_candidate(
      out, "speech",
      (bpm <= 0.0f || dynamics.attack_density < 0.8f ? 0.20f : 0.0f) +
          (spectral.centroid_hz > 600.0f && spectral.centroid_hz < 2600.0f ? 0.35f : 0.0f) +
          (spectral.low_rms_db < spectral.mid_rms_db ? 0.15f : 0.0f));
  add_candidate(out, "pop",
                0.25f + (bpm >= 90.0f && bpm <= 130.0f ? 0.25f : 0.0f) + 0.20f * bright +
                    (air_bias > -18.0f ? 0.10f : 0.0f));
  add_candidate(out, "ambient",
                (dynamics.attack_density < 0.5f ? 0.35f : 0.0f) +
                    (dynamics.sustain_ratio > 0.75f ? 0.30f : 0.0f) +
                    (spectral.centroid_hz < 1800.0f ? 0.15f : 0.0f));

  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.score == b.score) return a.name < b.name;
    return a.score > b.score;
  });
  if (out.size() > 3) out.resize(3);
  return out;
}

}  // namespace

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config) {
  if (samples == nullptr || length == 0 || sample_rate <= 0) return AudioProfile{};
  return analyze_audio_profile(Audio::from_buffer(samples, length, sample_rate), config);
}

AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config) {
  AudioProfile profile;
  if (audio.empty() || audio.sample_rate() <= 0) return profile;

  profile.duration_sec = audio.duration();

  const auto loudness = metering::lufs(audio);
  profile.loudness.integrated_lufs = loudness.integrated_lufs;
  profile.loudness.lra_lu = loudness.loudness_range;
  profile.loudness.true_peak_db = metering::true_peak_db(audio, config.true_peak_oversample);
  profile.loudness.crest_factor_db = metering::crest_factor_db(audio);

  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);
  const auto& mag = spec.magnitude();
  profile.spectral.sub_rms_db =
      band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(), audio.sample_rate(), 20, 60);
  profile.spectral.low_rms_db =
      band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(), audio.sample_rate(), 60, 250);
  profile.spectral.low_mid_rms_db =
      band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(), audio.sample_rate(), 250, 500);
  profile.spectral.mid_rms_db = band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                            audio.sample_rate(), 500, 2000);
  profile.spectral.high_mid_rms_db = band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                                 audio.sample_rate(), 2000, 6000);
  profile.spectral.high_rms_db = band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                             audio.sample_rate(), 6000, 12000);
  profile.spectral.air_rms_db =
      band_rms_db(mag, spec.n_bins(), spec.n_frames(), spec.n_fft(), audio.sample_rate(), 12000,
                  static_cast<float>(audio.sample_rate()) * 0.5f + 1.0f);
  profile.spectral.centroid_hz = mean_finite(spectral_centroid(spec, audio.sample_rate()));
  profile.spectral.flatness = mean_finite(spectral_flatness(spec));
  profile.spectral.rolloff_hz = mean_finite(spectral_rolloff(spec, audio.sample_rate()));

  const auto short_term = metering::short_term_lufs(audio);
  profile.dynamics.short_term_lufs_std = stddev_finite(short_term);

  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  const auto onset = compute_onset_strength(audio, mel_config, OnsetConfig{});
  profile.dynamics.attack_density = attack_density(onset, profile.duration_sec);
  profile.dynamics.sustain_ratio =
      sustain_ratio(rms_energy(audio, config.n_fft, config.hop_length));

  try {
    BpmConfig bpm_config;
    bpm_config.n_fft = config.n_fft;
    bpm_config.hop_length = config.hop_length;
    BpmAnalyzer bpm(onset, audio.sample_rate(), config.hop_length, bpm_config);
    profile.bpm = bpm.bpm();
    profile.bpm_confidence = bpm.confidence();
  } catch (...) {
    profile.bpm = 0.0f;
    profile.bpm_confidence = 0.0f;
  }

  profile.genre_candidates = infer_genres(profile.bpm, profile.spectral, profile.dynamics);
  return profile;
}

}  // namespace sonare::mastering::assistant
