/// @file audio_profile.cpp
/// @brief Mastering assistant audio profiling implementation.

#include "mastering/assistant/audio_profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#include "analysis/bpm_analyzer.h"
#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "metering/basic.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/json.h"

namespace sonare::mastering::assistant {

using sonare::constants::kEpsilon;
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
  return power_to_db_scalar(power);
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

// Infer up to three best-matching genre labels from coarse audio features.
//
// Each emitted label is a catalogue preset identifier (see api::Preset), so a
// suggested genre maps 1:1 onto a dedicated mastering voicing via
// preset_for_genre(). The labels cover the music-oriented presets that can be
// discriminated from BPM / spectral / dynamics alone:
//   pop, edm, techno, trance, drumAndBass, hipHop, trap, rnb, metal, jazz,
//   acoustic, classical, ambient, lofi, jpop, kpop, gameOst, speech.
// The delivery / platform presets (streaming, youtube, broadcast, podcast,
// audiobook, cinema, aiMusic) are intentionally NOT inferred from audio; they
// are selected via AssistantConfig.target_platform / explicit preset choice
// because they describe a distribution target, not the program material's
// character. They remain reachable through preset_for_genre().
std::vector<GenreCandidate> infer_genres(float bpm, const SpectralProfile& spectral,
                                         const DynamicsProfile& dynamics) {
  std::vector<GenreCandidate> out;
  const float low_bias = spectral.low_rms_db - spectral.mid_rms_db;
  const float air_bias = spectral.air_rms_db - spectral.mid_rms_db;
  const float bright = spectral.centroid_hz > 2500.0f ? 1.0f : spectral.centroid_hz / 2500.0f;
  const bool has_bpm = bpm > 0.0f;

  // ---------------------------------------------------------------------------
  // Scoring philosophy
  // ---------------------------------------------------------------------------
  // The six "primary" buckets (edm, hipHop, pop, ambient, classical, speech)
  // carry strong, broadly-applicable baselines so they remain robust top-3
  // candidates for typical material. The "sibling" buckets (techno, trance,
  // drumAndBass, trap, rnb, metal, jazz, acoustic, lofi, jpop, kpop, gameOst)
  // are refinements of a primary: each is *gated* on its single most
  // distinctive feature (a tempo window, an air/brightness signature, a
  // dynamics signature). When that gate is absent the sibling collapses to a
  // small floor so it never out-ranks the primary on generic material; when the
  // gate fires strongly the sibling can rise into the top-3. This keeps every
  // sibling reachable without letting weak, always-on bonuses displace the
  // obvious primary genre.
  add_candidate(out, "edm",
                (bpm >= 118.0f && bpm <= 150.0f ? 0.45f : 0.10f) +
                    (low_bias > -8.0f ? 0.25f : 0.0f) +
                    (dynamics.attack_density > 1.5f ? 0.20f : 0.0f) + 0.10f * bright);
  // Techno: gated on the four-on-the-floor tempo window; only then does its
  // darker / bass-forward signature add. Stays a refinement below edm.
  {
    const bool gate = bpm >= 122.0f && bpm <= 135.0f;
    add_candidate(out, "techno",
                  gate ? (0.30f + (low_bias > -4.0f ? 0.10f : 0.0f) +
                          (spectral.centroid_hz < 1500.0f ? 0.08f : 0.0f) +
                          (air_bias > -20.0f ? 0.05f : 0.0f))
                       : 0.05f);
  }
  // Trance: gated on a faster tempo window plus an airy/bright signature.
  {
    const bool gate = bpm >= 136.0f && bpm <= 145.0f && air_bias > -14.0f;
    add_candidate(
        out, "trance",
        gate ? (0.40f + 0.15f * bright + (dynamics.sustain_ratio > 0.5f ? 0.10f : 0.0f)) : 0.05f);
  }
  // Drum and bass: gated on a very fast tempo window.
  {
    const bool gate = bpm >= 160.0f && bpm <= 180.0f;
    add_candidate(out, "drumAndBass",
                  gate ? (0.45f + (low_bias > -6.0f ? 0.15f : 0.0f) +
                          (dynamics.attack_density > 2.0f ? 0.15f : 0.0f))
                       : 0.05f);
  }
  add_candidate(out, "hipHop",
                (bpm >= 70.0f && bpm <= 105.0f ? 0.35f : 0.10f) +
                    (low_bias > -6.0f ? 0.35f : 0.0f) +
                    (spectral.centroid_hz < 2500.0f ? 0.15f : 0.0f));
  // Trap: gated on hip-hop tempo AND very strong sub-bass with dense hi-hat
  // transients (both required) so it does not undercut plain hipHop on
  // moderate-bass material.
  {
    const bool gate =
        bpm >= 60.0f && bpm <= 90.0f && low_bias > 0.0f && dynamics.attack_density > 1.8f;
    add_candidate(out, "trap", gate ? 0.55f : 0.05f);
  }
  // R&B: gated on mid-tempo + smooth (low attack density); a refinement of pop.
  {
    const bool gate = bpm >= 60.0f && bpm <= 100.0f && dynamics.attack_density < 1.2f;
    add_candidate(
        out, "rnb",
        gate ? (0.30f + (low_bias > -8.0f && low_bias < 2.0f ? 0.10f : 0.0f) + 0.10f * bright)
             : 0.05f);
  }
  // Metal: gated on fast tempo + dense transients + bright + heavily-limited
  // (low short-term variation), all required.
  {
    const bool gate = bpm >= 100.0f && bpm <= 200.0f && dynamics.attack_density > 2.0f &&
                      spectral.centroid_hz > 2000.0f;
    add_candidate(
        out, "metal",
        gate ? (0.45f + 0.15f * bright + (dynamics.short_term_lufs_std < 3.0f ? 0.10f : 0.0f))
             : 0.05f);
  }
  // Jazz: gated on genuine dynamics (wide short-term variation) plus low
  // flatness and sparse transients; a refinement of acoustic.
  {
    const bool gate = dynamics.short_term_lufs_std > 3.0f && spectral.flatness < 0.2f &&
                      dynamics.attack_density < 1.5f;
    add_candidate(out, "jazz",
                  gate ? (0.45f + (bpm > 0.0f && bpm < 160.0f ? 0.10f : 0.0f)) : 0.05f);
  }
  // Acoustic: gated on dynamics + low flatness + sparse transients + not
  // bass-forward.
  {
    const bool gate = spectral.flatness < 0.2f && dynamics.attack_density < 1.0f &&
                      low_bias < -2.0f && dynamics.short_term_lufs_std > 2.0f;
    add_candidate(out, "acoustic", gate ? 0.55f : 0.05f);
  }
  add_candidate(out, "classical",
                (dynamics.short_term_lufs_std > 4.0f ? 0.35f : 0.05f) +
                    (dynamics.attack_density < 1.2f ? 0.25f : 0.0f) +
                    (spectral.flatness < 0.15f ? 0.20f : 0.0f) +
                    (bpm > 0.0f && bpm < 120.0f ? 0.10f : 0.0f));
  add_candidate(
      out, "speech",
      0.20f + (dynamics.attack_density < 0.8f ? 0.10f : 0.0f) +
          (spectral.centroid_hz > 600.0f && spectral.centroid_hz < 2600.0f ? 0.40f : 0.0f) +
          (air_bias < -30.0f ? 0.10f : 0.0f));
  add_candidate(out, "pop",
                0.30f + (bpm >= 90.0f && bpm <= 130.0f ? 0.25f : 0.0f) + 0.20f * bright +
                    (air_bias > -18.0f ? 0.10f : 0.0f));
  // J-pop / K-pop: pop tempo plus a markedly brighter / airier signature
  // (gated), so generic pop is not displaced on dull material.
  {
    const bool gate = bpm >= 95.0f && bpm <= 135.0f && air_bias > -12.0f;
    add_candidate(out, "jpop", gate ? (0.40f + 0.20f * bright) : 0.05f);
  }
  {
    const bool gate = bpm >= 100.0f && bpm <= 132.0f && air_bias > -10.0f && low_bias > -6.0f;
    add_candidate(out, "kpop", gate ? (0.40f + 0.20f * bright) : 0.05f);
  }
  add_candidate(out, "ambient",
                (dynamics.attack_density < 0.5f ? 0.35f : 0.0f) +
                    (dynamics.sustain_ratio > 0.75f ? 0.30f : 0.0f) +
                    (spectral.centroid_hz < 1800.0f ? 0.15f : 0.0f));
  // Lo-fi: gated on a dull spectrum (rolled-off highs) AND a low centroid AND
  // sparse transients together, so it does not fire on every bass-heavy tone.
  {
    const bool gate =
        air_bias < -22.0f && spectral.centroid_hz < 1600.0f && dynamics.attack_density < 1.2f;
    add_candidate(out, "lofi", gate ? 0.50f : 0.05f);
  }
  // Game OST: gated on wide dynamics with a (near-)beatless or loosely rhythmic
  // feel; cinematic refinement.
  {
    const bool gate =
        dynamics.short_term_lufs_std > 3.0f && (!has_bpm || dynamics.attack_density < 1.0f);
    add_candidate(out, "gameOst",
                  gate ? (0.40f + (spectral.flatness < 0.25f ? 0.10f : 0.0f) +
                          (air_bias > -20.0f ? 0.10f : 0.0f))
                       : 0.05f);
  }

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
  // Detrend (DC-remove) the onset envelope so the attack-density peak picking
  // discriminates transient bursts from steady energy. This consumer opts in
  // explicitly; the public OnsetConfig default is detrend=false (librosa).
  OnsetConfig onset_config;
  onset_config.detrend = true;
  const auto onset = compute_onset_strength(audio, mel_config, onset_config);
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

std::string audio_profile_to_json(const AudioProfile& profile) {
  namespace json = sonare::util::json;

  json::Object loudness;
  loudness.emplace("integratedLufs", json::Value(profile.loudness.integrated_lufs));
  loudness.emplace("lraLu", json::Value(profile.loudness.lra_lu));
  loudness.emplace("truePeakDb", json::Value(profile.loudness.true_peak_db));
  loudness.emplace("crestFactorDb", json::Value(profile.loudness.crest_factor_db));

  json::Object spectral;
  spectral.emplace("subRmsDb", json::Value(profile.spectral.sub_rms_db));
  spectral.emplace("lowRmsDb", json::Value(profile.spectral.low_rms_db));
  spectral.emplace("lowMidRmsDb", json::Value(profile.spectral.low_mid_rms_db));
  spectral.emplace("midRmsDb", json::Value(profile.spectral.mid_rms_db));
  spectral.emplace("highMidRmsDb", json::Value(profile.spectral.high_mid_rms_db));
  spectral.emplace("highRmsDb", json::Value(profile.spectral.high_rms_db));
  spectral.emplace("airRmsDb", json::Value(profile.spectral.air_rms_db));
  spectral.emplace("centroidHz", json::Value(profile.spectral.centroid_hz));
  spectral.emplace("flatness", json::Value(profile.spectral.flatness));
  spectral.emplace("rolloffHz", json::Value(profile.spectral.rolloff_hz));

  json::Object dynamics;
  dynamics.emplace("shortTermLufsStd", json::Value(profile.dynamics.short_term_lufs_std));
  dynamics.emplace("attackDensity", json::Value(profile.dynamics.attack_density));
  dynamics.emplace("sustainRatio", json::Value(profile.dynamics.sustain_ratio));

  json::Array genre_candidates;
  genre_candidates.reserve(profile.genre_candidates.size());
  for (const auto& candidate : profile.genre_candidates) {
    json::Object entry;
    entry.emplace("name", json::Value(candidate.name));
    entry.emplace("score", json::Value(candidate.score));
    genre_candidates.emplace_back(json::Value(std::move(entry)));
  }

  json::Object root;
  root.emplace("durationSec", json::Value(profile.duration_sec));
  root.emplace("bpm", json::Value(profile.bpm));
  root.emplace("bpmConfidence", json::Value(profile.bpm_confidence));
  root.emplace("loudness", json::Value(std::move(loudness)));
  root.emplace("spectral", json::Value(std::move(spectral)));
  root.emplace("dynamics", json::Value(std::move(dynamics)));
  root.emplace("genreCandidates", json::Value(std::move(genre_candidates)));
  return json::dump(json::Value(std::move(root)));
}

}  // namespace sonare::mastering::assistant
