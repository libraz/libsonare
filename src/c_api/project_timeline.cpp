#include <sonare/sonare_c_project_timeline.h>

#include "c_api/project_internal.h"
#include "c_api/project_timeline_internal.h"

void sonare_project_timeline_destroy(SonareProjectTimeline* timeline) {
#if defined(SONARE_WITH_ARRANGEMENT)
  delete timeline;
#else
  (void)timeline;
#endif
}
