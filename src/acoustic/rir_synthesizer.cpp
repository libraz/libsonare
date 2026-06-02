#include "acoustic/rir_synthesizer.h"

#include <algorithm>
#include <cmath>

#include "acoustic/image_source.h"
#include "util/constants.h"

namespace sonare::acoustic {

namespace {

// Auto mixing-time bounds (ms): the crossover sits after the early-reflection
// cluster but well inside any musically useful tail.
constexpr float kMinMixingMs = 3.0f;
constexpr float kMaxMixingMs = 150.0f;

// Safety ceiling for the auto-sized late tail, mirroring synthesize_late_tail's
// own internal cap so the clamp telemetry below does not false-positive (or the
// length estimate overflow) when a near-rigid room yields an unbounded RT60.
constexpr double kLateTailCeiling = static_cast<double>(1 << 26);

// RMS over [center - half, center + half], clamped to the buffer.
float local_rms(const std::vector<float>& x, int center, int half) noexcept {
  const int n = static_cast<int>(x.size());
  if (n == 0) return 0.0f;
  const int lo = std::max(0, center - half);
  const int hi = std::min(n, center + half + 1);
  if (hi <= lo) return 0.0f;
  double sum = 0.0;
  for (int i = lo; i < hi; ++i) sum += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(sum / static_cast<double>(hi - lo)));
}

}  // namespace

RirSynthResult synthesize_rir(const ShoeboxRoom& room, const SourceListener& placement,
                              int sample_rate, const RirSynthConfig& config) {
  RirSynthResult result;
  result.diagnostics = validate_shoebox(room, placement);
  if (has_error(result.diagnostics) || sample_rate <= 0) {
    result.rir = Audio::from_vector(std::vector<float>{}, sample_rate > 0 ? sample_rate : 1);
    return result;
  }

  const float sr = static_cast<float>(sample_rate);

  // Early reflections (image-source) and the per-band reverberation time.
  const std::vector<ImageSource> images = shoebox_image_sources(room, placement, config.ism_order);
  const Audio early_audio = synthesize_early_ir(images, sample_rate);
  const ReverbTime rt = shoebox_reverb_time(room, config.late_model);

  // Decide the cap up front so we can both avoid over-allocating the late tail
  // and report when max_seconds actually truncates the natural tail. The natural
  // tail length mirrors synthesize_late_tail's default headroom (2x the longest
  // RT60); if that grows past the cap, we clamp and emit a Warning.
  float longest = 0.0f;
  for (float t : rt.rt60_bands) longest = std::max(longest, t);
  // Mirror synthesize_late_tail's ceiling so the estimate cannot overflow int or
  // claim a clamp that the late tail's own cap (not max_seconds) actually made.
  const double natural_tail_d =
      std::min(std::ceil(static_cast<double>(longest) * 2.0 * sr), kLateTailCeiling);
  const int natural_len =
      std::max(static_cast<int>(early_audio.size()), static_cast<int>(natural_tail_d));

  LateReverbConfig late_cfg;
  late_cfg.seed = config.seed;
  int cap = 0;  // 0 = no cap
  if (config.max_seconds > 0.0f) {
    cap = std::max(1, static_cast<int>(std::ceil(config.max_seconds * sr)));
    late_cfg.max_samples = cap;  // avoid synthesizing tail past the cap
  }
  const Audio late_audio = synthesize_late_tail(rt, sample_rate, late_cfg);

  const std::vector<float> early(early_audio.begin(), early_audio.end());
  const std::vector<float> late(late_audio.begin(), late_audio.end());

  // Mixing time: the early/late crossover. Auto estimate ~ sqrt(V) ms (physical
  // mixing time grows with room volume), clamped to a sensible range.
  const float volume = shoebox_volume(room);
  float mixing_ms =
      config.mixing_time_ms > 0.0f ? config.mixing_time_ms : std::sqrt(std::max(volume, 0.0f));
  mixing_ms = std::clamp(mixing_ms, kMinMixingMs, kMaxMixingMs);
  const int half_xfade = std::max(
      1, static_cast<int>(std::lround(std::max(0.0f, config.crossfade_ms) * 0.001f * sr * 0.5f)));

  // The direct sound (and the crossfade head) must never be faded: push the
  // crossover so its start t0 = t_mix - half_xfade lands at or after the direct
  // arrival. sqrt(V) alone ignores the source->listener delay and can otherwise
  // attenuate the direct impulse.
  const float direct_dist = length(placement.listener - placement.source);
  const int direct_sample = static_cast<int>(std::lround(direct_dist / kSoundSpeed * sr));
  int t_mix = static_cast<int>(std::lround(mixing_ms * 0.001f * sr));
  t_mix = std::max(t_mix, direct_sample + half_xfade);

  // Level-match the late tail to the early reflections across the crossover so
  // the splice has no energy discontinuity. A wider window than the crossfade
  // gives a stable estimate of the (sparse, decaying) early-reflection level.
  const int level_half = std::max(half_xfade, static_cast<int>(std::lround(0.005f * sr)));
  const float early_ref = local_rms(early, t_mix, level_half);
  const int late_center = late.empty() ? 0 : std::min(t_mix, static_cast<int>(late.size()) - 1);
  const float late_ref = local_rms(late, late_center, level_half);
  float scale = 1.0f;
  if (late_ref > 1e-9f) {
    if (early_ref > 1e-9f) {
      scale = early_ref / late_ref;
    } else {
      // Sparse/absent early energy at the crossover: fall back to the physical
      // diffuse level a reflection travelling c*t_mix would carry, 1/(4*pi*d).
      const float d_mix = std::max(kSoundSpeed * static_cast<float>(t_mix) / sr, 0.1f);
      scale = (1.0f / (4.0f * sonare::constants::kPi * d_mix)) / late_ref;
    }
  }

  int length = std::max(static_cast<int>(early.size()), static_cast<int>(late.size()));
  if (cap > 0) {
    if (natural_len > cap) {
      result.diagnostics.push_back({Diagnostic::Severity::Warning, "acoustic.rir_length_clamped",
                                    "synthesized RIR length exceeded max_seconds and was clamped"});
    }
    length = std::min(length, cap);
  }
  if (length < 1) length = 1;

  // Equal-power crossfade (decorrelated early vs. noise late => energy-preserving):
  // early-only before t0, late-only after t1, ramping across [t0, t1]. t0 >= the
  // direct arrival (enforced above) so the direct sound is rendered at full level.
  const int t0 = std::max(0, t_mix - half_xfade);
  const int t1 = std::max(t0 + 1, t_mix + half_xfade);
  std::vector<float> rir(static_cast<size_t>(length), 0.0f);
  for (int i = 0; i < length; ++i) {
    const float e = i < static_cast<int>(early.size()) ? early[static_cast<size_t>(i)] : 0.0f;
    const float l =
        (i < static_cast<int>(late.size()) ? late[static_cast<size_t>(i)] : 0.0f) * scale;
    float x;
    if (i <= t0) {
      x = 0.0f;
    } else if (i >= t1) {
      x = 1.0f;
    } else {
      x = static_cast<float>(i - t0) / static_cast<float>(t1 - t0);
    }
    rir[static_cast<size_t>(i)] = e * std::sqrt(1.0f - x) + l * std::sqrt(x);
  }

  result.rir = Audio::from_vector(std::move(rir), sample_rate);
  return result;
}

}  // namespace sonare::acoustic
