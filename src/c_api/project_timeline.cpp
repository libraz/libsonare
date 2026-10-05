#include <sonare/sonare_c_project_timeline.h>

#include <memory>
#include <utility>

#include "c_api/project_internal.h"
#include "c_api/project_timeline_internal.h"

SonareError sonare_project_compile_timeline(SonareProject* project,
                                            SonareProjectCompileResult* out_result,
                                            SonareProjectTimeline** out_timeline) {
  SONARE_C_API_ENTRY;
  if (out_result) *out_result = {};
  if (out_timeline) *out_timeline = nullptr;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_result || !out_timeline) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::CompileResult result =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio);
  std::unique_ptr<SonareProjectTimeline> handle;
  if (result.timeline.has_value()) {
    handle = std::make_unique<SonareProjectTimeline>();
    handle->timeline = std::make_shared<const arr::CompiledTimeline>(std::move(*result.timeline));
  }
  fill_compile_result_from_diagnostics(result.diagnostics, handle != nullptr, out_result);
  *out_timeline = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_result, out_timeline);
#endif
}

void sonare_project_timeline_destroy(SonareProjectTimeline* timeline) {
#if defined(SONARE_WITH_ARRANGEMENT)
  delete timeline;
#else
  (void)timeline;
#endif
}
