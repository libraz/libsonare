#include "acoustic/rir_synthesizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

namespace sonare::acoustic {

namespace {

// Auto mixing-time bounds (ms): the crossover sits after the early-reflection
// cluster but well inside any musically useful tail.
constexpr float kMinMixingMs = 3.0f;
constexpr float kMaxMixingMs = 150.0f;

// Fraction by which fully-rough walls (mean scattering = 1) pull the auto mixing
// time earlier: at scattering = 1 the auto mixing time is (1 - this) of the
// purely volume-derived sqrt(V) ms. Bounded so the shift stays physically sane
// and the mixing time never collapses to zero.
constexpr float kScatterMixingShift = 0.4f;

// Relative boost applied to the level-matched late tail per unit mean scattering
// (scale *= 1 + this * mean_scattering). Bounded so a fully-rough room adds at
// most this fraction of diffuse energy; keeps the early/late balance monotonic
// in scattering even when the mixing time is pinned or clamped to the direct
// arrival.
constexpr float kScatterLateBoost = 0.5f;

// RMS over the half-open sample range [lo, hi), clamped to [0, n). Delegates to
// the shared sonare::rms primitive over the clamped sub-range. Takes a raw
// pointer + length so it reads the synthesized buffers in place (no copy).
float rms_range(const float* x, int n, int lo, int hi) noexcept {
  lo = std::max(0, lo);
  hi = std::min(n, hi);
  if (hi <= lo) return 0.0f;
  return sonare::rms(x + lo, static_cast<size_t>(hi - lo));
}

// True if any image carries a frequency-dependent (non-flat across octave bands)
// reflection product. A spectrally flat room's per-band reflection equals its
// broadband RMS collapse, so the broadband early IR already carries all of the
// colour and no per-band correction is needed (the coloured result is identical).
bool early_reflections_are_colored(const std::vector<ImageSource>& images) noexcept {
  for (const auto& im : images) {
    for (size_t b = 1; b < im.reflection.size(); ++b) {
      if (std::fabs(im.reflection[b] - im.reflection[0]) > 1e-6f) return true;
    }
  }
  return false;
}

// Colour the early reflections per octave band so material-dependent timbre
// (a curtain absorbing highs vs glass reflecting them) survives on the first
// arrivals, mirroring the per-band shaping the late tail already applies on the
// same octave grid. The broadband IR collapses each image's per-band reflection
// vector to a single RMS gain; here we add, per octave band, the bandpassed
// deviation of that band's own early IR from the broadband IR. Out-of-band
// energy stays at the broadband level, and a spectrally flat room yields a zero
// correction, so the coloured result reduces exactly to the broadband IR.
Audio color_early_ir(const std::vector<ImageSource>& images, int sample_rate,
                     const Audio& broadband, const EarlyIrConfig& base_cfg) {
  size_t bands = 1;
  for (const auto& im : images) bands = std::max(bands, im.reflection.size());
  const int n = static_cast<int>(broadband.size());
  const float* b = broadband.data();
  std::vector<float> out(b, b + n);

  const float nyquist = static_cast<float>(sample_rate) * 0.5f;
  // Reused across every band iteration instead of freshly allocated: per band
  // this function already holds out, broadband, and per_band concurrently
  // (each a full RIR-length buffer), and re-allocating dev on top of that on
  // every one of up to ~11 octave-band iterations multiplies allocator churn
  // well past what a caller sizing to the module's allocation cap expects.
  std::vector<float> dev(static_cast<size_t>(n), 0.0f);
  for (size_t band = 0; band < bands; ++band) {
    const float center = octave_center_hz(static_cast<int>(band));
    // Skip a band whose octave sits at/above Nyquist, exactly as the late tail
    // does; its content is not representable and stays at the broadband level.
    if (center * sonare::constants::kSqrt2 >= nyquist) continue;
    EarlyIrConfig cfg = base_cfg;
    cfg.band = static_cast<int>(band);
    const Audio per_band = synthesize_early_ir(images, sample_rate, cfg);
    const float* e = per_band.data();
    const int lim = std::min(n, static_cast<int>(per_band.size()));
    std::fill(dev.begin(), dev.end(), 0.0f);
    for (int i = 0; i < lim; ++i) dev[static_cast<size_t>(i)] = e[i] - b[i];
    octave_bandpass_zero_phase(dev, center, sample_rate);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] += dev[static_cast<size_t>(i)];
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

}  // namespace

std::vector<Diagnostic> validate_rir_synth_config(const RirSynthConfig& config) {
  std::vector<Diagnostic> diagnostics;
  if (config.ism_order < 0 ||
      !numeric::finite_in_closed_range(config.max_seconds, 0.0f, kMaxRirSeconds) ||
      !numeric::finite_in_closed_range(config.mixing_time_ms, 0.0f, kMaxRirMixingTimeMs) ||
      !numeric::finite_in_closed_range(config.crossfade_ms, 0.0f, kMaxRirCrossfadeMs)) {
    diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.invalid_rir_config",
                           "RIR timing values must be finite and within safe bounds"});
  }
  if (config.air_absorption_enabled &&
      (!numeric::finite(config.air.temperature_c) ||
       config.air.temperature_c <= kAbsoluteZeroCelsius ||
       !numeric::finite_in_closed_range(config.air.humidity_percent, 0.0f, 100.0f))) {
    diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.invalid_air_absorption",
                           "air absorption temperature/humidity is outside the physical range"});
  }
  return diagnostics;
}

RirSynthResult synthesize_rir(const ShoeboxRoom& room, const SourceListener& placement,
                              int sample_rate, const RirSynthConfig& config) {
  RirSynthResult result;
  result.diagnostics = validate_shoebox(room, placement);
  const std::vector<Diagnostic> config_diagnostics = validate_rir_synth_config(config);
  result.diagnostics.insert(result.diagnostics.end(), config_diagnostics.begin(),
                            config_diagnostics.end());
  if (sample_rate < kMinAudioSampleRate || sample_rate > kMaxAudioSampleRate) {
    result.diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.invalid_sample_rate",
                                  "sample rate is outside supported bounds"});
  }
  if (has_error(result.diagnostics)) {
    const int diagnostic_sample_rate =
        sample_rate >= kMinAudioSampleRate && sample_rate <= kMaxAudioSampleRate ? sample_rate
                                                                                 : 48000;
    result.rir = Audio::from_vector(std::vector<float>{}, diagnostic_sample_rate);
    return result;
  }

  const float sr = static_cast<float>(sample_rate);

  // Bound the image-source order: cost grows ~ order^3, so an unbounded value is
  // a memory/CPU exhaustion vector. shoebox_image_sources clamps internally; we
  // mirror the clamp here only to inform the caller via a diagnostic.
  const int ism_order = std::min(config.ism_order, kMaxImageSourceOrder);
  if (config.ism_order > kMaxImageSourceOrder) {
    result.diagnostics.push_back({Diagnostic::Severity::Warning, "acoustic.ism_order_clamped",
                                  "ism_order exceeded the safe maximum and was clamped"});
  }

  // A max_seconds cap bounds every synthesized buffer. The shared acoustic
  // working-set cap also applies when max_seconds is omitted, so the early,
  // late, colouring, and final RIR buffers remain bounded together.
  constexpr int kWorkingSetCap = static_cast<int>(resource::kMaxAcousticRirSamples);
  const int requested_cap = config.max_seconds > 0.0f
                                ? std::max(1, static_cast<int>(std::ceil(config.max_seconds * sr)))
                                : kWorkingSetCap;
  const int cap = std::min(requested_cap, kWorkingSetCap);

  // Early reflections (image-source) and the per-band reverberation time.
  const std::vector<ImageSource> images = shoebox_image_sources(room, placement, ism_order);
  EarlyIrConfig early_cfg;
  early_cfg.max_samples = cap;  // upper bound only; a shorter natural IR is not padded to it
  Audio early_audio = synthesize_early_ir(images, sample_rate, early_cfg);
  // Frequency-dependent walls colour early reflections per octave band; a
  // spectrally flat room already carries all its colour in the broadband IR, so
  // the coloured path is skipped and the result is unchanged bit-for-bit.
  if (early_reflections_are_colored(images)) {
    early_audio = color_early_ir(images, sample_rate, early_audio, early_cfg);
  }
  const ReverbTime rt = shoebox_reverb_time(room, config.late_model,
                                            config.air_absorption_enabled ? &config.air : nullptr);

  // The early IR is now capped to the cap, so early_audio.size() no longer reveals
  // the natural (uncapped) early length. Mirror synthesize_early_ir's own auto-size
  // formula here so the rir_length_clamped diagnostic below fires when the cap
  // truncates the early reflections, not just the late tail.
  float early_max_delay = 0.0f;
  for (const auto& im : images) {
    if (im.distance > 1e-6f) {
      early_max_delay = std::max(early_max_delay, im.distance / kSoundSpeed * sr);
    }
  }
  const int early_half = (early_cfg.fdl < 1 ? 1 : early_cfg.fdl | 1) / 2;
  const double early_raw =
      std::ceil(static_cast<double>(early_max_delay)) + static_cast<double>(early_half) + 2.0;
  const int early_natural_len =
      std::max(1, static_cast<int>(std::min(early_raw, static_cast<double>(kMaxAutoSamples))));

  // Keep clamp telemetry aligned with synthesize_late_tail() through the shared
  // allocation-free resolver: above-Nyquist bands and the 60-second sizing
  // policy are applied exactly once in the common helper.
  LateReverbConfig natural_late_cfg;
  const LateTailResolution late_resolution = resolve_late_tail(rt, sample_rate, natural_late_cfg);
  const std::size_t natural_tail_samples = late_resolution.samples;
  const std::size_t natural_len =
      std::max(static_cast<std::size_t>(early_natural_len), natural_tail_samples);

  LateReverbConfig late_cfg;
  late_cfg.seed = config.seed;
  late_cfg.max_samples = cap;  // avoid synthesizing tail past the cap
  const Audio late_audio = synthesize_late_tail(rt, sample_rate, late_cfg);

  // Read the synthesized buffers in place: a full std::vector copy of each would
  // transiently double the (already large) RIR working set for no benefit.
  const float* early = early_audio.data();
  const float* late = late_audio.data();
  const int early_n = static_cast<int>(early_audio.size());
  const int late_n = static_cast<int>(late_audio.size());

  // Mean wall scattering (rough surfaces) biases the early/late split: rougher
  // walls diffuse specular energy into the diffuse late field both *sooner* (an
  // earlier auto mixing time) and *more strongly* (a higher relative late level).
  // Both uses are bounded and monotonic in mean_scattering; an explicit
  // config.mixing_time_ms override skips the timing shift but keeps the energy
  // bias (the diffusion is a material property, not a crossover choice).
  const float mean_scattering = shoebox_mean_scattering(room);  // [0,1]

  // Mixing time: the early/late crossover. Auto estimate ~ sqrt(V) ms (physical
  // mixing time grows with room volume), pulled earlier by scattering, clamped
  // to a sensible range.
  const float volume = shoebox_volume(room);
  float mixing_ms;
  if (config.mixing_time_ms > 0.0f) {
    // Public validation permits an intentional long crossover up to
    // kMaxRirMixingTimeMs. Do not silently collapse that request to the much
    // smaller auto-estimate range.
    mixing_ms = config.mixing_time_ms;
  } else {
    const float scatter_factor = 1.0f - kScatterMixingShift * mean_scattering;
    mixing_ms = std::sqrt(std::max(volume, 0.0f)) * scatter_factor;
    mixing_ms = std::clamp(mixing_ms, kMinMixingMs, kMaxMixingMs);
  }
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
  // The early window must start strictly AFTER the direct tap: t_mix is clamped
  // to direct_sample + half_xfade, so a symmetric window would otherwise capture
  // the (loudest) direct impulse and inflate the early level, over-scaling the
  // tail in small rooms.
  const int level_half = std::max(half_xfade, static_cast<int>(std::lround(0.005f * sr)));
  const int early_lo = std::max(t_mix - level_half, direct_sample + 1);
  const float early_ref = rms_range(early, early_n, early_lo, t_mix + level_half + 1);
  const int late_center = late_n == 0 ? 0 : std::min(t_mix, late_n - 1);
  const float late_ref =
      rms_range(late, late_n, late_center - level_half, late_center + level_half + 1);
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
  // Scattering bias: rough surfaces feed proportionally more energy into the
  // diffuse late field, so boost the level-matched tail by up to
  // kScatterLateBoost at mean_scattering == 1 (1 + kScatterLateBoost * s). This
  // is the part of the scattering effect that survives an explicit/clamped
  // mixing time, keeping the early/late balance monotonic in mean_scattering.
  scale *= 1.0f + kScatterLateBoost * mean_scattering;

  int length = std::max(early_n, late_n);
  const bool resource_clamped =
      late_resolution.resource_clamped || early_natural_len > kWorkingSetCap;
  const bool max_seconds_clamped =
      config.max_seconds > 0.0f && requested_cap < kWorkingSetCap &&
      (static_cast<std::size_t>(early_natural_len) > static_cast<std::size_t>(requested_cap) ||
       natural_tail_samples > static_cast<std::size_t>(requested_cap));
  if (natural_len > static_cast<std::size_t>(cap) || resource_clamped || max_seconds_clamped) {
    const char* clamp_message =
        max_seconds_clamped ? "synthesized RIR length exceeded max_seconds and was clamped"
                            : "synthesized RIR length exceeded its resource limit and was clamped";
    result.diagnostics.push_back(
        {Diagnostic::Severity::Warning, "acoustic.rir_length_clamped", clamp_message});
  }
  length = std::min(length, cap);
  if (length < 1) length = 1;

  // Two cases yield no usable late tail across the crossover: a fully-rigid room
  // (every band RT60 == 0, so the tail is empty), and a highly-absorptive room
  // whose short tail ends before the early/late crossover (late_n < t1).
  // Crossfading either would ramp x toward 1 past t1 where the tail is zero,
  // silencing the early reflections and leaving an abruptly faded RIR. Fall back
  // to early-only (no crossfade) and note it, so the geometric energy is
  // preserved.
  const bool no_late_tail = late_n == 0 || late_n < t_mix + half_xfade;
  if (no_late_tail) {
    result.diagnostics.push_back(
        {Diagnostic::Severity::Warning, "acoustic.no_late_tail",
         "no usable late-reverberation tail at the mixing time; RIR is early reflections only"});
  }

  // Equal-power crossfade (decorrelated early vs. noise late => energy-preserving):
  // early-only before t0, late-only after t1, ramping across [t0, t1]. t0 >= the
  // direct arrival (enforced above) so the direct sound is rendered at full level.
  const int t0 = std::max(0, t_mix - half_xfade);
  const int t1 = std::max(t0 + 1, t_mix + half_xfade);
  std::vector<float> rir(static_cast<size_t>(length), 0.0f);
  for (int i = 0; i < length; ++i) {
    const float e = i < early_n ? early[static_cast<size_t>(i)] : 0.0f;
    if (no_late_tail) {
      rir[static_cast<size_t>(i)] = e;  // early-only: never fade toward silence
      continue;
    }
    if (i >= late_n) {
      // Past the end of the late tail (late_n < early_n in a large, highly
      // absorptive room whose RT60 tail ends before the last image reflection):
      // no diffuse field remains to cross into, so preserve the real early
      // reflection instead of crossfading it toward a zero tail (which would
      // silence it).
      rir[static_cast<size_t>(i)] = e;
      continue;
    }
    const float l = late[static_cast<size_t>(i)] * scale;
    float x;
    if (i <= t0) {
      x = 0.0f;
    } else if (i >= t1) {
      x = 1.0f;
    } else {
      x = static_cast<float>(i - t0) / static_cast<float>(t1 - t0);
    }
    rir[static_cast<size_t>(i)] = sonare::equal_power_crossfade(e, l, x);
  }

  result.rir = Audio::from_vector(std::move(rir), sample_rate);
  return result;
}

}  // namespace sonare::acoustic
