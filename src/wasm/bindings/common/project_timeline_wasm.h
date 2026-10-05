#pragma once

/// @file project_timeline_wasm.h
/// @brief The embind compiled-timeline handle shared by the project facade and
///        the realtime engine.
///
/// A timeline crosses from Project to RealtimeEngine as an integer id rather
/// than as an embind instance, for the same reason a sample bank does: an
/// instance cannot travel through the plain argument the engine reads, and an
/// id turns a released timeline into an InvalidParameter instead of a
/// use-after-free.

#ifdef __EMSCRIPTEN__

#include <sonare/sonare_c.h>

#include <cstdint>
#include <memory>

#include "wasm/bindings/common/common.h"

#if defined(SONARE_WITH_ARRANGEMENT)

/// Owner of one SonareProjectTimeline. Copyable (embind returns it by value);
/// the C handle is destroyed, and its id retired, when the last copy goes.
class ProjectTimelineWasm {
 public:
  /// Adopts @p adopted, which must be non-null.
  explicit ProjectTimelineWasm(SonareProjectTimeline* adopted);

  /// Identity an engine names this timeline by (never zero).
  uint32_t id() const { return id_; }

  /// The live timeline for @p id, or nullptr once every handle has been released.
  static SonareProjectTimeline* lookup(uint32_t id);

 private:
  std::shared_ptr<SonareProjectTimeline> timeline_;
  uint32_t id_ = 0;
};

void registerProjectTimeline();

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
