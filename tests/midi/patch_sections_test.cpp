/// @file patch_sections_test.cpp
/// @brief The engine-section blanks (midi/synth/patch_sections): no patch the
///        shipped bank hands out carries a byte in a section its `mode` does
///        not voice, which is the property the WebAssembly data-section
///        reduction rests on; and the strip keeps the voiced section intact.
///
/// The bank is asserted rather than the blank functions themselves because a
/// constant-initialised table has zeroed padding while a value returned from a
/// function does not, so a `memcmp` against a blank's return value measures the
/// padding rather than the fields. Reading the table also covers the case that
/// matters: a field added to an engine and not added to its blank puts its
/// default back into every patch, and shows up here.

#include "midi/synth/patch_sections.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "midi/synth/gm_fallback_map.h"

using namespace sonare::midi::synth;

namespace {

template <typename T>
bool is_zero(const T& section) {
  unsigned char bytes[sizeof(T)];
  std::memcpy(bytes, &section, sizeof(T));
  for (unsigned char b : bytes) {
    if (b != 0) return false;
  }
  return true;
}

/// Every section the patch's own `mode` does not select must be gone.
bool only_voiced_section_present(const NativeSynthPatch& p) {
  const SynthEngineMode m = p.mode;
  return (m == SynthEngineMode::kFm || is_zero(p.fm)) &&
         (m == SynthEngineMode::kKarplusStrong || is_zero(p.ks)) &&
         (m == SynthEngineMode::kModal || is_zero(p.modal)) &&
         (m == SynthEngineMode::kAdditive || is_zero(p.additive)) &&
         (m == SynthEngineMode::kPercussion || is_zero(p.percussion)) &&
         (m == SynthEngineMode::kPiano || is_zero(p.piano)) &&
         (m == SynthEngineMode::kPipeOrgan || is_zero(p.pipe_organ)) &&
         (m == SynthEngineMode::kBowedString || is_zero(p.bowed_string)) &&
         (m == SynthEngineMode::kReed || is_zero(p.reed)) &&
         (m == SynthEngineMode::kBrass || is_zero(p.brass)) &&
         (m == SynthEngineMode::kFlute || is_zero(p.flute)) &&
         (m == SynthEngineMode::kPluckedString || is_zero(p.plucked_string)) &&
         (m == SynthEngineMode::kVocal || is_zero(p.vocal)) &&
         (m == SynthEngineMode::kFreeReed || is_zero(p.free_reed)) &&
         (m == SynthEngineMode::kHarpsichord || is_zero(p.harpsichord));
}

}  // namespace

TEST_CASE("the shipped bank stores no unvoiced engine section", "[midi][synth]") {
  // Bank 0 is the capital tone; the others are the GS variations, which the map
  // resolves through separate patches of their own.
  for (uint16_t bank : {uint16_t{0}, uint16_t{1}, uint16_t{8}, uint16_t{16}}) {
    for (int program = 0; program < 128; ++program) {
      const NativeSynthPatch& p = gm_fallback_patch(bank, static_cast<uint8_t>(program));
      INFO("bank " << bank << " program " << program);
      CHECK(only_voiced_section_present(p));
    }
  }
  for (int note = 0; note < 128; ++note) {
    const NativeSynthPatch& p = gm_fallback_drum_patch(static_cast<uint8_t>(note));
    INFO("drum note " << note);
    CHECK(only_voiced_section_present(p));
  }
}

TEST_CASE("stripping keeps the voiced section and clears the rest", "[midi][synth]") {
  NativeSynthPatch p{};
  p.mode = SynthEngineMode::kBowedString;
  p.bowed_string.bow_force = 0.61f;
  p.reed.breath_pressure = 0.9f;
  p.gain = 0.42f;

  const NativeSynthPatch stripped = strip_unvoiced_sections(p);
  CHECK(stripped.mode == SynthEngineMode::kBowedString);
  CHECK(stripped.bowed_string.bow_force == 0.61f);
  CHECK(stripped.gain == 0.42f);
  CHECK(stripped.reed.breath_pressure == 0.0f);
  CHECK(stripped.piano.strings == 0);
  CHECK(stripped.percussion.mode_ratios[0] == 0.0f);
  CHECK(stripped.modal.modes[0].ratio == 0.0f);

  // A subtractive patch voices none of the sixteen, so all fifteen go.
  NativeSynthPatch sub{};
  sub.mode = SynthEngineMode::kSubtractive;
  const NativeSynthPatch sub_stripped = strip_unvoiced_sections(sub);
  CHECK(sub_stripped.fm.ops[0].ratio == 0.0f);
  CHECK(sub_stripped.harpsichord.eight_a == false);
  CHECK(sub_stripped.pipe_organ.ranks[0].footage_mult == 0.0f);
}
