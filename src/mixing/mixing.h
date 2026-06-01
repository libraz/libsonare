#pragma once

/// @file mixing.h
/// @brief Optional mixing library umbrella header.
///
/// @par Thread-safety overview
/// The mixing library follows the same audio-thread / control-thread split as
/// @c sonare::engine::RealtimeEngine:
/// - @c ChannelStrip::process / @c process_at, @c BusProcessor::process,
///   @c AutomationLane::pull, and all SPSC telemetry consumers (meter taps,
///   goniometer reads) are **audio-thread-safe** (noexcept, allocation-free
///   once @c prepare has run with the working block size and processor count).
/// - All structural mutations — @c add_insert, @c add_pre/post_insert,
///   @c add_send, @c set_eq_position, @c set_pan_law, etc. — and JSON-driven
///   helpers (@c scene_from_json, @c scene_preset_from_string) are
///   **control-thread-only**: they may allocate, throw
///   @c sonare::SonareException (InvalidParameter | InvalidState), and must
///   not run concurrently with the audio thread on the same strip/bus.
/// - Parameter automation is delivered via @c AutomationLane, a SPSC queue
///   where the control thread is the sole producer and the audio thread the
///   sole consumer. See @c automation_lane.h for the full contract.

#include "mixing/alignment_delay.h"
#include "mixing/api/presets.h"
#include "mixing/api/scene.h"
#include "mixing/automation_lane.h"
#include "mixing/bus.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/gain.h"
#include "mixing/goniometer_buffer.h"
#include "mixing/meter.h"
#include "mixing/mixer_controller.h"
#include "mixing/pan_law.h"
#include "mixing/panner.h"
#include "mixing/send.h"
#include "mixing/stereo_width.h"
#include "mixing/vca_group.h"
