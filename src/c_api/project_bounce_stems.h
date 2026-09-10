#pragma once

/// @file project_bounce_stems.h
/// @brief The MIDI source-stem render path of the project bounce.

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "c_api/project_bounce_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
#include "engine/track_mixer.h"

namespace sonare_c_bounce_detail {

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
                              int block_size, int64_t render_frames, MidiSourceStemSink* sink);

bool has_shared_hosted_midi_destination(const arr::CompiledTimeline& timeline,
                                        const std::vector<HostedInstrument>& instruments,
                                        bool* all_hosts_source_aware);

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_MIXING
