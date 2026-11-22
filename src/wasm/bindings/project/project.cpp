/// @file project.cpp
/// @brief Core of the embind headless-DAW project facade: lifecycle + the single
/// class_<> handle whose domain slices are registered from the sibling TUs.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

ProjectWasm::ProjectWasm() {
  SonareProject* handle = nullptr;
  const SonareError err = sonare_project_create(&handle);
  if (err != SONARE_OK) throwCodedCError(err);
  if (handle == nullptr) throwLastCError();
  project_ = std::shared_ptr<SonareProject>(handle, sonare_project_destroy);
}

ProjectWasm::ProjectWasm(SonareProject* adopted) : project_(adopted, sonare_project_destroy) {}

std::string ProjectWasm::toJson() const {
  char* json = nullptr;
  size_t len = 0;
  const SonareError err = sonare_project_serialize(project_.get(), &json, &len);
  if (err != SONARE_OK || json == nullptr) {
    sonare_free_string(json);
    if (err != SONARE_OK) throwCodedCError(err);
    throwLastCError();
  }
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

val ProjectWasm::fromJson(const std::string& json, const val& with_diagnostics_val) {
  const bool with_diagnostics =
      optionalBool(with_diagnostics_val, "withDiagnostics").value_or(false);
  wasmRefuseEmbeddedNul(json, "json");
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
  std::string diagnostics = diag != nullptr ? std::string(diag) : std::string();
  sonare_free_string(diag);
  if (err != SONARE_OK || handle == nullptr) {
    sonare_project_destroy(handle);
    const std::string message =
        diagnostics.empty() ? std::string("invalid project JSON") : diagnostics;
    // The C return code, as on every other surface.
    if (err != SONARE_OK) throwCErrorMessage(err, message);
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, message);
  }
  if (!with_diagnostics) return val(ProjectWasm(handle));
  val out = val::object();
  out.set("project", ProjectWasm(handle));
  out.set("diagnostics", diagnostics);
  return out;
}

void ProjectWasm::setSampleRate(const val& sample_rate) {
  checkCodedCError(
      sonare_project_set_sample_rate(project_.get(), wholeHertzSampleRateFromVal(sample_rate)));
}

#endif  // SONARE_WITH_ARRANGEMENT

void registerProjectBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
  // Headless DAW project. fromJson is a static factory returning a
  // by-value Project; bounce takes an optional options object.
  class_<ProjectWasm> cls("Project");
  cls.constructor<>()
      .class_function("fromJson", &ProjectWasm::fromJson)
      .function("toJson", &ProjectWasm::toJson)
      .function("setSampleRate", &ProjectWasm::setSampleRate);
  registerProjectArrange(cls);
  registerProjectExternalStems(cls);
  registerProjectVocalEdit(cls);
  registerProjectEdit(cls);
  registerProjectMidi(cls);
  registerProjectTranscribe(cls);
  registerProjectBounce(cls);
  registerProjectMeta(cls);
  registerProjectFreeFunctions();
  registerSampleBank();
  registerProjectTimeline();
#endif
}

#endif  // __EMSCRIPTEN__
