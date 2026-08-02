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
// arrangement command alone cannot restore an external-stem import. Keep the
// project and PCM snapshots together in one history command; apply_transaction
// then gives the public import one atomic undo/redo entry and clears stale redo.
class RestoreExternalStemImport final : public arr::EditCommand {
 public:
  RestoreExternalStemImport(arr::AudioContentStore* audio, arr::Project project_snapshot,
                            arr::AudioContentStore audio_snapshot)
      : audio_(audio),
        project_snapshot_(std::move(project_snapshot)),
        audio_snapshot_(std::move(audio_snapshot)) {}

  bool apply(arr::Project& project, arr::MidiContentStore&) override {
    if (audio_ == nullptr) return false;
    project = project_snapshot_;
    *audio_ = audio_snapshot_;
    return true;
  }

  arr::EditCommandPtr invert(const arr::Project& before,
                             const arr::MidiContentStore&) const override {
    // Restore commands are created only as the inverse owned by an import
    // entry; EditHistory reuses that entry's forward command for redo. Keep a
    // conservative self-inverse for callers that use this command directly.
    return std::make_unique<RestoreExternalStemImport>(audio_, before, audio_snapshot_);
  }

  const char* type_name() const noexcept override { return "RestoreExternalStemImport"; }
  bool mutates_midi_store() const noexcept override { return false; }

 private:
  arr::AudioContentStore* audio_ = nullptr;
  arr::Project project_snapshot_;
  arr::AudioContentStore audio_snapshot_;
};

class ImportExternalStems final : public arr::EditCommand {
 public:
  ImportExternalStems(arr::AudioContentStore* audio, arr::ExternalSeparatedStemSet input)
      : audio_(audio), input_(std::move(input)) {}

  bool apply(arr::Project& project, arr::MidiContentStore&) override {
    if (audio_ == nullptr) return false;
    if (!captured_before_) {
      project_before_ = project;
      audio_before_ = *audio_;
      captured_before_ = true;
    }
    result_ = arr::import_external_separated_stems(&project, audio_, input_);
    return result_.ok();
  }

  arr::EditCommandPtr invert(const arr::Project& before,
                             const arr::MidiContentStore&) const override {
    if (!captured_before_) return nullptr;
    // `before` is EditHistory's authoritative project snapshot; audio_before_
    // was captured at the same control-thread boundary before the import.
    return std::make_unique<RestoreExternalStemImport>(audio_, before, audio_before_);
  }

  const char* type_name() const noexcept override { return "ImportExternalStems"; }
  bool mutates_midi_store() const noexcept override { return false; }

  const arr::ExternalSeparatedStemImportResult& result() const noexcept { return result_; }

 private:
  arr::AudioContentStore* audio_ = nullptr;
  arr::ExternalSeparatedStemSet input_;
  bool captured_before_ = false;
  arr::Project project_before_;
  arr::AudioContentStore audio_before_;
  arr::ExternalSeparatedStemImportResult result_;
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
  // EditHistory: its bool-only transaction API intentionally cannot distinguish
  // malformed host input from a failed state transition. The staged probe is
  // control-thread-only and leaves the live project/PCM store untouched.
  arr::Project validation_project = project->history.project();
  arr::AudioContentStore validation_audio = project->audio;
  const auto validation =
      arr::import_external_separated_stems(&validation_project, &validation_audio, input);
  const SonareError validation_error = import_error_to_c(validation.error);
  if (validation_error != SONARE_OK) return validation_error;

  auto command = std::make_unique<ImportExternalStems>(&project->audio, std::move(input));
  ImportExternalStems* import = command.get();
  std::vector<arr::EditCommandPtr> commands;
  commands.push_back(std::move(command));
  if (!project->history.apply_transaction(std::move(commands))) {
    return SONARE_ERROR_INVALID_STATE;
  }
  const auto& result = import->result();
  const SonareError error = import_error_to_c(result.error);
  if (error != SONARE_OK) return error;
  auto track_ids = std::make_unique<uint32_t[]>(result.track_ids.size());
  auto clip_ids = std::make_unique<uint32_t[]>(result.clip_ids.size());
  std::copy(result.track_ids.begin(), result.track_ids.end(), track_ids.get());
  std::copy(result.clip_ids.begin(), result.clip_ids.end(), clip_ids.get());
  out->count = result.track_ids.size();
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
