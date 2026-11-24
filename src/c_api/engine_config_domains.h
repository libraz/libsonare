#pragma once

/// @file engine_config_domains.h
/// @brief Numeric domains of the engine C-ABI arguments and structs, stated once for the checks
///        that refuse them and for the table in numeric_domain_rows.h.

#include <sonare/sonare_c_engine.h>

#include <cstddef>
#include <cstdint>

#include "core/audio.h"
#include "engine/clip_page_limits.h"
#include "engine/insert_automation_id.h"
#include "engine/realtime_engine.h"
#include "mastering/eq/parametric.h"
#include "mastering/final/dither.h"
#include "mixing/alignment_delay.h"
#include "streaming/realtime_limits.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What a graph spec's num_channels accepts.
inline constexpr sonare::util::check::NumericDomain kEngineGraphChannelsDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a graph spec's node_count accepts.
inline constexpr sonare::util::check::NumericDomain kEngineGraphNodeCountDomain =
    sonare::util::check::integer_domain_of<size_t>(1);
/// @brief What a graph node's num_ports accepts; SONARE_ENGINE_GRAPH_PORTS_FROM_SPEC (-1) takes
///        the count from the spec.
inline constexpr sonare::util::check::NumericDomain kEngineGraphNodePortsDomain =
    sonare::util::check::with_sentinel(sonare::util::check::integer_domain_of<int>(1), -1);
/// @brief What an engine automation parameter id accepts.
inline constexpr sonare::util::check::NumericDomain kEngineParameterIdDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);

/// @brief What a capture buffer's num_channels accepts.
inline constexpr sonare::util::check::NumericDomain kEngineCaptureChannelsDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a capture buffer's capacity_frames accepts.
inline constexpr sonare::util::check::NumericDomain kEngineCaptureCapacityFramesDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What the capture punch window's start_sample accepts.
inline constexpr sonare::util::check::NumericDomain kEngineCapturePunchStartDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);

/// @brief What an audio clip's source channel count accepts.
inline constexpr sonare::util::check::NumericDomain kEngineClipChannelsDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What an audio clip's source length and a page provider's num_samples accept.
inline constexpr sonare::util::check::NumericDomain kEngineClipSamplesDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What an audio or MIDI clip's length_samples accepts when it is not the to-end sentinel.
inline constexpr sonare::util::check::NumericDomain kEngineClipLengthDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What an audio or MIDI clip's fade_in_samples and fade_out_samples accept.
inline constexpr sonare::util::check::NumericDomain kEngineClipFadeDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);
/// @brief What a clip page provider's num_channels accepts.
inline constexpr sonare::util::check::NumericDomain kEngineClipPageChannelsDomain =
    sonare::util::check::integer_domain(1, sonare::engine::kMaxClipPageChannels);
/// @brief What a clip page provider's page_frames accepts.
inline constexpr sonare::util::check::NumericDomain kEngineClipPageFramesDomain =
    sonare::util::check::integer_domain(1, sonare::engine::kMaxClipPageFrames);
/// @brief What the warp voice capacity accepts.
inline constexpr sonare::util::check::NumericDomain kEngineWarpVoicesDomain =
    sonare::util::check::integer_domain(0, SONARE_ENGINE_MAX_WARP_VOICES);
/// @brief What the clip page prefetch frame count accepts.
inline constexpr sonare::util::check::NumericDomain kEnginePrefetchFramesDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);

/// @brief What a controller binding's index accepts.
inline constexpr sonare::util::check::NumericDomain kEngineControllerBindingIndexDomain =
    sonare::util::check::integer_domain(0, 127);

/// @brief What a MIDI channel accepts.
inline constexpr sonare::util::check::NumericDomain kEngineMidiChannelDomain =
    sonare::util::check::integer_domain(0, 15);
/// @brief What a UMP group accepts.
inline constexpr sonare::util::check::NumericDomain kEngineUmpGroupDomain =
    sonare::util::check::integer_domain(0, 15);
/// @brief What a MIDI clip event's word_count accepts; zero asks the reader to infer.
inline constexpr sonare::util::check::NumericDomain kEngineUmpWordCountDomain =
    sonare::util::check::integer_domain(0, 4);
/// @brief A scheduled render frame, with -1 selecting the next block head.
inline constexpr sonare::util::check::NumericDomain kEngineScheduledRenderFrameDomain =
    sonare::util::check::integer_domain_of<int64_t>(-1);
/// @brief What a MIDI clip's loop count accepts.
inline constexpr sonare::util::check::NumericDomain kEngineMidiClipLoopDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What a MIDI clip's loop_length_samples accepts when it loops.
inline constexpr sonare::util::check::NumericDomain kEngineMidiClipLoopLengthDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What the payload size of a pushed SysEx message accepts.
inline constexpr sonare::util::check::NumericDomain kEngineSysExSizeDomain =
    sonare::util::check::integer_domain(
        1, static_cast<long long>(sonare::engine::RealtimeEngine::kMaxSysExPayloadBytes));

/// @brief What an EQ band index accepts.
inline constexpr sonare::util::check::NumericDomain kEngineBandIndexDomain =
    sonare::util::check::integer_domain(
        0, static_cast<long long>(sonare::mastering::eq::ParametricEq::kMaxBands) - 1);
/// @brief What an EQ band index accepts in a build without the mixing engine.
inline constexpr sonare::util::check::NumericDomain kEngineBandIndexFloorDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What an insert index accepts.
inline constexpr sonare::util::check::NumericDomain kEngineInsertIndexDomain =
    sonare::util::check::integer_domain(0, sonare::engine::kInsertIndexMask);
/// @brief What a track mixer's lane_count accepts.
inline constexpr sonare::util::check::NumericDomain kEngineLaneCountDomain =
    sonare::util::check::integer_domain(0, SONARE_ENGINE_MAX_TRACK_LANES);
/// @brief What a track's alignment delay_samples accepts.
inline constexpr sonare::util::check::NumericDomain kEngineAlignmentDelayDomain =
    sonare::util::check::integer_domain(0, sonare::mixing::kMaxAlignmentDelaySamples);

/// @brief What an offline render's num_channels accepts on an engine prepared at the most
///        channels; a render refuses past the prepared count.
inline constexpr sonare::util::check::NumericDomain kEngineRenderChannelsDomain =
    sonare::util::check::integer_domain(
        1, static_cast<long long>(sonare::engine::RealtimeEngine::kMaxAudioChannels));
/// @brief The minimum gap between scope records, or zero to disable them.
inline constexpr sonare::util::check::NumericDomain kEngineScopeIntervalDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief The number of spectrum bands carried by each scope record.
inline constexpr sonare::util::check::NumericDomain kEngineScopeBandCountDomain =
    sonare::util::check::integer_domain(1, SONARE_SCOPE_MAX_BANDS);

/// @brief What the total_frames argument of a render accepts.
inline constexpr sonare::util::check::NumericDomain kEngineRenderFramesDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);
/// @brief What the block_size argument of a render accepts; 0 takes the default offline block.
inline constexpr sonare::util::check::NumericDomain kEngineRenderBlockSizeDomain =
    sonare::util::check::with_sentinel(sonare::util::check::integer_domain_of<int>(0), 0);
/// @brief What the total_frames of bounce and freeze options accepts.
inline constexpr sonare::util::check::NumericDomain kEngineBounceFramesDomain =
    sonare::util::check::integer_domain_of<int64_t>(1);
/// @brief What the block_size of bounce and freeze options accepts when it is not automatic.
inline constexpr sonare::util::check::NumericDomain kEngineOfflineBlockDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a bounce sample rate accepts when it is not the prepared-rate sentinel.
inline constexpr sonare::util::check::NumericDomain kEngineSampleRateDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the dither_bits of bounce options accepts.
inline constexpr sonare::util::check::NumericDomain kEngineDitherBitsDomain =
    sonare::util::check::integer_domain(sonare::mastering::final::kMinDitherTargetBits,
                                        sonare::mastering::final::kMaxDitherTargetBits);

/// @brief What prepare's max_block_size accepts; 0 takes the engine default.
inline constexpr sonare::util::check::NumericDomain kEngineMaxBlockSizeDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(1, sonare::kMaxRealtimeBlockSize), 0);
/// @brief What prepare's command_capacity accepts; 0 takes the engine default.
inline constexpr sonare::util::check::NumericDomain kEngineCommandCapacityDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(0, SONARE_ENGINE_MAX_COMMAND_CAPACITY), 0);
/// @brief What prepare's telemetry_capacity accepts; 0 takes the engine default.
inline constexpr sonare::util::check::NumericDomain kEngineTelemetryCapacityDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(0, SONARE_ENGINE_MAX_TELEMETRY_CAPACITY), 0);
/// @brief What prepare's max_channels accepts; 0 takes the engine default.
inline constexpr sonare::util::check::NumericDomain kEngineMaxChannelsDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(
            1, static_cast<long long>(sonare::engine::RealtimeEngine::kMaxAudioChannels)),
        0);
/// @brief What a seek's timeline_sample accepts.
inline constexpr sonare::util::check::NumericDomain kEngineTimelineSampleDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);
/// @brief What a count-in's start_sample accepts.
inline constexpr sonare::util::check::NumericDomain kEngineCountInStartDomain =
    sonare::util::check::integer_domain_of<int64_t>(0);
/// @brief What a time signature's numerator accepts.
inline constexpr sonare::util::check::NumericDomain kEngineTimeSignatureNumeratorDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a time signature's denominator accepts.
inline constexpr sonare::util::check::NumericDomain kEngineTimeSignatureDenominatorDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What a tempo or time-signature segment count accepts.
inline constexpr sonare::util::check::NumericDomain kEngineSegmentCountDomain =
    sonare::util::check::integer_domain(0, static_cast<long long>(sonare::kMaxAudioBufferSize));
/// @brief What a marker id accepts.
inline constexpr sonare::util::check::NumericDomain kEngineMarkerIdDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);

/// @brief What a mixer lane's track_id and a bus's bus_id accept; 0 means unset.
inline constexpr sonare::util::check::NumericDomain kEngineMixerIdDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);

}  // namespace sonare_c_detail
