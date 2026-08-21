#pragma once

/// @file retained_bytes.h
/// @brief Allocation-free, conservative retained-memory accounting helpers.
///
/// These helpers intentionally account capacities rather than logical sizes.
/// They are used only on the control thread while trimming EditHistory; no
/// helper allocates, caches, or follows pointers into a live project/store.

#include <cstddef>
#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "arrangement/edit_model.h"
#include "automation/automation_lane.h"
#include "mixing/api/scene.h"

namespace sonare::arrangement {

// Defined in edit_compiler.h, which cannot be included here: it needs
// MidiContentStore from edit_command.h, and edit_command.h includes this
// header. The two overloads below are therefore declared against these
// forward declarations and defined in retained_bytes.cpp, which sees both.
struct AudioSourceSamples;
struct AudioContentStore;

}  // namespace sonare::arrangement

namespace sonare::arrangement::retained {

/// Saturating arithmetic keeps accounting itself noexcept even for hostile
/// capacities supplied by synthetic commands or fuzz tests.
std::size_t saturating_add(std::size_t lhs, std::size_t rhs) noexcept;
std::size_t saturating_multiply(std::size_t lhs, std::size_t rhs) noexcept;

/// Fallback for types that own nothing beyond their own storage.
///
/// The static_assert is the point of this overload. Without it the fallback
/// answers 0 for ANY type, including one that owns megabytes of decoded audio,
/// and the caller gets a plausible number with no indication that nothing was
/// counted -- EditHistory then never evicts what it cannot see and a byte
/// budget silently becomes advisory. Trivially copyable is the exact line: such
/// a type has no out-of-line storage to account, and anything else needs its
/// own overload below. A new type that owns dynamic storage therefore fails to
/// compile here rather than under-reporting at runtime.
template <typename T>
std::size_t dynamic_bytes(const T&) noexcept {
  static_assert(std::is_trivially_copyable_v<T>,
                "retained::dynamic_bytes has no overload for this type, and its fallback would "
                "report 0 bytes for storage the type owns. Add an overload in retained_bytes.h.");
  return 0;
}

template <typename T>
std::size_t dynamic_bytes(const std::vector<T>& values) noexcept;

template <typename K, typename V>
std::size_t dynamic_bytes(const std::map<K, V>& values) noexcept;

inline std::size_t dynamic_bytes(const std::string& value) noexcept {
  // Include space for the terminator. This is deliberately conservative for
  // implementations that reserve a little extra string storage.
  return saturating_add(value.capacity(), std::size_t{1});
}

inline std::size_t dynamic_bytes(const AudioSourceRef& value) noexcept {
  std::size_t total = dynamic_bytes(value.uri);
  total = saturating_add(total, dynamic_bytes(value.content_hash));
  return saturating_add(total, dynamic_bytes(value.external_stem_role));
}

inline std::size_t dynamic_bytes(const MidiSourceRef& value) noexcept {
  return dynamic_bytes(value.name);
}

inline std::size_t dynamic_bytes(const ClipSource& value) noexcept {
  return std::visit([](const auto& source) noexcept { return dynamic_bytes(source); }, value);
}

inline std::size_t dynamic_bytes(const ClipTake& value) noexcept {
  return dynamic_bytes(value.name);
}

inline std::size_t dynamic_bytes(const EditClip& value) noexcept {
  std::size_t total = dynamic_bytes(value.takes);
  return saturating_add(total, dynamic_bytes(value.comp_segments));
}

inline std::size_t dynamic_bytes(const Track& value) noexcept {
  std::size_t total = dynamic_bytes(value.name);
  total = saturating_add(total, dynamic_bytes(value.channel_strip_ref));
  total = saturating_add(total, dynamic_bytes(value.automation_lanes));
  return saturating_add(total, dynamic_bytes(value.output_target));
}

inline std::size_t dynamic_bytes(const WarpMapRef& value) noexcept {
  return saturating_add(dynamic_bytes(value.name), dynamic_bytes(value.anchors));
}

inline std::size_t dynamic_bytes(const ProjectMarker& value) noexcept {
  return dynamic_bytes(value.name);
}

inline std::size_t dynamic_bytes(const SectionSegment& value) noexcept {
  return dynamic_bytes(value.label);
}

inline std::size_t dynamic_bytes(const ChordSymbol& value) noexcept {
  return saturating_add(dynamic_bytes(value.extensions), dynamic_bytes(value.roman_numeral));
}

inline std::size_t dynamic_bytes(const ProjectAnnotation& value) noexcept {
  std::size_t total = dynamic_bytes(value.keys);
  total = saturating_add(total, dynamic_bytes(value.chords));
  total = saturating_add(total, dynamic_bytes(value.sections));
  return saturating_add(total, dynamic_bytes(value.onsets));
}

inline std::size_t dynamic_bytes(const AssistSidecar& value) noexcept {
  return saturating_add(dynamic_bytes(value.module_id), dynamic_bytes(value.payload));
}

/// Decoded PCM: the largest thing an EditHistory entry can hold, and the reason
/// the fallback above must not answer for it silently. Defined in the .cpp
/// (see the forward declarations above).
std::size_t dynamic_bytes(const AudioSourceSamples& value) noexcept;

/// Both maps, including warped_sources, which no caller was accounting.
std::size_t dynamic_bytes(const AudioContentStore& value) noexcept;

inline std::size_t dynamic_bytes(const automation::Breakpoint&) noexcept { return 0; }

inline std::size_t dynamic_bytes(const automation::AutomationLane& value) noexcept {
  return dynamic_bytes(value.points());
}

inline std::size_t dynamic_bytes(const mixing::api::Insert& value) noexcept {
  std::size_t total = dynamic_bytes(value.processor_name);
  total = saturating_add(total, dynamic_bytes(value.params_json));
  return saturating_add(total, dynamic_bytes(value.sidechain_key));
}

inline std::size_t dynamic_bytes(const mixing::api::Send& value) noexcept {
  return saturating_add(dynamic_bytes(value.id), dynamic_bytes(value.destination_bus_id));
}

inline std::size_t dynamic_bytes(const mixing::api::Strip& value) noexcept {
  std::size_t total = dynamic_bytes(value.id);
  total = saturating_add(total, dynamic_bytes(value.inserts));
  return saturating_add(total, dynamic_bytes(value.sends));
}

inline std::size_t dynamic_bytes(const mixing::api::Bus& value) noexcept {
  const std::size_t names = saturating_add(dynamic_bytes(value.id), dynamic_bytes(value.role));
  return saturating_add(names, dynamic_bytes(value.inserts));
}

inline std::size_t dynamic_bytes(const mixing::api::VcaGroup& value) noexcept {
  return saturating_add(dynamic_bytes(value.id), dynamic_bytes(value.members));
}

inline std::size_t dynamic_bytes(const mixing::api::Connection& value) noexcept {
  return saturating_add(dynamic_bytes(value.source), dynamic_bytes(value.destination));
}

inline std::size_t dynamic_bytes(const mixing::api::Scene& value) noexcept {
  std::size_t total = dynamic_bytes(value.strips);
  total = saturating_add(total, dynamic_bytes(value.buses));
  total = saturating_add(total, dynamic_bytes(value.vca_groups));
  return saturating_add(total, dynamic_bytes(value.connections));
}

/// Account all project-owned dynamic containers. The Project object itself is
/// counted by bytes(Project); this helper only describes its dynamic payload.
inline std::size_t dynamic_bytes(const Project& value) noexcept {
  std::size_t total = dynamic_bytes(value.tempo_segments());
  total = saturating_add(total, dynamic_bytes(value.time_signatures()));
  total = saturating_add(total, dynamic_bytes(value.sources()));
  total = saturating_add(total, dynamic_bytes(value.tracks()));
  total = saturating_add(total, dynamic_bytes(value.clips()));
  total = saturating_add(total, dynamic_bytes(value.markers()));
  total = saturating_add(total, dynamic_bytes(value.annotation()));
  total = saturating_add(total, dynamic_bytes(value.scene()));
  total = saturating_add(total, dynamic_bytes(value.assist_sidecars()));
  return saturating_add(total, dynamic_bytes(value.warp_maps()));
}

template <typename T>
std::size_t dynamic_bytes(const std::vector<T>& values) noexcept {
  std::size_t total = saturating_multiply(values.capacity(), sizeof(T));
  for (const auto& value : values) {
    total = saturating_add(total, dynamic_bytes(value));
  }
  return total;
}

template <typename K, typename V>
std::size_t dynamic_bytes(const std::map<K, V>& values) noexcept {
  // A standard map node carries at least the value plus several links and a
  // color/parent bookkeeping word. Four pointers is intentionally conservative
  // across the standard-library implementations supported by libsonare.
  constexpr std::size_t kNodeOverhead = 4u * sizeof(void*);
  const std::size_t node_bytes =
      saturating_add(sizeof(typename std::map<K, V>::value_type), kNodeOverhead);
  std::size_t total = saturating_multiply(values.size(), node_bytes);
  for (const auto& [key, value] : values) {
    total = saturating_add(total, dynamic_bytes(key));
    total = saturating_add(total, dynamic_bytes(value));
  }
  return total;
}

template <typename T>
std::size_t bytes(const T& value) noexcept {
  return saturating_add(sizeof(T), dynamic_bytes(value));
}

}  // namespace sonare::arrangement::retained
