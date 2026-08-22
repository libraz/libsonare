#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "c_api/project_internal.h"
#include "util/numeric_validation.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "c_api/synth_patch_common.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#if defined(SONARE_WITH_MIXING)
#include <sonare/sonare_c_mixing.h>

#include "c_api/mixing_internal.h"
#include "engine/mixing_runtime.h"
#include "mixing/api/scene.h"
#endif

namespace {

bool checked_nonnegative_add(int64_t lhs, int64_t rhs, int64_t* out) noexcept {
  return lhs >= 0 && rhs >= 0 && sonare::numeric::checked_add(lhs, rhs, out);
}

bool checked_frame_count(int64_t frames, size_t* out) noexcept {
  if (out == nullptr || frames < 0) return false;
  if (static_cast<uintmax_t>(frames) > static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  *out = static_cast<size_t>(frames);
  return true;
}

bool checked_frame_shape(int64_t frames, size_t channels, size_t* out) noexcept {
  size_t frame_count = 0;
  return checked_frame_count(frames, &frame_count) &&
         sonare::numeric::checked_size_product(frame_count, channels, kMaxBufferSize, out);
}

#if defined(SONARE_WITH_MIXING)
bool checked_frame_shape(int64_t frames, size_t channels, size_t copies, size_t* out) noexcept {
  size_t per_copy = 0;
  return checked_frame_shape(frames, channels, &per_copy) &&
         sonare::numeric::checked_size_product(per_copy, copies, kMaxBufferSize, out);
}

bool checked_midi_source_stem_shape(size_t track_count, int64_t frames, size_t* out) noexcept {
  size_t stem_count = 0;
  return sonare::numeric::checked_add(track_count, size_t{1}, &stem_count) &&
         checked_frame_shape(frames, 2, stem_count, out);
}
#endif

// Adapts a host's C callback table to a sonare::midi::MidiInstrument so the
// bounce engine can drive an external instrument: events are forwarded to
// on_event at their sample-accurate render frame and render() sums the audio.
// Only opaque UMP words / planar buffers cross the seam (invariant 6).
//
// Clock domain: the engine stamps events in DEVICE render frames (see "Event
// clock domain" in midi/instrument.h), a counter the C seam gives the host no
// way to observe -- render() reports only a frame count. Events are therefore
// held until process(), where the block's first device frame (from
// set_transport) turns each into an intra-block offset, and re-expressed in the
// only basis the host can keep: frames this instrument has been asked to render.
// The two bases differ whenever the engine renders nothing (a stopped transport
// with no sounding note, e.g. the smoother-priming block below), so forwarding
// the raw device frame would place every later event too late by that amount.
class CallbackInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit CallbackInstrument(const SonareInstrumentCallbacks& callbacks) : cb_(callbacks) {}

  void prepare(double sample_rate, int max_block_size) override {
    pending_count_ = 0;
    if (cb_.prepare) cb_.prepare(cb_.user_data, sample_rate, max_block_size);
  }
  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    block_first_frame_ = state.render_frame;
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    flush_pending(num_samples);
    if (cb_.render) cb_.render(cb_.user_data, channels, num_channels, num_samples);
    rendered_frames_ += num_samples;
  }
  // rendered_frames_ is deliberately NOT cleared: the C table has no reset
  // callback, so the host's own frame counter keeps running across the several
  // render passes a stem bounce makes, and this mirror must keep running too.
  void reset() override { pending_count_ = 0; }
  int latency_samples() const noexcept override { return cb_.latency_samples; }
  int tail_samples() const noexcept override { return cb_.tail_samples; }
  void on_event(uint32_t destination_id, const sonare::midi::MidiEvent& event) noexcept override {
    // Only the UMP words cross the seam, so nothing here borrows the event's
    // SysEx view, which is valid for the duration of this call alone.
    if (cb_.on_event == nullptr || pending_count_ >= pending_.size()) return;
    pending_[pending_count_++] = {destination_id, event.render_frame, event.ump};
  }

  /// CONTROL thread, once a render has produced its last block: forwards
  /// whatever the per-block hold still carries.
  ///
  /// The engine releases every note still sounding AFTER its final process()
  /// call (RealtimeEngine::render_offline), so the note-off that closes a
  /// sustained note -- and the channel reset that follows it -- arrive when no
  /// further block will ever flush them. Without this drain a host would see the
  /// note-on and never its release, and an external instrument would be left
  /// with the note hanging past the end of the bounce. They are placed at
  /// rendered_frames_, one past the last rendered frame, which is the host-basis
  /// image of the render frame the engine stamped them with.
  void flush_trailing_events() noexcept {
    if (cb_.on_event == nullptr) {
      pending_count_ = 0;
      return;
    }
    for (size_t i = 0; i < pending_count_; ++i) {
      cb_.on_event(cb_.user_data, pending_[i].destination_id, pending_[i].ump.words,
                   pending_[i].ump.word_count, rendered_frames_);
    }
    pending_count_ = 0;
  }

 private:
  struct PendingEvent {
    uint32_t destination_id = 0;
    int64_t render_frame = 0;
    sonare::midi::Ump ump{};
  };

  void flush_pending(int num_samples) noexcept {
    const int64_t last = num_samples > 0 ? num_samples - 1 : 0;
    for (size_t i = 0; i < pending_count_; ++i) {
      const int64_t offset = pending_[i].render_frame - block_first_frame_;
      const int64_t placed = offset < 0 ? 0 : offset > last ? last : offset;
      cb_.on_event(cb_.user_data, pending_[i].destination_id, pending_[i].ump.words,
                   pending_[i].ump.word_count, rendered_frames_ + placed);
    }
    pending_count_ = 0;
  }

  SonareInstrumentCallbacks cb_;
  // Bounded per-block event hold. One sub-block normally carries a single event
  // (the engine splits at every MIDI frame); the dense case is a hang-note
  // release, which is capped by the sequencer's active-note table.
  std::array<PendingEvent, 512> pending_{};
  size_t pending_count_ = 0;
  int64_t block_first_frame_ = 0;
  int64_t rendered_frames_ = 0;
};

// A destination id paired with a borrowed instrument pointer (the owning storage
// outlives the render in the caller). Used by the shared bounce core so the
// callback and built-in-synth paths share one render implementation.
struct HostedInstrument {
  uint32_t destination_id = 0;
  sonare::midi::MidiInstrument* instrument = nullptr;
  // Non-null only when `instrument` is the C callback seam. That adapter holds a
  // block's events until the block renders, so the end-of-render release needs
  // an explicit drain (see CallbackInstrument::flush_trailing_events). The
  // built-in synth and SF2 paths consume events as they arrive and leave this
  // null. Naming the concrete type here keeps the drain a compile-time fact
  // rather than a downcast at the end of every render.
  CallbackInstrument* callback = nullptr;
};

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

// Renders the compiled timeline offline through a fresh engine into `channels`
// (num_channels deinterleaved buffers of length render_frames). `keep` selects
// which clips to include by track id (a null/empty function keeps everything),
// so the channel-strip bounce can isolate one track's audio into a dry stem.
// The hosted instruments are reset and re-registered per render so a stem starts
// from a clean voice state; only clips whose track passes `keep` fire events.
bool render_timeline(const arr::CompiledTimeline& timeline,
                     const std::function<bool(uint32_t)>& keep,
                     const std::vector<HostedInstrument>& instruments, double sample_rate,
                     int block_size, int num_channels, int64_t render_frames,
                     std::vector<std::vector<float>>* channels, bool include_audio = true,
                     bool include_midi = true) {
  size_t render_frame_count = 0;
  size_t render_floats = 0;
  if (channels == nullptr || num_channels <= 0 ||
      !checked_frame_shape(render_frames, static_cast<size_t>(num_channels), &render_floats) ||
      !checked_frame_count(render_frames, &render_frame_count)) {
    return false;
  }
  arr::CompiledTimeline filtered = timeline;  // copy re-points marker name pointers
  if (!include_audio) filtered.audio_clips.clear();
  if (!include_midi) filtered.midi_clips.clear();
  if (keep) {
    filtered.audio_clips.erase(
        std::remove_if(filtered.audio_clips.begin(), filtered.audio_clips.end(),
                       [&](const sonare::engine::ClipSchedule& c) { return !keep(c.track_id); }),
        filtered.audio_clips.end());
    filtered.midi_clips.erase(
        std::remove_if(filtered.midi_clips.begin(), filtered.midi_clips.end(),
                       [&](const sonare::midi::MidiClipSchedule& c) { return !keep(c.track_id); }),
        filtered.midi_clips.end());
  }

  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(filtered, engine);
  for (const HostedInstrument& hosted : instruments) {
    hosted.instrument->reset();
    engine.set_midi_instrument(hosted.destination_id, hosted.instrument);
  }

  // Prime the parameter smoothers before the audible render so a non-default
  // static fader/pan does not fade in over the first ~5 ms block. Lane fader/pan
  // smoothers only advance while lanes render, so one process() pass with the
  // transport stopped applies automation at the start position and drains queued
  // commands (setting the smoother targets), then settle_parameters() snaps the
  // smoothers to those targets. Without this the bounce's first block ramps in
  // from 0 dB / centre, which live playback never does and which breaks bit-exact
  // determinism. The primed block renders into a throwaway buffer.
  {
    size_t block_count = 0;
    if (!checked_frame_count(block_size, &block_count)) return false;
    std::vector<std::vector<float>> prime(static_cast<size_t>(num_channels),
                                          std::vector<float>(block_count, 0.0f));
    std::vector<float*> prime_ptrs;
    prime_ptrs.reserve(prime.size());
    for (auto& channel : prime) prime_ptrs.push_back(channel.data());
    engine.process(prime_ptrs.data(), num_channels, block_size);
    engine.settle_parameters();
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  channels->assign(static_cast<size_t>(num_channels), std::vector<float>(render_frame_count, 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels->size());
  for (auto& channel : *channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, render_frames, block_size);
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }
  // Drain after unbinding, not before: clearing a destination releases anything
  // still sounding on it through the OUTGOING instrument, so a drain placed
  // first would leave those releases held for the next pass.
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.callback != nullptr) hosted.callback->flush_trailing_events();
  }
  return true;
}

#if defined(SONARE_WITH_MIXING)
// Offline-only collector for the engine's source-aware instrument seam. All
// storage is allocated before rendering; on_instrument_source_audio only sums
// a block into its already-present source stem.
class MidiSourceStemSink final : public sonare::engine::InstrumentSourceRenderSink {
 public:
  MidiSourceStemSink(const std::set<uint32_t>& track_ids, int num_channels, int64_t frames,
                     size_t frame_count, const arr::CompiledTimeline& timeline, double sample_rate,
                     int block_size)
      : num_channels_(num_channels),
        frames_(frames),
        frame_count_(frame_count),
        max_block_size_(block_size) {
    for (uint32_t track_id : track_ids) allocate(track_id);
    default_ = make_stem();
    tempo_map_.prepare(sample_rate);
    if (!timeline.tempo_segments.empty()) tempo_map_.set_segments(timeline.tempo_segments);
    if (!timeline.time_signatures.empty()) tempo_map_.set_time_signatures(timeline.time_signatures);

    std::vector<sonare::engine::TrackLaneConfig> lanes;
    if (!timeline.track_lanes.empty()) {
      lanes.reserve(timeline.track_lanes.size());
      for (const arr::CompiledTrackLane& lane : timeline.track_lanes) {
        lanes.emplace_back(lane.track_id);
      }
    } else {
      lanes.reserve(track_ids.size());
      for (uint32_t track_id : track_ids) lanes.emplace_back(track_id);
    }
    source_mixer_.prepare(sample_rate, block_size);
    ready_ = source_mixer_.set_track_lanes(std::move(lanes));

    for (const arr::MixerAutomationBinding& binding : timeline.mixer.automation_bindings) {
      if (binding.lane.target_kind() == sonare::automation::AutomationTargetKind::kOpaque) {
        continue;
      }
      auto state = std::make_unique<TypedAutomationState>();
      state->track_id = binding.track_id;
      state->lane_index = 0;
      bool found = false;
      for (size_t index = 0; index < timeline.track_lanes.size(); ++index) {
        if (timeline.track_lanes[index].track_id == binding.track_id) {
          state->lane_index = index;
          found = true;
          break;
        }
      }
      if (!found || state->lane_index >= sonare::engine::TrackMixerRuntime::kMaxTrackLanes) {
        ready_ = false;
        continue;
      }
      if (binding.lane.target_kind() == sonare::automation::AutomationTargetKind::kTrackFaderDb) {
        state->fader = &binding.lane;
      } else if (binding.lane.target_kind() ==
                 sonare::automation::AutomationTargetKind::kTrackPan) {
        state->pan = &binding.lane;
      }
      auto existing = std::find_if(typed_automation_.begin(), typed_automation_.end(),
                                   [state_ptr = state.get()](const auto& candidate) {
                                     return candidate->track_id == state_ptr->track_id;
                                   });
      if (existing == typed_automation_.end()) {
        typed_automation_.push_back(std::move(state));
      } else if (state->fader != nullptr) {
        (*existing)->fader = state->fader;
      } else {
        (*existing)->pan = state->pan;
      }
    }
    processed_.resize(static_cast<size_t>(std::max(num_channels, 0)));
    for (auto& channel : processed_) {
      channel.assign(static_cast<size_t>(std::max(block_size, 0)), 0.0f);
    }
  }

  bool ready() const noexcept { return ready_; }

  // The source-aware instrument seam bypasses RealtimeEngine's normal lane
  // merge so the raw per-source buffers can be retained for scene-strip
  // summing. Re-enter the same TrackMixerRuntime owner here before retaining a
  // stem; this keeps typed fader/pan ownership and smoothing identical to the
  // live source-mix route without scheduling those lanes on a scene strip.
  void settle_typed_automation() noexcept {
    update_typed_targets(0);
    source_mixer_.settle_smoothers();
    last_target_frame_ = kUninitializedFrame;
  }

  void on_instrument_source_audio(uint32_t /*destination_id*/, uint32_t source_track_id,
                                  float* const* channels, int num_channels, int num_frames,
                                  int64_t render_frame) noexcept override {
    if (channels == nullptr || num_channels <= 0 || num_frames <= 0 || render_frame < 0 ||
        render_frame >= frames_) {
      return;
    }
    if (!ready_ || num_channels_ <= 0 || num_frames > max_block_size_ ||
        processed_.size() < static_cast<size_t>(num_channels)) {
      return;
    }
    update_typed_targets(render_frame);
    for (auto& channel : processed_) {
      std::fill(channel.begin(), channel.begin() + num_frames, 0.0f);
    }
    // The destination plane array is sized by the mixer's lane capacity, so the
    // channel count handed to it (and read back below) is clamped to that
    // capacity rather than to the caller's count.
    const int mix_channels =
        std::min(num_channels, sonare::engine::TrackMixerRuntime::kMaxLaneChannels);
    std::array<float*, sonare::engine::TrackMixerRuntime::kMaxLaneChannels> processed_ptrs{};
    for (int ch = 0; ch < mix_channels; ++ch) {
      processed_ptrs[static_cast<size_t>(ch)] = processed_[static_cast<size_t>(ch)].data();
    }
    if (!source_mixer_.mix_source(source_track_id, channels, processed_ptrs.data(), mix_channels,
                                  num_frames)) {
      return;
    }
    auto it = stems_.find(source_track_id);
    std::vector<std::vector<float>>* stem = it != stems_.end() ? &it->second : &default_;
    const int64_t available = frames_ - render_frame;
    const int frames = static_cast<int>(std::min<int64_t>(num_frames, available));
    const int channel_count = std::min(num_channels_, mix_channels);
    for (int ch = 0; ch < channel_count; ++ch) {
      const float* source = processed_ptrs[static_cast<size_t>(ch)];
      float* destination = (*stem)[static_cast<size_t>(ch)].data() + render_frame;
      if (source == nullptr) continue;
      for (int frame = 0; frame < frames; ++frame) destination[frame] += source[frame];
    }
  }

  const std::vector<std::vector<float>>* stem(uint32_t track_id) const noexcept {
    const auto it = stems_.find(track_id);
    return it == stems_.end() ? nullptr : &it->second;
  }
  const std::vector<std::vector<float>>& default_stem() const noexcept { return default_; }

 private:
  struct TypedAutomationState {
    uint32_t track_id = 0;
    size_t lane_index = 0;
    const sonare::automation::AutomationLane* fader = nullptr;
    const sonare::automation::AutomationLane* pan = nullptr;
  };

  static constexpr int64_t kUninitializedFrame = std::numeric_limits<int64_t>::min();

  void update_typed_targets(int64_t render_frame) noexcept {
    if (render_frame == last_target_frame_) return;
    last_target_frame_ = render_frame;
    const double ppq = tempo_map_.sample_to_ppq(render_frame);
    for (const auto& state : typed_automation_) {
      if (state->fader != nullptr && !state->fader->points().empty()) {
        (void)source_mixer_.set_lane_parameter(state->lane_index,
                                               sonare::engine::TrackMixerRuntime::kFaderDb,
                                               state->fader->value_at(ppq));
      }
      if (state->pan != nullptr && !state->pan->points().empty()) {
        (void)source_mixer_.set_lane_parameter(
            state->lane_index, sonare::engine::TrackMixerRuntime::kPan, state->pan->value_at(ppq));
      }
    }
  }

  std::vector<std::vector<float>> make_stem() const {
    return std::vector<std::vector<float>>(static_cast<size_t>(num_channels_),
                                           std::vector<float>(frame_count_, 0.0f));
  }
  void allocate(uint32_t track_id) { stems_.emplace(track_id, make_stem()); }

  int num_channels_ = 0;
  int64_t frames_ = 0;
  size_t frame_count_ = 0;
  int max_block_size_ = 0;
  bool ready_ = false;
  int64_t last_target_frame_ = kUninitializedFrame;
  sonare::transport::TempoMap tempo_map_;
  sonare::engine::TrackMixerRuntime source_mixer_;
  std::vector<std::unique_ptr<TypedAutomationState>> typed_automation_;
  std::vector<std::vector<float>> processed_;
  std::map<uint32_t, std::vector<std::vector<float>>> stems_;
  std::vector<std::vector<float>> default_;
};

bool render_midi_source_stems(const arr::CompiledTimeline& timeline,
                              const std::vector<HostedInstrument>& instruments, double sample_rate,
                              int block_size, int64_t render_frames, MidiSourceStemSink* sink) {
  size_t render_frame_count = 0;
  size_t render_floats = 0;
  if (sink == nullptr || !checked_frame_shape(render_frames, 2, &render_floats) ||
      !checked_frame_count(render_frames, &render_frame_count)) {
    return false;
  }
  std::set<uint32_t> track_ids;
  for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
    if (clip.track_id != 0) track_ids.insert(clip.track_id);
  }
  if (track_ids.size() > sonare::engine::TrackMixerRuntime::kMaxTrackLanes) return false;

  arr::CompiledTimeline midi_only = timeline;
  midi_only.audio_clips.clear();
  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(midi_only, engine);

  // apply_to_engine already installed the compiled project-order lanes. Do not
  // replace them with a MIDI-only subset: typed automation ids encode those
  // original indices, and changing the vector here would retarget a lane to a
  // different track. The local set is retained only as a bounded consistency
  // check for source-aware rendering.
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr || !hosted.instrument->supports_source_track_rendering()) {
      return false;
    }
    hosted.instrument->reset();
    if (!engine.set_midi_instrument(hosted.destination_id, hosted.instrument)) return false;
  }
  // The engine's current source render bank is intentionally selected only for
  // zero-latency instruments. A per-source PDC bank would otherwise be needed
  // to retain independent delay history.
  if (engine.midi_instrument_latency_samples() != 0) return false;

  // Match render_timeline's offline pre-roll: publish lane state and snap its
  // smoothers before the first audible MIDI block, so the source stems do not
  // fade in relative to the live engine / external scene mixer.
  {
    size_t block_count = 0;
    if (!checked_frame_count(block_size, &block_count)) return false;
    std::vector<float> prime_l(block_count, 0.0f);
    std::vector<float> prime_r(block_count, 0.0f);
    float* prime[] = {prime_l.data(), prime_r.data()};
    engine.process(prime, 2, block_size);
    engine.settle_parameters();
    sink->settle_typed_automation();
  }
  engine.set_instrument_source_render_sink(sink);
  std::vector<std::vector<float>> discard(2, std::vector<float>(render_frame_count, 0.0f));
  float* channels[] = {discard[0].data(), discard[1].data()};
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);
  engine.render_offline(channels, 2, render_frames, block_size);
  engine.set_instrument_source_render_sink(nullptr);
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }
  return true;
}

bool has_shared_hosted_midi_destination(const arr::CompiledTimeline& timeline,
                                        const std::vector<HostedInstrument>& instruments,
                                        bool* all_hosts_source_aware) {
  if (all_hosts_source_aware != nullptr) *all_hosts_source_aware = true;
  std::map<uint32_t, std::set<uint32_t>> tracks_by_destination;
  for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
    tracks_by_destination[clip.destination_id].insert(clip.track_id);
  }
  bool shared = false;
  for (const auto& [destination_id, tracks] : tracks_by_destination) {
    if (tracks.size() < 2) continue;
    shared = true;
  }
  // The source-stem pass renders the project once, so every bound destination
  // participates even when only one destination is shared by several strips.
  // An opaque callback cannot be silently dropped from that pass or rendered
  // separately without recreating the very duplicated-pool bug this path fixes.
  if (shared) {
    for (const HostedInstrument& hosted : instruments) {
      if (hosted.instrument == nullptr || !hosted.instrument->supports_source_track_rendering()) {
        if (all_hosts_source_aware != nullptr) *all_hosts_source_aware = false;
      }
    }
  }
  return shared;
}

void add_stem(std::vector<std::vector<float>>* destination,
              const std::vector<std::vector<float>>& source) {
  if (destination == nullptr) return;
  const size_t channels = std::min(destination->size(), source.size());
  for (size_t ch = 0; ch < channels; ++ch) {
    const size_t frames = std::min((*destination)[ch].size(), source[ch].size());
    for (size_t frame = 0; frame < frames; ++frame) (*destination)[ch][frame] += source[ch][frame];
  }
}
#endif

#if defined(SONARE_WITH_MIXING)
sonare::mixing::AutomationCurveType to_mixing_curve(sonare::automation::CurveType curve) noexcept {
  return static_cast<sonare::mixing::AutomationCurveType>(static_cast<int>(curve));
}

// Resolved Track->Strip routing for a channel-strip bounce: the scene strip ids
// in their canonical order (= the mixer's process_stereo input index order),
// each strip's set of source tracks, and the union of all bound tracks.
struct MixerRouting {
  std::vector<std::string> strip_ids;
  std::vector<std::set<uint32_t>> strip_tracks;  // index-aligned with strip_ids
  std::set<uint32_t> bound_tracks;
};

struct MixerDeleter {
  void operator()(SonareMixer* mixer) const noexcept {
    if (mixer != nullptr) sonare_mixer_destroy(mixer);
  }
};

using MixerPtr = std::unique_ptr<SonareMixer, MixerDeleter>;

bool timeline_has_unbound_tracks(const arr::CompiledTimeline& timeline,
                                 const MixerRouting& routing) {
  const auto unbound = [&](uint32_t track_id) { return routing.bound_tracks.count(track_id) == 0; };
  for (const auto& clip : timeline.audio_clips) {
    if (unbound(clip.track_id)) return true;
  }
  for (const auto& clip : timeline.midi_clips) {
    if (unbound(clip.track_id)) return true;
  }
  return false;
}

std::string unique_direct_strip_id(const sonare::mixing::api::Scene& scene) {
  constexpr const char* kBase = "__sonare_direct_master__";
  auto exists = [&](const std::string& candidate) {
    return std::any_of(
        scene.strips.begin(), scene.strips.end(),
        [&](const sonare::mixing::api::Strip& strip) { return strip.id == candidate; });
  };
  if (!exists(kBase)) return kBase;
  for (int suffix = 1;; ++suffix) {
    std::string candidate = std::string(kBase) + "_" + std::to_string(suffix);
    if (!exists(candidate)) return candidate;
  }
}

// Resolves the compiled timeline's mixer bindings against its scene. Only
// bindings whose strip actually exists in the scene are honored; a binding to a
// missing strip leaves its track unbound (rendered dry into the master).
MixerRouting resolve_mixer_routing(const arr::CompiledTimeline& timeline) {
  MixerRouting routing;
  std::map<std::string, size_t> index_of;
  for (const auto& strip : timeline.mixer.scene.strips) {
    index_of.emplace(strip.id, routing.strip_ids.size());
    routing.strip_ids.push_back(strip.id);
    routing.strip_tracks.emplace_back();
  }
  for (const auto& binding : timeline.mixer.bindings) {
    const auto it = index_of.find(binding.strip_id);
    if (it == index_of.end()) continue;  // strip not in scene -> track stays unbound
    routing.strip_tracks[it->second].insert(binding.track_id);
    routing.bound_tracks.insert(binding.track_id);
  }
  return routing;
}

// True when any diagnostic in `diagnostics` is an error (a warning-only list
// still describes a renderable bounce).
bool has_error_diagnostic(const std::vector<arr::Diagnostic>& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const arr::Diagnostic& d) {
    return d.severity == arr::Diagnostic::Severity::kError;
  });
}

// Installs the compiled opaque automation lanes on the scene strips.
//
// A strip automation lane is a bounded ring, so a project lane with more
// breakpoints than it holds cannot be installed in full. Every push result is
// inspected and a rejection becomes a diagnostic naming the lane: the curve
// would otherwise render frozen at the last accepted breakpoint with nothing to
// tell the caller the ramp was cut short. `out_diagnostics` may be null for the
// probe mixers whose only job is to report latency.
void schedule_mixer_automation(const arr::CompiledTimeline& timeline, const MixerRouting& routing,
                               double sample_rate, SonareMixer* mixer,
                               std::vector<arr::Diagnostic>* out_diagnostics) {
  if (mixer == nullptr) return;
  std::map<uint32_t, std::string> strip_for_track;
  for (size_t i = 0; i < routing.strip_ids.size(); ++i) {
    for (uint32_t track_id : routing.strip_tracks[i]) {
      strip_for_track.emplace(track_id, routing.strip_ids[i]);
    }
  }

  sonare::transport::TempoMap tempo_map;
  tempo_map.prepare(sample_rate);
  if (!timeline.tempo_segments.empty()) {
    tempo_map.set_segments(timeline.tempo_segments);
  }
  if (!timeline.time_signatures.empty()) {
    tempo_map.set_time_signatures(timeline.time_signatures);
  }

  for (const auto& binding : timeline.mixer.automation_bindings) {
    const auto route = strip_for_track.find(binding.track_id);
    if (route == strip_for_track.end()) continue;
    SonareStrip* strip = sonare_mixer_strip_by_id(mixer, route->second.c_str());
    if (strip == nullptr) continue;
    const auto& lane = binding.lane;
    // Typed track fader/pan lanes are applied once by TrackMixerRuntime through
    // the reserved engine namespace. Scheduling them on the scene strip too
    // would apply the same automation a second time (notably -6 dB -> -12 dB).
    // Legacy opaque ids retain their historical strip-scheduler behavior.
    if (lane.target_kind() != sonare::automation::AutomationTargetKind::kOpaque) continue;
    if (!sonare::engine::MixingRuntime::is_supported_parameter(lane.target_param_id())) continue;
    const auto& points = lane.points();
    if (points.empty()) continue;
    const float initial_value = lane.value_at(0.0);

    switch (lane.target_param_id()) {
      case sonare::engine::MixingRuntime::kFaderDb:
        strip->strip.set_fader_db(initial_value);
        break;
      case sonare::engine::MixingRuntime::kPan:
        strip->strip.set_pan(initial_value);
        break;
      case sonare::engine::MixingRuntime::kWidth:
        strip->strip.set_width(initial_value);
        break;
      default:
        break;
    }

    const auto schedule = [&](int64_t sample, float value,
                              sonare::mixing::AutomationCurveType curve) {
      switch (lane.target_param_id()) {
        case sonare::engine::MixingRuntime::kFaderDb:
          return strip->strip.schedule_fader_automation_result(sample, value, curve);
        case sonare::engine::MixingRuntime::kPan:
          return strip->strip.schedule_pan_automation_result(sample, value, curve);
        case sonare::engine::MixingRuntime::kWidth:
          return strip->strip.schedule_width_automation_result(sample, value, curve);
        default:
          return sonare::mixing::AutomationPushResult::NonMonotonic;
      }
    };

    bool lane_full = false;
    if (schedule(0, initial_value, sonare::mixing::AutomationCurveType::Hold) ==
        sonare::mixing::AutomationPushResult::Full) {
      lane_full = true;
    }
    for (const auto& point : points) {
      if (lane_full) break;
      const int64_t sample = std::max<int64_t>(0, tempo_map.ppq_to_sample(point.ppq));
      if (schedule(sample, point.value, to_mixing_curve(point.curve_to_next)) ==
          sonare::mixing::AutomationPushResult::Full) {
        lane_full = true;
      }
    }
    if (lane_full && out_diagnostics != nullptr) {
      const char* parameter = "automation";
      switch (lane.target_param_id()) {
        case sonare::engine::MixingRuntime::kFaderDb:
          parameter = "fader";
          break;
        case sonare::engine::MixingRuntime::kPan:
          parameter = "pan";
          break;
        case sonare::engine::MixingRuntime::kWidth:
          parameter = "width";
          break;
        default:
          break;
      }
      arr::Diagnostic diagnostic;
      diagnostic.code = arr::Diagnostic::Code::kAutomationLaneCapacity;
      diagnostic.severity = arr::Diagnostic::Severity::kError;
      diagnostic.target_id = binding.track_id;
      diagnostic.message = std::string("the ") + parameter + " automation lane on channel strip '" +
                           route->second + "' has " + std::to_string(points.size()) +
                           " breakpoints, more than the strip's automation lane holds";
      out_diagnostics->push_back(std::move(diagnostic));
    }
  }
}

SonareMixer* create_timeline_mixer(const arr::CompiledTimeline& timeline,
                                   const MixerRouting& routing, double sample_rate, int block_size,
                                   const std::string& direct_strip_id = {},
                                   std::vector<arr::Diagnostic>* out_diagnostics = nullptr) {
  sonare::mixing::api::Scene scene = timeline.mixer.scene;
  if (!direct_strip_id.empty()) {
    sonare::mixing::api::Strip direct_strip;
    direct_strip.id = direct_strip_id;
    scene.strips.push_back(std::move(direct_strip));
  }
  const std::string scene_json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer =
      sonare_mixer_from_scene_json(scene_json.c_str(), static_cast<int>(sample_rate), block_size);
  if (mixer == nullptr) return nullptr;
  sonare_c_mixing_detail::build_and_compile(mixer);
  schedule_mixer_automation(timeline, routing, sample_rate, mixer, out_diagnostics);

  // Snap each strip's fader/input-trim/width/pan smoothers to their steady-state
  // targets so the mixer summing pass opens at the configured gain instead of
  // fading in from the previous value over the first ~5 ms block. This keeps the
  // offline bounce deterministic; without it a non-default static fader ramps in
  // on the master. (See ChannelStrip::settle for the full set of snapped stages.)
  for (const std::string& strip_id : routing.strip_ids) {
    if (SonareStrip* strip = sonare_mixer_strip_by_id(mixer, strip_id.c_str())) {
      strip->strip.settle();
    }
  }
  if (!direct_strip_id.empty()) {
    if (SonareStrip* strip = sonare_mixer_strip_by_id(mixer, direct_strip_id.c_str())) {
      strip->strip.settle();
    }
  }
  return mixer;
}

struct MixerLatencyTail {
  int latency_samples = 0;
  int tail_samples = 0;
  bool valid = false;
};

MixerLatencyTail mixer_latency_tail_for_timeline(
    const arr::CompiledTimeline& timeline, const MixerRouting& routing, double sample_rate,
    int block_size, MixerPtr* out_mixer = nullptr,
    std::vector<arr::Diagnostic>* out_diagnostics = nullptr) {
  MixerPtr mixer(create_timeline_mixer(timeline, routing, sample_rate, block_size,
                                       /*direct_strip_id=*/{}, out_diagnostics));
  if (!mixer) return {};
  MixerLatencyTail result;
  int latency = 0;
  int tail = 0;
  if (sonare_mixer_latency_samples(mixer.get(), &latency) != SONARE_OK ||
      sonare_mixer_tail_samples(mixer.get(), &tail) != SONARE_OK || latency < 0 || tail < 0) {
    return result;
  }
  result.latency_samples = latency;
  result.tail_samples = tail;
  result.valid = true;
  if (out_mixer != nullptr) {
    *out_mixer = std::move(mixer);
  }
  return result;
}

// Channel-strip bounce: renders each bound track as an isolated dry
// stereo stem and sums the stems through the scene's mixer so per-track EQ,
// inserts, pan, fader, sends and buses are applied. Tracks bound to no scene
// strip are rendered into a separate dry stem and summed straight into the
// master. `frames`/`pdc` are precomputed by the caller; stems are aligned to
// [0, frames) after dropping the leading PDC fill.
SonareError bounce_through_mixer(const arr::CompiledTimeline& timeline,
                                 const std::vector<HostedInstrument>& instruments,
                                 const MixerRouting& routing, double sample_rate, int block_size,
                                 int num_channels, int64_t frames, int64_t pdc,
                                 int64_t mixer_input_frames, float** out_interleaved,
                                 size_t* out_len, SonareMixer* prebuilt_mixer = nullptr,
                                 std::vector<arr::Diagnostic>* out_diagnostics = nullptr) {
  size_t total = 0;
  if (num_channels <= 0 ||
      !checked_frame_shape(frames, static_cast<size_t>(num_channels), &total) || pdc < 0 ||
      mixer_input_frames < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  MixerPtr mixer_owner(prebuilt_mixer);
  MixerRouting effective_routing = routing;
  bool shared_hosts_source_aware = true;
  const bool shared_midi_destination =
      has_shared_hosted_midi_destination(timeline, instruments, &shared_hosts_source_aware);
  // A shared-destination render has a destination-scoped residual target
  // (SF2 effects / native bodies). Reserve the direct identity strip for it
  // even when every authored track is bound to a scene strip.
  const bool route_direct =
      timeline_has_unbound_tracks(timeline, routing) || shared_midi_destination;
  const std::string direct_strip_id =
      route_direct ? unique_direct_strip_id(timeline.mixer.scene) : std::string();
  if (route_direct) {
    effective_routing.strip_ids.push_back(direct_strip_id);
    effective_routing.strip_tracks.emplace_back();
  }

  // Build the mixer before rendering stems so we know how many extra internal
  // frames are needed to compensate master-output latency. A prebuilt mixer is
  // reusable only when it holds exactly the strips this routing feeds; anything
  // else is discarded and rebuilt so the input count handed to
  // sonare_mixer_process_stereo always matches the mixer's strip count.
  if (mixer_owner &&
      sonare_mixer_strip_count(mixer_owner.get()) != effective_routing.strip_ids.size()) {
    mixer_owner.reset();
  }
  if (!mixer_owner) {
    mixer_owner.reset(create_timeline_mixer(timeline, routing, sample_rate, block_size,
                                            direct_strip_id, out_diagnostics));
  }
  if (!mixer_owner) return SONARE_ERROR_INVALID_STATE;
  // An automation lane that did not fit its strip is caught here, before any
  // stem is rendered, so the caller gets the diagnostic instead of audio whose
  // automation curve froze partway through.
  if (out_diagnostics != nullptr && has_error_diagnostic(*out_diagnostics)) {
    return SONARE_ERROR_INVALID_STATE;
  }
  int mixer_latency = 0;
  if (sonare_mixer_latency_samples(mixer_owner.get(), &mixer_latency) != SONARE_OK ||
      mixer_latency < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  int64_t mixer_render_frames = 0;
  if (!checked_nonnegative_add(frames, static_cast<int64_t>(mixer_latency), &mixer_render_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const size_t strip_count = effective_routing.strip_ids.size();
  // Stems are stereo (the mixer is stereo): one per strip plus one direct stem.
  size_t stem_floats = 0;
  if (!checked_frame_shape(mixer_render_frames, 2, strip_count, &stem_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  int64_t render_frames = 0;
  if (!checked_nonnegative_add(mixer_render_frames, pdc, &render_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t render_floats = 0;
  if (!checked_frame_shape(render_frames, 2, &render_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t mixer_frame_count = 0;
  size_t master_floats = 0;
  if (!checked_frame_count(mixer_render_frames, &mixer_frame_count) ||
      !checked_frame_shape(mixer_render_frames, 2, &master_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::unique_ptr<MidiSourceStemSink> midi_source_stems;
  if (shared_midi_destination) {
    if (!shared_hosts_source_aware || pdc != 0) {
      set_last_error(
          "a MIDI destination shared by channel strips requires source-aware, zero-latency "
          "instruments for project bounce");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    std::set<uint32_t> midi_tracks;
    for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
      if (clip.track_id != 0) midi_tracks.insert(clip.track_id);
    }
    size_t midi_source_floats = 0;
    size_t render_frame_count = 0;
    if (!checked_midi_source_stem_shape(midi_tracks.size(), render_frames, &midi_source_floats) ||
        !checked_frame_count(render_frames, &render_frame_count)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    midi_source_stems = std::make_unique<MidiSourceStemSink>(
        midi_tracks, 2, render_frames, render_frame_count, timeline, sample_rate, block_size);
    if (!midi_source_stems->ready()) {
      set_last_error("could not prepare source-track mixer lanes");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    if (!render_midi_source_stems(timeline, instruments, sample_rate, block_size, render_frames,
                                  midi_source_stems.get())) {
      set_last_error("could not render source-track MIDI stems for shared channel strips");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
  }
  auto stem_aligned = [&](const std::function<bool(uint32_t)>& keep,
                          std::vector<std::vector<float>>* out) {
    if (out == nullptr) return false;
    std::vector<std::vector<float>> ch;
    if (!render_timeline(timeline, keep, instruments, sample_rate, block_size,
                         /*num_channels=*/2, render_frames, &ch, /*include_audio=*/true,
                         /*include_midi=*/midi_source_stems == nullptr)) {
      return false;
    }
    for (auto& c : ch) {
      if (pdc > 0) c.erase(c.begin(), c.begin() + pdc);
      c.resize(mixer_frame_count);
    }
    *out = std::move(ch);
    return true;
  };

  // One stereo stem per strip (silent if the strip has no source track).
  std::vector<std::vector<std::vector<float>>> stems;
  stems.reserve(strip_count);
  for (size_t i = 0; i < effective_routing.strip_tracks.size(); ++i) {
    const std::set<uint32_t>& tracks = effective_routing.strip_tracks[i];
    if (tracks.empty()) {
      stems.emplace_back(2, std::vector<float>(mixer_frame_count, 0.0f));
      continue;
    }
    std::vector<std::vector<float>> stem;
    if (!stem_aligned([&tracks](uint32_t t) { return tracks.count(t) != 0; }, &stem)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    stems.push_back(std::move(stem));
    if (midi_source_stems) {
      for (uint32_t track_id : tracks) {
        if (const auto* stem = midi_source_stems->stem(track_id)) add_stem(&stems.back(), *stem);
      }
    }
  }

  // Direct stem: every track NOT bound to a scene strip (dry to master).
  if (route_direct) {
    const std::set<uint32_t>& bound = routing.bound_tracks;
    std::vector<std::vector<float>> direct;
    if (!stem_aligned([&bound](uint32_t t) { return bound.count(t) == 0; }, &direct)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (midi_source_stems) {
      std::set<uint32_t> direct_midi_tracks;
      for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
        if (bound.count(clip.track_id) == 0) direct_midi_tracks.insert(clip.track_id);
      }
      for (uint32_t track_id : direct_midi_tracks) {
        if (const auto* stem = midi_source_stems->stem(track_id)) add_stem(&direct, *stem);
      }
      add_stem(&direct, midi_source_stems->default_stem());
    }
    stems.back() = std::move(direct);
  }

  // Sum the strip stems through the mixer block by block.
  std::vector<float> master_l(mixer_frame_count, 0.0f);
  std::vector<float> master_r(mixer_frame_count, 0.0f);
  std::vector<const float*> in_l(strip_count, nullptr);
  std::vector<const float*> in_r(strip_count, nullptr);
  SonareError err = SONARE_OK;
  const int64_t input_frames = std::clamp<int64_t>(mixer_input_frames, 0, mixer_render_frames);
  for (int64_t off = 0; off < input_frames; off += block_size) {
    const size_t n = static_cast<size_t>(std::min<int64_t>(block_size, input_frames - off));
    for (size_t i = 0; i < strip_count; ++i) {
      in_l[i] = stems[i][0].data() + off;
      in_r[i] = stems[i][1].data() + off;
    }
    err = sonare_mixer_process_stereo(mixer_owner.get(), in_l.data(), in_r.data(), strip_count,
                                      master_l.data() + off, master_r.data() + off, n);
    if (err != SONARE_OK) break;
  }
  for (int64_t off = input_frames; err == SONARE_OK && off < mixer_render_frames;
       off += block_size) {
    const size_t n = static_cast<size_t>(std::min<int64_t>(block_size, mixer_render_frames - off));
    err = sonare_mixer_drain_tail_stereo(mixer_owner.get(), master_l.data() + off,
                                         master_r.data() + off, n);
  }
  if (err != SONARE_OK) return err;

  // Interleave into the requested channel count: mono downmixes the stereo
  // master; channels beyond stereo are left silent.
  const size_t mixer_latency_count = static_cast<size_t>(mixer_latency);
  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t f = 0; f < frames; ++f) {
    const size_t source = static_cast<size_t>(f) + mixer_latency_count;
    const float l = master_l[source];
    const float r = master_r[source];
    for (int ch = 0; ch < num_channels; ++ch) {
      float v = 0.0f;
      if (num_channels == 1) {
        v = 0.5f * (l + r);
      } else if (ch == 0) {
        v = l;
      } else if (ch == 1) {
        v = r;
      }
      interleaved[static_cast<size_t>(f) * num_channels + ch] = v;
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
}
#endif  // SONARE_WITH_MIXING

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
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  clear_last_bounce_result(project);

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

// Maps the public built-in waveform ordinal to the core enum (out-of-range
// values fall back to sine via clamp_synth_config).
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
// model-first field. Anything newer is rejected by the
// caller). The player clamps polyphony itself.
sonare::midi::synth::Sf2PlayerConfig sf2_config_from_c(
    const SonareSf2InstrumentConfig& c) noexcept {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (c.gain > 0.0f) cfg.gain = c.gain;
  if (c.polyphony > 0) cfg.polyphony = c.polyphony;
  if (c.struct_version >= 2) {
    cfg.prefer_model_for_modeled_families = c.prefer_model_for_modeled_families != 0;
  }
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
    owned.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(synth_config_from_c(instruments[i].config)));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

const char* sonare_synth_preset_names(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kNames = [] {
    std::string names;
    for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
      if (!names.empty()) names += '\n';
      names += sonare::midi::synth::synth_preset_at(i)->name;
    }
    return names;
  }();
  return kNames.c_str();
#else
  return "";
#endif
}

const char* sonare_synth_enum_names(int kind) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kEngineModes =
      "default\nsubtractive\nfm\nkarplus-strong\nmodal\nadditive\npercussion\npiano\npipe-organ\n"
      "bowed-string\nreed\nbrass\nflute\nplucked-string\nvocal\nfree-reed";
  static const std::string kWaveforms = "default\nsine\nsaw\nsquare\ntriangle\nnoise";
  static const std::string kBuiltinWaveforms = "sine\nsaw\nsawtooth\nsquare\ntriangle";
  static const std::string kFilterModels = "default\nsvf\nmoog-ladder\ndiode-ladder\nsallen-key";
  static const std::string kFilterOutputs = "default\nlowpass\nbandpass\nhighpass";
  static const std::string kBodyTypes =
      "default\nnone\nguitar\nviolin\nwood-tube\nbrass-bell\nvocal";
  static const std::string kModSources =
      "none\namp-env\nfilter-env\nlfo1\nlfo2\nvelocity\nkey-track\nmod-wheel\nrandom";
  static const std::string kModDestinations =
      "none\npitch-cents\ncutoff-cents\namp-gain\npan-units";

  switch (kind) {
    case SONARE_SYNTH_ENUM_ENGINE_MODE:
      return kEngineModes.c_str();
    case SONARE_SYNTH_ENUM_OSC_WAVEFORM:
      return kWaveforms.c_str();
    case SONARE_SYNTH_ENUM_FILTER_MODEL:
      return kFilterModels.c_str();
    case SONARE_SYNTH_ENUM_FILTER_OUTPUT:
      return kFilterOutputs.c_str();
    case SONARE_SYNTH_ENUM_BODY_TYPE:
      return kBodyTypes.c_str();
    case SONARE_SYNTH_ENUM_MOD_SOURCE:
      return kModSources.c_str();
    case SONARE_SYNTH_ENUM_MOD_DESTINATION:
      return kModDestinations.c_str();
    case SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM:
      return kBuiltinWaveforms.c_str();
    default:
      return "";
  }
#else
  (void)kind;
  return "";
#endif
}

int sonare_synth_builtin_waveform_from_name(const char* name) {
  if (name == nullptr) return -1;
  if (std::strcmp(name, "sine") == 0) return SONARE_SYNTH_WAVEFORM_SINE;
  if (std::strcmp(name, "saw") == 0 || std::strcmp(name, "sawtooth") == 0) {
    return SONARE_SYNTH_WAVEFORM_SAW;
  }
  if (std::strcmp(name, "square") == 0) return SONARE_SYNTH_WAVEFORM_SQUARE;
  if (std::strcmp(name, "triangle") == 0) return SONARE_SYNTH_WAVEFORM_TRIANGLE;
  return -1;
}

SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (!name || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(name);
  if (preset == nullptr) {
    set_last_error("unknown synth preset name");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare_c_detail::synth_patch_to_c(*preset, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(name, out);
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
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
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
    if (instruments[i].config.struct_version > 2) return SONARE_ERROR_INVALID_PARAMETER;
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
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}
