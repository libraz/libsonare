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

// Utility (MT 0x0) and System (MT 0x1) UMP messages are global, group-agnostic
// transport/clock/realtime messages (JR timestamp, MIDI clock, start/stop,
// active sensing, ...). They are NOT per-group channel-voice traffic, so the
// per-group filter must NOT drop them; routing them by group would silently
// strip clock/transport from any group-filtered route.
bool is_utility_or_system(const Ump& ump) noexcept {
  const UmpMessageType type = ump.message_type();
  return type == UmpMessageType::kUtility || type == UmpMessageType::kSystem;
}

}  // namespace

bool MidiRouter::passes_filter(const Ump& ump) const noexcept {
  if (config_.filter_group != kRouteAnyGroup && !is_utility_or_system(ump) &&
      ump.group != config_.filter_group) {
    return false;
  }
  if (config_.filter_channel != kRouteAnyChannel && is_channel_voice(ump) &&
      ump.channel() != config_.filter_channel) {
    return false;
  }
  return true;
}

Ump MidiRouter::with_channel(const Ump& ump, uint8_t channel) noexcept {
  // Rewrite the channel nibble (word[0] bits 16..19) in place. Both MIDI 1.0 and
  // 2.0 channel-voice messages place the channel in the same field.
  Ump out = ump;
  const uint32_t value = static_cast<uint32_t>(channel & 0x0Fu);
  out.words[0] = (out.words[0] & ~(0x0Fu << 16u)) | (value << 16u);
  return out;
}

size_t MidiRouter::find_active_remap(uint8_t group, uint8_t src_channel,
                                     uint8_t note) const noexcept {
  for (size_t i = 0; i < active_count_; ++i) {
    if (active_remaps_[i].group == group && active_remaps_[i].src_channel == src_channel &&
        active_remaps_[i].note == note) {
      return i;
    }
  }
  return kMaxActiveRemaps;
}

Ump MidiRouter::apply_remap(const Ump& ump) noexcept {
  if (!is_channel_voice(ump)) {
    return ump;
  }
  const uint8_t src_channel = ump.channel();

  // A note-off must follow its note-on onto the SAME remapped channel, even if
  // the remap config changed mid-note. Look the sounding note up by its original
  // identity and reuse the channel chosen at note-on time.
  if (ump.is_note_off()) {
    const size_t idx = find_active_remap(ump.group, src_channel, ump.note_number());
    if (idx != kMaxActiveRemaps) {
      const uint8_t out_channel = active_remaps_[idx].out_channel;
      // Swap-remove; order of sounding notes is not significant.
      active_remaps_[idx] = active_remaps_[active_count_ - 1];
      --active_count_;
      return with_channel(ump, out_channel);
    }
    // No tracked note-on (e.g. a stray note-off): fall through to the current
    // config so behaviour matches a plain remap.
  }

  const uint8_t out_channel =
      config_.remap_channel == kRouteNoRemap ? src_channel : config_.remap_channel;

  // Record the chosen channel for a note-on so the matching note-off is stable.
  if (ump.is_note_on()) {
    const uint8_t note = ump.note_number();
    const size_t existing = find_active_remap(ump.group, src_channel, note);
    if (existing != kMaxActiveRemaps) {
      active_remaps_[existing].out_channel = out_channel;
    } else if (active_count_ < kMaxActiveRemaps) {
      active_remaps_[active_count_++] = ActiveRemap{ump.group, src_channel, note, out_channel};
    }
    // On table overflow the note is still remapped with the current config; only
    // the stable-pair guarantee is dropped for the surplus note.
  }

  if (config_.remap_channel == kRouteNoRemap) {
    return ump;
  }
  return with_channel(ump, out_channel);
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
      overflow_count_.bump();
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
