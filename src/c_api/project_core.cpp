#include "c_api/project_internal.h"

// ============================================================================
// ABI version
// ============================================================================

#if defined(SONARE_WITH_ARRANGEMENT)
// Keep the C++ ABI constant and the C macro in lock-step. The check lives here
// (the C ABI bridge layer) rather than inside arrangement/ to preserve the layer
// rule "arrangement/ must not depend on the public C API header sonare_c.h".
static_assert(arr::kProjectAbiVersion == SONARE_PROJECT_ABI_VERSION,
              "C++ and C project ABI version constants drifted");
#endif

uint32_t sonare_project_abi_version(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return arr::kProjectAbiVersion;
#else
  return 0u;
#endif
}

// ============================================================================
// Lifecycle / IO / render
// ============================================================================

SonareError sonare_project_create(SonareProject** out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  SONARE_C_TRY
  *out = new SonareProject{};
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(out);
#endif
}

void sonare_project_destroy(SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  delete project;
#else
  (void)project;
#endif
}

SonareError sonare_project_serialize(const SonareProject* project, char** out_json,
                                     size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_TRY
  const std::string json = sonare::serialize::project_to_json(project->history.project(),
                                                              project->history.midi_content());
  *out_json = copy_string(json);
  if (out_len) *out_len = json.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_json, out_len);
#endif
}

SonareError sonare_project_deserialize(const char* json, size_t len, SonareProject** out,
                                       char** out_diag) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!json || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (out_diag) *out_diag = nullptr;
  SONARE_C_TRY
  sonare::serialize::DeserializeResult result =
      sonare::serialize::project_from_json(std::string(json, len));
  if (!result.ok()) {
    if (out_diag) {
      std::ostringstream stream;
      for (size_t i = 0; i < result.diagnostics.size(); ++i) {
        if (i > 0) stream << '\n';
        stream << result.diagnostics[i].code << ": " << result.diagnostics[i].message;
      }
      *out_diag = copy_string(stream.str());
    }
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto handle = std::make_unique<SonareProject>();
  handle->history = arr::EditHistory(std::move(*result.project));
  handle->history.midi_content() = std::move(result.midi);
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(json, len, out, out_diag);
#endif
}

SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Global property (not part of the command-driven invertible state); set
  // directly on the owned Project.
  project->history.project().set_sample_rate(sample_rate);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, sample_rate);
#endif
}

SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::CompileResult result =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio);
  out->has_timeline = result.timeline.has_value() ? 1 : 0;
  out->diagnostic_count = result.diagnostics.size();
  if (!result.diagnostics.empty()) {
    out->diagnostics = new SonareProjectDiagnostic[result.diagnostics.size()];
    std::ostringstream stream;
    for (size_t i = 0; i < result.diagnostics.size(); ++i) {
      const arr::Diagnostic& d = result.diagnostics[i];
      out->diagnostics[i].code = static_cast<uint32_t>(d.code);
      out->diagnostics[i].severity = static_cast<uint32_t>(d.severity);
      out->diagnostics[i].target_id = d.target_id;
      if (i > 0) stream << '\n';
      stream << d.message;
    }
    out->messages = copy_string(stream.str());
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out);
#endif
}

void sonare_project_free_compile_result(SonareProjectCompileResult* result) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!result) return;
  delete[] result->diagnostics;
  delete[] result->messages;
  *result = {};
#else
  (void)result;
#endif
}

SonareError sonare_project_set_assist_sidecar(SonareProject* project, const char* module_id,
                                              uint32_t schema_version, uint32_t target_track_id,
                                              double region_start_ppq, double region_end_ppq,
                                              const uint8_t* payload, size_t payload_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !module_id || module_id[0] == '\0' || (payload_len > 0 && !payload) ||
      payload_len > kMaxBufferSize || !finite_non_negative(region_start_ppq) ||
      !finite_non_negative(region_end_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::AssistSidecar sidecar;
  sidecar.module_id = module_id;
  sidecar.schema_version = schema_version;
  sidecar.target_track_id = target_track_id;
  sidecar.region_start_ppq = region_start_ppq;
  sidecar.region_end_ppq = region_end_ppq;
  if (payload_len > 0) {
    sidecar.payload.assign(payload, payload + payload_len);
  }
  auto command = std::make_unique<arr::SetAssistSidecar>(std::move(sidecar));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, module_id, schema_version, target_track_id, region_start_ppq,
                              region_end_ppq, payload, payload_len);
#endif
}

size_t sonare_project_assist_sidecar_count(const SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return 0;
  return project->history.project().assist_sidecars().size();
#else
  (void)project;
  return 0;
#endif
}

SonareError sonare_project_get_assist_sidecar(const SonareProject* project, size_t index,
                                              SonareProjectAssistSidecar* out) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& sidecars = project->history.project().assist_sidecars();
  if (index >= sidecars.size()) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const arr::AssistSidecar& sidecar = sidecars[index];
  std::unique_ptr<char[]> module_id(copy_string(sidecar.module_id));
  std::unique_ptr<uint8_t[]> payload;
  if (!sidecar.payload.empty()) {
    payload = std::make_unique<uint8_t[]>(sidecar.payload.size());
    std::memcpy(payload.get(), sidecar.payload.data(), sidecar.payload.size());
  }
  out->module_id = module_id.release();
  out->schema_version = sidecar.schema_version;
  out->target_track_id = sidecar.target_track_id;
  out->region_start_ppq = sidecar.region_start_ppq;
  out->region_end_ppq = sidecar.region_end_ppq;
  out->payload = payload.release();
  out->payload_len = sidecar.payload.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

void sonare_project_free_assist_sidecar(SonareProjectAssistSidecar* sidecar) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!sidecar) return;
  delete[] sidecar->module_id;
  delete[] sidecar->payload;
  *sidecar = {};
#else
  (void)sidecar;
#endif
}
