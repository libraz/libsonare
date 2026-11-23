#pragma once

/// @file project_input_domains.h
/// @brief Numeric domains of the C-ABI project entries' arguments that no core validator sees,
///        stated once for the checks that refuse them and for numeric_domain_rows.h.

#include <sonare/sonare_c_types_enums.h>

#include <cstdint>
#include <limits>

#include "core/audio.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the render length of a project bounce accepts;
///        SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT (-1) is "not given".
inline constexpr sonare::util::check::NumericDomain kProjectBounceTotalFramesDomain =
    sonare::util::check::with_sentinel(sonare::util::check::integer_domain_of<int64_t>(1), -1);
/// @brief What the block size of a project bounce accepts.
inline constexpr sonare::util::check::NumericDomain kProjectBounceBlockSizeDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the channel count of a project bounce accepts;
///        SONARE_PROJECT_BOUNCE_CHANNELS_FROM_MASTER (-1) is "not given".
inline constexpr sonare::util::check::NumericDomain kProjectBounceChannelCountDomain =
    sonare::util::check::with_sentinel(sonare::util::check::integer_domain_of<int>(1), -1);
/// @brief What the sample rate of a project bounce accepts: the audio rates;
///        SONARE_PROJECT_SAMPLE_RATE_PROJECT (-1) is "not given".
inline constexpr sonare::util::check::NumericDomain kProjectBounceSampleRateDomain =
    sonare::util::check::with_sentinel(sonare::kAudioSampleRateDomain, -1);
/// @brief What the instrument latency of a project bounce accepts.
inline constexpr sonare::util::check::NumericDomain kProjectBounceInstrumentLatencyDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What the initial count-in bars of a project query or bounce accept.
inline constexpr sonare::util::check::NumericDomain kProjectCountInBarsDomain =
    sonare::util::check::integer_domain(0, 16);
/// @brief The lower bound of a project position in seconds. The tempo map supplies the upper bound.
inline constexpr sonare::util::check::NumericDomain kProjectPositionSecDomain =
    sonare::util::check::real_domain_above(0.0, true);
/// @brief The base domain of a zero-based bar index. Its PPQ start must also be representable.
inline constexpr sonare::util::check::NumericDomain kProjectBarIndexDomain =
    sonare::util::check::integer_domain_at_least(0);

/// @brief What the frame count of an inline audio payload accepts.
inline constexpr sonare::util::check::NumericDomain kAudioPayloadFramesDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What the channel count of an inline audio payload accepts.
inline constexpr sonare::util::check::NumericDomain kAudioPayloadChannelsDomain =
    sonare::util::check::integer_domain_of<int>(1);

/// @brief What the target parameter id of an automation lane accepts.
inline constexpr sonare::util::check::NumericDomain kAutomationTargetParamIdDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);
/// @brief What an entity id the caller assigns accepts; the top value is the exhausted-counter
///        sentinel.
inline constexpr sonare::util::check::NumericDomain kAssignableEntityIdDomain =
    sonare::util::check::integer_domain(
        0, static_cast<long long>(std::numeric_limits<uint32_t>::max()) - 1);
/// @brief What the key signature of a marker accepts, the SMF `sf` byte.
inline constexpr sonare::util::check::NumericDomain kMarkerKeyFifthsDomain =
    sonare::util::check::integer_domain(-7, 7);
/// @brief What the numerator of a time signature segment accepts.
inline constexpr sonare::util::check::NumericDomain kTimeSignatureNumeratorDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the denominator of a time signature segment accepts.
inline constexpr sonare::util::check::NumericDomain kTimeSignatureDenominatorDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the id of a warp map accepts.
inline constexpr sonare::util::check::NumericDomain kWarpMapIdDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);

/// @brief What the sample rate of an external stem import request accepts.
inline constexpr sonare::util::check::NumericDomain kExternalStemSampleRateDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the start frame of an external stem accepts.
inline constexpr sonare::util::check::NumericDomain kExternalStemStartFrameDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);

/// @brief What the grid division of a snap accepts, 0 meaning the project's own grid.
inline constexpr sonare::util::check::NumericDomain kSnapDivisionDomain =
    sonare::util::check::integer_domain(0, 1024);
/// @brief What the entry count of a key or chord annotation list accepts.
inline constexpr sonare::util::check::NumericDomain kAnnotationCountDomain =
    sonare::util::check::integer_domain(0, static_cast<long long>(sonare::kMaxAudioBufferSize));
/// @brief What the root of a chord symbol accepts, a pitch class.
inline constexpr sonare::util::check::NumericDomain kChordRootPitchClassDomain =
    sonare::util::check::integer_domain(SONARE_PITCH_C, SONARE_PITCH_B);

/// @brief What the source id of an original source of a vocal edit accepts.
inline constexpr sonare::util::check::NumericDomain kVocalSourceIdDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<uint32_t>::max() - 1);
/// @brief What the length of an original source of a vocal edit accepts, in frames.
inline constexpr sonare::util::check::NumericDomain kVocalSourceSampleCountDomain =
    sonare::util::check::integer_domain(1, static_cast<long long>(sonare::kMaxAudioBufferSize));
/// @brief What the clip id of a vocal edit binding accepts.
inline constexpr sonare::util::check::NumericDomain kVocalClipIdDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<uint32_t>::max() - 1);
/// @brief What the take id of a vocal edit binding accepts; 0 (not given) is the clip's base
///        source.
inline constexpr sonare::util::check::NumericDomain kVocalTakeIdDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(0, std::numeric_limits<uint32_t>::max() - 1), 0);

}  // namespace sonare_c_detail
