#include "midi/clip_chase.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "midi/controller_numbers.h"
#include "midi/midi_event.h"
#include "midi/ump.h"
#include "util/numeric_validation.h"

namespace sonare::midi::detail {

bool is_reset_key(uint32_t key) noexcept {
  const uint32_t kind = key & ((1u << kChaseKindBits) - 1u);
  return kind == kChaseKindReset || kind == kChaseKindSystemReset;
}

/// Whether Reset All Controllers returns the state of @p key to the default the
/// controller reset a seek or wrap sends already restores.
bool reset_covers(uint32_t key) noexcept {
  switch (key & ((1u << kChaseKindBits) - 1u)) {
    case cc::kModulationWheel:
    case cc::kBreathController:
    case cc::kExpression:
    case cc::kDamperPedal:
    case cc::kPortamentoOnOff:
    case cc::kSostenuto:
    case cc::kSoftPedal:
    case kChaseKindChannelPressure:
    case kChaseKindPitchBend:
      return true;
    default:
      return false;
  }
}

/// Whether a SysEx payload (F0/F7 framing optional) is a GM, GS or XG system reset.
bool is_system_reset_sysex(const uint8_t* payload, size_t size) noexcept {
  if (payload == nullptr) return false;
  if (size > 0 && payload[0] == 0xF0u) {
    ++payload;
    --size;
  }
  if (size > 0 && payload[size - 1] == 0xF7u) --size;
  const bool gm = size == 4 && payload[0] == 0x7Eu && payload[2] == 0x09u &&
                  (payload[3] == 0x01u || payload[3] == 0x03u);
  const bool gs = size == 9 && payload[0] == 0x41u && payload[2] == 0x42u && payload[3] == 0x12u &&
                  payload[4] == 0x40u && payload[5] == 0x00u && payload[6] == 0x7Fu &&
                  payload[7] == 0x00u && payload[8] == 0x41u;
  const bool xg = size == 7 && payload[0] == 0x43u && (payload[1] & 0xF0u) == 0x10u &&
                  payload[2] == 0x4Cu && payload[3] == 0x00u && payload[4] == 0x00u &&
                  payload[5] == 0x7Eu && payload[6] == 0x00u;
  return gm || gs || xg;
}

/// The chase key of an event that resets channel state: Reset All Controllers on its
/// channel, or a system reset SysEx for the whole destination.
bool reset_chase_key(const MidiEvent& event, uint32_t* key) noexcept {
  const Ump& ump = event.ump;
  if (is_sysex_event(event)) {
    if (!is_system_reset_sysex(event.sysex_payload, event.sysex_payload_size)) return false;
    *key = kChaseKindSystemReset;
    return true;
  }
  if (!is_channel_voice(ump) ||
      static_cast<UmpStatus>(ump.status_nibble()) != UmpStatus::kControlChange ||
      ump.note_number() != cc::kResetAllControllers) {
    return false;
  }
  *key = ((static_cast<uint32_t>(ump.group & 0x0Fu) << 4 | ump.channel()) << kChaseKindBits) |
         kChaseKindReset;
  return true;
}

/// Controllers whose meaning depends on the messages around them, so no single value is a state.
bool is_sequence_controller(uint8_t controller) noexcept {
  return controller == cc::kDataEntryMsb || controller == cc::kDataEntryLsb ||
         controller == cc::kPortamentoControl ||
         (controller >= cc::kDataIncrement && controller <= cc::kRpnMsb) ||
         controller >= cc::kAllSoundOff;
}

/// The chase key of a message setting a channel state, or false for any other message.
bool chase_key(const Ump& ump, uint32_t* key) noexcept {
  if (!is_channel_voice(ump)) return false;
  uint32_t kind = 0;
  switch (static_cast<UmpStatus>(ump.status_nibble())) {
    case UmpStatus::kControlChange:
      if (is_sequence_controller(ump.note_number())) return false;
      kind = ump.note_number();
      break;
    case UmpStatus::kProgramChange:
      kind = kChaseKindProgram;
      break;
    case UmpStatus::kChannelPressure:
      kind = kChaseKindChannelPressure;
      break;
    case UmpStatus::kPitchBend:
      kind = kChaseKindPitchBend;
      break;
    case UmpStatus::kRegisteredController:
    case UmpStatus::kAssignableController:
      if (!is_registered_or_assignable_controller(ump)) return false;
      kind = (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kRegisteredController)
                  ? kChaseKindRegistered
                  : kChaseKindAssignable) |
             (static_cast<uint32_t>(ump.note_number()) << 7) | (ump.words[0] & 0x7Fu);
      break;
    default:
      return false;
  }
  *key = ((static_cast<uint32_t>(ump.group & 0x0Fu) << 4 | ump.channel()) << kChaseKindBits) | kind;
  return true;
}

/// Emission pass of a chase key: bank select, then program, then the rest, bend last.
int chase_pass(uint32_t key) noexcept {
  const uint32_t kind = key & ((1u << kChaseKindBits) - 1u);
  if (kind == cc::kBankSelectMsb || kind == cc::kBankSelectLsb) return 0;
  if (kind == kChaseKindProgram) return 1;
  if (kind == kChaseKindChannelPressure || kind == kChaseKindPitchBend) return 3;
  return 2;
}

std::shared_ptr<const MidiClipChaseIndex> build_chase_index(const MidiClipSchedule& clip) {
  std::vector<std::pair<uint32_t, uint32_t>> keyed;
  for (size_t i = 0; i < clip.events.size(); ++i) {
    uint32_t key = 0;
    if (reset_chase_key(clip.events[i], &key) || chase_key(clip.events[i].ump, &key)) {
      keyed.emplace_back(key, static_cast<uint32_t>(i));
    }
  }
  if (keyed.empty()) return nullptr;
  std::sort(keyed.begin(), keyed.end());
  auto index = std::make_shared<MidiClipChaseIndex>();
  index->event_indices.reserve(keyed.size());
  for (size_t i = 0; i < keyed.size(); ++i) {
    if (i == 0 || keyed[i].first != keyed[i - 1].first) {
      const auto at = static_cast<uint32_t>(i);
      index->runs.push_back({keyed[i].first, at, at});
    }
    index->event_indices.push_back(keyed[i].second);
    index->runs.back().end = static_cast<uint32_t>(i + 1);
  }
  return index;
}

/// The event of one chase run in force just before timeline frame @p at, and the
/// timeline frame it played at, under the same placement rules visit_scheduled uses.
const MidiEvent* chased_event(const MidiClipSchedule& clip, const uint32_t* first,
                              const uint32_t* last, int64_t at, int64_t* frame) noexcept {
  const auto frame_of = [&](uint32_t index) noexcept { return clip.events[index].render_frame; };
  // The last index in [first, last) whose event lies before @p bound, or nullptr.
  const auto last_before = [&](int64_t bound) noexcept -> const uint32_t* {
    const uint32_t* it = std::partition_point(
        first, last, [&](uint32_t index) noexcept { return frame_of(index) < bound; });
    return it == first ? nullptr : it - 1;
  };
  if (clip.loop_mode == MidiLoopMode::kLoop && clip.loop_length_samples > 0) {
    const int64_t bound = std::min(at, saturating_clip_end(clip));
    if (bound <= clip.start_sample) return nullptr;
    const int64_t loop_len = clip.loop_length_samples;
    const int64_t phase = loop_phase(clip, bound);
    const int64_t iter_start = numeric::saturating_sub(bound, phase);
    const uint32_t* hit = last_before(numeric::saturating_add(clip.start_sample, phase));
    int64_t base = iter_start;
    if ((hit == nullptr || frame_of(*hit) < clip.start_sample) && iter_start > clip.start_sample) {
      // Nothing yet in this iteration: the previous one ran to its end.
      hit = last_before(numeric::saturating_add(clip.start_sample, loop_len));
      base = numeric::saturating_sub(iter_start, loop_len);
    }
    if (hit == nullptr || frame_of(*hit) < clip.start_sample) return nullptr;
    *frame = numeric::saturating_add(base, frame_of(*hit) - clip.start_sample);
    return &clip.events[*hit];
  }
  const bool finite_one_shot = clip.loop_mode == MidiLoopMode::kOneShot && clip.length_samples > 0;
  const uint32_t* hit = last_before(finite_one_shot ? std::min(at, saturating_clip_end(clip)) : at);
  if (hit == nullptr) return nullptr;
  *frame = frame_of(*hit);
  return &clip.events[*hit];
}

}  // namespace sonare::midi::detail
