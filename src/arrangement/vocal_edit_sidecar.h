/// @file vocal_edit_sidecar.h
/// @brief Vocal-edit assist sidecars keyed by clip and take, and their lifecycle under clip edits.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "arrangement/edit_command.h"

namespace sonare::arrangement::vocal_sidecar {

inline constexpr std::string_view kPrefix = "libsonare.vocal-edit/clip/";
inline constexpr uint32_t kSchemaVersion = 1;

struct Key {
  ClipId clip_id = 0;
  TakeId take_id = 0;
};

struct Envelope {
  SourceId original_source_id = 0;
  SourceId derived_source_id = 0;
  uint32_t source_sample_rate = 0;
  uint32_t profile_id = 0;
  int64_t source_sample_count = 0;
  uint64_t committed_revision = 0;
  std::array<uint8_t, 32> original_digest{};
  std::array<uint8_t, 32> derived_digest{};
  std::vector<uint8_t> sve1;
};

std::optional<Key> parse_key(std::string_view module_id) noexcept;
std::string make_key(Key key);
bool decode_envelope(const AssistSidecar& sidecar, Envelope* out) noexcept;
AssistSidecar encode_envelope(Key key, const Envelope& envelope);

void remove_clip_sidecars(Project* project, ClipId clip_id);
void clone_clip_sidecars(Project* project, ClipId from, ClipId to);
/// Drops `after.id` sidecars whose take vanished or now resolves to another source. The envelope
/// covers the whole source, so a source-offset change (trim, split, slip) keeps it.
void prune_changed_bindings(Project* project, const EditClip& before, const EditClip& after);

/// Wraps an inverse so undo also restores, in place, only the sidecars of `affected_clip_ids`.
EditCommandPtr wrap_inverse(EditCommandPtr ordinary_inverse, const Project& before,
                            std::vector<ClipId> affected_clip_ids);
EditCommandPtr make_upsert_command(AssistSidecar canonical_sidecar);

/// Sources only the removed clips reach, sidecars included; empty on any malformed vocal sidecar.
std::vector<SourceId> collect_orphaned_sources_after_removing_clips(
    const Project& project, const std::vector<ClipId>& removed_clip_ids);

}  // namespace sonare::arrangement::vocal_sidecar
