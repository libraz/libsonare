#pragma once

/// @file generator_params.h
/// @brief The `params_json` blob the built-in generator modules read.
///
/// The seam keeps `AssistRequest::params_json` opaque -- the core never parses
/// it -- so each module owns its own reading of it. This is the built-in
/// modules' shared reading, in one place so the three of them cannot spell the
/// same field two ways.
///
/// **The built-in generators write into a clip that already exists.** They never
/// add a track, a source or a clip: making room for generated material is a
/// structural decision, and the seam's contract is that assist proposes content,
/// not structure. A request naming no target clip produces an empty result with
/// a diagnostic saying so, rather than inventing somewhere to put the notes.

#include <cstdint>
#include <string>

#include "arrangement/edit_model.h"

namespace sonare::midi::assist::modules {

/// @brief Fields the built-in generators share. Absent fields keep the default.
struct GeneratorParams {
  /// Clip the generated events are added to. 0 means "not named", which is a
  /// refusal rather than a guess.
  arrangement::ClipId target_clip_id = 0;
  /// Clip the material is derived FROM. 0 means "the same clip as the target",
  /// which is what harmonizing a line in place means.
  arrangement::ClipId source_clip_id = 0;
  /// Voice the placement judge holds the output to.
  uint8_t low_note = 36;
  uint8_t high_note = 84;
  /// Velocity of generated notes relative to what they were derived from, or to
  /// the flat default when nothing was derived. Must be in (0, 2].
  float velocity_scale = 0.85f;
  /// Flat velocity used where nothing was derived from.
  uint8_t base_velocity = 90;
};

/// @brief Reads @p params_json into @p out, leaving unnamed fields alone.
/// @param out_error Receives a message naming the offending field when the read
///        fails. Untouched on success.
/// @return False on malformed JSON, a non-object document, or a field outside
///         its domain. A field of the wrong TYPE is an error, not a default:
///         a silently substituted default is indistinguishable downstream from
///         a deliberate one.
bool read_generator_params(const std::string& params_json, GeneratorParams* out,
                           std::string* out_error);

}  // namespace sonare::midi::assist::modules
