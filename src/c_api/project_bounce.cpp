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
#include <optional>
#include <set>

#include "c_api/engine_config_domains.h"
#include "c_api/instrument_config_domains.h"
#include "c_api/project_bounce_internal.h"
#include "c_api/sample_bank_internal.h"
#include "c_api/synth_patch_common.h"
#include "core/channel_layout.h"
#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/controller_profile.h"
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
#endif

using namespace sonare_c_bounce_detail;

namespace {

// The refusal for a render length the release tail pushes past a 64-bit count.
constexpr const char* kTailOverflow =
    "the arrangement's end plus the instruments' release tail must fit in a 64-bit sample count";
double bounce_sample_rate(const arr::Project& project, int requested, const char* subject) {
  namespace check = sonare::util::check;
  const bool use_project_rate = requested == SONARE_PROJECT_SAMPLE_RATE_PROJECT;
  const double rate = use_project_rate ? project.sample_rate() : static_cast<double>(requested);
  if (!use_project_rate) {
    check::integer_within(requested, kProjectBounceSampleRateDomain, subject);
  }
  if (!sonare::is_supported_sample_rate(rate)) {
    check::refuse("project", "must have a supported sample rate");
  }
  if (!use_project_rate && std::abs(rate - project.sample_rate()) > 1.0e-6) {
    check::refuse(subject, "must equal the sample rate of the project");
  }
  return rate;
}

int64_t prepare_click_map(const arr::Project& project, double rate, int32_t count_in_bars,
                          sonare::transport::TempoMap& map, const char* subject) {
  using namespace sonare::transport;
  map.prepare(rate);
  map.set_segments(project.tempo_segments());
  map.set_time_signatures(project.time_signatures());
  if (count_in_bars == 0) return 0;

  const double bpm = map.bpm_at_ppq(0.0);
  const TimeSignature meter = map.time_signature_at_ppq(0.0);
  const double count_in_ppq = count_in_bars * bar_length_ppq(meter);
  std::vector<TempoSegment> tempo = project.tempo_segments();
  for (auto& segment : tempo) segment.start_ppq += count_in_ppq;
  // The authored PPQ-zero segment wins if it replaces the shifted default.
  tempo.insert(tempo.begin(), {{0.0, bpm}, {count_in_ppq, bpm}});
  map.set_segments(std::move(tempo));

  std::vector<TimeSignatureSegment> meters = project.time_signatures();
  for (auto& segment : meters) segment.start_ppq += count_in_ppq;
  meters.insert(meters.begin(), {{0.0, meter}, {count_in_ppq, meter}});
  map.set_time_signatures(std::move(meters));

  const int64_t frames = map.ppq_to_sample(count_in_ppq);
  if (frames == std::numeric_limits<int64_t>::max()) {
    sonare::util::check::refuse(subject, "must fit the count-in in a 64-bit frame count");
  }
  return frames;
}

sonare::engine::MetronomeConfig bounce_metronome(const SonareEngineMetronomeConfig* config) {
  if (config == nullptr) return {};
  namespace check = sonare::util::check;
  using namespace sonare_c_detail;
  require_struct_size(config->struct_size, sizeof(*config), "options.metronome",
                      "sonare_engine_metronome_config_init");
  check::within(config->beat_gain, kMetronomeGainDomain, "options.metronome.beat_gain");
  check::within(config->accent_gain, kMetronomeGainDomain, "options.metronome.accent_gain");
  const int click_samples = config->click_samples;
  if (click_samples != SONARE_ENGINE_METRONOME_CLICK_FROM_SEC) {
    check::integer_within(click_samples, kMetronomeClickSamplesDomain,
                          "options.metronome.click_samples");
  }
  check::within(config->click_sec, kMetronomeClickSecDomain, "options.metronome.click_sec");
  return {config->enabled != 0, config->beat_gain, config->accent_gain,
          click_samples == SONARE_ENGINE_METRONOME_CLICK_FROM_SEC ? 0 : click_samples,
          config->click_sec};
}

// An INVALID_STATE return that says what the project state could not support.
SonareError refuse_state(const char* message) {
  sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_STATE, message);
  return SONARE_ERROR_INVALID_STATE;
}

// The subject naming one hosted instrument as the caller's array spells it.
std::string instrument_subject(size_t index) {
  return "options.instruments[" + std::to_string(index) + "]";
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
      check::refuse("options.instruments", "must hold at most 16 instruments");
    case sonare::engine::MidiInstrumentBindStatus::kAlreadyBoundElsewhere:
      check::refuse(instrument_subject(index).c_str(), "must not be bound to two destinations");
    case sonare::engine::MidiInstrumentBindStatus::kInstrumentPrepareFailed:
      check::refuse(instrument_subject(index).c_str(),
                    "must be preparable at the bounce's sample rate and block size");
    case sonare::engine::MidiInstrumentBindStatus::kPreparationFailed:
      return refuse_state("the hosted instruments could not be prepared for the bounce");
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
int master_output_channels(const sonare::mixing::api::Scene* scene) noexcept {
  if (scene == nullptr) return 2;
  const sonare::mixing::api::Bus* master = nullptr;
  for (const auto& bus : scene->buses) {
    if (bus.role == sonare::mixing::api::bus_role::kMaster) {
      master = &bus;
      break;
    }
  }
  if (master == nullptr) {
    for (const auto& bus : scene->buses) {
      if (bus.id == "master") {
        master = &bus;
        break;
      }
    }
  }
  return master == nullptr ? 2 : sonare::channel_count(master->layout);
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
                              int64_t count_in_frames, float** out_interleaved, size_t* out_len,
                              int* out_num_channels) {
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
  check::integer_within(opts.instrument_latency_samples, kProjectBounceInstrumentLatencyDomain,
                        "options.instrument_latency_samples");
  const int block_size = opts.block_size;
  const int master_channels = master_output_channels(project->history.project().find_scene());
  const int num_channels = opts.num_channels == SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER
                               ? master_channels
                               : opts.num_channels;
  // The master is built at the requested width, so only a speaker-layout count
  // is meaningful, and the scene's master layout caps it.
  if (num_channels != 1 && num_channels != 2 && !sonare::is_surround_channel_count(num_channels)) {
    check::one_of(num_channels, std::array<int, 4>{1, 2, 6, 8}, "options.num_channels");
  }
  if (num_channels > std::max(2, master_channels)) {
    check::refuse("options.num_channels", "must be at most the width of the scene master's layout");
  }
  const double sample_rate =
      bounce_sample_rate(project->history.project(), opts.sample_rate, "options.sample_rate");
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
  if (!compiled.timeline || compiled.has_errors()) {
    return refuse_state("the project does not compile; its bounce diagnostics name the cause");
  }

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
      if (!hosted.apply_controls())
        return refuse_state("the instrument controls could not be applied");
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
      check::refuse("options.instruments", "must report a combined latency of at least 0 samples");
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
    return refuse_state(
        "the arrangement's end must fit in a 64-bit sample count at the bounce's sample rate");
  }
  int64_t mixer_input_frames = arrangement_frames;
  if (mixer_input_frames > 0 &&
      !checked_nonnegative_add(mixer_input_frames, instrument_tail, &mixer_input_frames)) {
    return refuse_state(kTailOverflow);
  }
  int64_t frames = derive_length ? 0 : std::max<int64_t>(0, opts.total_frames - count_in_frames);
  if (derive_length) {
    // An unbounded tail has no auto length; the caller has to name one.
    if (unbounded_tail && arrangement_frames > 0) {
      check::refuse("options.total_frames",
                    "must be positive when a hosted instrument's tail is unbounded");
    }
    frames = arrangement_frames;
    if (frames > 0 && !checked_nonnegative_add(frames, instrument_tail, &frames)) {
      return refuse_state(kTailOverflow);
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
        return refuse_state("the mix does not schedule; its bounce diagnostics name the cause");
      }
      if (!mixer_delay.valid ||
          !checked_nonnegative_add(frames, static_cast<int64_t>(mixer_delay.tail_samples),
                                   &frames)) {
        return refuse_state("the mix's latency and tail must fit in a 64-bit sample count");
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
  if (count_in_frames > 0) {
    const int64_t prefix =
        derive_length ? count_in_frames : std::min(count_in_frames, opts.total_frames);
    int64_t output_frames = 0;
    size_t output_samples = 0;
    if (!checked_nonnegative_add(prefix, frames, &output_frames) ||
        !checked_frame_shape(output_frames, static_cast<size_t>(num_channels), &output_samples)) {
      check::refuse_relation({"options.count_in_bars", "options.total_frames"},
                             "{0} and {1} must fit in the render frame count");
    }
  }

  if (frames == 0) {
#if defined(SONARE_WITH_MIXING)
    const auto& lanes = compiled.timeline->mixer.automation_bindings;
    if (std::any_of(lanes.begin(), lanes.end(), [](const auto& binding) {
          return binding.lane.target_kind() ==
                 sonare::automation::AutomationTargetKind::kTrackSendDb;
        })) {
      const MixerLatencyTail mixer_delay =
          mixer_latency_tail_for_timeline(*compiled.timeline, routing, sample_rate, block_size,
                                          nullptr, &project->last_bounce_diagnostics);
      if (!mixer_delay.valid || has_error_diagnostic(project->last_bounce_diagnostics)) {
        return refuse_state("the mix does not schedule; its bounce diagnostics name the cause");
      }
    }
#endif
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
    check::refuse("options.instruments", "must report a latency small enough to be addressed");
  }
  std::vector<std::vector<float>> channels;
  if (!render_timeline(*compiled.timeline, /*keep=*/{}, instruments, sample_rate, block_size,
                       /*num_channels=*/2, render_frames, &channels)) {
    return refuse_state("the timeline could not be rendered");
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
  *out_tail = hosted.instrument->offline_tail_samples();
  guard.reset_now();
  return true;
}

}  // namespace sonare_c_bounce_detail
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// The hosted instruments of one bounce and the objects that back them, in the order the options
// list them: callback, built-in, NativeSynth, then SoundFont.
struct BounceInstruments {
  std::vector<std::unique_ptr<CallbackInstrument>> callbacks;
  std::vector<std::unique_ptr<sonare::midi::BuiltinSynth>> builtins;
  std::vector<std::unique_ptr<sonare::midi::synth::NativeSynth>> synths;
  std::vector<std::unique_ptr<sonare::midi::synth::Sf2Player>> sf2_players;
  std::vector<HostedInstrument> hosted;
};

// Refuses every destination the four binding lists name more than once, within or across kinds.
void check_unique_destinations(const SonareProjectBounceOptions& options) {
  namespace check = sonare::util::check;
  std::set<uint32_t> seen;
  const auto claim = [&](uint32_t destination_id, const char* list, size_t index) {
    if (!seen.insert(destination_id).second) {
      check::refuse(check::indexed_subject(list, index, "destination_id").c_str(),
                    "must not be bound twice");
    }
  };
  for (size_t i = 0; i < options.instrument_count; ++i) {
    claim(options.instruments[i].destination_id, "options.instruments", i);
  }
  for (size_t i = 0; i < options.builtin_instrument_count; ++i) {
    claim(options.builtin_instruments[i].destination_id, "options.builtin_instruments", i);
  }
  for (size_t i = 0; i < options.synth_instrument_count; ++i) {
    claim(options.synth_instruments[i].destination_id, "options.synth_instruments", i);
  }
  for (size_t i = 0; i < options.sf2_instrument_count; ++i) {
    claim(options.sf2_instruments[i].destination_id, "options.sf2_instruments", i);
  }
}

// Validates every binding list of @p options, then builds the instruments they describe. Nothing
// is constructed until every list has passed, so a bad entry anywhere refuses the whole bounce.
SonareError collect_bounce_instruments(const SonareProject* project,
                                       const SonareProjectBounceOptions& options,
                                       BounceInstruments* out) {
  namespace check = sonare::util::check;
  check::non_null_when_count(options.instruments, options.instrument_count, "options.instruments",
                             "options.instrument_count");
  check::non_null_when_count(options.builtin_instruments, options.builtin_instrument_count,
                             "options.builtin_instruments", "options.builtin_instrument_count");
  check::non_null_when_count(options.synth_instruments, options.synth_instrument_count,
                             "options.synth_instruments", "options.synth_instrument_count");
  check::non_null_when_count(options.sf2_instruments, options.sf2_instrument_count,
                             "options.sf2_instruments", "options.sf2_instrument_count");

  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < options.instrument_count; ++i) {
    if (options.instruments[i].callbacks.render == nullptr) {
      check::refuse(check::indexed_subject("options.instruments", i, "callbacks.render").c_str(),
                    sonare::util::refusal::kNonNullPredicate);
    }
  }
  // A bad ordinal anywhere in a list refuses the whole bounce rather than rendering the earlier
  // bindings and then failing.
  for (size_t i = 0; i < options.builtin_instrument_count; ++i) {
    const SonareBuiltinInstrumentBinding& binding = options.builtin_instruments[i];
    sonare_c_detail::require_struct_size(
        binding.config.struct_size, sizeof(SonareBuiltinSynthConfig),
        "options.builtin_instruments[" + std::to_string(i) + "].config",
        "sonare_builtin_synth_config_init");
    check::integer_in_range(
        binding.config.waveform, 0, SONARE_SYNTH_WAVEFORM_COUNT - 1,
        check::indexed_subject("options.builtin_instruments", i, "config.waveform").c_str());
  }
  for (size_t i = 0; i < options.synth_instrument_count; ++i) {
    sonare_c_detail::require_struct_size(
        options.synth_instruments[i].patch.struct_size, sizeof(SonareSynthPatch),
        "options.synth_instruments[" + std::to_string(i) + "].patch", "sonare_synth_patch_init");
  }
  for (size_t i = 0; i < options.sf2_instrument_count; ++i) {
    sonare_c_detail::require_struct_size(
        options.sf2_instruments[i].config.struct_size, sizeof(SonareSf2InstrumentConfig),
        "options.sf2_instruments[" + std::to_string(i) + "].config",
        "sonare_sf2_instrument_config_init");
  }
  check_unique_destinations(options);

  const size_t total = options.instrument_count + options.builtin_instrument_count +
                       options.synth_instrument_count + options.sf2_instrument_count;
  out->hosted.reserve(total);

  out->callbacks.reserve(options.instrument_count);
  for (size_t i = 0; i < options.instrument_count; ++i) {
    const SonareInstrumentBinding& binding = options.instruments[i];
    out->callbacks.push_back(std::make_unique<CallbackInstrument>(binding.callbacks));
    out->hosted.push_back(
        {binding.destination_id, out->callbacks.back().get(), out->callbacks.back().get()});
  }

  out->builtins.reserve(options.builtin_instrument_count);
  for (size_t i = 0; i < options.builtin_instrument_count; ++i) {
    const SonareBuiltinInstrumentBinding& binding = options.builtin_instruments[i];
    out->builtins.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(sonare_c_detail::builtin_synth_config_from_c(
            binding.config, "options.builtin_instruments[" + std::to_string(i) + "].config.")));
    out->hosted.push_back({binding.destination_id, out->builtins.back().get()});
  }

  const auto add_synth = [&](uint32_t destination, const SonareSynthPatch& patch, bool use_gm,
                             const SonareSampleBank* sample_bank, const std::string& subject) {
    auto cfg = sonare_c_detail::synth_config_from_patch_c(patch, subject);
    cfg.use_gm_programs = use_gm;
#if defined(SONARE_WITH_MASTERING)
    cfg.insert_factory = [](std::string_view name,
                            std::string_view json) -> std::unique_ptr<sonare::rt::ProcessorBase> {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
#endif
    // The bounce is single-threaded and offline: pending EFX changes realise inline.
    cfg.realize_efx_inline = true;
    out->synths.push_back(std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
    // The synth takes a share, so a caller that destroys its own handle
    // mid-bounce cannot pull the pool out from under a sounding voice.
    if (sample_bank != nullptr) {
      out->synths.back()->set_sample_bank(
          std::shared_ptr<const sonare::midi::synth::SampleBank>(sample_bank->bank));
    }
    if (!apply_project_part_rigs(project, destination, out->synths.back().get())) {
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    out->hosted.push_back({destination, out->synths.back().get()});
    return SONARE_OK;
  };
  out->synths.reserve(options.synth_instrument_count);
  for (size_t i = 0; i < options.synth_instrument_count; ++i) {
    const auto& binding = options.synth_instruments[i];
    const auto err = add_synth(binding.destination_id, binding.patch, binding.use_gm_programs != 0,
                               binding.sample_bank,
                               check::indexed_subject("options.synth_instruments", i, "patch"));
    if (err != SONARE_OK) return err;
  }

  // No loaded SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor (every program still sounds; the manifest reports the
  // synth backend honestly).
  const auto add_sf2 = [&](uint32_t destination, const SonareSf2InstrumentConfig& config,
                           const std::optional<std::string>& uri, const std::string& subject) {
    auto font = project->sound_font(uri);
    if (uri && !font) {
      sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_STATE,
                                      ("unresolved SoundFont URI '" + *uri + "'").c_str(),
                                      sonare::ErrorReason::UnresolvedSoundFont);
      return SONARE_ERROR_INVALID_STATE;
    }
    auto player_config = sonare_c_detail::sf2_player_config_from_c(config, subject);
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
    player->set_soundfont(std::move(font));
    out->sf2_players.push_back(std::move(player));
    if (!apply_project_part_rigs(project, destination, out->sf2_players.back().get())) {
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    out->hosted.push_back({destination, out->sf2_players.back().get()});
    return SONARE_OK;
  };
  out->sf2_players.reserve(options.sf2_instrument_count);
  for (size_t i = 0; i < options.sf2_instrument_count; ++i) {
    const auto& binding = options.sf2_instruments[i];
    const auto err = add_sf2(binding.destination_id, binding.config, std::nullopt,
                             "options.sf2_instruments[" + std::to_string(i) + "].config.");
    if (err != SONARE_OK) return err;
  }

  const auto& model = project->history.project();
  const auto& tracks = model.tracks();
  for (const auto& entry : model.destination_instruments()) {
    const auto destination = entry.first;
    const auto& binding = entry.second;
    if (std::any_of(out->hosted.begin(), out->hosted.end(),
                    [&](const auto& hosted) { return hosted.destination_id == destination; }) ||
        std::none_of(tracks.begin(), tracks.end(),
                     [&](const auto& track) { return track.midi_destination_id == destination; })) {
      continue;
    }
    if (const auto* synth = std::get_if<arr::DestinationSynthInstrument>(&binding.instrument)) {
      std::vector<SonareSynthEngineParam> params;
      const auto patch = project_synth_patch(*synth, params);
      const auto err = add_synth(destination, patch, synth->use_gm_programs, nullptr,
                                 "destination_instrument.synth");
      if (err != SONARE_OK) return err;
    } else if (const auto* builtin =
                   std::get_if<arr::DestinationBuiltinInstrument>(&binding.instrument)) {
      const SonareBuiltinSynthConfig config{sizeof(SonareBuiltinSynthConfig),
                                            builtin->waveform,
                                            builtin->gain,
                                            builtin->attack_ms,
                                            builtin->decay_ms,
                                            builtin->sustain,
                                            builtin->release_ms,
                                            builtin->polyphony};
      out->builtins.push_back(std::make_unique<sonare::midi::BuiltinSynth>(
          sonare_c_detail::builtin_synth_config_from_c(config, "destination_instrument.builtin.")));
      out->hosted.push_back({destination, out->builtins.back().get()});
    } else {
      const auto& sf2 = std::get<arr::DestinationSf2Instrument>(binding.instrument);
      const SonareSf2InstrumentConfig config{
          sizeof(SonareSf2InstrumentConfig),     sf2.gain,           sf2.polyphony,
          sf2.prefer_model_for_modeled_families, sf2.clear_bank_rig, sf2.gs_efx_realization};
      const auto err =
          add_sf2(destination, config, sf2.sound_font_uri, "destination_instrument.sf2.");
      if (err != SONARE_OK) return err;
    }
  }
  return SONARE_OK;
}

SonareError apply_bounce_instrument_controls(const SonareProjectBounceOptions& options,
                                             std::vector<HostedInstrument>& hosted) {
  namespace check = sonare::util::check;
  check::non_null_when_count(options.controller_profiles, options.controller_profile_count,
                             "options.controller_profiles", "options.controller_profile_count");
  check::non_null_when_count(options.articulations, options.articulation_count,
                             "options.articulations", "options.articulation_count");
  const auto instrument_at = [&](uint32_t destination,
                                 const std::string& subject) -> HostedInstrument& {
    const auto found = std::find_if(hosted.begin(), hosted.end(), [&](const auto& binding) {
      return binding.destination_id == destination;
    });
    if (found == hosted.end()) {
      check::refuse_unknown_id(subject.c_str(), "an instrument destination", "bounce");
    }
    return *found;
  };
  std::set<uint32_t> profiles;
  for (size_t i = 0; i < options.controller_profile_count; ++i) {
    const auto& binding = options.controller_profiles[i];
    const auto subject = check::indexed_subject("options.controller_profiles", i, "destination_id");
    if (!profiles.insert(binding.destination_id).second) {
      check::refuse(subject.c_str(), "must not be bound twice");
    }
    sonare::midi::ControllerProfile profile;
    if (binding.preset_name == nullptr ||
        !sonare::midi::ControllerProfile::preset(binding.preset_name, &profile)) {
      check::refuse(check::indexed_subject("options.controller_profiles", i, "preset_name").c_str(),
                    "must name a controller profile preset");
    }
    auto& instrument = instrument_at(binding.destination_id, subject);
    if (!instrument.instrument->set_controller_profile(profile)) {
      throw sonare::SonareParameterRefusal::predicate(
          sonare::ErrorCode::NotSupported, subject,
          "must name an instrument with a controller profile");
    }
    instrument.controller_profile = profile;
  }

  std::set<std::pair<uint32_t, uint32_t>> articulations;
  for (size_t i = 0; i < options.articulation_count; ++i) {
    const auto& binding = options.articulations[i];
    const auto subject = [i](const char* field) {
      return check::indexed_subject("options.articulations", i, field);
    };
    check::integer_within(binding.channel, sonare_c_detail::kEngineMidiChannelDomain,
                          subject("channel").c_str());
    if (binding.articulation < SONARE_ARTICULATION_POLY ||
        binding.articulation > SONARE_ARTICULATION_MONO_LEGATO) {
      check::refuse(subject("articulation").c_str(),
                    sonare::util::refusal::kDeclaredValuePredicate);
    }
    if (!articulations.emplace(binding.destination_id, binding.channel).second) {
      check::refuse_relation({subject("destination_id"), subject("channel")},
                             "the pair of {0} and {1} must not be bound twice");
    }
    auto& instrument = instrument_at(binding.destination_id, subject("destination_id"));
    const auto channel = static_cast<uint8_t>(binding.channel);
    const auto mode = static_cast<sonare::midi::ArticulationMode>(binding.articulation);
    if (!instrument.instrument->set_articulation(channel, mode)) {
      throw sonare::SonareParameterRefusal::predicate(sonare::ErrorCode::NotSupported,
                                                      subject("destination_id"),
                                                      "must name an instrument with articulation");
    }
    instrument.articulations.emplace_back(channel, mode);
  }
  return SONARE_OK;
}

// Prepend a silent count-in and add the click after all music has passed through the master.
SonareError add_bounce_count_in(float** out, size_t* samples, int channels, int64_t prefix,
                                int block_size, double rate,
                                const sonare::transport::TempoMap& click_map,
                                sonare::engine::MetronomeConfig config) {
  if (prefix == 0 && !config.enabled) return SONARE_OK;
  std::unique_ptr<float[]> audio(*out);
  *out = nullptr;
  const int64_t music_frames = static_cast<int64_t>(*samples / static_cast<size_t>(channels));
  int64_t frames = 0;
  size_t total = 0;
  if (!checked_nonnegative_add(prefix, music_frames, &frames) ||
      !checked_frame_shape(frames, static_cast<size_t>(channels), &total)) {
    sonare::util::check::refuse_relation({"options.count_in_bars", "options.total_frames"},
                                         "{0} and {1} must fit in the render frame count");
  }
  if (prefix > 0) {
    auto with_prefix = std::make_unique<float[]>(total);
    std::copy_n(audio.get(), *samples, with_prefix.get() + total - *samples);
    audio = std::move(with_prefix);
  }
  if (config.enabled && frames > 0) {
    sonare::engine::Metronome metronome;
    metronome.prepare(rate, &click_map);
    metronome.set_config(config);
    const auto capacity = static_cast<size_t>(std::min<int64_t>(block_size, frames));
    std::vector<float> click(capacity);
    float* plane = click.data();
    for (int64_t first = 0; first < frames;) {
      const int n = static_cast<int>(std::min<int64_t>(frames - first, block_size));
      std::fill_n(plane, n, 0.0f);
      if (!metronome.process(&plane, 1, n, first)) {
        sonare_c_detail::set_last_error(SONARE_ERROR_UNKNOWN,
                                        "the bounce click event list overflowed");
        return SONARE_ERROR_UNKNOWN;
      }
      for (int i = 0; i < n; ++i) {
        const size_t offset = static_cast<size_t>(first + i) * channels;
        for (int channel = 0; channel < channels; ++channel) audio[offset + channel] += click[i];
      }
      first += n;
    }
  }
  *out = audio.release();
  *samples = total;
  return SONARE_OK;
}

}  // namespace
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

SonareError sonare_project_count_in_frames(const SonareProject* project, int32_t count_in_bars,
                                           int sample_rate, int64_t* out_frames) {
  SONARE_C_API_ENTRY;
  if (out_frames) *out_frames = 0;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_frames);
  SONARE_C_TRY
  sonare::util::check::integer_within(count_in_bars, sonare_c_detail::kProjectCountInBarsDomain,
                                      "count_in_bars");
  const auto& model = project->history.project();
  const double rate = bounce_sample_rate(model, sample_rate, "sample_rate");
  sonare::transport::TempoMap map;
  *out_frames = prepare_click_map(model, rate, count_in_bars, map, "count_in_bars");
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 count_in_bars, sample_rate, out_frames);
#endif
}

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out, size_t* out_frames, int* out_channels) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out) *out = nullptr;
  if (out_frames) *out_frames = 0;
  if (out_channels) *out_channels = 0;
  // Before the first argument rejection below, so an early return leaves the
  // recorded compile result empty rather than describing the previous bounce.
  clear_last_bounce_result(project);
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_frames);
  SONARE_C_REQUIRE_NONNULL(out_channels);
  sonare::engine::MetronomeConfig click;
  sonare::transport::TempoMap click_map;
  double rate = 0.0;
  int64_t count_in_frames = 0;
  BounceInstruments instruments;
  if (options != nullptr) {
    // The instrument members sit past the original fields, so the size is read first.
    sonare_c_detail::require_struct_size(options->struct_size, sizeof(SonareProjectBounceOptions),
                                         "options", "sonare_project_bounce_options_init");
    if (const SonareError err = collect_bounce_instruments(project, *options, &instruments);
        err != SONARE_OK) {
      return err;
    }
    if (const SonareError err = apply_bounce_instrument_controls(*options, instruments.hosted);
        err != SONARE_OK) {
      return err;
    }
    sonare::util::check::integer_within(options->count_in_bars,
                                        sonare_c_detail::kProjectCountInBarsDomain,
                                        "options.count_in_bars");
    click = bounce_metronome(options->metronome);
    if (options->count_in_bars > 0 || click.enabled) {
      const auto& model = project->history.project();
      rate = bounce_sample_rate(model, options->sample_rate, "options.sample_rate");
      count_in_frames = prepare_click_map(model, rate, options->count_in_bars, click_map,
                                          "options.count_in_bars");
    }
  } else if (!project->history.project().destination_instruments().empty()) {
    SonareProjectBounceOptions defaults;
    sonare_project_bounce_options_init(&defaults);
    if (const SonareError err = collect_bounce_instruments(project, defaults, &instruments);
        err != SONARE_OK) {
      return err;
    }
  }
#if defined(SONARE_TUNING) && SONARE_TUNING
  std::optional<RenderPathDump> path_dump;
  if (instruments.synths.empty() && !instruments.sf2_players.empty()) {
    path_dump.emplace(instruments.sf2_players);
  } else if (!instruments.synths.empty()) {
    path_dump.emplace(instruments.synths);
  }
#endif
  size_t samples = 0;
  int channels = 0;
  float* audio = nullptr;
  const SonareError err = do_project_bounce(project, options, instruments.hosted, count_in_frames,
                                            &audio, &samples, &channels);
  if (err != SONARE_OK) return err;
  const int64_t prefix =
      options != nullptr && options->total_frames != SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT
          ? std::min(count_in_frames, options->total_frames)
          : count_in_frames;
  const SonareError click_err =
      add_bounce_count_in(&audio, &samples, channels, prefix, options ? options->block_size : 0,
                          rate, click_map, click);
  if (click_err != SONARE_OK) return click_err;
  *out = audio;
  *out_channels = channels;
  *out_frames = samples / static_cast<size_t>(channels);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  if (out_frames) *out_frames = {};
  if (out_channels) *out_channels = 0;
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 options, out, out_frames, out_channels);
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
