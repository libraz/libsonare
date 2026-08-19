#pragma once

/// @file instrument_automation_id.h
/// @brief Reserved parameter-id encoding for realtime instrument-parameter
///        automation.

#include <cstdint>

namespace sonare::engine {

/// Reserved engine-namespace id encoding for automating a continuous parameter
/// of a hosted instrument (a NativeSynth patch scalar) from a PPQ breakpoint
/// lane.
///
/// Three id namespaces now share the 32-bit target space and must stay
/// mutually exclusive:
///
///     mixer fader/pan/width : 0x4D580000 (top 3 bits 010)
///     strip inserts         : 0xE0000000 (top 3 bits 111)
///     instrument params     : 0xC0000000 (top 3 bits 110)  <- this file
///
/// The insert namespace tests `(id & 0xE0000000) == 0xE0000000`, so the 110
/// octant is disjoint from it under the same mask and a single bit test still
/// separates the three.
///
///     [31:29] tag = 110
///     [28:16] instrument slot (13 bits)
///     [15:8]  reserved (0)
///     [7:0]   param (8 bits)
///
/// The slot field indexes the engine's instrument-automation destination table,
/// NOT the InstrumentRack's physical slot. The rack hands out the first free
/// slot, so an unbind/rebind cycle would otherwise silently retarget a live
/// automation lane at a different instrument. The destination table is keyed by
/// destination_id and entries are never reused for a different destination, so
/// a stale id resolves to an unbound destination and applies nothing.
constexpr uint32_t kInstrumentParamTag = 0xC0000000u;
constexpr uint32_t kInstrumentParamMask = 0xE0000000u;
constexpr uint32_t kInstrumentSlotShift = 16u;
constexpr uint32_t kInstrumentSlotMask = 0x1FFFu;
constexpr uint32_t kInstrumentParamFieldMask = 0xFFu;

/// Composes a reserved instrument-automation parameter id from a destination
/// slot and the instrument's integer param id. Each field is masked to its
/// width so an out-of-range argument cannot bleed into a neighbour.
constexpr uint32_t make_instrument_param_id(uint32_t slot, uint32_t param_id) noexcept {
  return kInstrumentParamTag | ((slot & kInstrumentSlotMask) << kInstrumentSlotShift) |
         (param_id & kInstrumentParamFieldMask);
}

/// True when @p id carries the reserved instrument-automation tag.
constexpr bool is_instrument_param_id(uint32_t id) noexcept {
  return (id & kInstrumentParamMask) == kInstrumentParamTag;
}

constexpr uint32_t instrument_param_slot(uint32_t id) noexcept {
  return (id >> kInstrumentSlotShift) & kInstrumentSlotMask;
}

constexpr uint32_t instrument_param_param(uint32_t id) noexcept {
  return id & kInstrumentParamFieldMask;
}

}  // namespace sonare::engine
