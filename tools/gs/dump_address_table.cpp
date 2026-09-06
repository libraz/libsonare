/// @file dump_address_table.cpp
/// @brief Emit the GS address table as JSON, for tools that have to reason about it.
///
/// The table is a constexpr array in a header, so a reader outside C++ has two
/// ways to it: parse the source, or include it and print. This is the second.
/// A parse would have to keep up with the row layout, the enumerator spellings
/// and the string escaping, and would go wrong quietly the first time a field
/// was added -- which is the failure mode the table exists to prevent elsewhere.
///
/// Built and run by `make gs-unit-diff`; it is not part of the library and links
/// nothing, since the table and its parameter names are both header content.

#include <cstdint>
#include <cstdio>
#include <string>

#include "midi/synth/gs_address_table.h"

namespace {

using sonare::midi::synth::GsAddressEntry;
using sonare::midi::synth::GsAddressRange;
using sonare::midi::synth::GsLevel;
using sonare::midi::synth::kGsAddressTable;
using sonare::midi::synth::kGsUndefinedRanges;

/// The enumerator spellings, taken from the same X-macro the enum is built from
/// rather than from the .cpp, so this links against nothing.
const char* param_name(sonare::midi::synth::GsParam param) {
  static const char* const kNames[] = {
#define SONARE_GS_PARAM_STRING(name) #name,
      SONARE_GS_PARAMS(SONARE_GS_PARAM_STRING)
#undef SONARE_GS_PARAM_STRING
  };
  return kNames[static_cast<size_t>(param)];
}

const char* level_name(GsLevel level) {
  switch (level) {
    case GsLevel::kAudible:
      return "AUDIBLE";
    case GsLevel::kState:
      return "STATE";
    case GsLevel::kAccept:
      return "ACCEPT";
    case GsLevel::kIgnore:
      return "IGNORE";
  }
  return "?";
}

/// An address in the archive's own spelling, so the two sides compare as text.
std::string spelled(uint32_t addr) {
  char out[9];
  std::snprintf(out, sizeof(out), "%02X %02X %02X", (addr >> 16) & 0xFF, (addr >> 8) & 0xFF,
                addr & 0xFF);
  return out;
}

std::string quoted(const char* text) {
  if (text == nullptr) return "null";
  std::string out = "\"";
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c == '"' || *c == '\\') out += '\\';
    out += *c;
  }
  return out + "\"";
}

}  // namespace

int main() {
  std::printf("{\n  \"rows\": [\n");
  for (size_t i = 0; i < kGsAddressTable.size(); ++i) {
    const GsAddressEntry& e = kGsAddressTable[i];
    std::printf(
        "    {\"address\": \"%s\", \"addr\": %u, \"mask\": %u, \"param\": \"%s\", "
        "\"level\": \"%s\", \"size\": %u, \"lo\": %u, \"hi\": %u, \"def\": %u, \"why\": %s}%s\n",
        spelled(e.addr).c_str(), e.addr, e.mask, param_name(e.param), level_name(e.level), e.size,
        e.lo, e.hi, e.def, quoted(e.why).c_str(), i + 1 == kGsAddressTable.size() ? "" : ",");
  }
  std::printf("  ],\n  \"undefined_ranges\": [\n");
  for (size_t i = 0; i < kGsUndefinedRanges.size(); ++i) {
    const GsAddressRange& r = kGsUndefinedRanges[i];
    std::printf(
        "    {\"from\": \"%s\", \"to\": \"%s\", \"lo_addr\": %u, \"hi_addr\": %u, "
        "\"mask\": %u, \"why\": %s}%s\n",
        spelled(r.lo_addr).c_str(), spelled(r.hi_addr).c_str(), r.lo_addr, r.hi_addr, r.mask,
        quoted(r.why).c_str(), i + 1 == kGsUndefinedRanges.size() ? "" : ",");
  }
  std::printf("  ]\n}\n");
  return 0;
}
