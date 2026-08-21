/// @file source_class.cpp
/// @brief Name mapping for the mixing assistant's source taxonomy.
/// @details Kept apart from both the profiler and the classifier so the
///          vocabulary has one owner. Enum order is the wire order every surface
///          reports, so entries are appended rather than reordered.

#include <array>

#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {
namespace {

constexpr std::array<const char*, kSourceClassCount> kSourceClassNames = {
    "unknown", "kick",    "snare", "hiHat", "tom",     "cymbal",     "bass", "guitar",
    "keys",    "strings", "lead",  "vocal", "backing", "percussion", "fx",
};

}  // namespace

const char* source_class_to_string(SourceClass source) noexcept {
  const int index = static_cast<int>(source);
  if (index < 0 || index >= kSourceClassCount) return "unknown";
  return kSourceClassNames[static_cast<std::size_t>(index)];
}

SourceClass source_class_from_string(const std::string& name) noexcept {
  for (int index = 0; index < kSourceClassCount; ++index) {
    if (name == kSourceClassNames[static_cast<std::size_t>(index)]) {
      return static_cast<SourceClass>(index);
    }
  }
  return SourceClass::Unknown;
}

std::vector<std::string> source_class_names() {
  std::vector<std::string> names;
  names.reserve(kSourceClassCount);
  for (const char* name : kSourceClassNames) names.emplace_back(name);
  return names;
}

}  // namespace sonare::mixing::assistant
