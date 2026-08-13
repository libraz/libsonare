#include "metering/lufs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "metering/bs1770_weighting.h"
#include "rt/biquad_design.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::metering {

namespace {

using Biquad = rt::BiquadCoeffsD;

// Energies are accumulated in `double`; treat anything below this as silence
// rather than feeding it to log10(). Smaller than the float `kEpsilon` so we
// keep the full double dynamic range when the input is genuinely quiet.
constexpr double kEnergyFloor = 1e-15;

// Silence sentinel rationale: unlike the level-in-dB meters (true_peak /
// spectrum / dynamic_range), which report the finite kFloorDb (-120 dB) for
// silence, LUFS deliberately keeps -inf. -inf ("below the measurement floor") is
// the canonical ITU-R BS.1770-4 / EBU R128 convention for an unmeasurable block,
// and the value doubles as an internal gating sentinel throughout this file
// (absolute/relative gate comparisons, last_or_silence). Forcing it to -120
// would silently alter a standardized loudness algorithm, so it is left as-is.
// JSON serialization safety for this -inf is handled in util/json.h instead.
float energy_to_lufs(double energy) {
  return power_to_offset_db(energy, rt::kLoudnessOffset, kEnergyFloor,
                            -std::numeric_limits<float>::infinity());
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

void apply_biquad_double_in_place(std::vector<double>* samples, const Biquad& coeffs) {
  double z1 = 0.0;
  double z2 = 0.0;
  for (double& sample : *samples) {
    const double x = sample;
    const double y = coeffs.b0 * x + z1;
    z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
    z2 = coeffs.b2 * x - coeffs.a2 * y;
    sample = y;
  }
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

struct BlockEnergyAccumulator {
  size_t window = 0;
  size_t hop = 0;
  std::vector<double> energies;
};

float short_term_overlap_for(int sample_rate, float duration_sec);

BlockEnergyAccumulator make_block_accumulator(size_t frames, int sample_rate, float duration_sec,
                                              float overlap, bool allow_partial_window) {
  BlockEnergyAccumulator accumulator;
  const size_t block_size =
      std::max<size_t>(1, static_cast<size_t>(std::round(duration_sec * sample_rate)));
  accumulator.window = allow_partial_window ? std::min(block_size, frames) : block_size;
  const float clamped_overlap = std::clamp(overlap, 0.0f, 0.99f);
  accumulator.hop = std::max<size_t>(
      1, static_cast<size_t>(std::round(accumulator.window * (1.0f - clamped_overlap))));
  if (accumulator.window > 0 && frames >= accumulator.window) {
    accumulator.energies.assign((frames - accumulator.window) / accumulator.hop + 1, 0.0);
  }
  return accumulator;
}

void accumulate_channel_blocks(const std::vector<double>& weighted, double channel_weight,
                               BlockEnergyAccumulator* accumulator) {
  if (accumulator->window == 0 || accumulator->energies.empty()) return;
  size_t block = 0;
  for (size_t start = 0; start + accumulator->window <= weighted.size();
       start += accumulator->hop, ++block) {
    accumulator->energies[block] +=
        channel_weight * mean_square(weighted.data(), start, accumulator->window);
  }
}

struct WeightedBlockEnergies {
  std::vector<double> integrated;
  std::vector<double> momentary;
  std::vector<double> short_term;
};

WeightedBlockEnergies k_weighted_block_energies(const float* interleaved, size_t frames,
                                                int channels, int sample_rate,
                                                const LufsConfig& config) {
  std::vector<double> scratch(frames);
  const auto [pre, rlb] = k_weighting_filters(sample_rate);
  BlockEnergyAccumulator integrated =
      make_block_accumulator(frames, sample_rate, config.block_duration_sec, config.block_overlap,
                             /*allow_partial_window=*/true);
  BlockEnergyAccumulator momentary =
      make_block_accumulator(frames, sample_rate, config.momentary_duration_sec,
                             kLufsMomentaryOverlap, /*allow_partial_window=*/false);
  BlockEnergyAccumulator short_term =
      make_block_accumulator(frames, sample_rate, config.short_term_duration_sec,
                             short_term_overlap_for(sample_rate, config.short_term_duration_sec),
                             /*allow_partial_window=*/false);
  for (int channel = 0; channel < channels; ++channel) {
    for (size_t frame = 0; frame < frames; ++frame) {
      scratch[frame] = static_cast<double>(
          interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)]);
    }
    apply_biquad_double_in_place(&scratch, pre);
    apply_biquad_double_in_place(&scratch, rlb);
    const double weight = bs1770_channel_weight(channel, channels);
    accumulate_channel_blocks(scratch, weight, &integrated);
    accumulate_channel_blocks(scratch, weight, &momentary);
    accumulate_channel_blocks(scratch, weight, &short_term);
  }
  return {std::move(integrated.energies), std::move(momentary.energies),
          std::move(short_term.energies)};
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

float max_or_silence(const std::vector<float>& values) {
  return values.empty() ? -std::numeric_limits<float>::infinity()
                        : *std::max_element(values.begin(), values.end());
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

  const WeightedBlockEnergies blocks =
      k_weighted_block_energies(samples, frames, channels, sample_rate, config);
  // Integrated loudness is an offline whole-clip measurement: allow the gating
  // window to shrink to the signal so a sub-400 ms clip still yields a value.
  // Momentary/short-term below stay strict (no measurement until a full window).
  // ITU-R BS.1770-4 Annex 2: momentary uses a fixed 75% overlap (100 ms hop @ 400 ms),
  // independent of `config.block_overlap` (which controls integrated gating density).
  const std::vector<float> momentary = energies_to_lufs(blocks.momentary);
  const std::vector<float> short_term = energies_to_lufs(blocks.short_term);

  LufsResult result;
  result.integrated_lufs = gated_integrated_lufs(blocks.integrated, config);
  result.momentary_lufs = last_or_silence(momentary);
  result.short_term_lufs = last_or_silence(short_term);
  result.max_momentary_lufs = max_or_silence(momentary);
  result.max_short_term_lufs = max_or_silence(short_term);
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
    mean_energy += db_to_power_scalar(static_cast<double>(value) - rt::kLoudnessOffset);
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
  // Interpolated, not nearest-rank: with only a handful of gated blocks a
  // nearest-rank selection collapses the 10th and 95th percentiles onto the
  // same sample and reports 0 LU even when the short-term values differ by
  // many LU. percentile_sorted is shared with the dynamic-range metrics so the
  // two windowed-distribution ranges cannot drift apart.
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
