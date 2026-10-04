#pragma once

/// @file render_cache.h
/// @brief Bounded cache of complete, immutable vocal note render artifacts.

#include <cstdint>
#include <memory>
#include <vector>

#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/render_snapshot.h"

namespace sonare::editing::vocal_edit {

struct RenderArtifact {
  Sha256Digest key{};
  VocalNoteId note_id = kInvalidVocalNoteId;
  SampleRange destination_range{};
  std::shared_ptr<const std::vector<float>> samples;
  uint64_t dry_passed_frames = 0;
};

/// A thread-safe cache containing only complete note artifacts.
///
/// Lookup and publication take a short control-side mutex. DSP is performed by
/// the renderer before publication and never while that mutex is held. The
/// cache is deliberately best-effort: an artifact larger than the budget is
/// returned to the caller but is not retained.
class VocalRenderCache final {
 public:
  explicit VocalRenderCache(uint64_t max_bytes);
  ~VocalRenderCache();

  VocalRenderCache(const VocalRenderCache&) = delete;
  VocalRenderCache& operator=(const VocalRenderCache&) = delete;

  std::shared_ptr<const RenderArtifact> find(const Sha256Digest& key) const;
  void publish(std::shared_ptr<const RenderArtifact> artifact);
  uint64_t max_bytes() const noexcept;
  uint64_t bytes() const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Computes a canonical key for a complete note artifact.
///
/// The whole-session revision is intentionally absent. Every source/edit,
/// final float pitch value, renderer profile/version, and local transition
/// dependency is encoded in a fixed little-endian byte stream instead.
Sha256Digest render_artifact_key(const VocalRenderSnapshotData& snapshot, const VocalNote& note,
                                 const PitchPlan& plan, const PitchTransition* incoming,
                                 const PitchTransition* outgoing);

}  // namespace sonare::editing::vocal_edit
