#pragma once

#if defined(SONARE_WITH_ARRANGEMENT)

#include <memory>

#include "arrangement/edit_compiler.h"

/// @brief C-ABI handle for a compiled project timeline.
/// @details Shares the immutable snapshot, so an engine that applied it can keep
///          the snapshot alive after the caller destroys the handle.
struct SonareProjectTimeline {
  std::shared_ptr<const sonare::arrangement::CompiledTimeline> timeline;
};

#endif  // SONARE_WITH_ARRANGEMENT
