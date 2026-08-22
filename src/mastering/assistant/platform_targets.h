#pragma once

/// @file platform_targets.h
/// @brief Delivery-target vocabulary for the mastering assistant.
/// @details This table is the single source of truth for the platform names the
///          assistant accepts and the loudness each one asks for. Every surface
///          derives from it rather than restating it: the C++ CLI, the Node
///          addon and the WASM bindings call these helpers directly, and the C
///          ABI exposes the name list and the name-to-index lookup for callers
///          that cannot link C++ (see sonare_mastering_platform_names /
///          sonare_mastering_platform_from_name). A binding that hard-codes the
///          names or the indices is the drift this file exists to prevent.
///
///          One restatement exists and is deliberate: the Python CLI's argparse
///          `--target-platform` needs its accepted set at parser-construction
///          time, which is before any handler has loaded the shared library, so
///          it lists the names literally. It is pinned to this table rather than
///          trusted -- the native CLI builds the same option's domain from
///          platform_names(), and the cross-surface option-domain comparison in
///          tests/conformance/cli_contract_v2.json fails when the two disagree,
///          so appending a row here fails that check until the parser follows.

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "mastering/assistant/suggester.h"
#include "util/exception.h"

namespace sonare::mastering::assistant {

/// One delivery target. `overrides_loudness == false` means the target keeps
/// whatever loudness the caller asked for; `target_lufs` / `ceiling_db` are then
/// unused rather than meaningful zeros.
struct PlatformTarget {
  const char* name;
  bool overrides_loudness;
  float target_lufs;
  float ceiling_db;
};

/// Accepted delivery targets. The array index is the value a numeric transport
/// carries, so entries are appended rather than reordered.
///
/// A row with `overrides_loudness == false` is a deliberate statement that the
/// target adds nothing to the caller's request, not a gap to be filled in
/// silently; the comment on each such row says which of the two it is.
inline constexpr std::array<PlatformTarget, 8> kPlatformTargets = {{
    // Deliberately unopinionated: the AssistantConfig defaults (-14 LUFS,
    // -1 dBTP) already express the streaming convention, so the default target
    // is a present row rather than a fallthrough.
    {"streaming", false, 0.0f, 0.0f},
    // Streaming family; no target distinct from the shared streaming default.
    {"youtube", false, 0.0f, 0.0f},
    // EBU R128 programme loudness.
    {"broadcast", true, -23.0f, -1.0f},
    {"podcast", true, -16.0f, -1.0f},
    // Not modelled: the assistant carries no audiobook-specific loudness rule,
    // so the caller's values stand until one is added.
    {"audiobook", false, 0.0f, 0.0f},
    // Theatrical delivery is set by calibrated monitoring level rather than an
    // integrated target, so an integrated-loudness override does not apply.
    {"cinema", false, 0.0f, 0.0f},
    // Loud delivery formats, mastered to a lower ceiling.
    {"club", true, -9.0f, -0.3f},
    {"cd", true, -9.0f, -0.3f},
}};

/// @brief Looks a delivery target up by name.
/// @return The matching row, or nullptr for a null or unknown name.
inline const PlatformTarget* platform_target_from_name(const char* name) noexcept {
  if (name == nullptr) return nullptr;
  for (const PlatformTarget& target : kPlatformTargets) {
    if (std::strcmp(target.name, name) == 0) return &target;
  }
  return nullptr;
}

/// @brief Index of @p name in @ref kPlatformTargets, or -1 when unknown.
/// @details The index is the encoding a numeric transport carries. Callers look
///          it up here instead of embedding it, so appending a row cannot
///          desynchronise a surface.
inline int platform_index_from_name(const char* name) noexcept {
  const PlatformTarget* target = platform_target_from_name(name);
  if (target == nullptr) return -1;
  return static_cast<int>(target - kPlatformTargets.data());
}

/// @brief Name at @p index, or nullptr when the index names no target.
inline const char* platform_name_at(int index) noexcept {
  if (index < 0 || static_cast<std::size_t>(index) >= kPlatformTargets.size()) return nullptr;
  return kPlatformTargets[static_cast<std::size_t>(index)].name;
}

/// @brief Every accepted delivery-target name, in index order.
inline std::vector<std::string> platform_names() {
  std::vector<std::string> names;
  names.reserve(kPlatformTargets.size());
  for (const PlatformTarget& target : kPlatformTargets) names.emplace_back(target.name);
  return names;
}

/// @brief The accepted names as one comma-separated string, for error messages.
inline std::string platform_names_joined() {
  std::string joined;
  for (const PlatformTarget& target : kPlatformTargets) {
    if (!joined.empty()) joined += ", ";
    joined += target.name;
  }
  return joined;
}

/// @brief Assigns a delivery target by name, rejecting an unknown one.
/// @details The single validation point for every surface, so a name the C ABI
///          rejects is a name the JS bindings reject with the same class of
///          error. Silently keeping the default on an unrecognised name is the
///          behaviour this replaces.
/// @throws SonareException InvalidParameter when @p name names no target.
inline void set_target_platform(AssistantConfig& config, const std::string& name) {
  SONARE_CHECK_MSG(platform_target_from_name(name.c_str()) != nullptr, ErrorCode::InvalidParameter,
                   "unknown mastering target platform '" + name +
                       "'; expected one of: " + platform_names_joined());
  config.target_platform = name;
}

}  // namespace sonare::mastering::assistant
