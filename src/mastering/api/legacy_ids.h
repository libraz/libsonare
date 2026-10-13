#pragma once

/// @file legacy_ids.h
/// @brief Spellings of renamed processor ids, amp presets and enum choices that
///        are still accepted on input.
///
/// Only input paths (scene / rig / preset loading, processor-by-name lookup,
/// choice-name resolution) fold a legacy spelling onto its current one; every
/// catalog, descriptor and serialized output carries the current spelling only.

#include <cstddef>
#include <string>

namespace sonare::mastering::api {

namespace legacy_detail {

struct Rename {
  const char* legacy;
  const char* current;
};

// Legacy spellings below are accepted for saved data only.
inline constexpr Rename kProcessorIds[] = {
    {"eq.pultec", "eq.passiveTube"},
    {"eq.apiStyle", "eq.steppedProportionalQ"},
};

inline constexpr Rename kAmpPresets[] = {
    {"tweedGrind", "vintageGrind"},
    {"britStack", "crunchStack"},
    {"rectifierChug", "heavyChug"},
};

struct ChoiceRename {
  const char* key;
  Rename rename;
};

inline constexpr ChoiceRename kChoices[] = {
    {"ampModel", {"fenderClean", "cleanCombo"}},    {"ampModel", {"tweed", "vintageCombo"}},
    {"ampModel", {"voxChime", "classAChime"}},      {"ampModel", {"rectifier", "heavyHiGain"}},
    {"componentModel", {"eqp1aWdf", "wdfCircuit"}},
};

template <std::size_t N>
inline const char* find_current(const Rename (&table)[N], const std::string& name) {
  for (const Rename& entry : table) {
    if (name == entry.legacy) return entry.current;
  }
  return nullptr;
}

}  // namespace legacy_detail

/// @brief The current processor id for @p id, which may be a legacy spelling.
inline std::string canonical_processor_id(const std::string& id) {
  const char* current = legacy_detail::find_current(legacy_detail::kProcessorIds, id);
  return current != nullptr ? std::string(current) : id;
}

/// @brief The current amp-preset name for @p name, which may be a legacy spelling.
inline std::string canonical_amp_preset_name(const std::string& name) {
  const char* current = legacy_detail::find_current(legacy_detail::kAmpPresets, name);
  return current != nullptr ? std::string(current) : name;
}

/// @brief The current choice name of parameter @p key for @p choice, which may
///        be a legacy spelling.
inline std::string canonical_choice_name(const std::string& key, const std::string& choice) {
  for (const legacy_detail::ChoiceRename& entry : legacy_detail::kChoices) {
    if (key == entry.key && choice == entry.rename.legacy) return entry.rename.current;
  }
  return choice;
}

}  // namespace sonare::mastering::api
