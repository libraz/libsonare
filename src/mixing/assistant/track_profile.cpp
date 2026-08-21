/// @file track_profile.cpp
/// @brief Per-track profiling for the mixing assistant.
/// @details Offline only. One STFT per track, folded straight into the band
///          envelope the cross-track phase reads and into the time-averaged
///          spectrum; the raw spectrogram is never retained.

#include "mixing/assistant/track_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "mastering/assistant/audio_profile.h"
#include "util/constants.h"

namespace sonare::mixing::assistant {

using sonare::constants::kFloorDb;

namespace {

// Marks an FFT bin that belongs to no analysis band: below the lowest band
// edge, or above the highest band edge Nyquist can reach.
constexpr int kUnbandedBin = -1;

// Silence test threshold, in LUFS. A digitally silent track measures -inf while
// a track whose content sits under the numerical dB floor measures at the
// floor, so the test lifts the floor by a margin rather than comparing against
// it exactly: an equality test on a float that reaches its value through two
// different paths would only ever be true for one of them.
constexpr float kSilenceMarginLu = 5.0f;
constexpr float kSilenceLufsThreshold = kFloorDb + kSilenceMarginLu;

// Coherent gain of the Hann analysis window as a fraction of n_fft
// (sum(w) = n_fft / 2). The STFT is unnormalized, so band power scales with it;
// dividing it out expresses band content as a signal amplitude rather than an
// FFT-size dependent number. This profiler never overrides the StftConfig
// window, so the Hann figure is the only one that applies.
constexpr double kHannCoherentGain = 0.5;

// Equivalent-amplitude floor below which a track carries no spectral content
// worth suggesting against. Roughly 100 dB under full scale, i.e. far below the
// least significant bit of 16-bit delivery, so nothing audible is excluded.
constexpr float kMinBandAmplitude = 1.0e-5f;

/// @brief Per-band energy sums over the whole track.
struct BandTotals {
  std::array<double, kBandCount> per_band{};
  double total = 0.0;
};

// Maps every FFT bin to its analysis band. Built once per track: the fold below
// visits n_bins * n_frames entries and must not redo this per frame.
std::vector<int> make_bin_band_map(int n_bins, int n_fft, int sample_rate) {
  const float nyquist = 0.5f * static_cast<float>(sample_rate);
  std::vector<int> bin_band(static_cast<std::size_t>(n_bins), kUnbandedBin);
  for (int bin = 0; bin < n_bins; ++bin) {
    const float hz =
        static_cast<float>(bin) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
    for (int band = 0; band < kBandCount; ++band) {
      const BandEdge& edge = kBands[static_cast<std::size_t>(band)];
      if (hz < edge.low_hz) break;
      // The top band's edge is infinite; the real upper bound is Nyquist.
      // Whichever band holds Nyquist takes that bin inclusively, since a
      // half-open test would drop it.
      const float high_hz = std::min(edge.high_hz, nyquist);
      if (hz < high_hz || (high_hz >= nyquist && hz <= high_hz)) {
        bin_band[static_cast<std::size_t>(bin)] = band;
        break;
      }
    }
  }
  return bin_band;
}

// Folds the power spectrum into per-band linear power over time.
BandEnergyEnvelope fold_bands(const Spectrogram& spec, int sample_rate) {
  BandEnergyEnvelope bands;
  bands.n_frames = spec.n_frames();
  bands.n_fft = spec.n_fft();
  bands.hop_length = spec.hop_length();
  bands.sample_rate = sample_rate;

  const int n_frames = spec.n_frames();
  const int n_bins = spec.n_bins();
  if (n_frames <= 0 || n_bins <= 0) return bands;

  bands.energy.assign(static_cast<std::size_t>(kBandCount) * static_cast<std::size_t>(n_frames),
                      0.0f);
  const std::vector<int> bin_band = make_bin_band_map(n_bins, spec.n_fft(), sample_rate);
  // Spectrogram is [n_bins x n_frames] row-major, so a bin's frames are
  // contiguous and the fold runs bin-major.
  const std::vector<float>& power = spec.power();
  for (int bin = 0; bin < n_bins; ++bin) {
    const int band = bin_band[static_cast<std::size_t>(bin)];
    if (band == kUnbandedBin) continue;
    const std::size_t source = static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames);
    const std::size_t target = static_cast<std::size_t>(band) * static_cast<std::size_t>(n_frames);
    for (int frame = 0; frame < n_frames; ++frame) {
      bands.energy[target + static_cast<std::size_t>(frame)] +=
          power[source + static_cast<std::size_t>(frame)];
    }
  }
  return bands;
}

// Averages the power spectrum over frames. Bin-major like the fold above, for
// the same reason: a bin's frames are contiguous.
MeanPowerSpectrum mean_power_spectrum(const Spectrogram& spec, int sample_rate) {
  MeanPowerSpectrum spectrum;
  spectrum.n_fft = spec.n_fft();
  spectrum.sample_rate = sample_rate;

  const int n_frames = spec.n_frames();
  const int n_bins = spec.n_bins();
  if (n_frames <= 0 || n_bins <= 0) return spectrum;

  spectrum.n_bins = n_bins;
  spectrum.power.assign(static_cast<std::size_t>(n_bins), 0.0f);
  const std::vector<float>& power = spec.power();
  for (int bin = 0; bin < n_bins; ++bin) {
    const std::size_t source = static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames);
    // Accumulated in double: a long track sums tens of thousands of frames, and
    // a quiet bin's contribution would disappear under a loud running total.
    double sum = 0.0;
    for (int frame = 0; frame < n_frames; ++frame) {
      sum += static_cast<double>(power[source + static_cast<std::size_t>(frame)]);
    }
    spectrum.power[static_cast<std::size_t>(bin)] =
        static_cast<float>(sum / static_cast<double>(n_frames));
  }
  return spectrum;
}

BandTotals band_totals(const BandEnergyEnvelope& bands) {
  BandTotals totals;
  for (int band = 0; band < kBandCount; ++band) {
    double sum = 0.0;
    for (int frame = 0; frame < bands.n_frames; ++frame) {
      sum += static_cast<double>(bands.at(band, frame));
    }
    totals.per_band[static_cast<std::size_t>(band)] = sum;
    totals.total += sum;
  }
  return totals;
}

// Normalized share of the track's energy per band; all zero for a silent track,
// which has no share to distribute.
std::array<float, kBandCount> band_occupancy(const BandTotals& totals) {
  std::array<float, kBandCount> occupancy{};
  if (!(totals.total > 0.0)) return occupancy;
  for (int band = 0; band < kBandCount; ++band) {
    const std::size_t index = static_cast<std::size_t>(band);
    occupancy[index] = static_cast<float>(totals.per_band[index] / totals.total);
  }
  return occupancy;
}

// Loudest band's mean per-frame power, mapped back to an equivalent signal
// amplitude so it can be compared against an absolute, FFT-size independent
// floor.
float peak_band_amplitude(const BandTotals& totals, int n_frames, int n_fft) {
  if (n_frames <= 0 || n_fft <= 0) return 0.0f;
  const double peak = *std::max_element(totals.per_band.begin(), totals.per_band.end());
  if (!(peak > 0.0)) return 0.0f;
  const double window_gain = kHannCoherentGain * static_cast<double>(n_fft);
  return static_cast<float>(std::sqrt(peak / static_cast<double>(n_frames)) / window_gain);
}

}  // namespace

float MeanPowerSpectrum::energy_share_below(float frequency_hz) const noexcept {
  // n_bins is what the measurement declares; power.size() is what it actually
  // holds. A hand-built spectrum can set one without the other, and reading past
  // the vector on the strength of a field is not a measurement error.
  const std::size_t bins = std::min(static_cast<std::size_t>(std::max(n_bins, 0)), power.size());
  if (bins == 0 || n_fft <= 0 || sample_rate <= 0) return 0.0f;
  // A corner that is not a positive real number is not a frequency. Infinity
  // would otherwise sweep the whole spectrum in and answer 1, which reads
  // downstream as a measurement rather than as the absence of one.
  if (!std::isfinite(frequency_hz) || !(frequency_hz > 0.0f)) return 0.0f;

  double total = 0.0;
  for (std::size_t bin = 0; bin < bins; ++bin) {
    total += static_cast<double>(power[bin]);
  }
  if (!(total > 0.0)) return 0.0f;

  const double bin_hz = static_cast<double>(sample_rate) / static_cast<double>(n_fft);
  const double cutoff = static_cast<double>(frequency_hz);
  double below = 0.0;
  for (std::size_t bin = 0; bin < bins; ++bin) {
    // Bin 0 is centred at DC, so its span starts there rather than below zero.
    const double low_hz = bin == 0 ? 0.0 : (static_cast<double>(bin) - 0.5) * bin_hz;
    const double high_hz = (static_cast<double>(bin) + 0.5) * bin_hz;
    if (low_hz >= cutoff) break;
    const double value = static_cast<double>(power[bin]);
    if (high_hz <= cutoff) {
      below += value;
      continue;
    }
    below += value * (cutoff - low_hz) / (high_hz - low_hz);
    break;
  }
  // The ratio of two sums of the same non-negative values cannot leave [0, 1],
  // but rounding can put it a hair outside, and a share above 1 would read
  // downstream as a measurement rather than as float noise.
  return static_cast<float>(std::clamp(below / total, 0.0, 1.0));
}

TrackProfile analyze_track_profile(const TrackInput& track, const TrackProfileConfig& config) {
  TrackProfile profile;
  profile.strip_id = track.id;
  profile.name = track.name;

  // Degenerate input is reported as an unusable track rather than thrown, so a
  // batch call needs no per-track error handling.
  if (track.left == nullptr || track.frame_count == 0) {
    profile.exclusion_reason = "track has no samples";
    return profile;
  }
  if (track.sample_rate <= 0) {
    profile.exclusion_reason = "track sample rate is not positive";
    return profile;
  }

  const std::size_t frames = track.frame_count;
  const bool stereo = track.right != nullptr;
  profile.channel_count = stereo ? 2 : 1;
  profile.duration_sec = static_cast<float>(frames) / static_cast<float>(track.sample_rate);

  mastering::assistant::AudioProfileConfig base_config;
  base_config.n_fft = config.n_fft;
  base_config.hop_length = config.hop_length;

  // Mono sum for the spectral fold. Only band shares are read from it and one
  // STFT is half the cost of one per channel, so L/R are never kept apart here.
  std::vector<float> mono;
  if (stereo) {
    std::vector<float> interleaved(frames * 2);
    mono.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const float left = track.left[frame];
      const float right = track.right[frame];
      interleaved[frame * 2] = left;
      interleaved[frame * 2 + 1] = right;
      mono[frame] = 0.5f * (left + right);
    }
    // BS.1770 channel summing. Profiling the mono downmix instead would read
    // decorrelated stereo roughly 6 dB low.
    profile.base = mastering::assistant::analyze_audio_profile_interleaved(
        interleaved.data(), frames, 2, track.sample_rate, base_config);
  } else {
    profile.base = mastering::assistant::analyze_audio_profile(track.left, frames,
                                                               track.sample_rate, base_config);
  }

  const Audio audio =
      Audio::from_buffer(stereo ? mono.data() : track.left, frames, track.sample_rate);
  const Spectrogram spec =
      Spectrogram::compute(audio, make_stft_config(config.n_fft, config.hop_length));
  profile.bands = fold_bands(spec, track.sample_rate);
  profile.spectrum = mean_power_spectrum(spec, track.sample_rate);

  const BandTotals totals = band_totals(profile.bands);
  profile.band_occupancy = band_occupancy(totals);

  if (profile.duration_sec < config.min_duration_sec) {
    profile.exclusion_reason = "track is shorter than the minimum measurable duration";
    return profile;
  }
  // Negated rather than written as `<=` so a non-finite measurement is excluded
  // too: silence gates out of the integrated loudness entirely and reads -inf.
  // A pure DC offset lands here as well, since K-weighting removes it.
  if (!(profile.base.loudness.integrated_lufs > kSilenceLufsThreshold)) {
    profile.exclusion_reason = "track is silent";
    return profile;
  }
  if (peak_band_amplitude(totals, profile.bands.n_frames, profile.bands.n_fft) <
      kMinBandAmplitude) {
    profile.exclusion_reason = "track has no energy in the analysis bands";
    return profile;
  }

  profile.usable = true;
  return profile;
}

std::vector<TrackProfile> analyze_track_profiles(const std::vector<TrackInput>& tracks,
                                                 const TrackProfileConfig& config) {
  // One config for every track: the cross-track phase compares frame indices
  // directly, which only holds while the STFT geometry is shared. Track lengths
  // are left alone — a short track keeps its own frame count and reads as
  // silent past its end.
  std::vector<TrackProfile> profiles;
  profiles.reserve(tracks.size());
  for (const TrackInput& track : tracks) {
    profiles.push_back(analyze_track_profile(track, config));
  }
  return profiles;
}

}  // namespace sonare::mixing::assistant
