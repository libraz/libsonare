#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "c_api/project_internal.h"
#include "util/numeric_validation.h"
#include "util/zero_is_default.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "c_api/project_bounce_internal.h"
#include "c_api/sample_bank_internal.h"
#include "c_api/synth_patch_common.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#if defined(SONARE_WITH_MIXING)
#include <sonare/sonare_c_mixing.h>

#include "c_api/mixing_internal.h"
#include "c_api/project_bounce_mixer.h"
#include "c_api/project_bounce_stems.h"
#include "engine/mixing_runtime.h"
#include "mixing/api/scene.h"
#endif

using namespace sonare_c_bounce_detail;

namespace {

// End of the arrangement in frames at the render sample rate: the latest sample
// touched by any audio or MIDI clip on the compiled timeline. Used to
// auto-derive a bounce length when the caller does not supply total_frames.
bool arrangement_end_frames(const arr::CompiledTimeline& timeline, int64_t* out_end) noexcept {
  if (out_end == nullptr) return false;
  int64_t end = 0;
  for (const auto& clip : timeline.audio_clips) {
    int64_t clip_end = 0;
    if (!checked_nonnegative_add(clip.start_sample, clip.length_samples, &clip_end)) return false;
    end = std::max(end, clip_end);
  }
  for (const auto& clip : timeline.midi_clips) {
    int64_t clip_end = 0;
    if (!checked_nonnegative_add(clip.start_sample, clip.length_samples, &clip_end)) return false;
    for (const auto& event : clip.events) {
      int64_t event_end = 0;
      if (!checked_nonnegative_add(event.render_frame, 1, &event_end)) return false;
      clip_end = std::max(clip_end, event_end);
    }
    end = std::max(end, clip_end);
  }
  *out_end = end;
  return true;
}

// Resets the recorded compile result to the empty state a project starts in.
// Every bounce entry point runs this before it can return, because the recorded
// result describes the LAST bounce: a call rejected for invalid arguments before
// it compiles has no result of its own, and leaving the previous one in place
// makes a query after the rejection report diagnostics from a bounce the caller
// never issued (sonare_c_project_core.h documents the empty state explicitly).
void clear_last_bounce_result(SonareProject* project) noexcept {
  if (project == nullptr) return;
  project->last_bounce_diagnostics.clear();
  project->last_bounce_has_timeline = false;
}

// Shared bounce core: validates options, compiles, registers any hosted
// instruments per destination, renders offline, and writes the interleaved
// result. `instruments` may be empty for a silent MIDI bounce. When
// opts.total_frames <= 0 the render length is auto-derived from the compiled
// timeline (plus the longest hosted-instrument release tail) so a caller can
// bounce a MIDI-only arrangement without computing a length by hand. When the
// project routes tracks through mixer channel strips (under SONARE_WITH_MIXING)
// the render fans out into per-track stems summed through the scene's mixer so
// channel-strip FX are applied; otherwise a single offline render is used.
// Returns through the SONARE_C_TRY/CATCH guard of the caller.
SonareError do_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                              const std::vector<HostedInstrument>& instruments,
                              float** out_interleaved, size_t* out_len) {
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  // Above the pointer rejection, not below it: sonare_project_bounce routes
  // straight here with no clear of its own, so a rejected call used to leave the
  // previous bounce's diagnostics queryable. Null-safe, and the four
  // bounce_with_* entries already cleared before calling, so this is idempotent
  // for them.
  clear_last_bounce_result(project);
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;

  SonareProjectBounceOptions opts{};
  if (options) opts = *options;
  const int block_size = opts.block_size > 0 ? opts.block_size : 128;
  const int num_channels = opts.num_channels > 0 ? opts.num_channels : 2;
  if (block_size <= 0 || num_channels <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // The project bounce sums to a stereo master and only writes a mono downmix or
  // the stereo pair; any wider count would leave the extra planes silent. Reject
  // unsupported widths up front, matching engine-bounce (which rejects channel
  // counts that do not map to a speaker layout) instead of emitting dead planes.
  if (num_channels != 1 && num_channels != 2) return SONARE_ERROR_INVALID_PARAMETER;
  const double project_sr = project->history.project().sample_rate();
  const double sample_rate =
      opts.sample_rate > 0 ? static_cast<double>(opts.sample_rate) : project_sr;
  if (!finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.sample_rate > 0 && std::abs(sample_rate - project_sr) > 1.0e-6) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.instrument_latency_samples < 0) return SONARE_ERROR_INVALID_PARAMETER;
  size_t block_floats = 0;
  if (!checked_frame_shape(static_cast<int64_t>(block_size), 2, &block_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
    if (hosted.instrument->latency_samples() < 0 || hosted.instrument->tail_samples() < 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  arr::CompileConfig config;
  config.instrument_latency_samples = opts.instrument_latency_samples;
  arr::CompileResult compiled = arr::compile(
      project->history.project(), project->history.midi_content(), project->audio, config);
  project->last_bounce_diagnostics = compiled.diagnostics;
  project->last_bounce_has_timeline = compiled.timeline.has_value();
  if (!compiled.timeline.has_value()) return SONARE_ERROR_INVALID_STATE;

#if defined(SONARE_WITH_MIXING)
  const MixerRouting routing = resolve_mixer_routing(*compiled.timeline);
  // Must match bounce_through_mixer's own direct-strip decision, shared MIDI
  // destination included: a mixer built here is reused there as-is, so deciding
  // the direct strip only afterwards would hand the summing pass one more input
  // than the mixer has strips.
  const bool mixer_route_direct =
      timeline_has_unbound_tracks(*compiled.timeline, routing) ||
      has_shared_hosted_midi_destination(*compiled.timeline, instruments,
                                         /*all_hosts_source_aware=*/nullptr);
  MixerPtr reusable_mixer;
#endif

  // Validate the hosted instruments and derive the project's PDC + longest tail
  // on a throwaway engine (latency depends only on the registered instruments,
  // not on the timeline), so both the single-render and the per-track-stem paths
  // share one render length and delay.
  int64_t instrument_tail = 0;
  int64_t pdc = 0;
  {
    sonare::engine::RealtimeEngine probe;
    probe.prepare(sample_rate, block_size);
    for (const HostedInstrument& hosted : instruments) {
      if (!probe.set_midi_instrument(hosted.destination_id, hosted.instrument)) {
        return SONARE_ERROR_INVALID_PARAMETER;  // more instruments than the rack holds
      }
      const int latency = hosted.instrument->latency_samples();
      const int tail = hosted.instrument->tail_samples();
      if (latency < 0 || tail < 0) return SONARE_ERROR_INVALID_PARAMETER;
      instrument_tail = std::max<int64_t>(instrument_tail, static_cast<int64_t>(tail));
    }
    const int pdc_samples = probe.midi_instrument_latency_samples();
    if (pdc_samples < 0) return SONARE_ERROR_INVALID_PARAMETER;
    pdc = static_cast<int64_t>(pdc_samples);
    for (const HostedInstrument& hosted : instruments) {
      probe.set_midi_instrument(hosted.destination_id, nullptr);
    }
  }

  // Determine the render length: caller-supplied, or auto-derived from the
  // arrangement (musical end + the longest instrument release tail).
  int64_t arrangement_frames = 0;
  if (!arrangement_end_frames(*compiled.timeline, &arrangement_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  int64_t mixer_input_frames = arrangement_frames;
  if (mixer_input_frames > 0 &&
      !checked_nonnegative_add(mixer_input_frames, instrument_tail, &mixer_input_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  int64_t frames = opts.total_frames;
  if (frames <= 0) {
    frames = arrangement_frames;
    if (frames > 0 && !checked_nonnegative_add(frames, instrument_tail, &frames)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
#if defined(SONARE_WITH_MIXING)
    if (frames > 0 && !routing.bound_tracks.empty()) {
      MixerPtr* reusable = mixer_route_direct ? nullptr : &reusable_mixer;
      const MixerLatencyTail mixer_delay =
          mixer_latency_tail_for_timeline(*compiled.timeline, routing, sample_rate, block_size,
                                          reusable, &project->last_bounce_diagnostics);
      // This mixer already scheduled the automation lanes, so an over-capacity
      // lane is known before the render window is even fixed.
      if (has_error_diagnostic(project->last_bounce_diagnostics)) {
        return SONARE_ERROR_INVALID_STATE;
      }
      if (!mixer_delay.valid ||
          !checked_nonnegative_add(frames, static_cast<int64_t>(mixer_delay.tail_samples),
                                   &frames)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
    }
#endif
  }
#if defined(SONARE_WITH_MIXING)
  // When the caller fixes the window with an explicit total_frames, feed the
  // per-track stems through the mixer across the whole window rather than
  // stopping at the arrangement's musical end. The drain-tail shortcut (zero
  // input past mixer_input_frames) is only valid for the auto-length branch
  // above, where everything past the arrangement plus the instrument release
  // tail is guaranteed silent; for an explicit length it would replace a real
  // instrument/stem tail with the mixer's decaying silence. bounce_through_mixer
  // clamps this to the render window, so this only ever lifts the input span.
  if (opts.total_frames > 0) {
    mixer_input_frames = std::max(mixer_input_frames, frames);
  }
#endif
  int64_t single_render_frames = 0;
  if (frames > 0 && !checked_nonnegative_add(frames, pdc, &single_render_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t total = 0;
  if (!checked_frame_shape(frames, static_cast<size_t>(num_channels), &total)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  if (frames == 0) {
    // Empty arrangement (or zero-length request): a valid empty render.
    *out_interleaved = new float[1];
    *out_len = 0;
    return SONARE_OK;
  }

#if defined(SONARE_WITH_MIXING)
  // Per-track channel-strip bounce when the project binds tracks to scene strips.
  if (!routing.bound_tracks.empty()) {
    return bounce_through_mixer(*compiled.timeline, instruments, routing, sample_rate, block_size,
                                num_channels, frames, pdc, mixer_input_frames, out_interleaved,
                                out_len, reusable_mixer.release(),
                                &project->last_bounce_diagnostics);
  }
#endif

  // Single-render path: no channel strips bound (output identical to the legacy
  // bounce). Plugin-delay compensation renders `pdc` extra frames and drops the
  // leading delay-line fill so musical time [0, frames) aligns to output 0.
  const int64_t render_frames = single_render_frames;
  size_t render_floats = 0;
  if (!checked_frame_shape(render_frames, 2, &render_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t pdc_count = 0;
  if (!checked_frame_count(pdc, &pdc_count)) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::vector<float>> channels;
  if (!render_timeline(*compiled.timeline, /*keep=*/{}, instruments, sample_rate, block_size,
                       /*num_channels=*/2, render_frames, &channels)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t frame = 0; frame < frames; ++frame) {
    const size_t source = static_cast<size_t>(frame) + pdc_count;
    const float l = channels[0][source];
    const float r = channels[1][source];
    for (int ch = 0; ch < num_channels; ++ch) {
      float v = 0.0f;
      if (num_channels == 1) {
        v = 0.5f * (l + r);
      } else if (ch == 0) {
        v = l;
      } else if (ch == 1) {
        v = r;
      }
      interleaved[static_cast<size_t>(frame) * num_channels + ch] = v;
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
}

// Maps the public built-in waveform ordinal to the core enum. The ordinal is
// the caller's to get right: valid_builtin_waveform gates it at the entry point
// before this runs.
sonare::midi::BuiltinSynthConfig synth_config_from_c(const SonareBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

// Maps the public versioned SF2 patch to the player config ("0 => default";
// struct_version 0/1 preserve the original layout; version 2 enables the
// model-first field and version 3 the rig clear. Anything newer is rejected by
// the caller). The player clamps polyphony itself.
sonare::midi::synth::Sf2PlayerConfig sf2_config_from_c(const SonareSf2InstrumentConfig& c) {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  // Passed through, the player's constructor would substitute for it in silence.
  cfg.gain = sonare::ZeroIsDefault(c.gain).checked_non_negative(cfg.gain, "gain");
  SONARE_CHECK_MSG(c.polyphony >= 0, sonare::ErrorCode::InvalidParameter,
                   "polyphony must be 0 (the library default) or a positive voice count");
  if (c.polyphony != 0) cfg.polyphony = c.polyphony;
  if (c.struct_version >= 2) {
    cfg.prefer_model_for_modeled_families = c.prefer_model_for_modeled_families != 0;
  }
  if (c.struct_version >= 3 && c.clear_bank_rig != 0) cfg.bank_rig_binding = false;
#if defined(SONARE_WITH_MASTERING)
  // Wire the GS insertion-effect (EFX) path: the SF2 player never depends on the
  // mastering factory itself, so the host injects it. An EFX SysEx on the
  // compiled timeline then installs its inserts and rings through the per-part
  // bus. The bounce is single-threaded and offline, so pending EFX changes are
  // realised inline in process() (the allocation is safe off the audio thread).
  cfg.insert_factory = [](std::string_view name,
                          std::string_view json) -> std::unique_ptr<sonare::rt::ProcessorBase> {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
#endif
  // Without a factory, kProcessor slots stay silent no-ops (see
  // Sf2PlayerConfig::insert_factory); harmless to leave set regardless.
  cfg.realize_efx_inline = true;
  return cfg;
}

}  // namespace
#endif

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  return do_project_bounce(project, options, {}, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, out_interleaved, out_len);
#endif
}

SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].callbacks.render == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<std::unique_ptr<CallbackInstrument>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(std::make_unique<CallbackInstrument>(instruments[i].callbacks));
    hosted.push_back({instruments[i].destination_id, owned.back().get(), owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::unique_ptr<sonare::midi::BuiltinSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    // Checked before any instrument is constructed, so a bad ordinal anywhere in
    // the list refuses the whole bounce rather than rendering the earlier
    // bindings and then failing.
    if (!sonare_c_detail::valid_builtin_waveform(instruments[i].config.waveform)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(synth_config_from_c(instruments[i].config)));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_synth_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSynthInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::unique_ptr<sonare::midi::synth::NativeSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    if (!sonare_c_detail::synth_config_from_patch_c(instruments[i].patch, &cfg, &error)) {
      set_last_error(error != nullptr ? error : "invalid synth patch");
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    cfg.use_gm_programs = instruments[i].use_gm_programs != 0;
    owned.push_back(std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
    // The synth takes a share, so a caller that destroys its own handle
    // mid-bounce cannot pull the pool out from under a sounding voice.
    if (instruments[i].sample_bank != nullptr) {
      owned.back()->set_sample_bank(
          std::shared_ptr<const sonare::midi::synth::SampleBank>(instruments[i].sample_bank->bank));
    }
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_sf2_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSf2InstrumentBinding* instruments, size_t instrument_count, float** out_interleaved,
    size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  // No loaded SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor (every program still sounds; the manifest reports the
  // synth backend honestly).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].config.struct_version > 3) return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<std::unique_ptr<sonare::midi::synth::Sf2Player>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    auto player =
        std::make_unique<sonare::midi::synth::Sf2Player>(sf2_config_from_c(instruments[i].config));
    player->set_soundfont(project->soundfont);
    owned.push_back(std::move(player));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}
