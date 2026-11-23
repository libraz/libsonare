#pragma once

/// @file midi_input_domains.h
/// @brief Numeric domains of the C-ABI MIDI builder and project MIDI entries' arguments that no
///        core validator sees, stated once for the checks that refuse them.

#include <cstdint>
#include <limits>

#include "core/audio.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What a 7-bit data byte (controller number, program, bank byte, data value) accepts.
inline constexpr sonare::util::check::NumericDomain kMidiDataByteDomain =
    sonare::util::check::integer_domain(0, 127);
/// @brief What the controller number of a 14-bit control change pair accepts.
inline constexpr sonare::util::check::NumericDomain kMidiControl14NumberDomain =
    sonare::util::check::integer_domain(0, 31);
/// @brief What a MIDI channel accepts.
inline constexpr sonare::util::check::NumericDomain kMidiChannelDomain =
    sonare::util::check::integer_domain(0, 15);
/// @brief What a UMP group accepts.
inline constexpr sonare::util::check::NumericDomain kMidiUmpGroupDomain =
    sonare::util::check::integer_domain(0, 15);
/// @brief What a MIDI 1.0 note-on velocity accepts; 0 is a note-off.
inline constexpr sonare::util::check::NumericDomain kMidiNoteOnVelocityDomain =
    sonare::util::check::integer_domain(1, 127);
/// @brief What a 14-bit controller value accepts.
inline constexpr sonare::util::check::NumericDomain kMidiControl14ValueDomain =
    sonare::util::check::integer_domain(0, 16383);
/// @brief What a 14-bit pitch bend value accepts.
inline constexpr sonare::util::check::NumericDomain kMidiPitchBend14Domain =
    sonare::util::check::integer_domain(0, 16383);
/// @brief What the event or binding count of a MIDI argument array accepts.
inline constexpr sonare::util::check::NumericDomain kMidiEventCountDomain =
    sonare::util::check::integer_domain(0,
                                        static_cast<long long>(sonare::kMaxAudioBufferSize / 16));
/// @brief What the parameter id a MIDI controller binding targets accepts.
inline constexpr sonare::util::check::NumericDomain kMidiTargetParamIdDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<uint32_t>::max());

}  // namespace sonare_c_detail
