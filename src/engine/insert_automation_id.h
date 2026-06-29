#pragma once

/// @file insert_automation_id.h
/// @brief Reserved parameter-id encoding for realtime insert-parameter automation.

#include <cstdint>

namespace sonare::engine {

/// Reserved engine-namespace id encoding for automating an insert parameter of a
/// mixer strip (track lane, master, or bus) from a PPQ breakpoint lane.
///
/// The mixer fader/pan/width namespace (0x4D580000, top bits 010) fills its low
/// 16 bits, leaving no room for the (strip, insert, param) triple an insert needs.
/// Insert ids therefore live in a disjoint octant (top 3 bits 111) so the two
/// namespaces never collide and a single bit test distinguishes them:
///
///     [31:29] tag = 111
///     [28:16] strip  (13 bits)
///     [15:8]  insert (8 bits)
///     [7:0]   param  (8 bits)
///
/// The strip field selects a track lane by index, the master strip
/// (kInsertStripMaster), or a bus (kInsertStripBusBase - bus_index). The whole id
/// fits in 32 bits, so no handle table is needed.
constexpr uint32_t kInsertParamTag = 0xE0000000u;
constexpr uint32_t kInsertParamMask = 0xE0000000u;
constexpr uint32_t kInsertStripShift = 16u;
constexpr uint32_t kInsertIndexShift = 8u;
constexpr uint32_t kInsertStripMask = 0x1FFFu;
constexpr uint32_t kInsertIndexMask = 0xFFu;
constexpr uint32_t kInsertParamFieldMask = 0xFFu;
// Strip selector for the master strip and the bus range (bus N occupies
// kInsertStripBusBase - N, growing downward from the master sentinel).
constexpr uint32_t kInsertStripMaster = 0x1FFFu;
constexpr uint32_t kInsertStripBusBase = 0x1FFEu;

/// Composes a reserved insert-automation parameter id from a strip selector,
/// insert index, and the processor's integer param id. Each field is masked to
/// its width so an out-of-range argument cannot bleed into a neighbour.
constexpr uint32_t make_insert_param_id(uint32_t strip_selector, uint32_t insert_index,
                                        uint32_t param_id) noexcept {
  return kInsertParamTag | ((strip_selector & kInsertStripMask) << kInsertStripShift) |
         ((insert_index & kInsertIndexMask) << kInsertIndexShift) |
         (param_id & kInsertParamFieldMask);
}

/// True when @p id carries the reserved insert-automation tag.
constexpr bool is_insert_param_id(uint32_t id) noexcept {
  return (id & kInsertParamMask) == kInsertParamTag;
}

constexpr uint32_t insert_param_strip(uint32_t id) noexcept {
  return (id >> kInsertStripShift) & kInsertStripMask;
}

constexpr uint32_t insert_param_index(uint32_t id) noexcept {
  return (id >> kInsertIndexShift) & kInsertIndexMask;
}

constexpr uint32_t insert_param_param(uint32_t id) noexcept { return id & kInsertParamFieldMask; }

}  // namespace sonare::engine
