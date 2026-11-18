#pragma once

/// @file retired_names.h
/// @brief Retired processor ids, amp presets and enum choices, each with its successor.
///
/// A retired spelling is refused like any unknown name. The table exists only so the refusal
/// can name the current spelling; nothing folds a retired name onto a current one.

#include <cstddef>
#include <string>

namespace sonare::mastering::api {

namespace retired_detail {

struct Rename {
  const char* retired;
  const char* current;
};

inline constexpr Rename kProcessorIds[] = {
    {"eq.pultec", "eq.passiveTube"},
    {"eq.apiStyle", "eq.steppedProportionalQ"},
    {"effects.reverb.dattorro", "effects.reverb.plate"},
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
inline std::string note_for(const Rename (&table)[N], const std::string& name) {
  for (const Rename& entry : table) {
    if (name == entry.retired) return std::string(" (renamed to ") + entry.current + ")";
  }
  return std::string();
}

}  // namespace retired_detail

/// @brief `" (renamed to <current>)"` when @p id is a retired processor id, else empty.
inline std::string retired_processor_note(const std::string& id) {
  return retired_detail::note_for(retired_detail::kProcessorIds, id);
}

/// @brief `" (renamed to <current>)"` when @p name is a retired amp preset, else empty.
inline std::string retired_amp_preset_note(const std::string& name) {
  return retired_detail::note_for(retired_detail::kAmpPresets, name);
}

/// @brief `" (renamed to <current>)"` when @p choice is a retired choice of parameter @p key.
inline std::string retired_choice_note(const std::string& key, const std::string& choice) {
  for (const retired_detail::ChoiceRename& entry : retired_detail::kChoices) {
    if (key == entry.key && choice == entry.rename.retired) {
      return std::string(" (renamed to ") + entry.rename.current + ")";
    }
  }
  return std::string();
}

}  // namespace sonare::mastering::api
