#pragma once

/// @file render_snapshot.h
/// @brief Immutable state handed from the control thread to a renderer.

#include <atomic>
#include <cstdint>
#include <memory>

#include "core/audio.h"
#include "editing/vocal_edit/types.h"

namespace sonare::editing::vocal_edit {

class VocalRenderCache;

// Defined by the renderer/cache unit. Keeping construction behind this seam
// lets the immutable state core propagate its cache budget without including
// renderer headers or depending on cache internals.
std::shared_ptr<VocalRenderCache> make_vocal_render_cache(uint64_t max_bytes);

struct VocalRenderSnapshotData {
  Audio source;
  SourceDescriptor source_descriptor{};
  std::shared_ptr<const VocalAnalysisData> analysis;
  std::shared_ptr<const VocalEditState> state;
  int64_t output_length_samples = 0;
  RenderSettings render_settings{};
  VocalStateToken token{};
  std::shared_ptr<VocalRenderCache> cache;
  // Session-owned render capacity is shared by snapshots and jobs. The
  // renderer increments this counter before starting a job and decrements it
  // on every terminal path, so retaining an old snapshot cannot bypass the
  // session's configured concurrency limit.
  std::shared_ptr<std::atomic<uint32_t>> render_job_count;
  uint32_t max_render_jobs = 4;
};

/// @brief A cross-thread immutable render view.
class VocalRenderSnapshot final {
 public:
  VocalRenderSnapshot() = default;
  explicit VocalRenderSnapshot(std::shared_ptr<const VocalRenderSnapshotData> data)
      : data_(std::move(data)) {}

  bool valid() const noexcept { return static_cast<bool>(data_); }
  const Audio& source() const { return data_->source; }
  const SourceDescriptor& source_descriptor() const { return data_->source_descriptor; }
  const VocalAnalysisData& analysis() const { return *data_->analysis; }
  const VocalEditState& edit_state() const { return *data_->state; }
  const VocalStateToken& token() const { return data_->token; }
  const RenderSettings& render_settings() const { return data_->render_settings; }
  int64_t output_length_samples() const { return data_->output_length_samples; }
  const VocalRenderSnapshotData& data() const noexcept { return *data_; }
  const std::shared_ptr<const VocalRenderSnapshotData>& shared_data() const noexcept {
    return data_;
  }

 private:
  std::shared_ptr<const VocalRenderSnapshotData> data_;
};

}  // namespace sonare::editing::vocal_edit
