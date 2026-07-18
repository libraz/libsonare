#include "c_api/project_internal.h"
#include "util/constants.h"

#if defined(SONARE_WITH_ARRANGEMENT)

// Pin the C fade-curve / loop-mode / overlap ordinals to their C++ enums so
// reordering a C++ enum is caught at compile time (these flat-POD ordinals are
// part of the project ABI).
static_assert(static_cast<int>(arr::FadeCurve::kLinear) == SONARE_FADE_CURVE_LINEAR,
              "SonareProjectFadeCurve linear ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kEqualPower) == SONARE_FADE_CURVE_EQUAL_POWER,
              "SonareProjectFadeCurve equal-power ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kExponential) == SONARE_FADE_CURVE_EXPONENTIAL,
              "SonareProjectFadeCurve exponential ordinal drift");
static_assert(static_cast<int>(arr::FadeCurve::kLogarithmic) == SONARE_FADE_CURVE_LOGARITHMIC,
              "SonareProjectFadeCurve logarithmic ordinal drift");
static_assert(static_cast<int>(arr::LoopMode::kOff) == SONARE_LOOP_MODE_OFF,
              "SonareProjectLoopMode off ordinal drift");
static_assert(static_cast<int>(arr::LoopMode::kLoop) == SONARE_LOOP_MODE_LOOP,
              "SonareProjectLoopMode loop ordinal drift");
static_assert(static_cast<uint32_t>(arr::OverlapPolicy::kDisallow) ==
                  SONARE_PROJECT_OVERLAP_DISALLOW,
              "SonareProjectOverlapPolicy disallow ordinal drift");
static_assert(static_cast<uint32_t>(arr::OverlapPolicy::kAllow) == SONARE_PROJECT_OVERLAP_ALLOW,
              "SonareProjectOverlapPolicy allow ordinal drift");

namespace {

enum class AudioContentTransferDirection { kStore, kHistory };

/// Shared ownership state for decoded audio added by a compound public edit.
///
/// While the edit is applied, `contents` is empty and the samples live only in
/// AudioContentStore. Undo extracts the map nodes into this history-owned state;
/// redo inserts the same nodes back without copying their sample buffers. If an
/// undone edit's redo branch is discarded, destroying the history entry also
/// destroys the detached samples, so decoded audio cannot survive as an orphan.
struct AudioContentTransferState {
  arr::AudioContentStore* store = nullptr;
  std::vector<arr::SourceId> source_ids;
  std::map<arr::SourceId, arr::AudioSourceSamples> contents;
};

class TransferAudioContent final : public arr::EditCommand {
 public:
  TransferAudioContent(std::shared_ptr<AudioContentTransferState> state,
                       AudioContentTransferDirection direction)
      : state_(std::move(state)), direction_(direction) {}

  bool apply(arr::Project& /*project*/, arr::MidiContentStore& /*midi*/) override {
    if (state_ == nullptr || state_->store == nullptr || state_->source_ids.empty()) {
      return false;
    }
    auto& store = state_->store->sources;
    auto& contents = state_->contents;
    if (direction_ == AudioContentTransferDirection::kStore) {
      if (contents.size() != state_->source_ids.size()) return false;
      for (arr::SourceId id : state_->source_ids) {
        if (contents.find(id) == contents.end() || store.find(id) != store.end()) return false;
      }
      for (arr::SourceId id : state_->source_ids) {
        store.insert(contents.extract(id));
      }
      return true;
    }

    if (!contents.empty()) return false;
    for (arr::SourceId id : state_->source_ids) {
      if (store.find(id) == store.end()) return false;
    }
    for (arr::SourceId id : state_->source_ids) {
      contents.insert(store.extract(id));
    }
    return true;
  }

  arr::EditCommandPtr invert(const arr::Project& /*before*/,
                             const arr::MidiContentStore& /*midi_before*/) const override {
    const auto inverse = direction_ == AudioContentTransferDirection::kStore
                             ? AudioContentTransferDirection::kHistory
                             : AudioContentTransferDirection::kStore;
    return std::make_unique<TransferAudioContent>(state_, inverse);
  }

  const char* type_name() const noexcept override { return "TransferAudioContent"; }

 private:
  std::shared_ptr<AudioContentTransferState> state_;
  AudioContentTransferDirection direction_;
};

arr::EditCommandPtr make_store_audio_content_command(
    arr::AudioContentStore* store, std::map<arr::SourceId, arr::AudioSourceSamples> contents) {
  if (store == nullptr || contents.empty()) return nullptr;
  auto state = std::make_shared<AudioContentTransferState>();
  state->store = store;
  state->source_ids.reserve(contents.size());
  for (const auto& [source_id, samples] : contents) {
    (void)samples;
    state->source_ids.push_back(source_id);
  }
  state->contents = std::move(contents);
  return std::make_unique<TransferAudioContent>(std::move(state),
                                                AudioContentTransferDirection::kStore);
}

bool valid_next_id(uint32_t id) noexcept {
  return id != 0 && id != std::numeric_limits<uint32_t>::max();
}

// Validates a fade desc and copies it into an arrangement ClipFade. Returns
// SONARE_OK on success; the fade length must be finite and >= 0 and the curve
// ordinal in range.
SonareError clip_fade_from_desc(const SonareProjectClipFade* desc, arr::ClipFade* out) {
  if (desc == nullptr || out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!finite_non_negative(desc->length_ppq)) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->curve > static_cast<uint32_t>(arr::FadeCurve::kLogarithmic)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->length_ppq = desc->length_ppq;
  out->curve = static_cast<arr::FadeCurve>(desc->curve);
  return SONARE_OK;
}

SonareError clip_takes_from_desc(const SonareProjectClipTake* takes, size_t take_count,
                                 std::vector<arr::ClipTake>* out) {
  if (!out || (take_count > 0 && takes == nullptr) || take_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->clear();
  out->reserve(take_count);
  for (size_t i = 0; i < take_count; ++i) {
    const SonareProjectClipTake& take = takes[i];
    if (take.id == 0 || !finite_non_negative(take.source_offset_ppq)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    arr::ClipTake next;
    next.id = take.id;
    next.source_id = take.source_id;
    next.source_offset_ppq = take.source_offset_ppq;
    if (take.name) next.name = take.name;
    out->push_back(std::move(next));
  }
  return SONARE_OK;
}

SonareError clip_comp_segments_from_desc(const SonareProjectClipCompSegment* segments,
                                         size_t segment_count,
                                         std::vector<arr::ClipCompSegment>* out) {
  if (!out || (segment_count > 0 && segments == nullptr) || segment_count > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->clear();
  out->reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectClipCompSegment& segment = segments[i];
    if (!finite_non_negative(segment.start_ppq) || !finite_positive(segment.end_ppq) ||
        !(segment.end_ppq > segment.start_ppq)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    out->push_back({segment.start_ppq, segment.end_ppq, segment.take_id});
  }
  return SONARE_OK;
}

}  // namespace

#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// Edit
// ============================================================================

SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !desc || !out_clip_id) return SONARE_ERROR_INVALID_PARAMETER;
  if (desc->track_id == 0 || !finite_positive(desc->length_ppq) ||
      !finite_non_negative(desc->start_ppq) || !finite_non_negative(desc->source_offset_ppq) ||
      !std::isfinite(desc->gain) || desc->gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_track(desc->track_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const SonareError audio_err =
      desc->is_midi == 0 ? validate_audio_clip_payload(desc, nullptr) : SONARE_OK;
  if (audio_err != SONARE_OK) return audio_err;
  SONARE_C_TRY
  const arr::SourceId source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  if (!valid_next_id(source_id) || !valid_next_id(clip_id)) {
    return SONARE_ERROR_INVALID_STATE;
  }

  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(3);
  std::map<arr::SourceId, arr::AudioSourceSamples> audio_contents;
  if (desc->is_midi != 0) {
    arr::MidiSourceRef ref;
    auto attach = std::make_unique<arr::AttachMidiSource>(ref);
    commands.push_back(std::move(attach));
  } else {
    arr::AudioSourceRef ref;
    if (desc->source_uri) ref.uri = desc->source_uri;
    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      ref.channel_count = static_cast<uint32_t>(desc->audio_channels);
      ref.sample_rate_hint = static_cast<double>(desc->audio_sample_rate);
    }
    auto attach = std::make_unique<arr::AttachAudioSource>(ref);
    commands.push_back(std::move(attach));

    if (desc->audio_interleaved && desc->audio_frames > 0 && desc->audio_channels > 0 &&
        desc->audio_sample_rate > 0) {
      arr::AudioSourceSamples samples;
      samples.sample_rate = static_cast<double>(desc->audio_sample_rate);
      samples.channels =
          deinterleave(desc->audio_interleaved, desc->audio_frames, desc->audio_channels);
      audio_contents.emplace(source_id, std::move(samples));
    }
  }

  arr::EditClip clip;
  clip.track_id = desc->track_id;
  clip.source_id = source_id;
  clip.start_ppq = desc->start_ppq;
  clip.length_ppq = desc->length_ppq;
  clip.source_offset_ppq = desc->source_offset_ppq;
  // Pass the gain through literally (validated finite and >= 0 above): a gain of
  // 0 is a legitimately silent clip, not a request for unity. No coercion here.
  clip.gain = desc->gain;
  auto command = std::make_unique<arr::AddClip>(clip);
  commands.push_back(std::move(command));
  if (!audio_contents.empty()) {
    commands.push_back(
        make_store_audio_content_command(&project->audio, std::move(audio_contents)));
  }
  if (!project->history.apply_transaction(std::move(commands))) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = clip_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_clip_id);
#endif
}

SonareError sonare_project_add_loop_recording_takes(SonareProject* project,
                                                    const SonareProjectLoopRecordingDesc* desc,
                                                    uint32_t* out_clip_id, size_t* out_take_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (out_take_count) *out_take_count = 0;
  if (!project || !desc || !out_clip_id || desc->track_id == 0 ||
      !finite_non_negative(desc->start_ppq) || !finite_positive(desc->loop_length_ppq) ||
      !desc->audio_interleaved || desc->audio_frames <= 0 || desc->audio_channels <= 0 ||
      desc->audio_sample_rate < kMinSampleRate || desc->audio_sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto total_frames = static_cast<uint64_t>(desc->audio_frames);
  const auto channels = static_cast<uint64_t>(desc->audio_channels);
  if (channels == 0 || total_frames > std::numeric_limits<size_t>::max() / channels) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t total_samples = static_cast<size_t>(total_frames * channels);
  if (total_samples == 0 || total_samples > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < total_samples; ++i) {
    if (!std::isfinite(desc->audio_interleaved[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::Track* track = project->history.project().find_track(desc->track_id);
  if (track == nullptr || track->kind != arr::Track::Kind::kAudio) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  SONARE_C_TRY
  sonare::transport::TempoMap tempo_map;
  tempo_map.prepare(project->history.project().sample_rate());
  std::vector<sonare::transport::TempoSegment> tempo_segments =
      project->history.project().tempo_segments();
  if (tempo_segments.empty()) {
    tempo_segments.push_back({0.0, sonare::constants::kDefaultBpm, 0.0});
  }
  tempo_map.set_segments(std::move(tempo_segments));
  std::vector<sonare::transport::TimeSignatureSegment> time_signatures =
      project->history.project().time_signatures();
  if (time_signatures.empty()) {
    time_signatures.push_back({0.0, {4, 4}});
  }
  tempo_map.set_time_signatures(std::move(time_signatures));

  const int64_t loop_start_project_sample = tempo_map.ppq_to_sample(desc->start_ppq);
  const int64_t loop_end_project_sample =
      tempo_map.ppq_to_sample(desc->start_ppq + desc->loop_length_ppq);
  const int64_t loop_project_samples = loop_end_project_sample - loop_start_project_sample;
  if (loop_project_samples <= 0 || !(project->history.project().sample_rate() > 0.0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const double loop_audio_frames_f =
      static_cast<double>(loop_project_samples) *
      (static_cast<double>(desc->audio_sample_rate) / project->history.project().sample_rate());
  if (!(loop_audio_frames_f > 0.0) || !std::isfinite(loop_audio_frames_f)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const int64_t loop_audio_frames = std::max<int64_t>(1, std::llround(loop_audio_frames_f));
  const size_t take_count =
      static_cast<size_t>((desc->audio_frames + loop_audio_frames - 1) / loop_audio_frames);
  if (take_count == 0 || take_count > kMaxBufferSize ||
      take_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const arr::SourceId first_source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  const auto reserved_id = std::numeric_limits<uint32_t>::max();
  if (!valid_next_id(first_source_id) || !valid_next_id(clip_id) ||
      take_count > static_cast<size_t>(reserved_id - first_source_id)) {
    return SONARE_ERROR_INVALID_STATE;
  }

  std::vector<arr::SourceId> source_ids;
  source_ids.reserve(take_count);
  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(take_count + 2);
  std::map<arr::SourceId, arr::AudioSourceSamples> audio_contents;

  for (size_t take_index = 0; take_index < take_count; ++take_index) {
    const int64_t frame_start = static_cast<int64_t>(take_index) * loop_audio_frames;
    const int64_t frame_count =
        std::min<int64_t>(loop_audio_frames, desc->audio_frames - frame_start);
    if (frame_count <= 0) break;

    arr::AudioSourceRef ref;
    ref.channel_count = static_cast<uint32_t>(desc->audio_channels);
    ref.sample_rate_hint = static_cast<double>(desc->audio_sample_rate);
    auto attach = std::make_unique<arr::AttachAudioSource>(ref);
    const arr::SourceId source_id = first_source_id + static_cast<arr::SourceId>(take_index);
    commands.push_back(std::move(attach));
    source_ids.push_back(source_id);

    arr::AudioSourceSamples samples;
    samples.sample_rate = static_cast<double>(desc->audio_sample_rate);
    samples.channels = deinterleave(desc->audio_interleaved + frame_start * desc->audio_channels,
                                    frame_count, desc->audio_channels);
    audio_contents.emplace(source_id, std::move(samples));
  }
  if (source_ids.empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }

  arr::EditClip clip;
  clip.track_id = desc->track_id;
  clip.source_id = source_ids.front();
  clip.start_ppq = desc->start_ppq;
  clip.length_ppq = desc->loop_length_ppq;
  clip.gain = 1.0f;
  clip.takes.reserve(source_ids.size());
  for (size_t i = 0; i < source_ids.size(); ++i) {
    const auto take_id = static_cast<arr::TakeId>(i + 1);
    clip.takes.push_back({take_id, source_ids[i], 0.0, "take " + std::to_string(i + 1)});
    clip.active_take_id = take_id;
  }

  auto command = std::make_unique<arr::AddClip>(clip);
  commands.push_back(std::move(command));
  commands.push_back(make_store_audio_content_command(&project->audio, std::move(audio_contents)));
  if (!project->history.apply_transaction(std::move(commands))) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = clip_id;
  if (out_take_count) *out_take_count = source_ids.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_clip_id, out_take_count);
#endif
}

SonareError sonare_project_add_midi_clip(SonareProject* project, double start_ppq,
                                         double length_ppq, uint32_t* out_track_id,
                                         uint32_t* out_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_track_id) *out_track_id = 0;
  if (out_clip_id) *out_clip_id = 0;
  if (!project || !out_track_id || !out_clip_id || !finite_non_negative(start_ppq) ||
      !finite_positive(length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const arr::TrackId track_id = project->history.project().next_track_id();
  const arr::SourceId source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  if (!valid_next_id(track_id) || !valid_next_id(source_id) || !valid_next_id(clip_id)) {
    return SONARE_ERROR_INVALID_STATE;
  }

  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(3);

  arr::Track track;
  track.kind = arr::Track::Kind::kMidi;
  track.name = "midi";
  auto add_track = std::make_unique<arr::AddTrack>(std::move(track));
  commands.push_back(std::move(add_track));

  auto attach = std::make_unique<arr::AttachMidiSource>(arr::MidiSourceRef{});
  commands.push_back(std::move(attach));

  arr::EditClip clip;
  clip.track_id = track_id;
  clip.source_id = source_id;
  clip.start_ppq = start_ppq;
  clip.length_ppq = length_ppq;
  clip.gain = 0.0f;
  auto add_clip = std::make_unique<arr::AddClip>(std::move(clip));
  commands.push_back(std::move(add_clip));

  if (!project->history.apply_transaction(std::move(commands))) {
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_track_id = track_id;
  *out_clip_id = clip_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, start_ppq, length_ppq, out_track_id, out_clip_id);
#endif
}

SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project || clip_id == 0 || !std::isfinite(split_ppq)) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SplitClip>(clip_id, split_ppq);
  arr::SplitClip* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_new_clip_id) *out_new_clip_id = raw->new_clip_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, split_ppq, out_new_clip_id);
#endif
}

SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq) ||
      !finite_positive(new_length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::TrimClip>(clip_id, new_start_ppq, new_length_ppq);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_length_ppq);
#endif
}

SonareError sonare_project_move_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     uint32_t new_track_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::MoveClip>(clip_id, new_start_ppq, new_track_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, new_track_id);
#endif
}

SonareError sonare_project_set_clip_warp_ref(SonareProject* project, uint32_t clip_id,
                                             uint32_t warp_ref_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipWarpRef>(clip_id, warp_ref_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, warp_ref_id);
#endif
}

SonareError sonare_project_set_clip_warp_mode(SonareProject* project, uint32_t clip_id,
                                              SonareProjectWarpMode mode) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (mode != SONARE_PROJECT_WARP_MODE_OFF && mode != SONARE_PROJECT_WARP_MODE_REPITCH &&
      mode != SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::WarpMode arr_mode = arr::WarpMode::kOff;
  if (mode == SONARE_PROJECT_WARP_MODE_REPITCH) {
    arr_mode = arr::WarpMode::kRepitch;
  } else if (mode == SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) {
    arr_mode = arr::WarpMode::kTempoSync;
  }
  auto command = std::make_unique<arr::SetClipWarpMode>(clip_id, arr_mode);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, mode);
#endif
}

SonareError sonare_project_set_clip_takes(SonareProject* project, uint32_t clip_id,
                                          const SonareProjectClipTake* takes, size_t take_count,
                                          uint32_t active_take_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<arr::ClipTake> arr_takes;
  const SonareError err = clip_takes_from_desc(takes, take_count, &arr_takes);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::SetClipTakes>(clip_id, std::move(arr_takes), active_take_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, takes, take_count, active_take_id);
#endif
}

SonareError sonare_project_set_clip_comp_segments(SonareProject* project, uint32_t clip_id,
                                                  const SonareProjectClipCompSegment* segments,
                                                  size_t segment_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<arr::ClipCompSegment> arr_segments;
  const SonareError err = clip_comp_segments_from_desc(segments, segment_count, &arr_segments);
  if (err != SONARE_OK) return err;
  auto command = std::make_unique<arr::SetClipCompSegments>(clip_id, std::move(arr_segments));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, segments, segment_count);
#endif
}

SonareError sonare_project_remove_clip(SonareProject* project, uint32_t clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::RemoveClip>(clip_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id);
#endif
}

SonareError sonare_project_set_clip_gain(SonareProject* project, uint32_t clip_id, float gain) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !std::isfinite(gain) || gain < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipGain>(clip_id, gain);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, gain);
#endif
}

SonareError sonare_project_set_clip_fade(SonareProject* project, uint32_t clip_id,
                                         const SonareProjectClipFade* fade_in,
                                         const SonareProjectClipFade* fade_out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || !fade_in || !fade_out) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  arr::ClipFade in_fade;
  arr::ClipFade out_fade;
  SonareError err = clip_fade_from_desc(fade_in, &in_fade);
  if (err != SONARE_OK) return err;
  err = clip_fade_from_desc(fade_out, &out_fade);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipFade>(clip_id, in_fade, out_fade);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, fade_in, fade_out);
#endif
}

SonareError sonare_project_set_clip_loop(SonareProject* project, uint32_t clip_id, int loop_mode,
                                         double loop_length_ppq, double loop_crossfade_ppq) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || loop_mode < SONARE_LOOP_MODE_OFF ||
      loop_mode > SONARE_LOOP_MODE_LOOP) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Under LOOP, a length of 0 means "loop the entire clip" (resolved from the
  // clip's own duration at compile time); any other mode also permits 0.
  if (!finite_non_negative(loop_length_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(loop_crossfade_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipLoop>(clip_id, static_cast<arr::LoopMode>(loop_mode),
                                                    loop_length_ppq, loop_crossfade_ppq);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, loop_mode, loop_length_ppq, loop_crossfade_ppq);
#endif
}

SonareError sonare_project_set_clip_source(SonareProject* project, uint32_t clip_id,
                                           uint32_t source_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || clip_id == 0 || source_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (project->history.project().find_clip(clip_id) == nullptr ||
      !project->history.project().has_source(source_id)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipSource>(clip_id, source_id);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, source_id);
#endif
}

SonareError sonare_project_duplicate_clip(SonareProject* project, uint32_t clip_id,
                                          double new_start_ppq, uint32_t* out_new_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project || clip_id == 0 || !finite_non_negative(new_start_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::DuplicateClip>(clip_id, new_start_ppq);
  arr::DuplicateClip* raw = command.get();
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  if (out_new_clip_id) *out_new_clip_id = raw->new_clip_id();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, out_new_clip_id);
#endif
}
