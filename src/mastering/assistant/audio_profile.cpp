/// @file audio_profile.cpp
/// @brief Mastering assistant audio profiling implementation.

#include "mastering/assistant/audio_profile.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/bpm_analyzer.h"
#include "core/spectrum.h"
#include "core/window.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "metering/basic.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/json.h"
#include "util/peak.h"

namespace sonare::mastering::assistant {

using sonare::constants::kEpsilon;
namespace {

constexpr float kMinDb = sonare::constants::kFloorDb;

// Onset rise (mel-band mean dB per 512-sample hop at 48 kHz) an attack must exceed; calibrated
// at the default hop. Geometric mean of the -40 dBFS white-noise peak (2.17, not counted) and a
// kick's peak under a +24 dB burst (8.44, counted): about 2x margin to each.
constexpr float kAttackOnsetFloor = 4.3f;
// Signal length per event a summary reference may set aside: a 1.2 s kit (two large hits) and a
// five-pluck guitar need none set aside, one burst in 12 s needs one.
constexpr float kRobustReferenceSecondsPerEvent = 10.0f;
// An RMS maximum is a sustain event when it doubles the minimum of the window before it.
constexpr float kSustainEventRise = 2.0f;

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

// The band's share of the signal's mean square in dBFS, the noise fields' convention (a full-scale
// sine reads -3.01 dBFS). By Parseval a frame's two-sided bin power is n_fft * Σw² times the mean
// square of the signal under the window, so the level needs no reference geometry.
float band_level_dbfs(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft,
                      int sr, double window_energy, float min_hz, float max_hz) {
  if (n_frames <= 0 || !(window_energy > 0.0)) return kMinDb;
  double power_sum = 0.0;
  for (int bin = 0; bin < n_bins; ++bin) {
    const float hz = static_cast<float>(bin) * static_cast<float>(sr) / static_cast<float>(n_fft);
    if (hz < min_hz || hz >= max_hz) continue;
    // A one-sided bin stands for its mirror too, except DC and Nyquist.
    const double weight = (bin == 0 || 2 * bin == n_fft) ? 1.0 : 2.0;
    for (int frame = 0; frame < n_frames; ++frame) {
      const float mag = magnitude[static_cast<size_t>(bin) * n_frames + frame];
      power_sum += weight * static_cast<double>(mag) * mag;
    }
  }
  const double mean_square =
      power_sum / (static_cast<double>(n_frames) * static_cast<double>(n_fft) * window_energy);
  return std::max(kMinDb, power_to_db(mean_square));
}

float attack_density(const std::vector<float>& onset, float duration_sec) {
  if (onset.size() < 3 || duration_sec <= 0.0f) return 0.0f;
  const float max_value = *std::max_element(onset.begin(), onset.end());
  if (max_value <= sonare::constants::kEpsilon) return 0.0f;
  const auto is_peak = [&onset](size_t i) {
    return onset[i] >= onset[i - 1] && onset[i] > onset[i + 1];
  };
  std::vector<float> events;
  for (size_t i = 1; i + 1 < onset.size(); ++i) {
    if (is_peak(i) && onset[i] > kAttackOnsetFloor) events.push_back(onset[i]);
  }
  const float reference = summary_reference(std::move(events), duration_sec, max_value);
  const float threshold = std::max(reference * 0.30f, kAttackOnsetFloor);
  int peaks = 0;
  for (size_t i = 1; i + 1 < onset.size(); ++i) {
    if (onset[i] > threshold && is_peak(i)) ++peaks;
  }
  return static_cast<float>(peaks) / duration_sec;
}

float sustain_ratio(const std::vector<float>& rms, float duration_sec, int lookback) {
  if (rms.empty()) return 0.0f;
  const float max_value = *std::max_element(rms.begin(), rms.end());
  const float reference = summary_reference(rising_peak_heights(rms, lookback, kSustainEventRise),
                                            duration_sec, max_value);
  if (reference <= sonare::constants::kSpectrumEpsilon) return 0.0f;
  int sustained = 0;
  for (float value : rms) {
    if (value >= reference * 0.35f) ++sustained;
  }
  return static_cast<float>(sustained) / static_cast<float>(rms.size());
}

struct HumCandidate {
  float fundamental_hz = 0.0f;
  float prominence = 1.0f;
  int harmonics = 0;
  float fundamental_dbfs = 0.0f;
  float peak_harmonic_dbfs = 0.0f;
  size_t index = 0;
};

HumCandidate make_hum_candidate(const repair::HumDetection& detection, size_t index) {
  HumCandidate candidate;
  candidate.fundamental_hz = detection.fundamental_hz;
  candidate.prominence = detection.fundamental_prominence;
  candidate.harmonics = detection.harmonics;
  candidate.fundamental_dbfs = detection.harmonic_dbfs[0];
  candidate.peak_harmonic_dbfs = *std::max_element(
      detection.harmonic_dbfs, detection.harmonic_dbfs + repair::kDehumMaxHarmonics);
  candidate.index = index;
  return candidate;
}

bool hum_candidate_better(const HumCandidate& candidate, const HumCandidate& current) {
  if (candidate.prominence != current.prominence) {
    return candidate.prominence > current.prominence;
  }
  if (candidate.peak_harmonic_dbfs != current.peak_harmonic_dbfs) {
    return candidate.peak_harmonic_dbfs > current.peak_harmonic_dbfs;
  }
  return candidate.index < current.index;
}

void assign_hum_candidate(DefectProfile& defects, const HumCandidate& candidate) {
  defects.hum_fundamental_hz = candidate.fundamental_hz;
  defects.hum_fundamental_prominence = candidate.prominence;
  defects.hum_harmonics = candidate.harmonics;
  defects.hum_fundamental_dbfs = candidate.fundamental_dbfs;
  defects.hum_peak_harmonic_dbfs = candidate.peak_harmonic_dbfs;
}

struct ChannelDefects {
  DefectProfile profile;
  std::vector<float> noise_band_dbfs;
  float peak_abs = 0.0f;
};

double measured_noise_power(float level_dbfs) {
  // kFloorDb is the detector's zero-energy sentinel. Treating it as a real
  // power before summing would turn duplicated silence into a false +3 dB
  // measurement.
  return level_dbfs > kMinDb ? db_to_power_scalar(static_cast<double>(level_dbfs)) : 0.0;
}

float noise_power_to_db(double power) {
  return power > 0.0 ? std::max(kMinDb, static_cast<float>(power_to_db_scalar(power))) : kMinDb;
}

}  // namespace

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config) {
  return analyze_audio_profile(samples, length, sample_rate, config, nullptr);
}

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config, Spectrogram* spec_out) {
  if (samples == nullptr || length == 0 || sample_rate <= 0) return AudioProfile{};
  return analyze_audio_profile(Audio::from_buffer(samples, length, sample_rate), config, spec_out);
}

namespace {

/// Whether a declip pass at @p clip_flat_level leaves this channel's audio alone. Unsafe when
/// the channel carries audio louder than the pinned level (the repair threshold would reach it)
/// or, for a channel with no pinned runs of its own, when it reaches the clamped threshold.
bool channel_allows_declip(float clip_flat_level, float peak_abs, std::size_t flat_run_count) {
  if (!(clip_flat_level > 0.0f)) return true;
  if (peak_abs > clip_flat_level * db_to_linear(repair::kDeclipFlatRunLevelWindowDb)) return false;
  return flat_run_count > 0 || peak_abs < std::min(clip_flat_level, 1.0f);
}

// Runs the six repair detectors over one signal, each with its own default
// config. Only a non-finite sample throws (the clipping detector refuses it); a
// non-positive rate and an input shorter than the STFT are settled beforehand.
DefectProfile measure_defects(const Audio& audio, std::vector<float>* noise_band_dbfs = nullptr) {
  DefectProfile defects;

  const repair::DenoiseClassicalConfig denoise_config;
  // An input this short leaves the whole block unmeasured rather than five
  // detectors filled and a noise floor left at 0.0f, which reads as 0 dBFS.
  if (audio.size() < static_cast<std::size_t>(denoise_config.n_fft)) return defects;

  const float* samples = audio.data();
  const std::size_t size = audio.size();
  const int sample_rate = audio.sample_rate();

  const auto clicks = repair::detect_clicks(samples, size, sample_rate);
  defects.click_count = clicks.count;
  defects.click_rejected = clicks.rejected;
  defects.click_longest_run_samples = clicks.longest_run_samples;
  defects.click_per_second = clicks.per_second;

  const auto crackle = repair::detect_crackle(samples, size, sample_rate);
  defects.crackle_sample_count = crackle.sample_count;
  defects.crackle_sample_fraction = crackle.sample_fraction;
  defects.crackle_per_second = crackle.per_second;

  const auto clipping = repair::detect_clipping(samples, size, sample_rate);
  defects.clip_sample_count = clipping.sample_count;
  defects.clip_run_count = clipping.run_count;
  defects.clip_longest_run_samples = clipping.longest_run_samples;
  defects.clip_sample_fraction = clipping.sample_fraction;
  defects.clip_flat_run_count = clipping.flat_run_count;
  defects.clip_flat_sample_count = clipping.flat_sample_count;
  defects.clip_longest_flat_run_samples = clipping.longest_flat_run_samples;
  defects.clip_flat_level = clipping.flat_level;
  float peak_abs = 0.0f;
  for (std::size_t i = 0; i < size; ++i) peak_abs = std::max(peak_abs, std::abs(samples[i]));
  defects.declip_threshold_safe =
      channel_allows_declip(defects.clip_flat_level, peak_abs, defects.clip_flat_run_count);

  const auto noise = repair::detect_noise_floor(samples, size, sample_rate, denoise_config);
  defects.noise_floor_dbfs = noise.floor_dbfs;
  if (noise_band_dbfs != nullptr) {
    noise_band_dbfs->assign(std::begin(noise.band_floor_dbfs), std::end(noise.band_floor_dbfs));
  }
  const auto* peak_band = std::max_element(noise.band_floor_dbfs,
                                           noise.band_floor_dbfs + repair::kRepairNoiseBandCount);
  defects.noise_band_peak_dbfs = *peak_band;
  defects.noise_band_peak_index = static_cast<int>(peak_band - noise.band_floor_dbfs);

  // Searched at both mains frequencies and the more prominent one kept. The
  // detector's default search spans a couple of Hz around 50, so a 60 Hz series
  // lands at the search boundary with a prominence barely above clean material:
  // neither found nor reported as absent.
  repair::DehumConfig hum_50;
  repair::DehumConfig hum_60;
  hum_60.fundamental_hz = 60.0f;
  const auto at_50 = repair::detect_hum(samples, size, sample_rate, hum_50);
  const auto at_60 = repair::detect_hum(samples, size, sample_rate, hum_60);
  // Keep the mono search's historical tie behaviour: the 60 Hz candidate only
  // wins when its prominence is strictly greater, so an equal result keeps 50.
  const bool use_60 = at_60.fundamental_prominence > at_50.fundamental_prominence;
  assign_hum_candidate(defects, make_hum_candidate(use_60 ? at_60 : at_50, use_60 ? 1 : 0));

  defects.late_decay_ratio_db =
      repair::detect_reverb(samples, size, sample_rate).late_decay_ratio_db;

  defects.measured = true;
  return defects;
}

DefectProfile aggregate_defects(const std::vector<ChannelDefects>& per_channel) {
  DefectProfile defects;
  if (per_channel.empty()) return defects;
  for (const ChannelDefects& channel : per_channel) {
    if (!channel.profile.measured ||
        channel.noise_band_dbfs.size() != repair::kRepairNoiseBandCount) {
      return defects;
    }
  }

  double click_per_second_sum = 0.0;
  double crackle_fraction_sum = 0.0;
  double crackle_per_second_sum = 0.0;
  double clip_fraction_sum = 0.0;
  double noise_floor_power = 0.0;
  std::vector<double> noise_band_power(repair::kRepairNoiseBandCount, 0.0);
  HumCandidate hum;
  bool have_hum = false;

  for (size_t index = 0; index < per_channel.size(); ++index) {
    const DefectProfile& channel = per_channel[index].profile;
    defects.click_count += channel.click_count;
    defects.click_rejected += channel.click_rejected;
    defects.click_longest_run_samples =
        std::max(defects.click_longest_run_samples, channel.click_longest_run_samples);
    click_per_second_sum += channel.click_per_second;

    defects.crackle_sample_count += channel.crackle_sample_count;
    crackle_fraction_sum += channel.crackle_sample_fraction;
    crackle_per_second_sum += channel.crackle_per_second;

    defects.clip_sample_count += channel.clip_sample_count;
    defects.clip_run_count += channel.clip_run_count;
    defects.clip_longest_run_samples =
        std::max(defects.clip_longest_run_samples, channel.clip_longest_run_samples);
    clip_fraction_sum += channel.clip_sample_fraction;
    defects.clip_flat_run_count += channel.clip_flat_run_count;
    defects.clip_flat_sample_count += channel.clip_flat_sample_count;
    defects.clip_longest_flat_run_samples =
        std::max(defects.clip_longest_flat_run_samples, channel.clip_longest_flat_run_samples);
    if (channel.clip_flat_level > 0.0f &&
        (defects.clip_flat_level == 0.0f || channel.clip_flat_level > defects.clip_flat_level)) {
      defects.clip_flat_level = channel.clip_flat_level;
    }

    noise_floor_power += measured_noise_power(channel.noise_floor_dbfs);
    for (size_t band = 0; band < noise_band_power.size(); ++band) {
      noise_band_power[band] += measured_noise_power(per_channel[index].noise_band_dbfs[band]);
    }

    const HumCandidate channel_hum = {
        channel.hum_fundamental_hz,   channel.hum_fundamental_prominence, channel.hum_harmonics,
        channel.hum_fundamental_dbfs, channel.hum_peak_harmonic_dbfs,     index};
    if (!have_hum || hum_candidate_better(channel_hum, hum)) {
      hum = channel_hum;
      have_hum = true;
    }
    if (index == 0) {
      defects.late_decay_ratio_db = channel.late_decay_ratio_db;
    } else {
      defects.late_decay_ratio_db =
          std::max(defects.late_decay_ratio_db, channel.late_decay_ratio_db);
    }
  }

  const float channel_count = static_cast<float>(per_channel.size());
  defects.click_per_second = static_cast<float>(click_per_second_sum / channel_count);
  defects.crackle_sample_fraction = static_cast<float>(crackle_fraction_sum / channel_count);
  defects.crackle_per_second = static_cast<float>(crackle_per_second_sum / channel_count);
  defects.clip_sample_fraction = static_cast<float>(clip_fraction_sum / channel_count);
  defects.noise_floor_dbfs = noise_power_to_db(noise_floor_power);
  defects.noise_band_peak_dbfs = kMinDb;
  defects.noise_band_peak_index = -1;
  for (size_t band = 0; band < noise_band_power.size(); ++band) {
    const float level = noise_power_to_db(noise_band_power[band]);
    if (band == 0 || level > defects.noise_band_peak_dbfs) {
      defects.noise_band_peak_dbfs = level;
      defects.noise_band_peak_index = static_cast<int>(band);
    }
  }
  defects.declip_threshold_safe = true;
  for (const ChannelDefects& channel : per_channel) {
    if (!channel_allows_declip(defects.clip_flat_level, channel.peak_abs,
                               channel.profile.clip_flat_run_count)) {
      defects.declip_threshold_safe = false;
      break;
    }
  }
  if (have_hum) assign_hum_candidate(defects, hum);
  defects.measured = true;
  return defects;
}

}  // namespace

ChannelSetDefects measure_defects_planar(const float* const* channels, std::size_t channel_count,
                                         std::size_t length, int sample_rate) {
  ChannelSetDefects result;
  std::vector<ChannelDefects> per_channel;
  per_channel.reserve(channel_count);
  for (size_t channel = 0; channel < channel_count; ++channel) {
    const Audio channel_audio = Audio::from_buffer(channels[channel], length, sample_rate);
    ChannelDefects measured;
    measured.profile = measure_defects(channel_audio, &measured.noise_band_dbfs);
    for (float sample : channel_audio) {
      measured.peak_abs = std::max(measured.peak_abs, std::abs(sample));
    }
    result.channels.push_back(measured.profile);
    per_channel.push_back(std::move(measured));
  }
  // One channel is its own aggregate; the reductions would only round-trip its levels.
  result.aggregate = channel_count == 1 ? result.channels.front() : aggregate_defects(per_channel);
  return result;
}

namespace {

DefectProfile measure_defects_interleaved(const float* samples, std::size_t frames, int channels,
                                          int sample_rate) {
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels), std::vector<float>(frames));
  std::vector<const float*> pointers;
  for (int channel = 0; channel < channels; ++channel) {
    std::vector<float>& plane = planes[static_cast<size_t>(channel)];
    for (size_t frame = 0; frame < frames; ++frame) {
      plane[frame] = samples[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
    }
    pointers.push_back(plane.data());
  }
  return measure_defects_planar(pointers.data(), pointers.size(), frames, sample_rate).aggregate;
}

// Everything outside the loudness and defect blocks: spectral shape, dynamics
// and tempo. These describe shape and timing rather than absolute level, so the
// stereo entry point measures them on the downmix and only replaces the
// loudness block, keeping mono and stereo profiles comparable field by field.
//
// `short_term_series`, when given, is the short-term loudness the caller's own
// loudness pass already measured over this same signal; `spec_out`, when given,
// receives the analysis STFT.
void fill_profile_body(const Audio& audio, const AudioProfileConfig& config, AudioProfile& profile,
                       const std::vector<float>* short_term_series, Spectrogram* spec_out) {
  // Detrend (DC-remove) the onset envelope so the attack-density peak picking
  // discriminates transient bursts from steady energy. This consumer opts in
  // explicitly; the public OnsetConfig default is detrend=false (librosa).
  OnsetConfig onset_config;
  onset_config.detrend = true;

  const StftConfig stft_config = profile_stft_config(config, audio.sample_rate());
  // One STFT feeds both the spectral block and the onset envelope below, so the framing the
  // onset path asks for is the framing this spectrogram is built with rather than a second
  // default that happens to agree.
  onset_config.center = stft_config.center;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  // Built locally rather than through spec.magnitude(): magnitude() is abs(z)
  // unconditionally now, but this avoids materializing the class-level cache
  // when a private buffer is needed here anyway.
  std::vector<float> mag(static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
  const std::complex<float>* spectrum = spec.complex_data();
  for (size_t i = 0; i < mag.size(); ++i) {
    mag[i] = std::abs(spectrum[i]);
  }

  double window_energy = 0.0;
  for (float w : create_window(spec.window(), spec.win_length(), true)) {
    window_energy += static_cast<double>(w) * w;
  }
  const auto band_db = [&](float min_hz, float max_hz) {
    return band_level_dbfs(mag, n_bins, n_frames, spec.n_fft(), audio.sample_rate(), window_energy,
                           min_hz, max_hz);
  };
  profile.spectral.sub_rms_db = band_db(20, 60);
  profile.spectral.low_rms_db = band_db(60, 250);
  profile.spectral.low_mid_rms_db = band_db(250, 500);
  profile.spectral.mid_rms_db = band_db(500, 2000);
  profile.spectral.high_mid_rms_db = band_db(2000, 6000);
  profile.spectral.high_rms_db = band_db(6000, 12000);
  profile.spectral.air_rms_db =
      band_db(12000, static_cast<float>(audio.sample_rate()) * 0.5f + 1.0f);
  profile.spectral.centroid_hz = mean_finite(
      spectral_centroid(mag.data(), n_bins, n_frames, audio.sample_rate(), spec.n_fft()));
  profile.spectral.flatness = mean_finite(spectral_flatness(mag.data(), n_bins, n_frames));
  profile.spectral.rolloff_hz = mean_finite(
      spectral_rolloff(mag.data(), n_bins, n_frames, audio.sample_rate(), spec.n_fft()));

  // Measured here only when the caller has no series to hand down: the stereo
  // entry point's loudness runs on the channel-summed program rather than on this
  // downmix, so its short-term blocks describe a different signal.
  std::vector<float> measured_short_term;
  if (short_term_series == nullptr) measured_short_term = metering::short_term_lufs(audio);
  profile.dynamics.short_term_lufs_std =
      stddev_finite(short_term_series != nullptr ? *short_term_series : measured_short_term);

  MelConfig mel_config;
  mel_config.n_fft = stft_config.n_fft;
  mel_config.hop_length = stft_config.hop_length;
  mel_config.win_length = stft_config.win_length;
  // The Audio overload of compute_onset_strength would run its own STFT of this same geometry;
  // splitting it at the Mel spectrogram reuses the one above and keeps the trailing alignment
  // step the overload applies.
  const MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, audio.sample_rate(),
                                                              mel_config.to_mel_filter_config());
  // Last read of the STFT; the caller takes it from here rather than paying for
  // a second one over the same signal.
  if (spec_out != nullptr) *spec_out = std::move(spec);
  const auto onset = center_onset_strength(compute_onset_strength(mel, onset_config),
                                           stft_config.actual_win_length(), stft_config.hop_length,
                                           onset_config.center);
  profile.dynamics.attack_density = attack_density(onset, profile.duration_sec);
  const int win_length = stft_config.actual_win_length();
  // Frames one RMS window spans, plus one: the minimum a maximum rises from predates its window.
  const int sustain_lookback =
      (win_length + stft_config.hop_length - 1) / stft_config.hop_length + 1;
  profile.dynamics.sustain_ratio =
      sustain_ratio(rms_energy(audio, win_length, stft_config.hop_length), profile.duration_sec,
                    sustain_lookback);

  try {
    // The envelope is already framed; the onset-envelope constructor reads no window length.
    BpmConfig bpm_config;
    bpm_config.hop_length = stft_config.hop_length;
    BpmAnalyzer bpm(onset, audio.sample_rate(), stft_config.hop_length, bpm_config);
    profile.bpm = bpm.bpm();
    profile.bpm_confidence = bpm.confidence();
  } catch (...) {
    profile.bpm = 0.0f;
    profile.bpm_confidence = 0.0f;
  }
}

}  // namespace

StftConfig profile_stft_config(const AudioProfileConfig& config, int sample_rate) {
  return stft_config_scaled_to_rate(make_stft_config(config.n_fft, config.hop_length), sample_rate,
                                    kProfileReferenceRate);
}

double reference_power_scale(const Spectrogram& spec, int n_fft_at_reference_rate) {
  const int win_length = spec.win_length();
  if (spec.sample_rate() == kProfileReferenceRate && win_length == n_fft_at_reference_rate &&
      spec.window() == WindowType::Hann) {
    return 1.0;
  }
  const auto sum_of_squares = [](const std::vector<float>& window) {
    double sum = 0.0;
    for (float w : window) sum += static_cast<double>(w) * w;
    return sum;
  };
  const double actual = sum_of_squares(create_window(spec.window(), win_length, true));
  const double reference =
      sum_of_squares(create_window(WindowType::Hann, n_fft_at_reference_rate, true));
  if (!(actual > 0.0) || spec.sample_rate() <= 0) return 1.0;
  return (static_cast<double>(kProfileReferenceRate) * reference) /
         (static_cast<double>(spec.sample_rate()) * actual);
}

double reference_bin_width_ratio(const Spectrogram& spec, int n_fft_at_reference_rate) {
  if (spec.sample_rate() == kProfileReferenceRate && spec.n_fft() == n_fft_at_reference_rate) {
    return 1.0;
  }
  return (static_cast<double>(spec.sample_rate()) * n_fft_at_reference_rate) /
         (static_cast<double>(kProfileReferenceRate) * spec.n_fft());
}

std::vector<float> rising_peak_heights(const std::vector<float>& series, int lookback, float rise) {
  std::vector<float> heights;
  for (size_t i = 1; i + 1 < series.size(); ++i) {
    if (!(series[i] > series[i - 1] && series[i] >= series[i + 1])) continue;
    const size_t first = i > static_cast<size_t>(lookback) ? i - static_cast<size_t>(lookback) : 0;
    const float recent = *std::min_element(series.begin() + static_cast<std::ptrdiff_t>(first),
                                           series.begin() + static_cast<std::ptrdiff_t>(i));
    if (series[i] > rise * recent) heights.push_back(series[i]);
  }
  return heights;
}

float summary_reference(std::vector<float> events, float duration_sec, float series_max) {
  const auto by_duration =
      static_cast<std::size_t>(std::max(0.0f, duration_sec / kRobustReferenceSecondsPerEvent));
  const std::size_t ignored = std::min(kReferenceIgnoredTopEvents, by_duration);
  if (ignored == 0 || events.size() <= ignored) return series_max;
  return max_excluding_top(std::move(events), ignored);
}

AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config) {
  return analyze_audio_profile(audio, config, nullptr);
}

AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config,
                                   Spectrogram* spec_out) {
  AudioProfile profile;
  if (audio.empty() || audio.sample_rate() <= 0) return profile;

  profile.duration_sec = audio.duration();

  // One K-weighting pass over the signal serves both loudness readings: the
  // scalars here and the short-term spread the body reduces from the series.
  std::vector<float> short_term;
  const auto loudness = metering::lufs(audio, metering::LufsConfig(), &short_term);
  profile.loudness.integrated_lufs = loudness.integrated_lufs;
  profile.loudness.lra_lu = loudness.loudness_range;
  profile.loudness.true_peak_db = metering::true_peak_db(audio, config.true_peak_oversample);
  profile.loudness.crest_factor_db = metering::crest_factor_db(audio);

  fill_profile_body(audio, config, profile, &short_term, spec_out);
  if (config.detect_defects) profile.defects = measure_defects(audio);
  return profile;
}

AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config) {
  return analyze_audio_profile_interleaved(samples, frames, channels, sample_rate, config, nullptr);
}

AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config,
                                               Spectrogram* spec_out) {
  AudioProfile profile;
  if (samples == nullptr || frames == 0 || channels <= 0 || sample_rate <= 0) return profile;

  std::vector<float> mono(frames, 0.0f);
  const float scale = 1.0f / static_cast<float>(channels);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0f;
    for (int channel = 0; channel < channels; ++channel) {
      sum +=
          samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
    }
    mono[frame] = sum * scale;
  }
  const Audio audio = Audio::from_buffer(mono.data(), mono.size(), sample_rate);

  profile.duration_sec = audio.duration();

  const auto summary = common::measure_loudness_summary_interleaved(
      samples, frames, channels, sample_rate, config.true_peak_oversample);
  profile.loudness.integrated_lufs = summary.integrated_lufs;
  profile.loudness.lra_lu = summary.loudness_range;
  profile.loudness.true_peak_db = summary.true_peak_dbtp;
  profile.loudness.crest_factor_db =
      metering::crest_factor_db_interleaved(samples, frames, channels);

  fill_profile_body(audio, config, profile, nullptr, spec_out);
  if (config.detect_defects) {
    profile.defects = measure_defects_interleaved(samples, frames, channels, sample_rate);
  }
  return profile;
}

sonare::util::json::Object defects_to_json(const DefectProfile& defects) {
  namespace json = sonare::util::json;
  // Emitted whatever `measured` says, so the object's shape does not depend on
  // the config: a consumer reads one flag rather than having to tell an absent
  // block from a core too old to write one.
  json::Object out;
  util::json::put(out, "measured", defects.measured);
  util::json::put(out, "clickCount", static_cast<double>(defects.click_count));
  util::json::put(out, "clickRejected", static_cast<double>(defects.click_rejected));
  util::json::put(out, "clickLongestRunSamples",
                  static_cast<double>(defects.click_longest_run_samples));
  util::json::put(out, "clickPerSecond", defects.click_per_second);
  util::json::put(out, "crackleSampleCount", static_cast<double>(defects.crackle_sample_count));
  util::json::put(out, "crackleSampleFraction", defects.crackle_sample_fraction);
  util::json::put(out, "cracklePerSecond", defects.crackle_per_second);
  util::json::put(out, "clipSampleCount", static_cast<double>(defects.clip_sample_count));
  util::json::put(out, "clipRunCount", static_cast<double>(defects.clip_run_count));
  util::json::put(out, "clipLongestRunSamples",
                  static_cast<double>(defects.clip_longest_run_samples));
  util::json::put(out, "clipSampleFraction", defects.clip_sample_fraction);
  util::json::put(out, "clipFlatRunCount", static_cast<double>(defects.clip_flat_run_count));
  util::json::put(out, "clipFlatSampleCount", static_cast<double>(defects.clip_flat_sample_count));
  util::json::put(out, "clipLongestFlatRunSamples",
                  static_cast<double>(defects.clip_longest_flat_run_samples));
  util::json::put(out, "clipFlatLevel", defects.clip_flat_level);
  util::json::put(out, "noiseFloorDbfs", defects.noise_floor_dbfs);
  util::json::put(out, "noiseBandPeakDbfs", defects.noise_band_peak_dbfs);
  util::json::put(out, "noiseBandPeakIndex", defects.noise_band_peak_index);
  util::json::put(out, "humFundamentalHz", defects.hum_fundamental_hz);
  util::json::put(out, "humFundamentalProminence", defects.hum_fundamental_prominence);
  util::json::put(out, "humHarmonics", defects.hum_harmonics);
  util::json::put(out, "humFundamentalDbfs", defects.hum_fundamental_dbfs);
  util::json::put(out, "humPeakHarmonicDbfs", defects.hum_peak_harmonic_dbfs);
  util::json::put(out, "lateDecayRatioDb", defects.late_decay_ratio_db);
  return out;
}

std::string audio_profile_to_json(const AudioProfile& profile) {
  namespace json = sonare::util::json;

  json::Object loudness;
  util::json::put(loudness, "integratedLufs", profile.loudness.integrated_lufs);
  util::json::put(loudness, "lraLu", profile.loudness.lra_lu);
  util::json::put(loudness, "truePeakDb", profile.loudness.true_peak_db);
  util::json::put(loudness, "crestFactorDb", profile.loudness.crest_factor_db);

  json::Object spectral;
  util::json::put(spectral, "subRmsDb", profile.spectral.sub_rms_db);
  util::json::put(spectral, "lowRmsDb", profile.spectral.low_rms_db);
  util::json::put(spectral, "lowMidRmsDb", profile.spectral.low_mid_rms_db);
  util::json::put(spectral, "midRmsDb", profile.spectral.mid_rms_db);
  util::json::put(spectral, "highMidRmsDb", profile.spectral.high_mid_rms_db);
  util::json::put(spectral, "highRmsDb", profile.spectral.high_rms_db);
  util::json::put(spectral, "airRmsDb", profile.spectral.air_rms_db);
  util::json::put(spectral, "centroidHz", profile.spectral.centroid_hz);
  util::json::put(spectral, "flatness", profile.spectral.flatness);
  util::json::put(spectral, "rolloffHz", profile.spectral.rolloff_hz);

  json::Object dynamics;
  util::json::put(dynamics, "shortTermLufsStd", profile.dynamics.short_term_lufs_std);
  util::json::put(dynamics, "attackDensity", profile.dynamics.attack_density);
  util::json::put(dynamics, "sustainRatio", profile.dynamics.sustain_ratio);

  json::Object defects = defects_to_json(profile.defects);

  json::Object root;
  util::json::put(root, "durationSec", profile.duration_sec);
  util::json::put(root, "bpm", profile.bpm);
  util::json::put(root, "bpmConfidence", profile.bpm_confidence);
  util::json::put(root, "loudness", std::move(loudness));
  util::json::put(root, "spectral", std::move(spectral));
  util::json::put(root, "dynamics", std::move(dynamics));
  util::json::put(root, "defects", std::move(defects));
  return json::dump(json::Value(std::move(root)));
}

const std::vector<std::string>& audio_profile_schema_paths() {
  static const std::vector<std::string> paths = {
      "bpm",
      "bpmConfidence",
      "defects",
      "defects.clickCount",
      "defects.clickLongestRunSamples",
      "defects.clickPerSecond",
      "defects.clickRejected",
      "defects.clipFlatLevel",
      "defects.clipFlatRunCount",
      "defects.clipFlatSampleCount",
      "defects.clipLongestFlatRunSamples",
      "defects.clipLongestRunSamples",
      "defects.clipRunCount",
      "defects.clipSampleCount",
      "defects.clipSampleFraction",
      "defects.cracklePerSecond",
      "defects.crackleSampleCount",
      "defects.crackleSampleFraction",
      "defects.humFundamentalDbfs",
      "defects.humFundamentalHz",
      "defects.humFundamentalProminence",
      "defects.humHarmonics",
      "defects.humPeakHarmonicDbfs",
      "defects.lateDecayRatioDb",
      "defects.measured",
      "defects.noiseBandPeakDbfs",
      "defects.noiseBandPeakIndex",
      "defects.noiseFloorDbfs",
      "durationSec",
      "dynamics",
      "dynamics.attackDensity",
      "dynamics.shortTermLufsStd",
      "dynamics.sustainRatio",
      "loudness",
      "loudness.crestFactorDb",
      "loudness.integratedLufs",
      "loudness.lraLu",
      "loudness.truePeakDb",
      "spectral",
      "spectral.airRmsDb",
      "spectral.centroidHz",
      "spectral.flatness",
      "spectral.highMidRmsDb",
      "spectral.highRmsDb",
      "spectral.lowMidRmsDb",
      "spectral.lowRmsDb",
      "spectral.midRmsDb",
      "spectral.rolloffHz",
      "spectral.subRmsDb",
  };
  return paths;
}

}  // namespace sonare::mastering::assistant
