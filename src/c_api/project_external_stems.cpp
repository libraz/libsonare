#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#include "arrangement/external_stems.h"

namespace {

size_t channel_count(uint32_t layout) noexcept {
  switch (layout) {
    case SONARE_EXTERNAL_STEM_MONO:
      return 1;
    case SONARE_EXTERNAL_STEM_STEREO:
      return 2;
    default:
      return 0;
  }
}

SonareError import_error_to_c(sonare::arrangement::ExternalSeparatedStemImportError error) {
  using Error = sonare::arrangement::ExternalSeparatedStemImportError;
  switch (error) {
    case Error::kOk:
      return SONARE_OK;
    case Error::kProjectMutationFailed:
      return SONARE_ERROR_INVALID_STATE;
    case Error::kInvalidArgument:
    case Error::kSampleRateMismatch:
    case Error::kFramePositionNotRepresentable:
      return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_ERROR_UNKNOWN;
}

// PCM lives alongside EditHistory rather than inside Project, so a regular
// arrangement command alone cannot restore an external-stem import. Keep one
// shared state object for the staged Project and only the newly imported PCM.
// The Project slot swaps between the live and staged values; the PCM map moves
// through std::map node handles. Undo therefore retains the exact source nodes,
// while discarding a redo branch destroys detached samples instead of leaving
// an orphaned AudioContentStore entry.
struct ExternalStemImportState {
  arr::AudioContentStore* audio = nullptr;
  arr::Project staged_project;
  std::vector<arr::SourceId> source_ids;
  std::map<arr::SourceId, arr::AudioSourceSamples> detached_audio;
  bool audio_in_store = false;

  // All callers preflight the complete id set before the first node move.
  // Consequently each branch below is a sequence of non-allocating map-node
  // transfers followed by a value swap, and is safe to use from the history's
  // noexcept rollback path.
  bool toggle(arr::Project& project) noexcept {
    if (audio == nullptr || source_ids.empty()) return false;
    auto& store = audio->sources;
    if (!audio_in_store) {
      if (detached_audio.size() != source_ids.size()) return false;
      for (arr::SourceId source_id : source_ids) {
        if (detached_audio.find(source_id) == detached_audio.end() ||
            store.find(source_id) != store.end()) {
          return false;
        }
      }
      for (arr::SourceId source_id : source_ids) {
        const auto inserted = store.insert(detached_audio.extract(source_id));
        if (!inserted.inserted) return false;
      }
      std::swap(project, staged_project);
      audio_in_store = true;
      return true;
    }

    if (!detached_audio.empty()) return false;
    for (arr::SourceId source_id : source_ids) {
      if (store.find(source_id) == store.end()) return false;
    }
    for (arr::SourceId source_id : source_ids) {
      const auto inserted = detached_audio.insert(store.extract(source_id));
      if (!inserted.inserted) return false;
    }
    std::swap(project, staged_project);
    audio_in_store = false;
    return true;
  }
};

std::size_t retained_external_stem_state_bytes(const ExternalStemImportState& state) noexcept {
  std::size_t total = sizeof(state);
  total = arr::retained::saturating_add(total, arr::retained::bytes(state.staged_project));
  total = arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.source_ids));
  return arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.detached_audio));
}

class ExternalStemImportCommand final : public arr::EditCommand, public arr::EditCommandRollback {
 public:
  explicit ExternalStemImportCommand(std::shared_ptr<ExternalStemImportState> state)
      : state_(std::move(state)) {}

  bool apply(arr::Project& project, arr::MidiContentStore&) override {
    if (state_ == nullptr || state_->audio == nullptr || state_->source_ids.empty()) {
      return false;
    }
    applied_ = false;
    applied_ = state_->toggle(project);
    return applied_;
  }

  void rollback_apply(arr::Project& project, arr::MidiContentStore&) noexcept override {
    if (!applied_ || state_ == nullptr) return;
    // The command's preflight guarantees this cannot fail.  Keep the hook
    // noexcept so it remains usable while unwinding a bad_alloc from invert().
    (void)state_->toggle(project);
    applied_ = false;
  }

  arr::EditCommandPtr invert(const arr::Project&, const arr::MidiContentStore&) const override {
    if (state_ == nullptr) return nullptr;
    return std::make_unique<ExternalStemImportCommand>(state_);
  }

  const char* type_name() const noexcept override { return "ImportExternalStems"; }
  bool mutates_midi_store() const noexcept override { return false; }

  std::size_t retained_bytes() const noexcept override {
    std::size_t total = sizeof(*this);
    if (state_ != nullptr) {
      total = arr::retained::saturating_add(total, retained_external_stem_state_bytes(*state_));
    }
    return total;
  }

 private:
  std::shared_ptr<ExternalStemImportState> state_;
  bool applied_ = false;
};

}  // namespace

#endif

SonareError sonare_project_import_external_stems(SonareProject* project,
                                                 const SonareExternalStemImportRequest* request,
                                                 SonareExternalStemImportResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out != nullptr) *out = {};
  if (project == nullptr || request == nullptr || out == nullptr ||
      (request->stem_count > 0 && request->stems == nullptr) || request->stem_count == 0 ||
      request->stem_count > sonare::resource::kMaxOfflineAudioSamples ||
      (request->struct_version != 0 && request->struct_version != 1)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::arrangement::ExternalSeparatedStemSet input;
  input.sample_rate = request->sample_rate;
  input.stems.reserve(request->stem_count);
  size_t total_samples = 0;
  for (size_t i = 0; i < request->stem_count; ++i) {
    const SonareExternalStemDesc& desc = request->stems[i];
    const size_t channels = channel_count(desc.layout);
    if (desc.name == nullptr || desc.planar_samples == nullptr || desc.frame_count <= 0 ||
        channels == 0) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    const size_t frames = static_cast<size_t>(desc.frame_count);
    if (frames > sonare::resource::kMaxOfflineAudioSamples / channels ||
        total_samples > sonare::resource::kMaxOfflineAudioSamples - frames * channels) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    sonare::arrangement::ExternalSeparatedStem stem;
    stem.name = desc.name;
    if (desc.role != nullptr) stem.role = std::string(desc.role);
    stem.layout = static_cast<sonare::arrangement::ExternalSeparatedStemLayout>(desc.layout);
    stem.start_frame = desc.start_frame;
    stem.planar_samples.reserve(channels);
    for (size_t channel = 0; channel < channels; ++channel) {
      if (desc.planar_samples[channel] == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
      stem.planar_samples.emplace_back(desc.planar_samples[channel],
                                       desc.planar_samples[channel] + frames);
    }
    total_samples += frames * channels;
    input.stems.push_back(std::move(stem));
  }

  // Preserve the public import error taxonomy before handing the mutation to
  // EditHistory: its bool-only command API cannot distinguish malformed host
  // input from a failed state transition. Stage the Project and newly-created
  // PCM only; the live audio map is deliberately never copied.
  arr::Project validation_project = project->history.project();
  arr::AudioContentStore validation_audio;
  const auto validation =
      arr::import_external_separated_stems(&validation_project, &validation_audio, input);
  const SonareError validation_error = import_error_to_c(validation.error);
  if (validation_error != SONARE_OK) return validation_error;

  // The project allocator and the sidecar map normally advance together, but
  // callers can construct an inconsistent store through the C++ seam. Reject
  // an id collision before history sees the command so no history entry or
  // live state is changed.
  for (const auto& [source_id, samples] : validation_audio.sources) {
    (void)samples;
    if (project->audio.sources.find(source_id) != project->audio.sources.end()) {
      return SONARE_ERROR_INVALID_STATE;
    }
  }

  // Marshal the result before committing history. Once apply() succeeds the
  // C ABI path performs no allocations; a caller therefore cannot observe a
  // successful structural edit followed by an output-buffer failure.
  auto track_ids = std::make_unique<uint32_t[]>(validation.track_ids.size());
  auto clip_ids = std::make_unique<uint32_t[]>(validation.clip_ids.size());
  std::copy(validation.track_ids.begin(), validation.track_ids.end(), track_ids.get());
  std::copy(validation.clip_ids.begin(), validation.clip_ids.end(), clip_ids.get());

  auto state = std::make_shared<ExternalStemImportState>();
  state->audio = &project->audio;
  state->staged_project = std::move(validation_project);
  state->detached_audio = std::move(validation_audio.sources);
  state->source_ids.reserve(state->detached_audio.size());
  for (const auto& [source_id, samples] : state->detached_audio) {
    (void)samples;
    state->source_ids.push_back(source_id);
  }
  if (state->source_ids.empty() ||
      !project->history.apply(std::make_unique<ExternalStemImportCommand>(state))) {
    return SONARE_ERROR_INVALID_STATE;
  }

  out->count = validation.track_ids.size();
  out->track_ids = track_ids.release();
  out->clip_ids = clip_ids.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, request, out);
#endif
}

void sonare_free_external_stem_import_result(SonareExternalStemImportResult* result) {
  if (result == nullptr) return;
  delete[] result->track_ids;
  delete[] result->clip_ids;
  *result = {};
}
