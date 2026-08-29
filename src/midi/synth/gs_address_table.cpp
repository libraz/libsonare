#include "midi/synth/gs_address_table.h"

namespace sonare::midi::synth {

namespace {

/// Roland DT1/RQ1 checksum: the address and data bytes sum to a multiple of 128
/// with the trailing byte.
bool valid_roland_checksum(const uint8_t* body, size_t size) noexcept {
  uint32_t sum = 0;
  for (size_t i = 4; i + 1 < size; ++i) {
    sum += body[i];
  }
  return body[size - 1] == static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
}

}  // namespace

const char* gs_param_name(GsParam param) noexcept {
  switch (param) {
#define SONARE_GS_PARAM_CASE(name) \
  case GsParam::name:              \
    return #name;
    SONARE_GS_PARAMS(SONARE_GS_PARAM_CASE)
#undef SONARE_GS_PARAM_CASE
  }
  return "?";
}

GsFrame gs_sysex_frame(const uint8_t* data, size_t size) noexcept {
  GsFrame frame;
  if (data == nullptr || size == 0) return frame;
  // Strip optional F0 ... F7 framing.
  if (data[0] == 0xF0) {
    ++data;
    --size;
  }
  if (size > 0 && data[size - 1] == 0xF7) --size;
  // 41 dd mm cc aa bb cc <data...> sum: the shortest useful frame carries one
  // data byte.
  if (size < 9 || data[0] != kRolandManufacturerId) return frame;
  for (size_t i = 1; i < size; ++i) {
    if ((data[i] & 0x80u) != 0) return frame;  // not a 7-bit SysEx body
  }
  if (!valid_roland_checksum(data, size)) return frame;

  frame.valid = true;
  frame.device = data[1];
  frame.model = data[2];
  frame.command = data[3];
  frame.addr = (static_cast<uint32_t>(data[4]) << 16) | (static_cast<uint32_t>(data[5]) << 8) |
               static_cast<uint32_t>(data[6]);
  frame.data = data + 7;
  frame.len = size - 8;
  return frame;
}

const GsAddressEntry* gs_lookup_address(uint32_t addr) noexcept {
  for (const GsAddressEntry& entry : kGsAddressTable) {
    if (detail::gs_row_claims(entry, addr)) return &entry;
  }
  return nullptr;
}

const GsAddressRange* gs_lookup_range(uint32_t addr) noexcept {
  for (const GsAddressRange& range : kGsUndefinedRanges) {
    if (addr >= range.lo_addr && addr <= range.hi_addr) return &range;
  }
  return nullptr;
}

uint8_t gs_address_block_index(uint32_t addr, uint32_t mask) noexcept {
  if ((mask & 0x000F00u) != 0) {
    const uint8_t nibble = static_cast<uint8_t>((addr >> 8) & 0x0Fu);
    const uint8_t block = static_cast<uint8_t>((addr >> 8) & 0xF0u);
    const bool part_block =
        ((addr >> 16) & 0x7Fu) == 0x40 && (block == 0x10 || block == 0x20 || block == 0x40);
    return part_block ? gs_part_block_to_channel(nibble) : nibble;
  }
  if ((mask & 0x00F000u) != 0) return static_cast<uint8_t>((addr >> 12) & 0x0Fu);
  return 0;
}

size_t gs_decode_writes(const GsFrame& frame, GsWrite* out, size_t capacity,
                        uint32_t* unknown_writes) noexcept {
  if (!frame.valid || frame.command != kGsCommandDt1 || frame.model != kGsModelId) return 0;
  if (frame.data == nullptr) return 0;

  size_t produced = 0;
  for (size_t i = 0; i < frame.len; ++i) {
    GsWrite write;
    write.addr = gs_address_offset(frame.addr, static_cast<uint32_t>(i));
    write.value = static_cast<uint8_t>(frame.data[i] & 0x7Fu);
    if (const GsAddressEntry* entry = gs_lookup_address(write.addr)) {
      write.param = entry->param;
      write.part = gs_address_block_index(write.addr, entry->mask);
      write.index = (entry->mask & 0xFFu) != 0
                        ? static_cast<uint8_t>(write.addr & 0xFFu)
                        : static_cast<uint8_t>((write.addr & 0xFFu) - (entry->addr & 0xFFu));
    } else if (gs_lookup_range(write.addr) != nullptr) {
      write.param = GsParam::kUndefined;
    } else if (unknown_writes != nullptr) {
      ++*unknown_writes;
    }
    if (out != nullptr && produced < capacity) out[produced] = write;
    ++produced;
  }
  return produced;
}

size_t gs_decode_sysex(const uint8_t* data, size_t size, GsWrite* out, size_t capacity,
                       uint32_t* unknown_writes) noexcept {
  return gs_decode_writes(gs_sysex_frame(data, size), out, capacity, unknown_writes);
}

}  // namespace sonare::midi::synth
