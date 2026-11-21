#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "c_api/project_input_domains.h"
#include "c_api/project_internal.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <set>

#include "c_api/project_bounce_internal.h"
#include "c_api/sample_bank_internal.h"
#include "c_api/synth_patch_common.h"
#include "core/channel_layout.h"
#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/part_rig.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#include "mixing/api/scene.h"
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

// The subject naming one hosted instrument as the caller's array spells it.
std::string instrument_subject(size_t index) {
  return "instruments[" + std::to_string(index) + "]";
}

static_assert(sonare::engine::InstrumentRack::kMaxInstruments == 16,
              "the instrument-count refusal below states the rack size");

// Maps a probe engine's refused bind of hosted instrument @p index. The probe is
// fresh, so a full rack is the caller's instrument count, not earlier state.
SonareError hosted_instrument_bind_error(const sonare::engine::RealtimeEngine& probe,
                                         size_t index) {
  namespace check = sonare::util::check;
  switch (probe.last_midi_instrument_bind_status()) {
    case sonare::engine::MidiInstrumentBindStatus::kRackFull:
      check::refuse("instruments", "must hold at most 16 instruments");
    case sonare::engine::MidiInstrumentBindStatus::kAlreadyBoundElsewhere:
      check::refuse(instrument_subject(index).c_str(), "must not be bound to two destinations");
    case sonare::engine::MidiInstrumentBindStatus::kInstrumentPrepareFailed:
      check::refuse(instrument_subject(index).c_str(),
                    "must be preparable at the bounce's sample rate and block size");
    case sonare::engine::MidiInstrumentBindStatus::kPreparationFailed:
      return SONARE_ERROR_INVALID_STATE;
    default:
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
}

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

// Feeds every scheduled event bound for @p hosted's destination to it in render
// order, so an instrument whose tail follows control events (a GS envelope-time
// edit) reports the tail this arrangement asks for. The caller resets it after
// reading the tail, and the render prepares it again.
bool replay_destination_events(const arr::CompiledTimeline& timeline,
                               const HostedInstrument& hosted) {
  const std::vector<TimelineEvent> events =
      timeline_events_in_render_order(timeline, hosted.destination_id);
  // A prepared token is owned only by the published schedule (or a live
  // payload slot), neither of which exists for this throwaway probe. Keep the
  // replay's tokens alive until every copied event has been consumed.
  std::vector<std::shared_ptr<const sonare::midi::PreparedMidiSysEx>> prepared;
  prepared.reserve(events.size());
  for (const TimelineEvent& scheduled : events) {
    sonare::midi::MidiEvent replay = *scheduled.event;
    if (sonare::midi::is_sysex_event(replay) && replay.sysex_payload_size > 0) {
      std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
      try {
        if (!hosted.instrument->prepare_sysex(replay.sysex_payload, replay.sysex_payload_size,
                                              token)) {
          return false;
        }
      } catch (const std::bad_alloc&) {
        throw;
      } catch (...) {
        return false;
      }
      replay.prepared_sysex = token.get();
      prepared.push_back(std::move(token));
    }
    hosted.instrument->on_event(hosted.destination_id, replay);
    try {
      if (!hosted.instrument->materialize_tail_probe()) return false;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (...) {
      return false;
    }
  }
  return true;
}

// Width of the scene master's layout, or 2 for a scene with no master. The master is
// resolved the way the mixer graph resolves it: role "master" first, then id "master".
int master_output_channels(const sonare::mixing::api::Scene& scene) noexcept {
  const sonare::mixing::api::Bus* master = nullptr;
  for (const auto& bus : scene.buses) {
    if (bus.role == "master") {
      master = &bus;
      break;
    }
  }
  if (master == nullptr) {
    for (const auto& bus : scene.buses) {
      if (bus.id == "master") {
        master = &bus;
        break;
      }
    }
  }
  return master == nullptr ? 2 : sonare::channel_count(master->layout);
}

// Widest output a scene's master allows: its layout's width, with mono allowing two.
int max_output_channels(const sonare::mixing::api::Scene& scene) noexcept {
  return std::max(2, master_output_channels(scene));
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
// result. `instruments` may be empty for a silent MIDI bounce. When opts.total_frames is
// SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT the render length is derived from the compiled
// timeline (plus the longest hosted-instrument release tail) so a caller can
// bounce a MIDI-only arrangement without computing a length by hand; an
// instrument reporting an unbounded (INT_MAX) tail refuses that. When the
// project routes tracks through mixer channel strips (under SONARE_WITH_MIXING)
// the render fans out into per-track stems summed through the scene's mixer so
// channel-strip FX are applied; otherwise a single offline render is used.
// Returns through the SONARE_C_TRY/CATCH guard of the caller.
SonareError do_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                              const std::vector<HostedInstrument>& instruments,
                              float** out_interleaved, size_t* out_len, int* out_num_channels) {
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  // Every entry has already refused a NULL pointer; the recorded result is cleared here as well
  // so a call rejected for its options leaves no previous bounce's diagnostics queryable.
  clear_last_bounce_result(project);

  namespace check = sonare::util::check;
  SonareProjectBounceOptions opts;
  if (options) {
    opts = *options;
    sonare_c_detail::require_struct_size(opts.struct_size, sizeof(SonareProjectBounceOptions),
                                         "options", "sonare_project_bounce_options_init");
  } else {
    sonare_project_bounce_options_init(&opts);
  }
  const bool derive_length = opts.total_frames == SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT;
  if (!derive_length) {
    check::integer_within(opts.total_frames, kProjectBounceTotalFramesDomain,
                          "options.total_frames");
  }
  check::integer_within(opts.block_size, kProjectBounceBlockSizeDomain, "options.block_size");
  if (opts.num_channels != SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER) {
    check::integer_within(opts.num_channels, kProjectBounceChannelCountDomain,
                          "options.num_channels");
  }
  if (opts.sample_rate != SONARE_PROJECT_SAMPLE_RATE_PROJECT) {
    check::integer_within(opts.sample_rate, kProjectBounceSampleRateDomain, "options.sample_rate");
  }
  check::integer_within(opts.instrument_latency_samples, kProjectBounceInstrumentLatencyDomain,
                        "options.instrument_latency_samples");
  const int block_size = opts.block_size;
  const int num_channels = opts.num_channels == SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER
                               ? master_output_channels(project->history.project().scene())
                               : opts.num_channels;
  // The master is built at the requested width, so only a speaker-layout count
  // is meaningful, and the scene's master layout caps it.
  if (num_channels != 1 && num_channels != 2 && !sonare::is_surround_channel_count(num_channels)) {
    check::one_of(num_channels, std::array<int, 4>{1, 2, 6, 8}, "options.num_channels");
  }
  if (num_channels > max_output_channels(project->history.project().scene())) {
    check::refuse("options.num_channels", "must be at most the width of the scene master's layout");
  }
  const double project_sr = project->history.project().sample_rate();
  const bool project_rate = opts.sample_rate == SONARE_PROJECT_SAMPLE_RATE_PROJECT;
  const double sample_rate = project_rate ? project_sr : static_cast<double>(opts.sample_rate);
  if (!sonare::is_supported_sample_rate(sample_rate)) {
    check::refuse("project", "must have a supported sample rate");
  }
  if (!project_rate && std::abs(sample_rate - project_sr) > 1.0e-6) {
    check::refuse("options.sample_rate", "must equal the sample rate of the project");
  }
  size_t block_floats = 0;
  if (!checked_frame_shape(static_cast<int64_t>(block_size), 2, &block_floats)) {
    check::refuse("options.block_size", "must be small enough for its buffer to be addressed");
  }
  for (size_t i = 0; i < instruments.size(); ++i) {
    const HostedInstrument& hosted = instruments[i];
    if (hosted.instrument == nullptr) {
      check::refuse(instrument_subject(i).c_str(), sonare::util::refusal::kNonNullPredicate);
    }
    if (hosted.instrument->latency_samples() < 0 || hosted.instrument->tail_samples() < 0) {
      check::refuse(instrument_subject(i).c_str(),
                    "must report a latency and a tail of at least 0 samples");
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
  const bool mixer_required =
      timeline_requires_mixer(*compiled.timeline, routing) ||
      has_shared_hosted_midi_destination(*compiled.timeline, routing, instruments,
                                         /*all_hosts_source_aware=*/nullptr);
  const bool mixer_route_direct =
      timeline_has_unbound_tracks(*compiled.timeline, routing) ||
      has_shared_hosted_midi_destination(*compiled.timeline, routing, instruments,
                                         /*all_hosts_source_aware=*/nullptr);
  MixerPtr reusable_mixer;
#endif

  // Validate the hosted instruments and derive the project's PDC + longest tail
  // on a throwaway engine (latency depends only on the registered instruments,
  // not on the timeline), so both the single-render and the per-track-stem paths
  // share one render length and delay.
  int64_t instrument_tail = 0;
  bool unbounded_tail = false;
  int64_t pdc = 0;
  {
    sonare::engine::RealtimeEngine probe;
    probe.prepare(sample_rate, block_size);
    for (size_t i = 0; i < instruments.size(); ++i) {
      const HostedInstrument& hosted = instruments[i];
      if (!probe.set_midi_instrument(hosted.destination_id, hosted.instrument)) {
        return hosted_instrument_bind_error(probe, i);
      }
      const bool replay = derive_length && hosted.instrument->tail_follows_events();
      int latency = 0;
      int tail = 0;
      if (!probe_hosted_latency_tail(*compiled.timeline, hosted, replay, &latency, &tail)) {
        check::refuse(instrument_subject(i).c_str(),
                      "must accept every SysEx scheduled on its destination");
      }
      if (latency < 0 || tail < 0) {
        check::refuse(instrument_subject(i).c_str(),
                      "must report a latency and a tail of at least 0 samples");
      }
      unbounded_tail |= tail == std::numeric_limits<int>::max();
      instrument_tail = std::max<int64_t>(instrument_tail, static_cast<int64_t>(tail));
    }
    const int pdc_samples = probe.midi_instrument_latency_samples();
    if (pdc_samples < 0) {
      check::refuse("instruments", "must report a combined latency of at least 0 samples");
    }
    pdc = static_cast<int64_t>(pdc_samples);
    for (const HostedInstrument& hosted : instruments) {
      probe.set_midi_instrument(hosted.destination_id, nullptr);
    }
  }

  // Determine the render length: caller-supplied, or auto-derived from the
  // arrangement (musical end + the longest instrument release tail).
  int64_t arrangement_frames = 0;
  if (!arrangement_end_frames(*compiled.timeline, &arrangement_frames)) {
    return SONARE_ERROR_INVALID_STATE;
  }
  int64_t mixer_input_frames = arrangement_frames;
  if (mixer_input_frames > 0 &&
      !checked_nonnegative_add(mixer_input_frames, instrument_tail, &mixer_input_frames)) {
    return SONARE_ERROR_INVALID_STATE;
  }
  int64_t frames = opts.total_frames;
  if (derive_length) {
    // An unbounded tail has no auto length; the caller has to name one.
    if (unbounded_tail && arrangement_frames > 0) {
      check::refuse("options.total_frames",
                    "must be positive when a hosted instrument's tail is unbounded");
    }
    frames = arrangement_frames;
    if (frames > 0 && !checked_nonnegative_add(frames, instrument_tail, &frames)) {
      return SONARE_ERROR_INVALID_STATE;
    }
#if defined(SONARE_WITH_MIXING)
    if (frames > 0 && mixer_required) {
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
        return SONARE_ERROR_INVALID_STATE;
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
  if (!derive_length) {
    mixer_input_frames = std::max(mixer_input_frames, frames);
  }
#endif
  int64_t single_render_frames = 0;
  if (frames > 0 && !checked_nonnegative_add(frames, pdc, &single_render_frames)) {
    check::refuse("options.total_frames", "must be small enough for the render to be addressed");
  }
  size_t total = 0;
  if (!checked_frame_shape(frames, static_cast<size_t>(num_channels), &total)) {
    check::refuse("options.total_frames", "must be small enough for the render to be addressed");
  }

  if (frames == 0) {
    // Empty arrangement (or zero-length request): a valid empty render.
    *out_interleaved = new float[1];
    *out_len = 0;
    *out_num_channels = num_channels;
    return SONARE_OK;
  }

#if defined(SONARE_WITH_MIXING)
  // Per-track channel-strip bounce when the project binds tracks to scene strips.
  if (mixer_required) {
    const SonareError mixed =
        bounce_through_mixer(*compiled.timeline, instruments, routing, sample_rate, block_size,
                             num_channels, frames, pdc, mixer_input_frames, out_interleaved,
                             out_len, reusable_mixer.release(), &project->last_bounce_diagnostics);
    if (mixed == SONARE_OK) *out_num_channels = num_channels;
    return mixed;
  }
#endif

  // Single-render path: no channel strips bound (output identical to the legacy
  // bounce). Plugin-delay compensation renders `pdc` extra frames and drops the
  // leading delay-line fill so musical time [0, frames) aligns to output 0. The
  // unmixed render is a stereo pair, so a wider output carries it on the front
  // pair and leaves the other planes silent.
  const int64_t render_frames = single_render_frames;
  size_t render_floats = 0;
  if (!checked_frame_shape(render_frames, 2, &render_floats)) {
    check::refuse("options.total_frames", "must be small enough for the render to be addressed");
  }
  size_t pdc_count = 0;
  if (!checked_frame_count(pdc, &pdc_count)) {
    check::refuse("instruments", "must report a latency small enough to be addressed");
  }
  std::vector<std::vector<float>> channels;
  if (!render_timeline(*compiled.timeline, /*keep=*/{}, instruments, sample_rate, block_size,
                       /*num_channels=*/2, render_frames, &channels)) {
    return SONARE_ERROR_INVALID_STATE;
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
  *out_num_channels = num_channels;
  return SONARE_OK;
}

#if defined(SONARE_TUNING) && SONARE_TUNING
// SONARE_RENDER_PATH_DUMP=<path>: the signal path one bounce realised, for the
// first hosted instrument. Unlike SONARE_TUNING_DUMP, which a static destructor
// writes at process exit, this is written per bounce on the calling thread, by
// the destructor of an object declared after the instruments it watches, so
// before they are destroyed.
class RenderPathDump {
 public:
  template <typename Instrument>
  explicit RenderPathDump(const std::vector<std::unique_ptr<Instrument>>& owned)
      : path_(sonare::midi::synth::render_path_dump_path()) {
    if (path_.empty()) return;
    if (owned.empty()) {
      recorder_.mark_incomplete("no instrument was bound");
      return;
    }
    if (owned.size() > 1) {
      recorder_.mark_incomplete("more than one instrument; only the first is recorded");
    }
    if (!owned.front()->set_render_path_recorder(&recorder_)) {
      recorder_.mark_incomplete("the instrument does not realise its EFX inline");
      return;
    }
    detach_ = [instrument = owned.front().get()]() {
      instrument->set_render_path_recorder(nullptr);
    };
  }
  RenderPathDump(const RenderPathDump&) = delete;
  RenderPathDump& operator=(const RenderPathDump&) = delete;
  ~RenderPathDump() {
    if (path_.empty()) return;
    if (detach_) detach_();
    try {
      if (!sonare::midi::synth::write_render_path_dump(path_, recorder_, sonare_version())) {
        std::fprintf(stderr, "SONARE_RENDER_PATH_DUMP: could not write '%s'\n", path_.c_str());
      }
    } catch (...) {
      std::fprintf(stderr, "SONARE_RENDER_PATH_DUMP: could not serialise the record\n");
    }
  }

 private:
  std::string path_;
  sonare::midi::synth::RenderPathRecorder recorder_;
  std::function<void()> detach_;
};
#endif

// Applies the project's rig entries for @p destination_id to its instrument: the
// destination default first, then each part, so a part overrides the default.
// False when the instrument refuses an entry.
bool apply_project_part_rigs(const SonareProject* project, uint32_t destination_id,
                             sonare::midi::MidiInstrument* instrument) {
  const auto& entries = project->history.project().part_rigs();
  for (const bool defaults : {true, false}) {
    for (const arr::ProjectPartRig& entry : entries) {
      if (entry.destination_id != destination_id) continue;
      if ((entry.part == sonare::midi::kPartRigAllParts) != defaults) continue;
      if (!instrument->set_part_rig(entry.part, entry.rig)) return false;
    }
  }
  return true;
}

// Resets a replayed instrument on every exit from the probe. The normal path
// resets through reset_now() so a failure there still reaches the caller.
class ReplayResetGuard {
 public:
  explicit ReplayResetGuard(sonare::midi::MidiInstrument* instrument) noexcept
      : instrument_(instrument) {}
  ReplayResetGuard(const ReplayResetGuard&) = delete;
  ReplayResetGuard& operator=(const ReplayResetGuard&) = delete;
  ~ReplayResetGuard() {
    if (instrument_ == nullptr) return;
    // Already leaving with a refusal or an exception; that one is what the caller sees.
    try {
      instrument_->reset();
    } catch (...) {
    }
  }
  void reset_now() {
    sonare::midi::MidiInstrument* instrument = instrument_;
    instrument_ = nullptr;
    if (instrument != nullptr) instrument->reset();
  }

 private:
  sonare::midi::MidiInstrument* instrument_;
};

}  // namespace

namespace sonare_c_bounce_detail {

bool probe_hosted_latency_tail(const arr::CompiledTimeline& timeline,
                               const HostedInstrument& hosted, bool replay, int* out_latency,
                               int* out_tail) {
  ReplayResetGuard guard(replay ? hosted.instrument : nullptr);
  if (replay && !replay_destination_events(timeline, hosted)) return false;
  *out_latency = hosted.instrument->latency_samples();
  *out_tail = hosted.instrument->tail_samples();
  guard.reset_now();
  return true;
}

}  // namespace sonare_c_bounce_detail
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
static_assert(sonare::engine::RealtimeEngine::kDefaultOfflineBlock == 128,
              "the documented default block of SonareProjectBounceOptions");
#endif

void sonare_project_bounce_options_init(SonareProjectBounceOptions* options) {
  if (options == nullptr) return;
  *options = SonareProjectBounceOptions{};
  options->struct_size = sizeof(SonareProjectBounceOptions);
  options->total_frames = SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT;
  options->block_size = 128;
  options->num_channels = SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER;
  options->sample_rate = SONARE_PROJECT_SAMPLE_RATE_PROJECT;
}

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len, int* out_num_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_interleaved);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  return do_project_bounce(project, options, {}, out_interleaved, out_len, out_num_channels);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  if (out_num_channels) *out_num_channels = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, options, out_interleaved, out_len, out_num_channels);
#endif
}

SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len, int* out_num_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(instruments, instrument_count);
  SONARE_C_REQUIRE_NONNULL(out_interleaved);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].callbacks.render == nullptr) {
      sonare::util::check::refuse(
          sonare::util::check::indexed_subject("instruments", i, "callbacks.render").c_str(),
          sonare::util::refusal::kNonNullPredicate);
    }
  }
  std::vector<std::unique_ptr<CallbackInstrument>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(std::make_unique<CallbackInstrument>(instruments[i].callbacks));
    hosted.push_back({instruments[i].destination_id, owned.back().get(), owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len, out_num_channels);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  if (out_num_channels) *out_num_channels = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len, out_num_channels);
#endif
}

void sonare_builtin_synth_config_init(SonareBuiltinSynthConfig* config) {
  if (config == nullptr) return;
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare_c_detail::builtin_synth_config_init(config);
#else
  *config = SonareBuiltinSynthConfig{};
  config->struct_size = sizeof(SonareBuiltinSynthConfig);
#endif
}

void sonare_sf2_instrument_config_init(SonareSf2InstrumentConfig* config) {
  if (config == nullptr) return;
#if defined(SONARE_WITH_ARRANGEMENT)
  sonare_c_detail::sf2_instrument_config_init(config);
#else
  *config = SonareSf2InstrumentConfig{};
  config->struct_size = sizeof(SonareSf2InstrumentConfig);
#endif
}

SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len, int* out_num_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(instruments, instrument_count);
  SONARE_C_REQUIRE_NONNULL(out_interleaved);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  std::vector<std::unique_ptr<sonare::midi::BuiltinSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    // Checked before any instrument is constructed, so a bad ordinal anywhere in
    // the list refuses the whole bounce rather than rendering the earlier
    // bindings and then failing.
    sonare_c_detail::require_struct_size(
        instruments[i].config.struct_size, sizeof(SonareBuiltinSynthConfig),
        "instruments[" + std::to_string(i) + "].config", "sonare_builtin_synth_config_init");
    sonare::util::check::integer_in_range(
        instruments[i].config.waveform, 0, SONARE_SYNTH_WAVEFORM_COUNT - 1,
        sonare::util::check::indexed_subject("instruments", i, "config.waveform").c_str());
  }
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(sonare_c_detail::builtin_synth_config_from_c(
            instruments[i].config, "instruments[" + std::to_string(i) + "].config.")));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len, out_num_channels);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  if (out_num_channels) *out_num_channels = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len, out_num_channels);
#endif
}

SonareError sonare_project_bounce_with_synth_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSynthInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len, int* out_num_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(instruments, instrument_count);
  SONARE_C_REQUIRE_NONNULL(out_interleaved);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  std::vector<std::unique_ptr<sonare::midi::synth::NativeSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    sonare_c_detail::require_struct_size(instruments[i].patch.struct_size, sizeof(SonareSynthPatch),
                                         "instruments[" + std::to_string(i) + "].patch",
                                         "sonare_synth_patch_init");
  }
  for (size_t i = 0; i < instrument_count; ++i) {
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    if (!sonare_c_detail::synth_config_from_patch_c(instruments[i].patch, &cfg, &error)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     error != nullptr ? error : "invalid synth patch");
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    cfg.use_gm_programs = instruments[i].use_gm_programs != 0;
#if defined(SONARE_WITH_MASTERING)
    cfg.insert_factory = [](std::string_view name,
                            std::string_view json) -> std::unique_ptr<sonare::rt::ProcessorBase> {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
#endif
    // The bounce is single-threaded and offline: pending EFX changes realise inline.
    cfg.realize_efx_inline = true;
    owned.push_back(std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
    // The synth takes a share, so a caller that destroys its own handle
    // mid-bounce cannot pull the pool out from under a sounding voice.
    if (instruments[i].sample_bank != nullptr) {
      owned.back()->set_sample_bank(
          std::shared_ptr<const sonare::midi::synth::SampleBank>(instruments[i].sample_bank->bank));
    }
    if (!apply_project_part_rigs(project, instruments[i].destination_id, owned.back().get())) {
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
#if defined(SONARE_TUNING) && SONARE_TUNING
  const RenderPathDump path_dump(owned);
#endif
  return do_project_bounce(project, options, hosted, out_interleaved, out_len, out_num_channels);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  if (out_num_channels) *out_num_channels = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len, out_num_channels);
#endif
}

SonareError sonare_project_bounce_with_sf2_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSf2InstrumentBinding* instruments, size_t instrument_count, float** out_interleaved,
    size_t* out_len, int* out_num_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (out_num_channels) *out_num_channels = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(instruments, instrument_count);
  SONARE_C_REQUIRE_NONNULL(out_interleaved);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_REQUIRE_NONNULL(out_num_channels);
  // No loaded SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor (every program still sounds; the manifest reports the
  // synth backend honestly).
  for (size_t i = 0; i < instrument_count; ++i) {
    sonare_c_detail::require_struct_size(
        instruments[i].config.struct_size, sizeof(SonareSf2InstrumentConfig),
        "instruments[" + std::to_string(i) + "].config", "sonare_sf2_instrument_config_init");
  }
  std::vector<std::unique_ptr<sonare::midi::synth::Sf2Player>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    auto player_config = sonare_c_detail::sf2_player_config_from_c(
        instruments[i].config, "instruments[" + std::to_string(i) + "].config.");
#if defined(SONARE_WITH_MASTERING)
    // Wire the GS insertion-effect (EFX) path: the SF2 player never depends on the
    // mastering factory itself, so the host injects it.
    player_config.insert_factory =
        [](std::string_view name,
           std::string_view json) -> std::unique_ptr<sonare::rt::ProcessorBase> {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
#endif
    // The bounce is single-threaded and offline: pending EFX changes realise inline.
    player_config.realize_efx_inline = true;
    auto player = std::make_unique<sonare::midi::synth::Sf2Player>(std::move(player_config));
    player->set_soundfont(project->soundfont);
    owned.push_back(std::move(player));
    if (!apply_project_part_rigs(project, instruments[i].destination_id, owned.back().get())) {
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
#if defined(SONARE_TUNING) && SONARE_TUNING
  const RenderPathDump path_dump(owned);
#endif
  return do_project_bounce(project, options, hosted, out_interleaved, out_len, out_num_channels);
  SONARE_C_CATCH
#else
  if (out_interleaved) *out_interleaved = {};
  if (out_len) *out_len = {};
  if (out_num_channels) *out_num_channels = 0;
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len, out_num_channels);
#endif
}
