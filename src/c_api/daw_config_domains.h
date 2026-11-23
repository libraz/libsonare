#pragma once

/// @file daw_config_domains.h
/// @brief Numeric domains of the pitch-editing, sample-bank and voice-changer C-ABI arguments,
///        stated once for the checks that refuse them and for the table in numeric_domain_rows.h.

#include <sonare/sonare_c.h>

#include <cstdint>
#include <limits>

#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// Keymap sets are dense, so an index also sizes the table it creates.
inline constexpr uint32_t kMaxKeymapSets = 4096u;

/// @brief What a percussive separation config's harmonic and percussive HPSS kernels accept.
inline constexpr sonare::util::check::NumericDomain kPercussiveHpssKernelDomain =
    sonare::util::check::integer_domain_of<int32_t>(1);
/// @brief What the frame count of a time-varying correction call accepts.
inline constexpr sonare::util::check::NumericDomain kTimeVaryingFrameCountDomain =
    sonare::util::check::integer_domain_of<size_t>(1);
/// @brief What the frame count of a pitch track's f0 curve accepts.
inline constexpr sonare::util::check::NumericDomain kF0FrameCountDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<int>::max());
/// @brief What a note edit's amplitude_envelope_offset accepts.
inline constexpr sonare::util::check::NumericDomain kEnvelopeOffsetDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);
/// @brief What a note object's frame_start accepts.
inline constexpr sonare::util::check::NumericDomain kNoteFrameStartDomain =
    sonare::util::check::integer_domain_of<int32_t>(0);
/// @brief What a note region's offset_sample accepts: the int sample index the note editor holds.
inline constexpr sonare::util::check::NumericDomain kNoteSampleIndexDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What the sample rate of a note-target assignment accepts.
inline constexpr sonare::util::check::NumericDomain kNoteTargetSampleRateDomain =
    sonare::util::check::integer_domain_of<int>(1);

/// @brief What a sample bank sample's n_frames accepts.
inline constexpr sonare::util::check::NumericDomain kSampleBankFrameCountDomain =
    sonare::util::check::integer_domain_of<size_t>(1);
/// @brief What a sample descriptor's loop_mode accepts (a SoundFont sampleModes value).
inline constexpr sonare::util::check::NumericDomain kSampleLoopModeDomain =
    sonare::util::check::integer_domain(0, 3);
/// @brief What a sample bank's keymap set_index accepts.
inline constexpr sonare::util::check::NumericDomain kKeymapSetIndexDomain =
    sonare::util::check::integer_domain(0, kMaxKeymapSets - 1);

/// @brief What the voice changer's channels accepts.
inline constexpr sonare::util::check::NumericDomain kVoiceChangerChannelsDomain =
    sonare::util::check::integer_domain(1, 2);
/// @brief What the realtime voice changer's block_size accepts.
inline constexpr sonare::util::check::NumericDomain kVoiceChangerBlockSizeDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a voice character preset ordinal accepts.
inline constexpr sonare::util::check::NumericDomain kVoiceChangerPresetDomain =
    sonare::util::check::integer_domain(SONARE_VC_PRESET_NEUTRAL_MONITOR,
                                        SONARE_VC_PRESET_RADIO_VOICE);

}  // namespace sonare_c_detail
