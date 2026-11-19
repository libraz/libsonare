#include <algorithm>
#include <set>
#include <string>

#include "arrangement/vocal_edit_sidecar.h"
#include "c_api/project_internal.h"
#include "util/constants.h"

SonareError sonare_project_fade_curve_from_name(const char* name, uint32_t* out_curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  if (name == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_fade_curve_from_name: name must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (out_curve == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_fade_curve_from_name: out_curve must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const std::string token(name);
  if (token == "linear") {
    *out_curve = SONARE_FADE_CURVE_LINEAR;
  } else if (token == "equalPower") {
    *out_curve = SONARE_FADE_CURVE_EQUAL_POWER;
  } else if (token == "exponential") {
    *out_curve = SONARE_FADE_CURVE_EXPONENTIAL;
  } else if (token == "logarithmic") {
    *out_curve = SONARE_FADE_CURVE_LOGARITHMIC;
  } else {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_fade_curve_from_name: name is not a fade curve name: " + token).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

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

std::size_t retained_audio_transfer_state_bytes(const AudioContentTransferState& state) noexcept {
  std::size_t total = sizeof(state);
  total = arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.source_ids));
  return arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.contents));
}

class TransferAudioContent final : public arr::EditCommand, public arr::EditCommandExternalEffects {
 public:
  TransferAudioContent(std::shared_ptr<AudioContentTransferState> state,
                       AudioContentTransferDirection direction)
      : state_(std::move(state)), direction_(direction) {}

  bool apply(arr::Project& /*project*/, arr::MidiContentStore& /*midi*/) override {
    // A command instance is replayed during undo/redo.  Clear the flag before
    // validation so a failed replay never asks rollback to undo an earlier
    // successful application of the same command instance.
    applied_ = false;
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
      // Mark before the first node move so a hypothetical exception from a
      // comparator or allocator seam still has an idempotent rollback path.
      applied_ = true;
      for (arr::SourceId id : state_->source_ids) {
        store.insert(contents.extract(id));
      }
      return true;
    }

    if (!contents.empty()) return false;
    for (arr::SourceId id : state_->source_ids) {
      if (store.find(id) == store.end()) return false;
    }
    applied_ = true;
    for (arr::SourceId id : state_->source_ids) {
      contents.insert(store.extract(id));
    }
    return true;
  }

  void rollback_apply(arr::Project& /*project*/,
                      arr::MidiContentStore& /*midi*/) noexcept override {
    if (!applied_ || state_ == nullptr || state_->store == nullptr) return;

    auto& store = state_->store->sources;
    auto& contents = state_->contents;
    if (direction_ == AudioContentTransferDirection::kStore) {
      // The forward direction moved nodes from history-owned contents into the
      // live store.  Node handles keep the PCM allocations intact and do not
      // allocate while unwinding a failed history operation.
      for (arr::SourceId id : state_->source_ids) {
        contents.insert(store.extract(id));
      }
    } else {
      // The history direction is the exact inverse transfer.
      for (arr::SourceId id : state_->source_ids) {
        store.insert(contents.extract(id));
      }
    }
    applied_ = false;
  }

  bool affects_audio_source(arr::SourceId source_id) const noexcept override {
    if (state_ == nullptr) return false;
    return std::find(state_->source_ids.begin(), state_->source_ids.end(), source_id) !=
           state_->source_ids.end();
  }

  bool affects_warp_source(arr::WarpRefId /*warp_id*/) const noexcept override { return false; }

  arr::EditCommandPtr invert(const arr::Project& /*before*/,
                             const arr::MidiContentStore& /*midi_before*/) const override {
    const auto inverse = direction_ == AudioContentTransferDirection::kStore
                             ? AudioContentTransferDirection::kHistory
                             : AudioContentTransferDirection::kStore;
    return std::make_unique<TransferAudioContent>(state_, inverse);
  }

  const char* type_name() const noexcept override { return "TransferAudioContent"; }

  std::size_t retained_bytes() const noexcept override {
    std::size_t total = sizeof(*this);
    if (state_ != nullptr) {
      total = arr::retained::saturating_add(total, retained_audio_transfer_state_bytes(*state_));
    }
    return total;
  }

 private:
  std::shared_ptr<AudioContentTransferState> state_;
  AudioContentTransferDirection direction_;
  bool applied_ = false;
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

// Moves decoded PCM out of AudioContentStore and into the command history.
// This is the removal counterpart to make_store_audio_content_command(): the
// inverse restores the exact map nodes before the source metadata is restored.
arr::EditCommandPtr make_remove_audio_content_command(arr::AudioContentStore* store,
                                                      std::vector<arr::SourceId> source_ids) {
  if (store == nullptr || source_ids.empty()) return nullptr;
  auto state = std::make_shared<AudioContentTransferState>();
  state->store = store;
  state->source_ids = std::move(source_ids);
  return std::make_unique<TransferAudioContent>(std::move(state),
                                                AudioContentTransferDirection::kHistory);
}

// Returns only source ids that become unreferenced when `removed_clip` goes
// away. This deliberately includes take sources as well as the clip's base
// source; a source shared by any other clip remains intact.
std::vector<arr::SourceId> collect_orphaned_sources(const arr::Project& project,
                                                    const arr::EditClip& removed_clip) {
  return arr::vocal_sidecar::collect_orphaned_sources_after_removing_clips(project,
                                                                           {removed_clip.id});
}

std::vector<arr::SourceId> collect_orphaned_sources_for_track(const arr::Project& project,
                                                              arr::TrackId removed_track_id) {
  std::vector<arr::ClipId> removed_clip_ids;
  for (const arr::EditClip& clip : project.clips()) {
    if (clip.track_id != removed_track_id) continue;
    removed_clip_ids.push_back(clip.id);
  }
  return arr::vocal_sidecar::collect_orphaned_sources_after_removing_clips(project,
                                                                           removed_clip_ids);
}

enum class AudioContentReplaceDirection { kSet, kRestore };

struct AudioContentReplaceState {
  arr::AudioContentStore* store = nullptr;
  arr::SourceId source_id = 0;
  // Keep the replacement in a map node so both directions can move it back
  // into AudioContentStore without allocating during rollback.  For an
  // existing source this node holds the samples on the opposite side of the
  // swap; for a new source it is the detached source node itself.
  std::map<arr::SourceId, arr::AudioSourceSamples> contents;
  bool had_existing = false;
  bool initialized = false;
};

std::size_t retained_audio_replace_state_bytes(const AudioContentReplaceState& state) noexcept {
  return arr::retained::saturating_add(sizeof(state), arr::retained::dynamic_bytes(state.contents));
}

class ReplaceAudioContent final : public arr::EditCommand, public arr::EditCommandExternalEffects {
 public:
  ReplaceAudioContent(std::shared_ptr<AudioContentReplaceState> state,
                      AudioContentReplaceDirection direction)
      : state_(std::move(state)), direction_(direction) {}

  bool apply(arr::Project& /*project*/, arr::MidiContentStore& /*midi*/) override {
    // Forward and inverse command objects are replayed many times.  The hook
    // must describe this invocation only, not a previous successful replay.
    applied_ = false;
    if (state_ == nullptr || state_->store == nullptr || state_->source_id == 0) {
      return false;
    }
    auto& sources = state_->store->sources;
    auto it = sources.find(state_->source_id);
    auto contents_it = state_->contents.find(state_->source_id);
    if (direction_ == AudioContentReplaceDirection::kSet) {
      if (contents_it == state_->contents.end()) return false;
      if (!state_->initialized) {
        state_->had_existing = it != sources.end();
        state_->initialized = true;
      }
      if (state_->had_existing) {
        if (it == sources.end()) return false;
        applied_ = true;
        std::swap(it->second, contents_it->second);
      } else {
        if (it != sources.end()) return false;
        applied_ = true;
        sources.insert(state_->contents.extract(contents_it));
      }
      return true;
    }

    if (it == sources.end()) return false;
    if (state_->had_existing) {
      if (contents_it == state_->contents.end()) return false;
      applied_ = true;
      std::swap(it->second, contents_it->second);
    } else {
      if (contents_it != state_->contents.end()) return false;
      applied_ = true;
      state_->contents.insert(sources.extract(it));
    }
    return true;
  }

  void rollback_apply(arr::Project& /*project*/,
                      arr::MidiContentStore& /*midi*/) noexcept override {
    if (!applied_ || state_ == nullptr || state_->store == nullptr) return;

    auto& sources = state_->store->sources;
    auto it = sources.find(state_->source_id);
    auto contents_it = state_->contents.find(state_->source_id);
    if (direction_ == AudioContentReplaceDirection::kSet) {
      if (state_->had_existing) {
        if (it != sources.end() && contents_it != state_->contents.end()) {
          std::swap(it->second, contents_it->second);
        }
      } else if (it != sources.end() && contents_it == state_->contents.end()) {
        // Return the inserted source node to command-owned storage.  Node
        // handles preserve the PCM allocations and require no map allocation.
        state_->contents.insert(sources.extract(it));
      }
    } else {
      if (state_->had_existing) {
        if (it != sources.end() && contents_it != state_->contents.end()) {
          std::swap(it->second, contents_it->second);
        }
      } else if (it == sources.end() && contents_it != state_->contents.end()) {
        // Restore the node detached by the history direction, again without
        // allocating on the failure path.
        sources.insert(state_->contents.extract(contents_it));
      }
    }
    applied_ = false;
  }

  bool affects_audio_source(arr::SourceId source_id) const noexcept override {
    return state_ != nullptr && state_->source_id == source_id;
  }

  bool affects_warp_source(arr::WarpRefId /*warp_id*/) const noexcept override { return false; }

  arr::EditCommandPtr invert(const arr::Project& /*before*/,
                             const arr::MidiContentStore& /*midi_before*/) const override {
    const auto inverse = direction_ == AudioContentReplaceDirection::kSet
                             ? AudioContentReplaceDirection::kRestore
                             : AudioContentReplaceDirection::kSet;
    return std::make_unique<ReplaceAudioContent>(state_, inverse);
  }

  const char* type_name() const noexcept override { return "ReplaceAudioContent"; }

  std::size_t retained_bytes() const noexcept override {
    std::size_t total = sizeof(*this);
    if (state_ != nullptr) {
      total = arr::retained::saturating_add(total, retained_audio_replace_state_bytes(*state_));
    }
    return total;
  }

 private:
  std::shared_ptr<AudioContentReplaceState> state_;
  AudioContentReplaceDirection direction_;
  bool applied_ = false;
};

arr::EditCommandPtr make_replace_audio_content_command(arr::AudioContentStore* store,
                                                       arr::SourceId source_id,
                                                       arr::AudioSourceSamples contents) {
  if (store == nullptr || source_id == 0) return nullptr;
  auto state = std::make_shared<AudioContentReplaceState>();
  state->store = store;
  state->source_id = source_id;
  state->contents.emplace(source_id, std::move(contents));
  return std::make_unique<ReplaceAudioContent>(std::move(state),
                                               AudioContentReplaceDirection::kSet);
}

/// Argument spellings a payload refusal quotes, since the entries name them differently.
struct AudioPayloadNames {
  const char* interleaved;
  const char* frames;
  const char* channels;
  const char* sample_rate;
};

// Runs validate_audio_clip_payload and, on refusal, names the first field it rejects.
SonareError refuse_audio_payload(const char* fn, const AudioPayloadNames& n,
                                 const SonareProjectClipDesc* desc) {
  const SonareError err = validate_audio_clip_payload(desc, nullptr);
  if (err == SONARE_OK) return SONARE_OK;
  const std::string at = std::string(fn) + ": ";
  std::string msg;
  if (!desc->audio_interleaved) {
    msg = at + n.interleaved + " must not be NULL when " + n.frames + " is not 0";
  } else if (desc->audio_frames <= 0) {
    msg = at + n.frames + " must be positive";
  } else if (desc->audio_channels <= 0) {
    msg = at + n.channels + " must be positive";
  } else if (desc->audio_sample_rate < kMinSampleRate || desc->audio_sample_rate > kMaxSampleRate) {
    msg = at + n.sample_rate + " must be between " + std::to_string(kMinSampleRate) + " and " +
          std::to_string(kMaxSampleRate);
  } else if (static_cast<uint64_t>(desc->audio_frames) >
                 std::numeric_limits<size_t>::max() / static_cast<uint64_t>(desc->audio_channels) ||
             static_cast<uint64_t>(desc->audio_frames) *
                     static_cast<uint64_t>(desc->audio_channels) >
                 kMaxBufferSize) {
    msg = at + n.frames + " times " + n.channels + " exceeds the maximum of " +
          std::to_string(kMaxBufferSize) + " samples";
  } else {
    const auto total =
        static_cast<size_t>(desc->audio_frames) * static_cast<size_t>(desc->audio_channels);
    size_t index = 0;
    while (index < total && std::isfinite(desc->audio_interleaved[index])) ++index;
    msg = at + n.interleaved + " contains a non-finite sample at index " + std::to_string(index);
  }
  set_last_error(err, msg.c_str());
  return err;
}

bool valid_next_id(uint32_t id) noexcept {
  return id != 0 && id != std::numeric_limits<uint32_t>::max();
}

// Validates a fade desc and copies it into an arrangement ClipFade. Returns
// SONARE_OK on success; the fade length must be finite and >= 0 and the curve
// ordinal in range.
SonareError clip_fade_from_desc(const char* fn, const char* arg, const SonareProjectClipFade* desc,
                                arr::ClipFade* out) {
  const std::string name = std::string(fn) + ": " + arg;
  if (desc == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, (name + " must not be NULL").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (out == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, (name + ": output must not be NULL").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(desc->length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   (name + "->length_ppq must be finite and not negative").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!ordinal_to_enum(desc->curve, 0, static_cast<int>(arr::FadeCurve::kLogarithmic),
                       &out->curve)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   (name + "->curve is not a fade curve: " + std::to_string(desc->curve)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->length_ppq = desc->length_ppq;
  return SONARE_OK;
}

SonareError clip_takes_from_desc(const char* fn, const SonareProjectClipTake* takes,
                                 size_t take_count, std::vector<arr::ClipTake>* out) {
  const std::string name(fn);
  if (!out) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, (name + ": output must not be NULL").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (take_count > 0 && takes == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   (name + ": takes must not be NULL when take_count is not 0").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (take_count > kMaxBufferSize) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        (name + ": take_count exceeds the maximum of " + std::to_string(kMaxBufferSize)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->clear();
  out->reserve(take_count);
  for (size_t i = 0; i < take_count; ++i) {
    const SonareProjectClipTake& take = takes[i];
    if (take.id == 0) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (name + ": takes[" + std::to_string(i) + "].id must not be 0").c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!finite_non_negative(take.source_offset_ppq)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (name + ": takes[" + std::to_string(i) +
                      "].source_offset_ppq must be finite and not negative")
                         .c_str());
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

SonareError clip_comp_segments_from_desc(const char* fn,
                                         const SonareProjectClipCompSegment* segments,
                                         size_t segment_count,
                                         std::vector<arr::ClipCompSegment>* out) {
  const std::string name(fn);
  if (!out) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, (name + ": output must not be NULL").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (segment_count > 0 && segments == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   (name + ": segments must not be NULL when segment_count is not 0").c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (segment_count > kMaxBufferSize) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        (name + ": segment_count exceeds the maximum of " + std::to_string(kMaxBufferSize))
            .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  out->clear();
  out->reserve(segment_count);
  for (size_t i = 0; i < segment_count; ++i) {
    const SonareProjectClipCompSegment& segment = segments[i];
    const std::string at = name + ": segments[" + std::to_string(i) + "].";
    if (!finite_non_negative(segment.start_ppq)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (at + "start_ppq must be finite and not negative").c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!finite_positive(segment.end_ppq)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (at + "end_ppq must be finite and greater than 0").c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!(segment.end_ppq > segment.start_ppq)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (at + "end_ppq must be greater than start_ppq").c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!finite_non_negative(segment.crossfade_ppq)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     (at + "crossfade_ppq must be finite and not negative").c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    // The bounds a crossfade has against its neighbours need the whole lane, so
    // they are the edit command's to check; this rejects only the value itself.
    out->push_back({segment.start_ppq, segment.end_ppq, segment.take_id, segment.crossfade_ppq});
  }
  return SONARE_OK;
}

}  // namespace

arr::EditCommandPtr sonare_project_make_remove_audio_content_command(
    arr::AudioContentStore* store, std::vector<arr::SourceId> source_ids) {
  return make_remove_audio_content_command(store, std::move(source_ids));
}

arr::EditCommandPtr sonare_project_make_store_audio_content_command(
    arr::AudioContentStore* store, std::map<arr::SourceId, arr::AudioSourceSamples> contents) {
  return make_store_audio_content_command(store, std::move(contents));
}

std::vector<arr::SourceId> sonare_project_collect_orphaned_sources_for_track(
    const arr::Project& project, arr::TrackId removed_track_id) {
  return collect_orphaned_sources_for_track(project, removed_track_id);
}

#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// Edit
// ============================================================================

SonareError sonare_project_add_clip(SonareProject* project, const SonareProjectClipDesc* desc,
                                    uint32_t* out_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_add_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!desc) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_add_clip: desc must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_clip_id) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: out_clip_id must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->track_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: desc->track_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_positive(desc->length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: desc->length_ppq must be finite and greater than 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(desc->start_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: desc->start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(desc->source_offset_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: desc->source_offset_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(desc->gain) || desc->gain < 0.0f) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_clip: desc->gain must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_track(desc->track_id)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_add_clip: unknown desc->track_id " + std::to_string(desc->track_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const SonareError audio_err =
      desc->is_midi == 0 ? refuse_audio_payload("project_add_clip",
                                                {"desc->audio_interleaved", "desc->audio_frames",
                                                 "desc->audio_channels", "desc->audio_sample_rate"},
                                                desc)
                         : SONARE_OK;
  if (audio_err != SONARE_OK) return audio_err;
  SONARE_C_TRY
  const arr::SourceId source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  if (!valid_next_id(source_id) || !valid_next_id(clip_id)) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_add_clip: the project has no source or clip id left to assign");
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
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_add_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = clip_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_clip_id) *out_clip_id = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, desc, out_clip_id);
#endif
}

SonareError sonare_project_set_source_audio(SonareProject* project, uint32_t source_id,
                                            const float* interleaved, int64_t frames, int channels,
                                            int sample_rate) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_source_audio: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (source_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_source_audio: source_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareProjectClipDesc desc{};
  desc.audio_interleaved = interleaved;
  desc.audio_frames = frames;
  desc.audio_channels = channels;
  desc.audio_sample_rate = sample_rate;
  if (refuse_audio_payload("project_set_source_audio",
                           {"interleaved", "frames", "channels", "sample_rate"},
                           &desc) != SONARE_OK) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::ClipSource* source = project->history.project().find_source(source_id);
  const auto* audio = source ? std::get_if<arr::AudioSourceRef>(source) : nullptr;
  if (source == nullptr) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_source_audio: unknown source_id " + std::to_string(source_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (audio == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_source_audio: source_id " + std::to_string(source_id) +
                    " is not an audio source")
                       .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::AudioSourceSamples contents;
  contents.sample_rate = static_cast<double>(sample_rate);
  contents.channels = deinterleave(interleaved, frames, channels);
  auto content =
      make_replace_audio_content_command(&project->audio, source_id, std::move(contents));
  if (!content) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_set_source_audio: the audio content edit could not be built");
    return SONARE_ERROR_INVALID_STATE;
  }
  // The source's channel and rate hints describe the content, so they move with
  // it in the same undoable step.
  arr::AudioSourceRef replacement = *audio;
  replacement.channel_count = static_cast<uint32_t>(channels);
  replacement.sample_rate_hint = static_cast<double>(sample_rate);
  std::vector<arr::EditCommandPtr> commands;
  commands.push_back(std::move(content));
  commands.push_back(
      std::make_unique<arr::ReplaceSource>(source_id, arr::ClipSource{std::move(replacement)}));
  if (!project->history.apply_transaction(std::move(commands))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_set_source_audio: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, source_id, interleaved, frames, channels, sample_rate);
#endif
}

SonareError sonare_project_set_audio_source_metadata(SonareProject* project, uint32_t source_id,
                                                     const char* content_hash,
                                                     const char* external_stem_role) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_audio_source_metadata: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (source_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_audio_source_metadata: source_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!content_hash) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_audio_source_metadata: content_hash must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!external_stem_role) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_audio_source_metadata: external_stem_role must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::ClipSource* source = project->history.project().find_source(source_id);
  const auto* audio = source ? std::get_if<arr::AudioSourceRef>(source) : nullptr;
  if (source == nullptr) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_audio_source_metadata: unknown source_id " + std::to_string(source_id))
            .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (audio == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_audio_source_metadata: source_id " + std::to_string(source_id) +
                    " is not an audio source")
                       .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Copy the complete source first so changing these two metadata fields can
  // never discard URI, channel/rate hints, storage handles, or future fields.
  arr::AudioSourceRef replacement = *audio;
  replacement.content_hash = content_hash;
  replacement.external_stem_role = external_stem_role;
  auto command =
      std::make_unique<arr::ReplaceSource>(source_id, arr::ClipSource{std::move(replacement)});
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_set_audio_source_metadata: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, source_id, content_hash, external_stem_role);
#endif
}

SonareError sonare_project_add_loop_recording_takes(SonareProject* project,
                                                    const SonareProjectLoopRecordingDesc* desc,
                                                    uint32_t* out_clip_id, size_t* out_take_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_clip_id) *out_clip_id = 0;
  if (out_take_count) *out_take_count = 0;
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!desc) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_clip_id) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: out_clip_id must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if ((desc->flags & ~SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL) != 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->flags has unknown bits set");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->track_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->track_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(desc->start_ppq)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        "project_add_loop_recording_takes: desc->start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_positive(desc->loop_length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->loop_length_ppq must be finite and "
                   "greater than 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!desc->audio_interleaved) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_interleaved must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->audio_frames <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_frames must be positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->audio_channels <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_channels must be positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (desc->audio_sample_rate < kMinSampleRate || desc->audio_sample_rate > kMaxSampleRate) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_add_loop_recording_takes: desc->audio_sample_rate must be between " +
                    std::to_string(kMinSampleRate) + " and " + std::to_string(kMaxSampleRate))
                       .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const auto total_frames = static_cast<uint64_t>(desc->audio_frames);
  const auto channels = static_cast<uint64_t>(desc->audio_channels);
  const std::string too_large =
      "project_add_loop_recording_takes: desc->audio_frames times desc->audio_channels exceeds "
      "the maximum of " +
      std::to_string(kMaxBufferSize) + " samples";
  if (channels == 0 || total_frames > std::numeric_limits<size_t>::max() / channels) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, too_large.c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t total_samples = static_cast<size_t>(total_frames * channels);
  if (total_samples == 0 || total_samples > kMaxBufferSize) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, too_large.c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < total_samples; ++i) {
    if (!std::isfinite(desc->audio_interleaved[i])) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("project_add_loop_recording_takes: desc->audio_interleaved contains a "
                      "non-finite sample at index " +
                      std::to_string(i))
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  const arr::Track* track = project->history.project().find_track(desc->track_id);
  if (track == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_add_loop_recording_takes: unknown desc->track_id " +
                    std::to_string(desc->track_id))
                       .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (track->kind != arr::Track::Kind::kAudio) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_add_loop_recording_takes: desc->track_id " +
                    std::to_string(desc->track_id) + " is not an audio track")
                       .c_str());
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
  if (loop_project_samples <= 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->loop_length_ppq is shorter than one "
                   "project sample");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!(project->history.project().sample_rate() > 0.0)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: the project sample rate is not positive");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const double loop_audio_frames_f =
      static_cast<double>(loop_project_samples) *
      (static_cast<double>(desc->audio_sample_rate) / project->history.project().sample_rate());
  if (!(loop_audio_frames_f > 0.0) || !std::isfinite(loop_audio_frames_f)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->loop_length_ppq does not give a finite "
                   "frame count at desc->audio_sample_rate");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const int64_t loop_audio_frames = std::max<int64_t>(1, std::llround(loop_audio_frames_f));
  const int64_t full_loops = desc->audio_frames / loop_audio_frames;
  const int64_t remainder = desc->audio_frames - full_loops * loop_audio_frames;
  // A remainder of one frame or less is rounding noise once a full loop exists.
  const bool has_tail = remainder > (full_loops > 0 ? 1 : 0);
  const bool tail_complete = has_tail && loop_audio_frames - remainder <= 1;
  const bool tail_partial = has_tail && !tail_complete;
  const bool drop_tail = (desc->flags & SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL) != 0;
  const bool make_tail = has_tail && !(tail_partial && drop_tail);
  const size_t take_count = static_cast<size_t>(full_loops) + (make_tail ? 1 : 0);
  if (take_count == 0 && tail_partial && drop_tail) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_frames is shorter than one loop "
                   "and desc->flags drops the partial tail");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (take_count == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_frames yields no take");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (take_count > kMaxBufferSize ||
      take_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_loop_recording_takes: desc->audio_frames yields more takes than "
                   "the supported maximum");
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const arr::SourceId first_source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  const auto reserved_id = std::numeric_limits<uint32_t>::max();
  if (!valid_next_id(first_source_id) || !valid_next_id(clip_id) ||
      take_count > static_cast<size_t>(reserved_id - first_source_id)) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_add_loop_recording_takes: the project has too few free source or clip "
                   "ids left for the takes");
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
    set_last_error(
        SONARE_ERROR_INVALID_STATE,
        "project_add_loop_recording_takes: no take could be built from desc->audio_frames");
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
    const bool is_partial_last = tail_partial && make_tail && i + 1 == source_ids.size();
    clip.takes.back().partial = is_partial_last;
    // A partial last take stays inactive while an earlier complete take exists.
    if (!(is_partial_last && i > 0)) clip.active_take_id = take_id;
  }

  auto command = std::make_unique<arr::AddClip>(clip);
  commands.push_back(std::move(command));
  commands.push_back(make_store_audio_content_command(&project->audio, std::move(audio_contents)));
  if (!project->history.apply_transaction(std::move(commands))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_add_loop_recording_takes: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_clip_id = clip_id;
  if (out_take_count) *out_take_count = source_ids.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_clip_id) *out_clip_id = {};
  if (out_take_count) *out_take_count = {};
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_midi_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_track_id) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_midi_clip: out_track_id must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!out_clip_id) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_midi_clip: out_clip_id must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(start_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_midi_clip: start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_positive(length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_add_midi_clip: length_ppq must be finite and greater than 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const arr::TrackId track_id = project->history.project().next_track_id();
  const arr::SourceId source_id = project->history.project().next_source_id();
  const arr::ClipId clip_id = project->history.project().next_clip_id();
  if (!valid_next_id(track_id) || !valid_next_id(source_id) || !valid_next_id(clip_id)) {
    set_last_error(
        SONARE_ERROR_INVALID_STATE,
        "project_add_midi_clip: the project has no track, source or clip id left to assign");
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
  clip.gain = 1.0f;
  auto add_clip = std::make_unique<arr::AddClip>(std::move(clip));
  commands.push_back(std::move(add_clip));

  if (!project->history.apply_transaction(std::move(commands))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_add_midi_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  *out_track_id = track_id;
  *out_clip_id = clip_id;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_clip_id) *out_clip_id = {};
  if (out_track_id) *out_track_id = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, start_ppq, length_ppq, out_track_id, out_clip_id);
#endif
}

SonareError sonare_project_split_clip(SonareProject* project, uint32_t clip_id, double split_ppq,
                                      uint32_t* out_new_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_new_clip_id) *out_new_clip_id = 0;
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_split_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_split_clip: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(split_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_split_clip: split_ppq must be finite");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_split_clip: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SplitClip>(clip_id, split_ppq);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_split_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  if (out_new_clip_id) {
    if (project->history.project().clips().empty()) {
      set_last_error(SONARE_ERROR_INVALID_STATE,
                     "project_split_clip: the project holds no clip after the split");
      return SONARE_ERROR_INVALID_STATE;
    }
    *out_new_clip_id = project->history.project().clips().back().id;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_new_clip_id) *out_new_clip_id = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, split_ppq, out_new_clip_id);
#endif
}

SonareError sonare_project_trim_clip(SonareProject* project, uint32_t clip_id, double new_start_ppq,
                                     double new_length_ppq) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_trim_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_trim_clip: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(new_start_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_trim_clip: new_start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_positive(new_length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_trim_clip: new_length_ppq must be finite and greater than 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_trim_clip: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::TrimClip>(clip_id, new_start_ppq, new_length_ppq);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_trim_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_move_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_move_clip: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(new_start_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_move_clip: new_start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_move_clip: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (new_track_id != 0 && !project->history.project().has_track(new_track_id)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_move_clip: unknown new_track_id " + std::to_string(new_track_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::MoveClip>(clip_id, new_start_ppq, new_track_id);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_move_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_warp_ref: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_warp_ref: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_warp_ref: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // A non-zero id must name a warp map already registered on the project;
  // an unknown id is a caller mistake (like an unknown clip_id), not a state
  // conflict. 0 stays legal and clears the clip's warp reference.
  if (warp_ref_id != 0 && !project->history.project().has_warp_map(warp_ref_id)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_warp_ref: unknown warp_ref_id " + std::to_string(warp_ref_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipWarpRef>(clip_id, warp_ref_id);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_set_clip_warp_ref: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_warp_mode: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_warp_mode: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (mode != SONARE_PROJECT_WARP_MODE_OFF && mode != SONARE_PROJECT_WARP_MODE_REPITCH &&
      mode != SONARE_PROJECT_WARP_MODE_TEMPO_SYNC &&
      mode != SONARE_PROJECT_WARP_MODE_TIME_STRETCH) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_warp_mode: mode is not a warp mode: " +
                    std::to_string(static_cast<int>(mode)))
                       .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_warp_mode: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::WarpMode arr_mode = arr::WarpMode::kOff;
  if (mode == SONARE_PROJECT_WARP_MODE_REPITCH) {
    arr_mode = arr::WarpMode::kRepitch;
  } else if (mode == SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) {
    arr_mode = arr::WarpMode::kTempoSync;
  } else if (mode == SONARE_PROJECT_WARP_MODE_TIME_STRETCH) {
    arr_mode = arr::WarpMode::kTimeStretch;
  }
  auto command = std::make_unique<arr::SetClipWarpMode>(clip_id, arr_mode);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE,
                   "project_set_clip_warp_mode: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_takes: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_set_clip_takes: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_takes: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<arr::ClipTake> arr_takes;
  const SonareError err =
      clip_takes_from_desc("project_set_clip_takes", takes, take_count, &arr_takes);
  if (err != SONARE_OK) return err;
  for (size_t i = 0; i < arr_takes.size(); ++i) {
    const arr::SourceId take_source = arr_takes[i].source_id;
    if (take_source != 0 && !project->history.project().has_source(take_source)) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("project_set_clip_takes: unknown source_id " + std::to_string(take_source) +
                      " in takes[" + std::to_string(i) + "]")
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  const bool active_take_known =
      active_take_id == 0 ||
      std::any_of(arr_takes.begin(), arr_takes.end(),
                  [&](const arr::ClipTake& t) { return t.id == active_take_id; });
  if (!active_take_known) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_takes: unknown active_take_id " + std::to_string(active_take_id))
            .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto command = std::make_unique<arr::SetClipTakes>(clip_id, std::move(arr_takes), active_take_id);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_takes: the takes were not applied to the clip");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_comp_segments: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_comp_segments: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::EditClip* target_clip = project->history.project().find_clip(clip_id);
  if (target_clip == nullptr) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_comp_segments: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  std::vector<arr::ClipCompSegment> arr_segments;
  const SonareError err = clip_comp_segments_from_desc("project_set_clip_comp_segments", segments,
                                                       segment_count, &arr_segments);
  if (err != SONARE_OK) return err;
  for (size_t i = 0; i < arr_segments.size(); ++i) {
    const arr::TakeId take_id = arr_segments[i].take_id;
    const bool known =
        take_id == 0 || std::any_of(target_clip->takes.begin(), target_clip->takes.end(),
                                    [&](const arr::ClipTake& t) { return t.id == take_id; });
    if (!known) {
      set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                     ("project_set_clip_comp_segments: unknown take_id " + std::to_string(take_id) +
                      " in segments[" + std::to_string(i) + "]")
                         .c_str());
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  auto command = std::make_unique<arr::SetClipCompSegments>(clip_id, std::move(arr_segments));
  if (!project->history.apply(std::move(command))) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        "project_set_clip_comp_segments: the comp segments were not applied to the clip");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, segments, segment_count);
#endif
}

SonareError sonare_project_remove_clip(SonareProject* project, uint32_t clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_remove_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_remove_clip: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const arr::Project& edit_project = project->history.project();
  const arr::EditClip* clip = edit_project.find_clip(clip_id);
  if (clip == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_remove_clip: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  const std::vector<arr::SourceId> orphaned_sources = collect_orphaned_sources(edit_project, *clip);
  std::vector<arr::SourceId> orphaned_audio_contents;
  std::vector<arr::EditCommandPtr> commands;
  commands.reserve(1 + orphaned_sources.size() + 1);
  commands.push_back(std::make_unique<arr::RemoveClip>(clip_id));
  for (const arr::SourceId source_id : orphaned_sources) {
    commands.push_back(std::make_unique<arr::RemoveSourceInternal>(source_id));
    if (project->audio.sources.find(source_id) != project->audio.sources.end()) {
      orphaned_audio_contents.push_back(source_id);
    }
  }
  if (!orphaned_audio_contents.empty()) {
    commands.push_back(
        make_remove_audio_content_command(&project->audio, std::move(orphaned_audio_contents)));
  }
  if (!project->history.apply_transaction(std::move(commands))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_remove_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id);
#endif
}

SonareError sonare_project_set_clip_gain(SonareProject* project, uint32_t clip_id, float gain) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_gain: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_set_clip_gain: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(gain) || gain < 0.0f) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_gain: gain must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_gain: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipGain>(clip_id, gain);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_set_clip_gain: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_fade: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_set_clip_fade: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!fade_in) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_fade: fade_in must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!fade_out) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_fade: fade_out must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_fade: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  arr::ClipFade in_fade;
  arr::ClipFade out_fade;
  SonareError err = clip_fade_from_desc("project_set_clip_fade", "fade_in", fade_in, &in_fade);
  if (err != SONARE_OK) return err;
  err = clip_fade_from_desc("project_set_clip_fade", "fade_out", fade_out, &out_fade);
  if (err != SONARE_OK) return err;
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipFade>(clip_id, in_fade, out_fade);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_set_clip_fade: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  arr::LoopMode mode{};
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_loop: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_set_clip_loop: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!ordinal_to_enum(loop_mode, SONARE_LOOP_MODE_OFF, SONARE_LOOP_MODE_LOOP, &mode)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_loop: loop_mode is not a loop mode: " + std::to_string(loop_mode))
            .c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Under LOOP, a length of 0 means "loop the entire clip" (resolved from the
  // clip's own duration at compile time); any other mode also permits 0.
  if (!finite_non_negative(loop_length_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_loop: loop_length_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(loop_crossfade_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_loop: loop_crossfade_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_loop: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command =
      std::make_unique<arr::SetClipLoop>(clip_id, mode, loop_length_ppq, loop_crossfade_ppq);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_set_clip_loop: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_source: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_source: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (source_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_set_clip_source: source_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_set_clip_source: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!project->history.project().has_source(source_id)) {
    set_last_error(
        SONARE_ERROR_INVALID_PARAMETER,
        ("project_set_clip_source: unknown source_id " + std::to_string(source_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::SetClipSource>(clip_id, source_id);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_set_clip_source: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
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
  if (!project) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_duplicate_clip: project must not be NULL");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (clip_id == 0) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "project_duplicate_clip: clip_id must not be 0");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!finite_non_negative(new_start_ppq)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "project_duplicate_clip: new_start_ppq must be finite and not negative");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (project->history.project().find_clip(clip_id) == nullptr) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   ("project_duplicate_clip: unknown clip_id " + std::to_string(clip_id)).c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  auto command = std::make_unique<arr::DuplicateClip>(clip_id, new_start_ppq);
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "project_duplicate_clip: the edit was not applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  if (out_new_clip_id) {
    if (project->history.project().clips().empty()) {
      set_last_error(SONARE_ERROR_INVALID_STATE,
                     "project_duplicate_clip: the project holds no clip after the duplicate");
      return SONARE_ERROR_INVALID_STATE;
    }
    *out_new_clip_id = project->history.project().clips().back().id;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_new_clip_id) *out_new_clip_id = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, clip_id, new_start_ppq, out_new_clip_id);
#endif
}
