#include "metering/lufs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "rt/biquad_design.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::metering {

namespace {

using Biquad = rt::BiquadCoeffsD;

// Energies are accumulated in `double`; treat anything below this as silence
// rather than feeding it to log10(). Smaller than the float `kEpsilon` so we
// keep the full double dynamic range when the input is genuinely quiet.
constexpr double kEnergyFloor = 1e-15;

// ITU-R BS.1770-4 §2.4 — surround channel weighting for L_s / R_s when
// computing the weighted loudness sum. Applied to the mean-square energy (power
// domain, equivalent to +1.5 dB): 10^(1.5/10) = 1.4125375446227544.
// Spec-mandated bit-exact value; do not round.
constexpr double kBs1770SurroundWeight = 1.4125375446227544;

float energy_to_lufs(double energy) {
  if (energy < kEnergyFloor) return -std::numeric_limits<float>::infinity();
  return static_cast<float>(rt::kLoudnessOffset + 10.0 * std::log10(energy));
}

std::pair<Biquad, Biquad> k_weighting_filters(int sample_rate) {
  const auto coeffs = rt::k_weighting_coefficients(static_cast<double>(sample_rate));
  return {coeffs.pre, coeffs.rlb};
}

// Keep the K-weighted intermediate signal in `double` for the duration of the
// two-stage filter. Narrowing to `float` between stages introduces ~0.01 dB
// rounding on quiet signals and contradicts the project's "double for
// K-weighting" precision contract.
std::vector<double> apply_biquad_double(const double* input, size_t size, const Biquad& coeffs) {
  std::vector<double> output(size);
  double z1 = 0.0;
  double z2 = 0.0;
  for (size_t i = 0; i < size; ++i) {
    const double x = input[i];
    const double y = coeffs.b0 * x + z1;
    z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
    z2 = coeffs.b2 * x - coeffs.a2 * y;
    output[i] = y;
  }
  return output;
}

std::vector<double> to_double(const float* input, size_t size) {
  std::vector<double> output(size);
  for (size_t i = 0; i < size; ++i) {
    output[i] = static_cast<double>(input[i]);
  }
  return output;
}

std::vector<double> k_weighted(const Audio& audio) {
  if (audio.empty()) return {};

  const auto [pre, rlb] = k_weighting_filters(audio.sample_rate());

  const std::vector<double> input_d = to_double(audio.data(), audio.size());
  std::vector<double> filtered = apply_biquad_double(input_d.data(), input_d.size(), pre);
  return apply_biquad_double(filtered.data(), filtered.size(), rlb);
}

std::vector<double> k_weighted_channel(const float* interleaved, size_t frames, int channels,
                                       int channel, int sample_rate) {
  std::vector<double> mono(frames);
  for (size_t frame = 0; frame < frames; ++frame) {
    mono[frame] = static_cast<double>(
        interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
  }

  const auto [pre, rlb] = k_weighting_filters(sample_rate);

  std::vector<double> filtered = apply_biquad_double(mono.data(), mono.size(), pre);
  return apply_biquad_double(filtered.data(), filtered.size(), rlb);
}

double bs1770_channel_weight(int channel, int channels) {
  // ITU-R BS.1770-4 §2.4: front channels (L/R/C) carry unity weight, surround
  // channels (Ls/Rs and, for 7.1, the rear pair Lb/Rb) are weighted +1.5 dB, and
  // the LFE channel is excluded entirely (weight 0). Layouts are keyed off the
  // canonical SMPTE/WAV interleaving order for each channel count.
  switch (channels) {
    case 4:
      // Quad: L, R, Ls(2), Rs(3). No center, no LFE.
      if (channel == 2 || channel == 3) return kBs1770SurroundWeight;
      return 1.0;
    case 5:
      // 5.0: L, R, C, Ls(3), Rs(4).
      if (channel == 3 || channel == 4) return kBs1770SurroundWeight;
      return 1.0;
    case 6:
      // 5.1: L, R, C, LFE(3), Ls(4), Rs(5).
      if (channel == 3) return 0.0;  // LFE excluded.
      if (channel == 4 || channel == 5) return kBs1770SurroundWeight;
      return 1.0;
    case 8:
      // 7.1: L, R, C, LFE(3), Ls(4), Rs(5), Lb(6), Rb(7).
      if (channel == 3) return 0.0;                    // LFE excluded.
      if (channel >= 4) return kBs1770SurroundWeight;  // side + rear surround.
      return 1.0;
    default:
      // Mono, stereo, and unspecified layouts: treat every channel as unity.
      return 1.0;
  }
}

double mean_square(const double* data, size_t start, size_t length) {
  if (length == 0) return 0.0;
  double sum_sq = 0.0;
  for (size_t i = start; i < start + length; ++i) {
    sum_sq += data[i] * data[i];
  }
  return sum_sq / static_cast<double>(length);
}

// When `allow_partial_window` is false (momentary/short-term meters) the window
// is NOT clamped down to the signal length: a signal shorter than the requested
// window emits zero blocks, so the scalar meter reports -inf ("no measurement")
// rather than a value measured over a sub-spec window (metering#2), matching the
// ebur128_loudness_range guard. When true (offline integrated loudness for
// finite clips), the window is clamped to the whole signal so a short clip still
// yields a measurable loudness over the audio that exists.
std::vector<double> block_energies(const std::vector<double>& samples, int sample_rate,
                                   float duration_sec, float overlap,
                                   bool allow_partial_window = false) {
  if (samples.empty()) return {};
  const size_t block_size =
      std::max<size_t>(1, static_cast<size_t>(std::round(duration_sec * sample_rate)));
  const size_t window = allow_partial_window ? std::min(block_size, samples.size()) : block_size;
  // Ceiling is 0.99 (not 0.95) so the short-term 100 ms hop @ 3 s window
  // (overlap = 1 - 4800/144000 = 0.9667 @ 48 kHz) survives. A 0.95 ceiling would
  // round the hop up to 150 ms and break EBU R128 short-term/LRA.
  const float clamped_overlap = std::clamp(overlap, 0.0f, 0.99f);
  const size_t hop =
      std::max<size_t>(1, static_cast<size_t>(std::round(window * (1.0f - clamped_overlap))));

  // Emit only complete `window` blocks (ITU-R BS.1770-4): a trailing partial
  // block would be averaged over its own short length, inflating its energy and
  // contaminating the momentary/short-term/gating statistics.
  std::vector<double> energies;
  for (size_t start = 0; start + window <= samples.size(); start += hop) {
    energies.push_back(mean_square(samples.data(), start, window));
  }
  return energies;
}

std::vector<std::vector<double>> k_weighted_channels(const float* interleaved, size_t frames,
                                                     int channels, int sample_rate) {
  std::vector<std::vector<double>> weighted_channels;
  weighted_channels.reserve(static_cast<size_t>(channels));
  for (int channel = 0; channel < channels; ++channel) {
    weighted_channels.push_back(
        k_weighted_channel(interleaved, frames, channels, channel, sample_rate));
  }
  return weighted_channels;
}

// See block_energies for the `allow_partial_window` contract (metering#2).
std::vector<double> block_energies_weighted_channels(
    const std::vector<std::vector<double>>& weighted_channels, size_t frames, int channels,
    int sample_rate, float duration_sec, float overlap, bool allow_partial_window = false) {
  if (weighted_channels.empty() || frames == 0) return {};

  const size_t block_size =
      std::max<size_t>(1, static_cast<size_t>(std::round(duration_sec * sample_rate)));
  const size_t window = allow_partial_window ? std::min(block_size, frames) : block_size;
  // Ceiling is 0.99 (not 0.95) so the short-term 100 ms hop @ 3 s window
  // (overlap = 1 - 4800/144000 = 0.9667 @ 48 kHz) survives. A 0.95 ceiling would
  // round the hop up to 150 ms and break EBU R128 short-term/LRA.
  const float clamped_overlap = std::clamp(overlap, 0.0f, 0.99f);
  const size_t hop =
      std::max<size_t>(1, static_cast<size_t>(std::round(window * (1.0f - clamped_overlap))));

  // Emit only complete `window` blocks (ITU-R BS.1770-4); a trailing partial
  // block would inflate its mean-square energy and contaminate the gated
  // loudness.
  std::vector<double> energies;
  for (size_t start = 0; start + window <= frames; start += hop) {
    double energy = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      energy += bs1770_channel_weight(channel, channels) *
                mean_square(weighted_channels[static_cast<size_t>(channel)].data(), start, window);
    }
    energies.push_back(energy);
  }
  return energies;
}

float gated_integrated_lufs(const std::vector<double>& energies, const LufsConfig& config) {
  std::vector<double> absolute_gated;
  absolute_gated.reserve(energies.size());
  for (double energy : energies) {
    if (energy_to_lufs(energy) >= config.absolute_gate_lufs) {
      absolute_gated.push_back(energy);
    }
  }

  if (absolute_gated.empty()) return -std::numeric_limits<float>::infinity();

  const double preliminary = std::accumulate(absolute_gated.begin(), absolute_gated.end(), 0.0) /
                             static_cast<double>(absolute_gated.size());
  const float relative_gate = energy_to_lufs(preliminary) + config.relative_gate_lu;

  std::vector<double> relative_gated;
  relative_gated.reserve(absolute_gated.size());
  for (double energy : absolute_gated) {
    if (energy_to_lufs(energy) >= relative_gate) {
      relative_gated.push_back(energy);
    }
  }

  if (relative_gated.empty()) return -std::numeric_limits<float>::infinity();
  const double integrated = std::accumulate(relative_gated.begin(), relative_gated.end(), 0.0) /
                            static_cast<double>(relative_gated.size());
  return energy_to_lufs(integrated);
}

std::vector<float> energies_to_lufs(const std::vector<double>& energies) {
  std::vector<float> out;
  out.reserve(energies.size());
  for (double energy : energies) {
    out.push_back(energy_to_lufs(energy));
  }
  return out;
}

float last_or_silence(const std::vector<float>& values) {
  return values.empty() ? -std::numeric_limits<float>::infinity() : values.back();
}

// Interpolated percentile on an ascending-sorted vector: linear interpolation
// between the bracketing ranks, matching dynamic_range.cpp's percentile_sorted
// so the two windowed-distribution range metrics stay consistent. The previous
// nearest-rank (floor) selection collapsed the 10th and 95th percentiles onto
// the same sample for very small block counts -- e.g. exactly 2 gated blocks
// gave low_index == high_index == 0 and thus LRA 0 LU even when the two
// short-term values differed by many LU (metering#1).
double percentile_sorted(const std::vector<float>& sorted, double percentile) {
  if (sorted.empty()) return 0.0;
  const double position = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
  const size_t low = static_cast<size_t>(std::floor(position));
  const size_t high = static_cast<size_t>(std::ceil(position));
  if (low == high) return static_cast<double>(sorted[low]);
  const double frac = position - static_cast<double>(low);
  return static_cast<double>(sorted[low]) * (1.0 - frac) + static_cast<double>(sorted[high]) * frac;
}

float short_term_overlap_for(int sample_rate, float duration_sec) {
  if (sample_rate <= 0 || duration_sec <= 0.0f) return 0.0f;
  const float block = duration_sec * static_cast<float>(sample_rate);
  const float hop =
      std::max(1.0f, std::round(kLufsShortTermHopSec * static_cast<float>(sample_rate)));
  // Ceiling matches block_energies* (0.99) so the derived overlap reproduces the
  // exact 100 ms short-term hop instead of being truncated to a 150 ms hop.
  return std::clamp(1.0f - hop / block, 0.0f, 0.99f);
}

void validate_config(const LufsConfig& config) {
  SONARE_CHECK(config.block_duration_sec > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.block_overlap >= 0.0f && config.block_overlap < 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.momentary_duration_sec > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.short_term_duration_sec > 0.0f, ErrorCode::InvalidParameter);
}

}  // namespace

LufsResult lufs(const Audio& audio, const LufsConfig& config) {
  return lufs_interleaved(audio.data(), audio.size(), 1, audio.sample_rate(), config);
}

LufsResult lufs_interleaved(const float* samples, size_t frames, int channels, int sample_rate,
                            const LufsConfig& config) {
  validate_config(config);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(channels > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(samples != nullptr || frames == 0, ErrorCode::InvalidParameter);

  const auto weighted_channels = k_weighted_channels(samples, frames, channels, sample_rate);
  // Integrated loudness is an offline whole-clip measurement: allow the gating
  // window to shrink to the signal so a sub-400 ms clip still yields a value.
  // Momentary/short-term below stay strict (no measurement until a full window).
  const std::vector<double> integrated_blocks = block_energies_weighted_channels(
      weighted_channels, frames, channels, sample_rate, config.block_duration_sec,
      config.block_overlap, /*allow_partial_window=*/true);
  // ITU-R BS.1770-4 Annex 2: momentary uses a fixed 75% overlap (100 ms hop @ 400 ms),
  // independent of `config.block_overlap` (which controls integrated gating density).
  const std::vector<float> momentary = energies_to_lufs(
      block_energies_weighted_channels(weighted_channels, frames, channels, sample_rate,
                                       config.momentary_duration_sec, kLufsMomentaryOverlap));
  const std::vector<float> short_term = energies_to_lufs(block_energies_weighted_channels(
      weighted_channels, frames, channels, sample_rate, config.short_term_duration_sec,
      short_term_overlap_for(sample_rate, config.short_term_duration_sec)));

  LufsResult result;
  result.integrated_lufs = gated_integrated_lufs(integrated_blocks, config);
  result.momentary_lufs = last_or_silence(momentary);
  result.short_term_lufs = last_or_silence(short_term);
  result.loudness_range = lra_from_short_term_blocks(short_term);
  return result;
}

float ebur128_loudness_range(const Audio& audio) {
  // k_weighted() interprets `audio.data()` as a single mono channel. Reject
  // multi-channel input explicitly so future Audio subclasses (or callers that
  // smuggle interleaved buffers via a wrapper) get a clear error instead of a
  // garbage LRA reading. Today Audio is mono-only so this branch is effectively
  // a future-proof guard, but it documents the contract at runtime.
  SONARE_CHECK_MSG(audio.channels() == 1, ErrorCode::InvalidParameter,
                   "ebur128_loudness_range requires mono input");
  if (audio.empty()) return 0.0f;

  // EBU Tech 3342: short-term loudness, 3 s window, 100 ms hop.
  constexpr double kWindowSec = 3.0;
  constexpr double kHopSec = 0.1;
  const int sample_rate = audio.sample_rate();
  const std::vector<double> weighted = k_weighted(audio);
  if (weighted.empty()) return 0.0f;

  const size_t block_size =
      std::max<size_t>(1, static_cast<size_t>(std::round(kWindowSec * sample_rate)));
  const size_t hop = std::max<size_t>(1, static_cast<size_t>(std::round(kHopSec * sample_rate)));

  // Compute short-term loudness blocks (full-length blocks only, per EBU R128).
  std::vector<float> short_term;
  if (weighted.size() >= block_size) {
    short_term.reserve((weighted.size() - block_size) / hop + 1);
    for (size_t start = 0; start + block_size <= weighted.size(); start += hop) {
      short_term.push_back(energy_to_lufs(mean_square(weighted.data(), start, block_size)));
    }
  }

  return lra_from_short_term_blocks(short_term);
}

float lra_from_short_term_blocks(const std::vector<float>& short_term_lufs) {
  // Stage 1 — absolute gate at -70 LUFS.
  std::vector<float> abs_gated;
  abs_gated.reserve(short_term_lufs.size());
  for (float value : short_term_lufs) {
    if (std::isfinite(value) && value >= kLufsAbsoluteGate) abs_gated.push_back(value);
  }
  if (abs_gated.size() < 2) return 0.0f;

  // Stage 2 — relative gate 20 LU below the mean of the absolute-gated loudness.
  // Average in the linear (energy) domain, then convert back to LUFS.
  double mean_energy = 0.0;
  for (float value : abs_gated) {
    mean_energy += std::pow(10.0, (static_cast<double>(value) - rt::kLoudnessOffset) / 10.0);
  }
  mean_energy /= static_cast<double>(abs_gated.size());
  const float relative_gate = energy_to_lufs(mean_energy) + kLufsRangeRelativeGate;

  std::vector<float> gated;
  gated.reserve(abs_gated.size());
  for (float value : abs_gated) {
    if (value >= relative_gate) gated.push_back(value);
  }
  if (gated.size() < 2) return 0.0f;

  std::sort(gated.begin(), gated.end());
  const double low = percentile_sorted(gated, 0.10);
  const double high = percentile_sorted(gated, 0.95);
  return static_cast<float>(high - low);
}

std::vector<float> momentary_lufs(const Audio& audio, const LufsConfig& config) {
  validate_config(config);

  // ITU-R BS.1770-4 Annex 2: momentary uses a fixed 75% overlap (100 ms hop @ 400 ms),
  // independent of `config.block_overlap`.
  const std::vector<double> weighted = k_weighted(audio);
  return energies_to_lufs(block_energies(weighted, audio.sample_rate(),
                                         config.momentary_duration_sec, kLufsMomentaryOverlap));
}

std::vector<float> short_term_lufs(const Audio& audio, const LufsConfig& config) {
  validate_config(config);

  const std::vector<double> weighted = k_weighted(audio);
  return energies_to_lufs(
      block_energies(weighted, audio.sample_rate(), config.short_term_duration_sec,
                     short_term_overlap_for(audio.sample_rate(), config.short_term_duration_sec)));
}

}  // namespace sonare::metering
