#include "midi/routing.h"

namespace sonare::midi {
namespace {

// A channel-voice UMP carries a meaningful channel nibble; other message types
// (utility, system, data/SysEx handles) do not and bypass the channel filter
// and channel remap.
bool is_channel_voice(const Ump& ump) noexcept {
  const UmpMessageType type = ump.message_type();
  return type == UmpMessageType::kMidi1ChannelVoice || type == UmpMessageType::kMidi2ChannelVoice;
}

}  // namespace

bool MidiRouter::passes_filter(const Ump& ump) const noexcept {
  if (config_.filter_group != kRouteAnyGroup && ump.group != config_.filter_group) {
    return false;
  }
  if (config_.filter_channel != kRouteAnyChannel && is_channel_voice(ump) &&
      ump.channel() != config_.filter_channel) {
    return false;
  }
  return true;
}

Ump MidiRouter::apply_remap(const Ump& ump) const noexcept {
  if (config_.remap_channel == kRouteNoRemap || !is_channel_voice(ump)) {
    return ump;
  }
  // Rewrite the channel nibble (word[0] bits 16..19) in place. Both MIDI 1.0 and
  // 2.0 channel-voice messages place the channel in the same field.
  Ump out = ump;
  const uint32_t channel = static_cast<uint32_t>(config_.remap_channel & 0x0Fu);
  out.words[0] = (out.words[0] & ~(0x0Fu << 16u)) | (channel << 16u);
  return out;
}

size_t MidiRouter::process(const MidiEvent* input, size_t count, MidiRouteOutput* out) noexcept {
  if (out == nullptr) {
    return 0;
  }
  out->clear();
  if (input == nullptr || !config_.thru) {
    // Thru disabled: emit nothing. Dropping silenced events is intentional and
    // is NOT counted as overflow (there is no output capacity pressure).
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    const MidiEvent& ev = input[i];
    if (!passes_filter(ev.ump)) {
      continue;
    }
    if (out->size >= MidiRouteOutput::kCapacity) {
      out->overflowed = true;
      overflow_count_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    MidiEvent routed;
    routed.render_frame = ev.render_frame;
    routed.ump = apply_remap(ev.ump);
    out->events[out->size++] = routed;
  }
  return out->size;
}

}  // namespace sonare::midi
