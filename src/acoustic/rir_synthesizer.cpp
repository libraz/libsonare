#include "acoustic/rir_synthesizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
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
// vector to a single RMS gain; here we add, per octave band, the band-split
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

  // Reused across every band iteration instead of freshly allocated: per band
  // this function already holds out, broadband, and per_band concurrently
  // (each a full RIR-length buffer), and re-allocating dev on top of that on
  // every one of up to ~11 octave-band iterations multiplies allocator churn
  // well past what a caller sizing to the module's allocation cap expects.
  std::vector<float> dev(static_cast<size_t>(n), 0.0f);
  // Bands whose octave sits at/above Nyquist are left out of the split, as in the late tail.
  const int band_count = octave_split_band_count(bands, sample_rate);
  for (size_t band = 0; band < static_cast<size_t>(band_count); ++band) {
    EarlyIrConfig cfg = base_cfg;
    cfg.band = static_cast<int>(band);
    const Audio per_band = synthesize_early_ir(images, sample_rate, cfg);
    const float* e = per_band.data();
    const int lim = std::min(n, static_cast<int>(per_band.size()));
    std::fill(dev.begin(), dev.end(), 0.0f);
    for (int i = 0; i < lim; ++i) dev[static_cast<size_t>(i)] = e[i] - b[i];
    octave_band_zero_phase(dev, static_cast<int>(band), band_count, sample_rate);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] += dev[static_cast<size_t>(i)];
  }
  return Audio::from_vector(std::move(out), sample_rate);
}

// Everything the RIR needs from one receiver before the late tail is synthesized.
struct RirPlan {
  float sr = 0.0f;
  int direct_sample = 0;
  int cap = 0;
  bool length_floored = false;
  std::vector<ImageSource> images;
  Audio early_audio;
  ReverbTime rt;
  int early_natural_len = 0;
  LateTailResolution late_resolution;
  std::size_t natural_tail_samples = 0;
  std::size_t natural_len = 0;
  float mean_scattering = 0.0f;
  int half_xfade = 0;
  int t_mix = 0;
  int level_half = 0;
  int early_lo = 0;
};

constexpr int kWorkingSetCap = static_cast<int>(resource::kMaxAcousticRirSamples);

// Geometry, configuration and sample-rate validation shared by the mono and pair entries.
std::vector<Diagnostic> validate_rir_request(const ShoeboxRoom& room,
                                             const SourceListener& placement, int sample_rate,
                                             const RirSynthConfig& config) {
  std::vector<Diagnostic> diagnostics = validate_shoebox(room, placement);
  const std::vector<Diagnostic> config_diagnostics = validate_rir_synth_config(config);
  diagnostics.insert(diagnostics.end(), config_diagnostics.begin(), config_diagnostics.end());
  if (sample_rate < kMinAudioSampleRate || sample_rate > kMaxAudioSampleRate) {
    diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.invalid_sample_rate",
                           "sample rate is outside supported bounds"});
  }
  return diagnostics;
}

// Sample rate an empty RIR reports when the request was refused.
int diagnostic_sample_rate(int sample_rate) noexcept {
  return sample_rate >= kMinAudioSampleRate && sample_rate <= kMaxAudioSampleRate ? sample_rate
                                                                                  : 48000;
}

RirPlan plan_rir(const ShoeboxRoom& room, const SourceListener& placement, int sample_rate,
                 const RirSynthConfig& config, std::vector<Diagnostic>& diagnostics) {
  RirPlan plan;
  const float sr = static_cast<float>(sample_rate);
  plan.sr = sr;

  // Bound the image-source order: cost grows ~ order^3, so an unbounded value is
  // a memory/CPU exhaustion vector. shoebox_image_sources clamps internally; we
  // mirror the clamp here only to inform the caller via a diagnostic.
  const int ism_order = std::min(config.ism_order, kMaxImageSourceOrder);
  if (config.ism_order > kMaxImageSourceOrder) {
    diagnostics.push_back({Diagnostic::Severity::Warning, "acoustic.ism_order_clamped",
                           "ism_order exceeded the safe maximum and was clamped"});
  }

  // Flight time of the direct sound. Needed twice: to floor the length cap below
  // so the first tap is always inside the RIR, and to keep the crossover from
  // fading the direct impulse further down.
  const float direct_dist = length(placement.listener - placement.source);
  plan.direct_sample = static_cast<int>(std::lround(direct_dist / kSoundSpeed * sr));

  EarlyIrConfig early_cfg;
  // Half-width of the fractional-delay kernel each image is rendered through;
  // synthesize_early_ir sizes itself to the last arrival plus this plus 2.
  const int early_half = (early_cfg.fdl < 1 ? 1 : early_cfg.fdl | 1) / 2;

  // A max_seconds cap bounds every synthesized buffer. The shared acoustic
  // working-set cap also applies when max_seconds is omitted, so the early,
  // late, colouring, and final RIR buffers remain bounded together.
  const int requested_cap = config.max_seconds > 0.0f
                                ? std::max(1, static_cast<int>(std::ceil(config.max_seconds * sr)))
                                : kWorkingSetCap;
  // A cap below the direct sound's arrival truncates every buffer before the
  // first tap is rendered, so the RIR comes back all zeros -- digital silence on
  // a convolution insert, with no error anywhere. max_seconds is an upper bound
  // on the *tail*, not a licence to drop the direct sound, so floor it at the
  // arrival plus the kernel half-width (the whole direct tap, sized exactly as
  // synthesize_early_ir would) and tell the caller its request was widened. The
  // working-set cap still wins: it is a memory bound, not a length preference.
  const int direct_floor = std::min(plan.direct_sample + early_half + 2, kWorkingSetCap);
  plan.length_floored = requested_cap < direct_floor;
  plan.cap = std::min(std::max(requested_cap, direct_floor), kWorkingSetCap);

  // Early reflections (image-source) and the per-band reverberation time.
  plan.images = shoebox_image_sources(room, placement, ism_order);
  early_cfg.max_samples = plan.cap;  // upper bound only; a shorter natural IR is not padded to it
  plan.early_audio = synthesize_early_ir(plan.images, sample_rate, early_cfg);
  // Frequency-dependent walls colour early reflections per octave band; a
  // spectrally flat room already carries all its colour in the broadband IR, so
  // the coloured path is skipped and the result is unchanged bit-for-bit.
  if (early_reflections_are_colored(plan.images)) {
    plan.early_audio = color_early_ir(plan.images, sample_rate, plan.early_audio, early_cfg);
  }
  plan.rt = shoebox_reverb_time(room, config.late_model,
                                config.air_absorption_enabled ? &config.air : nullptr);

  // The early IR is now capped to the cap, so early_audio.size() no longer reveals
  // the natural (uncapped) early length. Mirror synthesize_early_ir's own auto-size
  // formula here so the rir_length_clamped diagnostic below fires when the cap
  // truncates the early reflections, not just the late tail.
  float early_max_delay = 0.0f;
  for (const auto& im : plan.images) {
    if (im.distance > 1e-6f) {
      early_max_delay = std::max(early_max_delay, im.distance / kSoundSpeed * sr);
    }
  }
  const double early_raw =
      std::ceil(static_cast<double>(early_max_delay)) + static_cast<double>(early_half) + 2.0;
  plan.early_natural_len =
      std::max(1, static_cast<int>(std::min(early_raw, static_cast<double>(kMaxAutoSamples))));

  // Keep clamp telemetry aligned with synthesize_late_tail() through the shared
  // allocation-free resolver: above-Nyquist bands and the 60-second sizing
  // policy are applied exactly once in the common helper.
  LateReverbConfig natural_late_cfg;
  plan.late_resolution = resolve_late_tail(plan.rt, sample_rate, natural_late_cfg);
  plan.natural_tail_samples = plan.late_resolution.samples;
  plan.natural_len =
      std::max(static_cast<std::size_t>(plan.early_natural_len), plan.natural_tail_samples);

  // Mean wall scattering (rough surfaces) biases the early/late split: rougher
  // walls diffuse specular energy into the diffuse late field both *sooner* (an
  // earlier auto mixing time) and *more strongly* (a higher relative late level).
  // Both uses are bounded and monotonic in mean_scattering; an explicit
  // config.mixing_time_ms override skips the timing shift but keeps the energy
  // bias (the diffusion is a material property, not a crossover choice).
  plan.mean_scattering = shoebox_mean_scattering(room);  // [0,1]

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
    const float scatter_factor = 1.0f - kScatterMixingShift * plan.mean_scattering;
    mixing_ms = std::sqrt(std::max(volume, 0.0f)) * scatter_factor;
    mixing_ms = std::clamp(mixing_ms, kMinMixingMs, kMaxMixingMs);
  }
  plan.half_xfade = std::max(
      1, static_cast<int>(std::lround(std::max(0.0f, config.crossfade_ms) * 0.001f * sr * 0.5f)));

  // The direct sound (and the crossfade head) must never be faded: push the
  // crossover so its start t0 = t_mix - half_xfade lands at or after the direct
  // arrival. sqrt(V) alone ignores the source->listener delay and can otherwise
  // attenuate the direct impulse.
  plan.t_mix = static_cast<int>(std::lround(mixing_ms * 0.001f * sr));
  plan.t_mix = std::max(plan.t_mix, plan.direct_sample + plan.half_xfade);

  // A wider window than the crossfade gives a stable estimate of the (sparse,
  // decaying) early-reflection level. The early window must start strictly AFTER
  // the direct tap: t_mix is clamped to direct_sample + half_xfade, so a symmetric
  // window would otherwise capture the (loudest) direct impulse and inflate the
  // early level, over-scaling the tail in small rooms.
  plan.level_half = std::max(plan.half_xfade, static_cast<int>(std::lround(0.005f * sr)));
  plan.early_lo = std::max(plan.t_mix - plan.level_half, plan.direct_sample + 1);
  return plan;
}

// Level-match the late tail to @p plan's early reflections across its crossover so the splice
// has no energy discontinuity.
float level_match_scale(const RirPlan& plan, const Audio& late_audio) {
  const float sr = plan.sr;
  const float* early = plan.early_audio.data();
  const float* late = late_audio.data();
  const int early_n = static_cast<int>(plan.early_audio.size());
  const int late_n = static_cast<int>(late_audio.size());
  const int t_mix = plan.t_mix;
  const int level_half = plan.level_half;
  const int early_lo = plan.early_lo;

  const float early_ref = rms_range(early, early_n, early_lo, t_mix + level_half + 1);
  const int late_center = late_n == 0 ? 0 : std::min(t_mix, late_n - 1);
  const float late_ref =
      rms_range(late, late_n, late_center - level_half, late_center + level_half + 1);
  // No image arrival in the window: its sidelobes alone would set the tail ~90 dB low.
  bool window_has_arrival = false;
  for (const auto& im : plan.images) {
    const float arrival = im.distance / kSoundSpeed * sr;
    if (arrival >= static_cast<float>(early_lo) &&
        arrival < static_cast<float>(t_mix + level_half + 1)) {
      window_has_arrival = true;
      break;
    }
  }
  float scale = 1.0f;
  if (late_ref > 1e-9f) {
    if (window_has_arrival && early_ref > 1e-9f) {
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
  scale *= 1.0f + kScatterLateBoost * plan.mean_scattering;
  return scale;
}

// Splice @p plan's early reflections onto @p late_audio scaled by @p scale, appending the
// length and tail warnings to @p diagnostics.
Audio assemble_rir(const RirPlan& plan, const Audio& late_audio, float scale,
                   const RirSynthConfig& config, int sample_rate,
                   std::vector<Diagnostic>& diagnostics) {
  const float sr = plan.sr;
  const int cap = plan.cap;
  const int t_mix = plan.t_mix;
  const int half_xfade = plan.half_xfade;
  // Read the synthesized buffers in place: a full std::vector copy of each would
  // transiently double the (already large) RIR working set for no benefit.
  const float* early = plan.early_audio.data();
  const float* late = late_audio.data();
  const int early_n = static_cast<int>(plan.early_audio.size());
  const int late_n = static_cast<int>(late_audio.size());

  int length = std::max(early_n, late_n);
  const bool resource_clamped =
      plan.late_resolution.resource_clamped || plan.early_natural_len > kWorkingSetCap;
  // Measured against the effective cap, not the raw request: when the request
  // was floored to fit the direct sound the RIR is longer than max_seconds, and
  // reporting that as "exceeded max_seconds and was clamped" would contradict
  // the rir_length_floored warning standing next to it.
  const bool max_seconds_clamped =
      config.max_seconds > 0.0f && cap < kWorkingSetCap &&
      (static_cast<std::size_t>(plan.early_natural_len) > static_cast<std::size_t>(cap) ||
       plan.natural_tail_samples > static_cast<std::size_t>(cap));
  if (plan.natural_len > static_cast<std::size_t>(cap) || resource_clamped || max_seconds_clamped) {
    const char* clamp_message =
        max_seconds_clamped ? "synthesized RIR length exceeded max_seconds and was clamped"
                            : "synthesized RIR length exceeded its resource limit and was clamped";
    diagnostics.push_back(
        {Diagnostic::Severity::Warning, "acoustic.rir_length_clamped", clamp_message});
  }
  // Ordered after the clamp: a floored length is also a length the caller did
  // not get, and surfaces that publish a single warning string (the C ABI's
  // sonare_last_warning_message) should keep leading with the general one. The
  // specific code travels in the full diagnostics list.
  if (plan.length_floored) {
    diagnostics.push_back(
        {Diagnostic::Severity::Warning, "acoustic.rir_length_floored",
         "max_seconds was shorter than the direct-sound arrival and was extended to fit it"});
  }
  // A cap inside the longest band's RT60 cuts it before a 60 dB decay can be read back.
  float longest_rt60 = 0.0f;
  const int split_bands = octave_split_band_count(plan.rt.rt60_bands.size(), sample_rate);
  for (int b = 0; b < split_bands; ++b) {
    const float band_rt60 = plan.rt.rt60_bands[static_cast<size_t>(b)];
    if (band_rt60 > 0.0f) longest_rt60 = std::max(longest_rt60, band_rt60);
  }
  if (config.max_seconds > 0.0f && static_cast<double>(cap) < longest_rt60 * sr) {
    diagnostics.push_back(
        {Diagnostic::Severity::Warning, "acoustic.rir_tail_truncated",
         "max_seconds is shorter than the longest band RT60; that band is cut before it decays "
         "by 60 dB and its reverberation time cannot be measured from the RIR"});
  }
  length = std::min(length, cap);
  if (length < 1) length = 1;

  // Two cases yield no usable late tail across the crossover: no measurable
  // decay in any in-range band (a rigid room's 0 RT60 clamps to the maximal tail
  // instead), and a highly-absorptive room whose tail ends before t1.
  // Crossfading either would ramp x toward 1 past t1 where the tail is zero,
  // silencing the early reflections and leaving an abruptly faded RIR. Fall back
  // to early-only (no crossfade) and note it, so the geometric energy is
  // preserved.
  const bool no_late_tail = late_n == 0 || late_n < t_mix + half_xfade;
  if (no_late_tail) {
    diagnostics.push_back(
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
  return Audio::from_vector(std::move(rir), sample_rate);
}

// Late-tail request for a plan: the configured seed, capped at the plan's buffer bound.
LateReverbConfig late_config_for(const RirPlan& plan, const RirSynthConfig& config) {
  LateReverbConfig late_cfg;
  late_cfg.seed = config.seed;
  late_cfg.max_samples = plan.cap;  // avoid synthesizing tail past the cap
  return late_cfg;
}

// Fixed-point coordinates for a receiver diagnostic message.
std::string format_point(const Vec3& p) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f)", static_cast<double>(p.x),
                static_cast<double>(p.y), static_cast<double>(p.z));
  return buffer;
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

std::string first_error_text(const std::vector<Diagnostic>& diagnostics) {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Diagnostic::Severity::Error) {
      return diagnostic.code + ": " + diagnostic.message;
    }
  }
  return {};
}

RirSynthResult synthesize_rir(const ShoeboxRoom& room, const SourceListener& placement,
                              int sample_rate, const RirSynthConfig& config) {
  RirSynthResult result;
  result.diagnostics = validate_rir_request(room, placement, sample_rate, config);
  if (has_error(result.diagnostics)) {
    result.rir = Audio::from_vector(std::vector<float>{}, diagnostic_sample_rate(sample_rate));
    return result;
  }

  const RirPlan plan = plan_rir(room, placement, sample_rate, config, result.diagnostics);
  const Audio late_audio =
      synthesize_late_tail(plan.rt, sample_rate, late_config_for(plan, config));
  const float scale = level_match_scale(plan, late_audio);
  result.rir = assemble_rir(plan, late_audio, scale, config, sample_rate, result.diagnostics);
  return result;
}

ReceiverPair receiver_pair(const SourceListener& placement, float spacing_m) noexcept {
  const Vec3 d = placement.source - placement.listener;
  const float horizontal = std::sqrt(d.x * d.x + d.y * d.y);
  const Vec3 f =
      horizontal < 1e-6f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{d.x / horizontal, d.y / horizontal, 0.0f};
  const Vec3 axis{-f.y, f.x, 0.0f};
  const Vec3 offset = axis * (0.5f * spacing_m);
  return {placement.listener + offset, placement.listener - offset};
}

std::vector<Diagnostic> validate_receiver_pair(const ShoeboxRoom& room,
                                               const SourceListener& placement, float spacing_m) {
  std::vector<Diagnostic> diagnostics;
  if (!numeric::finite(spacing_m) || !(spacing_m > 0.0f) || spacing_m > kMaxReceiverSpacingM) {
    diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.receiver_spacing_out_of_range",
                           "receiver spacing must be finite and in (0, 4] m"});
    return diagnostics;
  }
  const ReceiverPair pair = receiver_pair(placement, spacing_m);
  const std::pair<const char*, Vec3> receivers[] = {{"left", pair.left}, {"right", pair.right}};
  for (const auto& [name, position] : receivers) {
    if (!point_inside_shoebox(room, position)) {
      diagnostics.push_back({Diagnostic::Severity::Error, "acoustic.receiver_outside_room",
                             std::string(name) + " receiver at " + format_point(position) +
                                 " lies outside the room"});
    }
  }
  return diagnostics;
}

RirPairResult synthesize_rir_pair(const ShoeboxRoom& room, const SourceListener& placement,
                                  float spacing_m, int sample_rate, const RirSynthConfig& config) {
  RirPairResult result;
  std::vector<Diagnostic> centre_diagnostics =
      validate_rir_request(room, placement, sample_rate, config);
  if (!has_error(centre_diagnostics)) {
    const std::vector<Diagnostic> pair_diagnostics =
        validate_receiver_pair(room, placement, spacing_m);
    centre_diagnostics.insert(centre_diagnostics.end(), pair_diagnostics.begin(),
                              pair_diagnostics.end());
  }
  if (has_error(centre_diagnostics)) {
    const int empty_rate = diagnostic_sample_rate(sample_rate);
    result.left = Audio::from_vector(std::vector<float>{}, empty_rate);
    result.right = Audio::from_vector(std::vector<float>{}, empty_rate);
    result.diagnostics = std::move(centre_diagnostics);
    return result;
  }

  const ReceiverPair receivers = receiver_pair(placement, spacing_m);
  const SourceListener left_placement{placement.source, receivers.left};
  const SourceListener right_placement{placement.source, receivers.right};
  std::vector<Diagnostic> left_diagnostics =
      validate_rir_request(room, left_placement, sample_rate, config);
  std::vector<Diagnostic> right_diagnostics =
      validate_rir_request(room, right_placement, sample_rate, config);

  std::vector<Diagnostic> unused;
  const RirPlan centre = plan_rir(room, placement, sample_rate, config, unused);
  const RirPlan left = plan_rir(room, left_placement, sample_rate, config, left_diagnostics);
  const RirPlan right = plan_rir(room, right_placement, sample_rate, config, right_diagnostics);
  SONARE_CHECK_MSG(left.cap == right.cap, ErrorCode::InvalidState,
                   "receiver pair resolved different RIR length caps");

  const LateTailPair late =
      synthesize_late_tail_pair(left.rt, sample_rate, late_config_for(left, config), spacing_m);
  // One scale for both channels, taken at the centre listener against the mono-stream tail.
  const float scale = level_match_scale(centre, late.left);
  const Audio left_rir =
      assemble_rir(left, late.left, scale, config, sample_rate, left_diagnostics);
  const Audio right_rir =
      assemble_rir(right, late.right, scale, config, sample_rate, right_diagnostics);

  const std::size_t length = std::max(left_rir.size(), right_rir.size());
  std::vector<float> left_out(left_rir.data(), left_rir.data() + left_rir.size());
  std::vector<float> right_out(right_rir.data(), right_rir.data() + right_rir.size());
  left_out.resize(length, 0.0f);
  right_out.resize(length, 0.0f);
  result.left = Audio::from_vector(std::move(left_out), sample_rate);
  result.right = Audio::from_vector(std::move(right_out), sample_rate);

  append_new_diagnostic_codes(result.diagnostics, left_diagnostics);
  append_new_diagnostic_codes(result.diagnostics, right_diagnostics);
  return result;
}

}  // namespace sonare::acoustic
