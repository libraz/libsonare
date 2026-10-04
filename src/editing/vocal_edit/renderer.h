#pragma once

/// @file renderer.h
/// @brief Deterministic vocal note rendering and incremental render jobs.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "editing/vocal_edit/render_snapshot.h"

namespace sonare::editing::vocal_edit {

namespace detail {
float stretch_rate_for_length(std::size_t input_size, std::size_t output_size);
std::vector<float> fit_stretched_note_output(std::vector<float> output, std::size_t output_size);
}  // namespace detail

using VocalCancelProbe = std::function<bool()>;

struct VocalRenderRequest {
  SampleRange range{};
  uint64_t request_id = 0;
};

struct VocalRenderDiagnostics {
  uint64_t limited_correction_frames = 0;
  uint64_t dry_passed_frames = 0;
};

struct VocalRenderResult {
  std::vector<float> samples;
  int64_t start_sample = 0;
  VocalStateToken state_token{};
  uint64_t request_id = 0;
  RenderProfile profile = RenderProfile::kVocalPsolaV1;
  std::vector<SampleRange> processed_ranges;
  uint64_t cache_hit_units = 0;
  VocalRenderDiagnostics diagnostics{};
};

enum class VocalRenderJobState : uint32_t {
  kRenderingUnits = 0,
  kAssembling = 1,
  kComplete = 2,
  kAborted = 3,
};

struct VocalRenderProgress {
  VocalRenderJobState state = VocalRenderJobState::kRenderingUnits;
  uint64_t completed_units = 0;
  uint64_t total_units = 0;
};

class VocalRenderJob final {
 public:
  VocalRenderJob(std::shared_ptr<const VocalRenderSnapshot> snapshot, VocalRenderRequest request);
  VocalRenderJob(VocalRenderJob&&) noexcept;
  VocalRenderJob& operator=(VocalRenderJob&&) noexcept;
  VocalRenderJob(const VocalRenderJob&) = delete;
  VocalRenderJob& operator=(const VocalRenderJob&) = delete;
  ~VocalRenderJob();

  VocalRenderProgress progress() const noexcept;
  bool next(const VocalCancelProbe& cancel = {});
  VocalRenderResult finalize(const VocalCancelProbe& cancel = {});
  void abort() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

VocalRenderResult render_snapshot(std::shared_ptr<const VocalRenderSnapshot> snapshot,
                                  const VocalRenderRequest& request,
                                  const VocalCancelProbe& cancel = {});

}  // namespace sonare::editing::vocal_edit
