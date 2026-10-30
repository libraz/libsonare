#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "midi/synth/envelope.h"
#include "midi/synth/fm_voice.h"
#include "midi/synth/gm_fallback_data.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/svf.h"
#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/numeric_validation.h"

namespace sonare::midi::synth {

using detail::drum_note_table;
using detail::family_patches;
using detail::program_overrides;

namespace {

struct TptTailExtrema {
  bool enabled = false;
  float min_cutoff_hz = std::numeric_limits<float>::infinity();
  float max_cutoff_hz = -std::numeric_limits<float>::infinity();
  float max_q = 0.5f;

  /// Record one fixed TPT setting for the rate envelope.
  void observe(float cutoff_hz, float q) noexcept {
    if (!std::isfinite(cutoff_hz) || !std::isfinite(q)) return;
    enabled = true;
    min_cutoff_hz = std::min(min_cutoff_hz, cutoff_hz);
    max_cutoff_hz = std::max(max_cutoff_hz, cutoff_hz);
    max_q = std::max(max_q, std::clamp(q, 0.5f, 100.0f));
  }
};

struct PercussionTailEnvelope {
  float max_physical_s = 0.0f;
  TptTailExtrema particle_resonance;
  TptTailExtrema particle_body;
  TptTailExtrema shimmer_filter;
  TptTailExtrema shimmer_air;
  TptTailExtrema wire_filter;
  TptTailExtrema wire_air;
  float max_shimmer_attack_ms = 0.0f;
};

/// Envelope-stage maxima across the fallback tables. The maxima are independent
/// of one another, so combining the slowest stages of different patches bounds
/// every patch at once without a per-patch walk on the audio thread.
struct FallbackTailBounds {
  float max_release_ms = 0.0f;
  float max_zero_sustain_decay_ms = 0.0f;
  double max_harpsichord_tail_s = 0.0;
  bool has_one_shot = false;
  DahdsrConfig one_shot_env{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  PercussionTailEnvelope percussion;
  std::array<const FmPatchParams*, detail::kLiveBases.size() + 128 + detail::kProgramOverrideCount>
      sustained_fm{};
  size_t sustained_fm_count = 0;
  bool unbounded = false;
};

void observe_fallback_tail(FallbackTailBounds& bounds, const NativeSynthPatch& patch,
                           const NativeSynthPatch* stored) noexcept {
  const DahdsrConfig& amp = patch.amp_env;
  if (patch.mode == SynthEngineMode::kHarpsichord) {
    bounds.max_harpsichord_tail_s = std::max(
        bounds.max_harpsichord_tail_s, harpsichord_max_release_tail_seconds(patch.harpsichord));
  }
  const bool zero_sustain = amp.sustain <= DahdsrEnvelope::kSilenceLevel;
  if (patch.one_shot && !zero_sustain) {
    // The cases mirror sustained_one_shot_tail_samples; no fallback patch is a sample voice.
    switch (patch.mode) {
      case SynthEngineMode::kPercussion: {
        PercussionPatchParams physical = patch.percussion;
        physical.phisem_res_hz = 0.0f;
        physical.phisem_body_gain = 0.0f;
        physical.shimmer = 0.0f;
        // The wire keeps its decay here; its rate-dependent rings are the extrema below.
        physical.wire_cutoff_hz = std::numeric_limits<float>::max();
        physical.noise_air_hz = 0.0f;
        bounds.percussion.max_physical_s =
            std::max(bounds.percussion.max_physical_s, PercussionVoiceCore::ring_bound_s(physical));

        const PercussionPatchParams& perc = patch.percussion;
        if (perc.phisem_beans > 0.0f && perc.phisem_res_hz > 0.0f) {
          bounds.percussion.particle_resonance.observe(perc.phisem_res_hz,
                                                       std::max(0.5f, perc.phisem_res_q));
          bounds.percussion.particle_resonance.observe(
              perc.phisem_res_hz * (1.0f + perc.phisem_pitch_glide),
              std::max(0.5f, perc.phisem_res_q));
        }
        if (perc.phisem_beans > 0.0f && perc.phisem_body_hz > 0.0f &&
            perc.phisem_body_gain > 0.0f) {
          bounds.percussion.particle_body.observe(perc.phisem_body_hz,
                                                  std::max(0.5f, perc.phisem_body_q));
        }
        if (perc.num_modes > 0 && perc.shimmer > 0.0f) {
          bounds.percussion.max_shimmer_attack_ms = std::max(
              bounds.percussion.max_shimmer_attack_ms, std::max(1.0f, perc.shimmer_attack_ms));
          bounds.percussion.shimmer_filter.observe(perc.shimmer_cutoff_hz, kShimmerFilterQ);
          if (perc.noise_air_hz > 0.0f) {
            bounds.percussion.shimmer_air.observe(perc.noise_air_hz, constants::kInvSqrt2);
          }
        }
        if (perc.num_modes > 0 && perc.wire_buzz > 0.0f) {
          bounds.percussion.wire_filter.observe(perc.wire_cutoff_hz,
                                                PercussionVoiceCore::kWireFilterQ);
          if (perc.noise_air_hz > 0.0f) {
            bounds.percussion.wire_air.observe(perc.noise_air_hz, constants::kInvSqrt2);
          }
        }
        break;
      }
      case SynthEngineMode::kFm:
        // A kit variation leaves the FM section alone, so one entry per stored patch.
        if (bounds.sustained_fm_count == 0 ||
            bounds.sustained_fm[bounds.sustained_fm_count - 1] != &stored->fm) {
          bounds.sustained_fm[bounds.sustained_fm_count++] = &stored->fm;
        }
        break;
      default:
        bounds.unbounded = true;
        break;
    }
    return;
  }
  bounds.max_release_ms = std::max(bounds.max_release_ms, amp.release_ms);
  if (zero_sustain) {
    bounds.max_zero_sustain_decay_ms = std::max(bounds.max_zero_sustain_decay_ms, amp.decay_ms);
  }
  if (patch.one_shot) {
    bounds.has_one_shot = true;
    bounds.one_shot_env.delay_ms = std::max(bounds.one_shot_env.delay_ms, amp.delay_ms);
    bounds.one_shot_env.attack_ms = std::max(bounds.one_shot_env.attack_ms, amp.attack_ms);
    bounds.one_shot_env.hold_ms = std::max(bounds.one_shot_env.hold_ms, amp.hold_ms);
    bounds.one_shot_env.decay_ms = std::max(bounds.one_shot_env.decay_ms, amp.decay_ms);
  }
}

/// Folds every kit of every note into \p bounds.
void observe_drum_kit_tails(FallbackTailBounds& bounds) noexcept {
  // Every kit of every note: open triangle, belltree and TR-808 outring the Standard kit.
  for (const NativeSynthPatch& piece : drum_note_table()) {
    const uint8_t note = static_cast<uint8_t>(&piece - drum_note_table().data());
    for (size_t kit = 0; kit < kGsDrumKits.size(); ++kit) {
      NativeSynthPatch varied = piece;
      apply_gs_drum_kit(varied.percussion, varied.amp_env, static_cast<uint8_t>(kit), note);
      observe_fallback_tail(bounds, varied, &piece);
    }
  }
}

const FallbackTailBounds& fallback_tail_bounds() noexcept {
  static const FallbackTailBounds kBounds = [] {
    FallbackTailBounds bounds;
    const auto& families = family_patches();
    for (size_t i : detail::kLiveBases) observe_fallback_tail(bounds, families[i], &families[i]);
    observe_drum_kit_tails(bounds);
    const NativeSynthPatch* overrides = detail::program_override_patches(program_overrides());
    for (std::size_t i = 0; i < detail::kProgramOverrideCount; ++i) {
      observe_fallback_tail(bounds, overrides[i], &overrides[i]);
    }
    return bounds;
  }();
  return kBounds;
}

const FallbackTailBounds& drum_kit_tail_bounds() noexcept {
  static const FallbackTailBounds kBounds = [] {
    FallbackTailBounds bounds;
    observe_drum_kit_tails(bounds);
    return bounds;
  }();
  return kBounds;
}

/// Evaluate the slower TPT endpoint in a stored cutoff envelope.
int64_t tpt_extrema_samples(const TptTailExtrema& extrema, double sample_rate) noexcept {
  if (!extrema.enabled) return 0;
  return std::max(
      rt::tail_budget_samples(TptSvf::ring(extrema.min_cutoff_hz, extrema.max_q, sample_rate)),
      rt::tail_budget_samples(TptSvf::ring(extrema.max_cutoff_hz, extrema.max_q, sample_rate)));
}

/// Evaluate the shimmer follower from its slowest observed attack setting.
int64_t shimmer_follower_samples(const TptTailExtrema& shimmer, float max_attack_ms,
                                 double sample_rate) noexcept {
  if (!shimmer.enabled) return 0;
  const float attack_ms = std::max(1.0f, max_attack_ms);
  const float attack_coeff =
      1.0f - std::exp(-1.0f / (attack_ms * 0.001f * static_cast<float>(sample_rate)));
  const float follower_radius = 1.0f - attack_coeff;
  return rt::tail_budget_samples(rt::TailBudget().decay(static_cast<double>(follower_radius)));
}

int64_t tail_samples_for(const FallbackTailBounds& bounds, double sample_rate, float attack_scale,
                         float decay_scale, float release_scale) noexcept {
  if (bounds.unbounded) return std::numeric_limits<int64_t>::max();
  const float stage_ms = std::max(bounds.max_release_ms * std::max(0.0f, release_scale),
                                  bounds.max_zero_sustain_decay_ms * std::max(0.0f, decay_scale));
  int64_t tail = DahdsrEnvelope::release_tail_samples(sample_rate, stage_ms);
  if (bounds.has_one_shot) {
    tail = std::max(tail, DahdsrEnvelope::one_shot_tail_samples(sample_rate, bounds.one_shot_env,
                                                                attack_scale, decay_scale));
  }
  const PercussionTailEnvelope& percussion = bounds.percussion;
  const int64_t physical =
      numeric::ceil_sample_count(static_cast<double>(percussion.max_physical_s) * sample_rate);
  const int64_t resonance_extra = tpt_extrema_samples(percussion.particle_resonance, sample_rate);
  const int64_t body_one = tpt_extrema_samples(percussion.particle_body, sample_rate);
  const int64_t body_extra = numeric::saturating_add(body_one, body_one);
  const int64_t shimmer_filter = tpt_extrema_samples(percussion.shimmer_filter, sample_rate);
  const int64_t shimmer_air = tpt_extrema_samples(percussion.shimmer_air, sample_rate);
  const int64_t follower = shimmer_follower_samples(percussion.shimmer_filter,
                                                    percussion.max_shimmer_attack_ms, sample_rate);
  const int64_t shimmer_extra =
      numeric::saturating_add(numeric::saturating_add(follower, shimmer_filter), shimmer_air);
  const int64_t wire_extra =
      numeric::saturating_add(tpt_extrema_samples(percussion.wire_filter, sample_rate),
                              tpt_extrema_samples(percussion.wire_air, sample_rate));
  const int64_t filter_extra = std::max({resonance_extra, body_extra, shimmer_extra, wire_extra});
  tail = std::max(tail, numeric::saturating_add(physical, filter_extra));
  tail = std::max(tail, numeric::ceil_sample_count(bounds.max_harpsichord_tail_s * sample_rate));
  for (size_t i = 0; i < bounds.sustained_fm_count; ++i) {
    tail = std::max(tail, fm_one_shot_tail_samples(*bounds.sustained_fm[i], sample_rate));
  }
  return tail;
}

}  // namespace

int64_t gm_fallback_max_tail_samples(double sample_rate, float attack_scale, float decay_scale,
                                     float release_scale) noexcept {
  return tail_samples_for(fallback_tail_bounds(), sample_rate, attack_scale, decay_scale,
                          release_scale);
}

int64_t gm_drum_kit_max_tail_samples(double sample_rate, float attack_scale, float decay_scale,
                                     float release_scale) noexcept {
  return tail_samples_for(drum_kit_tail_bounds(), sample_rate, attack_scale, decay_scale,
                          release_scale);
}

}  // namespace sonare::midi::synth
