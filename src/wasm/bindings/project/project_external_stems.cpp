/// @file project_external_stems.cpp
/// @brief embind request-object bridge for host-provided separated PCM stems.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

size_t channel_count(uint32_t layout) {
  if (layout == SONARE_EXTERNAL_STEM_MONO) return 1;
  if (layout == SONARE_EXTERNAL_STEM_STEREO) return 2;
  return 0;
}

}  // namespace

val ProjectWasm::importExternalStems(val request) {
  if (request.isUndefined() || request.isNull() || !hasProperty(request, "stems") ||
      !val::global("Array").call<bool>("isArray", request["stems"])) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "external stem import requires a request with stems array");
  }
  const val stem_values = request["stems"];
  const size_t count = stem_values["length"].as<size_t>();
  std::vector<SonareExternalStemDesc> descriptors;
  std::vector<std::string> names;
  std::vector<std::string> roles;
  std::vector<std::vector<std::vector<float>>> sample_storage;
  std::vector<std::vector<const float*>> plane_storage;
  descriptors.reserve(count);
  names.reserve(count);
  roles.reserve(count);
  sample_storage.reserve(count);
  plane_storage.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const val stem = stem_values[static_cast<unsigned>(index)];
    if (stem.isUndefined() || stem.isNull() || !hasProperty(stem, "name") ||
        !stem["name"].isString() || !hasProperty(stem, "layout") ||
        !hasProperty(stem, "planarSamples") ||
        !val::global("Array").call<bool>("isArray", stem["planarSamples"])) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "each external stem needs name, layout, and planarSamples");
    }
    const uint32_t layout = stem["layout"].as<uint32_t>();
    const size_t channels = channel_count(layout);
    const val source_planes = stem["planarSamples"];
    if (channels == 0 || source_planes["length"].as<size_t>() != channels) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "planarSamples must match mono or stereo layout");
    }
    names.push_back(stem["name"].as<std::string>());
    const val role = stem["role"];
    const bool has_role = !role.isUndefined() && !role.isNull();
    if (has_role && !role.isString()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "external stem role must be a string");
    }
    if (has_role) roles.push_back(role.as<std::string>());
    sample_storage.emplace_back();
    plane_storage.emplace_back();
    auto& samples = sample_storage.back();
    auto& planes = plane_storage.back();
    samples.reserve(channels);
    planes.reserve(channels);
    size_t frames = 0;
    for (size_t channel = 0; channel < channels; ++channel) {
      samples.push_back(float32ArrayToVector(source_planes[static_cast<unsigned>(channel)]));
      if (channel == 0) {
        frames = samples.back().size();
      } else if (samples.back().size() != frames) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all external stem planes must have equal length");
      }
      planes.push_back(samples.back().data());
    }
    SonareExternalStemDesc descriptor{};
    descriptor.name = names.back().c_str();
    descriptor.role = has_role ? roles.back().c_str() : nullptr;
    descriptor.layout = layout;
    descriptor.planar_samples = planes.data();
    descriptor.frame_count = static_cast<int64_t>(frames);
    descriptor.start_frame = hasProperty(stem, "startFrame") ? stem["startFrame"].as<int64_t>() : 0;
    descriptors.push_back(descriptor);
  }
  SonareExternalStemImportRequest request_desc{};
  request_desc.sample_rate = request["sampleRate"].as<int>();
  request_desc.stems = descriptors.data();
  request_desc.stem_count = descriptors.size();
  SonareExternalStemImportResult result{};
  const SonareError error =
      sonare_project_import_external_stems(project_.get(), &request_desc, &result);
  if (error != SONARE_OK) throwCError(error, "failed to import external stems");
  try {
    val output = val::object();
    val track_ids = val::array();
    val clip_ids = val::array();
    for (size_t index = 0; index < result.count; ++index) {
      track_ids.set(static_cast<unsigned>(index), static_cast<double>(result.track_ids[index]));
      clip_ids.set(static_cast<unsigned>(index), static_cast<double>(result.clip_ids[index]));
    }
    output.set("trackIds", track_ids);
    output.set("clipIds", clip_ids);
    sonare_free_external_stem_import_result(&result);
    return output;
  } catch (...) {
    sonare_free_external_stem_import_result(&result);
    throw;
  }
}

void registerProjectExternalStems(class_<ProjectWasm>& cls) {
  cls.function("importExternalStems", &ProjectWasm::importExternalStems);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
