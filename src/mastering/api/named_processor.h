#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mastering/api/result_types.h"

namespace sonare::mastering::api {

struct Param {
  std::string key;
  double value = 0.0;
};

// Per-processor results share the same shape as the shared audio-result base
// types. They are exposed as aliases so callers (binding layers, tests) can
// keep using `MonoResult` / `StereoResult` while the field definitions live
// in a single place (@ref result_types.h).
using MonoResult = MonoAudioResult;
using StereoResult = StereoAudioResult;

std::vector<std::string> processor_names();

MonoResult apply_named_processor(const std::string& name, const float* samples, std::size_t length,
                                 int sample_rate, const std::vector<Param>& params = {});

StereoResult apply_named_processor_stereo(const std::string& name, const float* left,
                                          const float* right, std::size_t length, int sample_rate,
                                          const std::vector<Param>& params = {});

// Subset of processor_names() that have no mono implementation and must be run
// through apply_named_processor_stereo(). Calling apply_named_processor() (the
// mono entry point) with one of these names throws InvalidParameter. Exposed so
// callers can distinguish "stereo-only processor" from "unknown processor".
std::vector<std::string> stereo_processor_names();

std::vector<std::string> pair_processor_names();
std::vector<std::string> pair_analysis_names();
std::vector<std::string> stereo_analysis_names();

// How a processor handles a buffer with more than two channels (a surround
// bed). The mixer consults this to wrap a >2ch bus insert correctly instead of
// inferring channel handling (which is silent-data-loss-prone). Values are
// stable wire strings in the catalog JSON.
enum class ChannelPolicy : uint8_t {
  // One full-buffer call processes every plane correctly. Every per-channel and
  // linked-dynamics processor (the bulk of the catalog) — their per-channel
  // state is sized to the prepared channel count and bounds-checked. The
  // default for any id not explicitly classified otherwise.
  Multichannel,
  // Operates on planes 0/1 (front L/R) and leaves planes 2..N untouched: the
  // stereo-image processors plus all reverb/modulation/delay effects, which
  // already touch only 0/1. The mixer hands these a 2-plane view of {0,1}.
  StereoPairOnly,
  // Strictly one channel at a time (no linking). Unused by the current catalog;
  // reserved for a hypothetical mono-only processor.
  PerChannel,
  // Untouched (dry). Reserved; unused by the current catalog.
  Passthrough,
};

// Channel policy for a processor id. Returns StereoPairOnly for the inherently
// stereo set (stereo-image processors, eq.midSide, multiband.imager, and every
// reverb/modulation/delay effect); Multichannel for everything else, including
// any unknown/legacy id (one full-buffer call never drops channels).
ChannelPolicy channel_policy(const std::string& id);

// Stable catalog/wire string for a ChannelPolicy
// ("multichannel" | "stereoPairOnly" | "perChannel" | "passthrough").
const char* channel_policy_to_string(ChannelPolicy policy) noexcept;

// Machine-readable classification catalog for every named processor id, merging
// the offline registry (processor_names), the realtime insert factory
// (insert_factory_names), and the pair registry (pair_processor_names) into a
// single JSON array. Each entry is
//   {"id":string,"kind":"realtime"|"offline"|"pair",
//    "realtimeInsertable":bool,"stereoOnly":bool,"channelPolicy":string}
// where kind is pair > realtime > offline by precedence, realtimeInsertable is
// membership in insert_factory_names(), stereoOnly is membership in
// stereo_processor_names(), and channelPolicy is channel_policy(id) as a wire
// string (how the mixer wraps a >2ch bus insert). The id universe is the union
// of the three lists, so
// realtime-only ids (e.g. effects.reverb.room) and pair ids that are absent from
// processor_names() are still reported. Ids are emitted in sorted order.
std::string processor_catalog_json();

// Apply a two-input "match.*" processor. The source and reference buffers may
// have different lengths; the underlying match primitives consume each buffer at
// its own length. The single-length overload delegates with
// reference_length == length for backward compatibility.
MonoResult apply_named_pair_processor(const std::string& name, const float* source,
                                      const float* reference, std::size_t source_length,
                                      std::size_t reference_length, int sample_rate,
                                      const std::vector<Param>& params = {});

MonoResult apply_named_pair_processor(const std::string& name, const float* source,
                                      const float* reference, std::size_t length, int sample_rate,
                                      const std::vector<Param>& params = {});

std::string analyze_named_pair(const std::string& name, const float* source, const float* reference,
                               std::size_t source_length, std::size_t reference_length,
                               int sample_rate, const std::vector<Param>& params = {});

std::string analyze_named_pair(const std::string& name, const float* source, const float* reference,
                               std::size_t length, int sample_rate,
                               const std::vector<Param>& params = {});

std::string analyze_named_stereo(const std::string& name, const float* left, const float* right,
                                 std::size_t length, int sample_rate,
                                 const std::vector<Param>& params = {});

}  // namespace sonare::mastering::api
