#pragma once

/// @file project_bounce_mixer.h
/// @brief The channel-strip path of the project bounce: routing, scene mixer
///        and stem summing.

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "c_api/project_bounce_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
#include <sonare/sonare_c_mixing.h>

namespace sonare_c_bounce_detail {

// Resolved Track->Strip routing for a channel-strip bounce: the scene strip ids
// in their canonical order (= the mixer's process_stereo input index order),
// each strip's set of source tracks, and the union of all bound tracks.
struct MixerRouting {
  std::vector<std::string> strip_ids;
  std::vector<std::set<uint32_t>> strip_tracks;  // index-aligned with strip_ids
  std::set<uint32_t> bound_tracks;
};

struct MixerDeleter {
  void operator()(SonareMixer* mixer) const noexcept {
    if (mixer != nullptr) sonare_mixer_destroy(mixer);
  }
};

using MixerPtr = std::unique_ptr<SonareMixer, MixerDeleter>;

bool timeline_has_unbound_tracks(const arr::CompiledTimeline& timeline,
                                 const MixerRouting& routing);

// Resolves the compiled timeline's mixer bindings against its scene. Only
// bindings whose strip actually exists in the scene are honored; a binding to a
// missing strip leaves its track unbound (rendered dry into the master).
MixerRouting resolve_mixer_routing(const arr::CompiledTimeline& timeline);

// True when any diagnostic in `diagnostics` is an error (a warning-only list
// still describes a renderable bounce).
bool has_error_diagnostic(const std::vector<arr::Diagnostic>& diagnostics);

struct MixerLatencyTail {
  int latency_samples = 0;
  int tail_samples = 0;
  bool valid = false;
};

MixerLatencyTail mixer_latency_tail_for_timeline(
    const arr::CompiledTimeline& timeline, const MixerRouting& routing, double sample_rate,
    int block_size, MixerPtr* out_mixer = nullptr,
    std::vector<arr::Diagnostic>* out_diagnostics = nullptr);

// Channel-strip bounce: renders each bound track as an isolated dry
// stereo stem and sums the stems through the scene's mixer so per-track EQ,
// inserts, pan, fader, sends and buses are applied. Tracks bound to no scene
// strip are rendered into a separate dry stem and summed straight into the
// master. `frames`/`pdc` are precomputed by the caller; stems are aligned to
// [0, frames) after dropping the leading PDC fill.
SonareError bounce_through_mixer(const arr::CompiledTimeline& timeline,
                                 const std::vector<HostedInstrument>& instruments,
                                 const MixerRouting& routing, double sample_rate, int block_size,
                                 int num_channels, int64_t frames, int64_t pdc,
                                 int64_t mixer_input_frames, float** out_interleaved,
                                 size_t* out_len, SonareMixer* prebuilt_mixer = nullptr,
                                 std::vector<arr::Diagnostic>* out_diagnostics = nullptr);

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_MIXING
