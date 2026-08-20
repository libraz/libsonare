#pragma once

/// @file section_form.h
/// @brief Shared helpers for asserting on SectionAnalyzer form strings.

#include <algorithm>
#include <array>

#include "analysis/section_analyzer.h"
#include "util/types.h"

namespace sonare::test {

/// Every SectionType value. This is the one place a new member has to be added;
/// the checks below derive from it rather than restating the form alphabet, so a
/// type whose character is missing or duplicated fails rather than slipping
/// through a hand-written literal in whichever test file happens to list it.
inline constexpr std::array<SectionType, 8> kAllSectionTypes = {
    SectionType::Intro,  SectionType::Verse,        SectionType::PreChorus, SectionType::Chorus,
    SectionType::Bridge, SectionType::Instrumental, SectionType::Outro,     SectionType::Unknown,
};

/// Whether @p c is the form character of some SectionType.
inline bool is_section_form_char(char c) {
  return std::any_of(kAllSectionTypes.begin(), kAllSectionTypes.end(),
                     [c](SectionType type) { return section_type_to_char(type) == c; });
}

/// Whether every character of @p form names a SectionType.
inline bool is_section_form(const std::string& form) {
  return std::all_of(form.begin(), form.end(), is_section_form_char);
}

}  // namespace sonare::test
