#pragma once

/// @file strip_spec.h
/// @brief Building, comparing and patching a ChannelStrip from a retained mixing::api::Strip.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"

namespace sonare::engine {

/// @param build_sample_rate Rate the strip runs at, or 0 when unknown. An equalizer insert built
///        for a known rate accepts band frequencies up to that rate's Nyquist.
std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec,
                                                                   double build_sample_rate = 0.0);

/// Builds the processor for @p insert, or nullptr when it cannot be built. On success
/// @p stereo_pair_only receives whether the processor only accepts a stereo pair.
std::unique_ptr<rt::ProcessorBase> make_insert_processor(const mixing::api::Insert& insert,
                                                         double build_sample_rate,
                                                         bool* stereo_pair_only);

/// True when two insert chains have the same effective processors and semantic
/// parameters, so a strip or bus can be updated in place instead of rebuilt.
/// Track/master strips use the runtime's combined [pre...post...] order;
/// buses retain their declared order because their send graph addresses it.
bool strip_inserts_equal(const std::vector<mixing::api::Insert>& a,
                         const std::vector<mixing::api::Insert>& b,
                         bool canonical_track_order = true);

/// Applies every scalar of @p next to an existing strip through its setters
/// (smoothed values ramp rather than snap), and its EQ as a diff against
/// @p previous so unchanged bands keep their filter state.
void apply_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& next,
                         const mixing::api::Strip& previous);

/// True when every band of @p eq can be installed at @p sample_rate, so a
/// caller can refuse a spec before applying any part of it.
bool strip_eq_acceptable(const mixing::api::StripEq& eq, double sample_rate) noexcept;

/// Records @p band at @p band_index in a retained spec, padding any gap with
/// default bands.
void store_eq_band(mixing::api::StripEq& eq, size_t band_index, const mastering::eq::EqBand& band);

/// The registered processor name at @p insert_index in @p spec's combined
/// insert order (PreFader entries in spec order, then PostFader entries in
/// spec order) -- the same order ChannelStrip::insert_parameter_id_for_key
/// addresses. nullptr when out of range. Shared by TrackMixerRuntime's lane
/// insert resolution and RealtimeEngine's master-strip one, since both read a
/// retained mixing::api::Strip.
const std::string* strip_insert_processor_name_at(const mixing::api::Strip& spec,
                                                  unsigned int insert_index) noexcept;

}  // namespace sonare::engine
