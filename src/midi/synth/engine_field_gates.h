#pragma once

/// @file engine_field_gates.h
/// @brief The conditions under which an engine-section field reaches the voice.

#include <string>
#include <vector>

#include "midi/synth/native_synth.h"
#include "midi/synth/patch_tuning.h"

namespace sonare::midi::synth::patch_tuning_detail {

/// The gate of the field at @p member_path as alternatives; empty when it always reaches the
/// voice. @p index is the array index the walker put into the path (0 at a plain member),
/// @p own_key the field's public key and @p base the patch the descriptors are taken from.
std::vector<ActiveAlternative> engine_field_gates(const std::string& member_path, int index,
                                                  const std::string& own_key,
                                                  const NativeSynthPatch& base);

}  // namespace sonare::midi::synth::patch_tuning_detail
