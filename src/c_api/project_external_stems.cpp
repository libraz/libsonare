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

  const auto result = sonare::arrangement::import_external_separated_stems(
      &project->history.project(), &project->audio, input);
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
