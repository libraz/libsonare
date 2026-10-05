#include <sonare/sonare_c.h>

#include <utility>

#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "arrangement/edit_compiler.h"
#include "c_api/project_timeline_internal.h"
#endif

using namespace sonare_c_detail;

SonareError sonare_engine_apply_project_timeline(SonareRealtimeEngine* engine,
                                                 const SonareProjectTimeline* timeline) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  namespace arr = sonare::arrangement;
  if (!engine || !timeline || !timeline->timeline) return SONARE_ERROR_INVALID_PARAMETER;
  if (engine->engine.transport_state_control().playing) {
    set_last_error("a project timeline can only be applied while the transport is stopped");
    return SONARE_ERROR_INVALID_STATE;
  }
  SONARE_C_TRY
  arr::ApplyOptions options;
  options.bind_strips = true;
  arr::ApplyResult result = arr::apply_to_engine(*timeline->timeline, engine->engine, options);
  // The wrapper's lane copy is what sonare_engine_set_automation_lane republishes, and its
  // marker strings backed markers the apply has replaced; both follow the engine's new state.
  if (result.outcome == arr::ApplyOutcome::kApplied) {
    engine->automation_lanes = std::move(result.installed_automation);
    engine->marker_strings.clear();
    engine->applied_timeline = timeline->timeline;
  } else if (result.outcome == arr::ApplyOutcome::kCleared) {
    engine->automation_lanes.clear();
    engine->marker_strings.clear();
    engine->applied_timeline.reset();
  }
  if (!result.ok()) throw sonare::SonareException(result.code, result.message);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(engine, timeline);
#endif
}
