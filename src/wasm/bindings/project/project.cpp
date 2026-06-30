/// @file project.cpp
/// @brief Core of the embind headless-DAW project facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

ProjectWasm::ProjectWasm() {
  SonareProject* handle = nullptr;
  const SonareError err = sonare_project_create(&handle);
  if (err != SONARE_OK || handle == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to create headless project");
  }
  project_ = std::shared_ptr<SonareProject>(handle, sonare_project_destroy);
}

ProjectWasm::ProjectWasm(SonareProject* adopted) : project_(adopted, sonare_project_destroy) {}

std::string ProjectWasm::toJson() const {
  char* json = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_serialize(project_.get(), &json, &len);
  if (err != SONARE_OK || json == nullptr) {
    sonare_free_string(json);
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to serialize project");
  }
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

ProjectWasm ProjectWasm::fromJson(const std::string& json) {
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
  if (err != SONARE_OK || handle == nullptr) {
    std::string message = diag != nullptr ? std::string(diag) : std::string("invalid project JSON");
    sonare_free_string(diag);
    sonare_project_destroy(handle);
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, message);
  }
  sonare_free_string(diag);
  return ProjectWasm(handle);
}

val ProjectWasm::fromJsonWithDiagnostics(const std::string& json) {
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
  std::string diagnostics = diag != nullptr ? std::string(diag) : std::string();
  sonare_free_string(diag);
  if (err != SONARE_OK || handle == nullptr) {
    sonare_project_destroy(handle);
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidFormat,
        diagnostics.empty() ? std::string("invalid project JSON") : diagnostics);
  }
  val out = val::object();
  out.set("project", ProjectWasm(handle));
  out.set("diagnostics", diagnostics);
  return out;
}

void ProjectWasm::setSampleRate(double sample_rate) {
  const SonareError err = sonare_project_set_sample_rate(project_.get(), sample_rate);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid project sample rate");
  }
}

#endif  // SONARE_WITH_ARRANGEMENT

void registerProjectBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
  // Headless DAW project. fromJson is a static factory returning a
  // by-value Project; bounce takes an optional options object.
  class_<ProjectWasm> cls("Project");
  cls.constructor<>()
      .class_function("fromJson", &ProjectWasm::fromJson)
      .class_function("fromJsonWithDiagnostics", &ProjectWasm::fromJsonWithDiagnostics)
      .function("toJson", &ProjectWasm::toJson)
      .function("setSampleRate", &ProjectWasm::setSampleRate);
  registerProjectArrange(cls);
  registerProjectEdit(cls);
  registerProjectMidi(cls);
  registerProjectBounce(cls);
  registerProjectMeta(cls);
  registerProjectFreeFunctions();
#endif
}

#endif  // __EMSCRIPTEN__
