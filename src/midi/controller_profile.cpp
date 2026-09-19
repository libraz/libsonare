#include "midi/controller_profile.h"

#include <cmath>
#include <iterator>

namespace sonare::midi {
namespace {

// Full-scale denominators of the protocol fields a gesture arrives in. Local
// rather than shared: these are message widths, not quantities.
constexpr float kMidi1ControllerMax = 127.0f;
constexpr float kMidi1BendMax = 16383.0f;
constexpr float kMidi2VelocityMax = 65535.0f;
constexpr float kMidi2FieldMax = 4294967295.0f;

constexpr uint8_t kBreathCc = 2;
constexpr uint8_t kTimbreCc = 74;

bool is_midi_channel_voice(const Ump& ump) noexcept {
  return ump.message_type() == UmpMessageType::kMidi1ChannelVoice ||
         ump.message_type() == UmpMessageType::kMidi2ChannelVoice;
}

/// CC74 reaches both spellings of the second axis. They never coexist — a bowed
/// string has no bell and nothing else has a contact point — so one gesture
/// filling both is unambiguous rather than a guess.
bool bind_second_axis(ControllerProfile* out) noexcept {
  return out->bind({ControllerInput::kControlChange, kTimbreCc, ControllerAxis::kBrightness}) &&
         out->bind({ControllerInput::kControlChange, kTimbreCc, ControllerAxis::kPosition});
}

bool build_gm(ControllerProfile* out) noexcept {
  out->velocity_meaningful = true;
  return out->bind({ControllerInput::kControlChange, kBreathCc, ControllerAxis::kExcitation}) &&
         bind_second_axis(out);
}

/// Breath drives loudness as well as timbre. Neither of the two modelled wind
/// synthesizers with a published design keeps them apart: one derives volume,
/// timbre and articulation from a single breath axis, the other defines its
/// pressure control as changing both and adds a separate volume-only control on
/// top. Holding breath to timbre alone is a consequence of a bore whose valve
/// stops buzzing when overdriven, not a convention.
bool build_breath(ControllerProfile* out) noexcept {
  out->velocity_meaningful = false;
  return out->bind({ControllerInput::kControlChange, kBreathCc, ControllerAxis::kExcitation}) &&
         out->bind({ControllerInput::kControlChange, kBreathCc, ControllerAxis::kLoudness}) &&
         bind_second_axis(out);
}

/// Both spellings of breath, for a controller that ships sending CC2 and channel
/// aftertouch together. They carry one gesture, so the later message replacing
/// the earlier is the merge rule — summing them would double the gesture.
bool build_breath_aftertouch(ControllerProfile* out) noexcept {
  out->velocity_meaningful = false;
  return build_breath(out) &&
         out->bind({ControllerInput::kChannelPressure, 0, ControllerAxis::kExcitation}) &&
         out->bind({ControllerInput::kChannelPressure, 0, ControllerAxis::kLoudness});
}

/// The three per-note dimensions MPE defines are bend, CC74 and channel
/// pressure. Bend keeps its own protocol meaning, and MPE defines no per-note
/// loudness, so pressure lands on excitation alone.
bool build_mpe(ControllerProfile* out) noexcept {
  out->velocity_meaningful = true;
  return out->bind({ControllerInput::kChannelPressure, 0, ControllerAxis::kExcitation}) &&
         bind_second_axis(out);
}

struct PresetEntry {
  const char* name;
  bool (*build)(ControllerProfile*) noexcept;
};

constexpr PresetEntry kPresets[] = {
    {"gm", build_gm},
    {"breath", build_breath},
    {"breath-aftertouch", build_breath_aftertouch},
    {"mpe", build_mpe},
};

}  // namespace

bool controller_input_of(const Ump& ump, ControllerInputValue* out) noexcept {
  if (out == nullptr || !is_midi_channel_voice(ump)) return false;
  const bool midi1 = ump.message_type() == UmpMessageType::kMidi1ChannelVoice;
  ControllerInputValue value{};
  value.channel = ump.channel();
  if (ump.is_note_on()) {
    value.input = ControllerInput::kVelocity;
    value.norm = midi1 ? static_cast<float>(ump.data2_7bit()) / kMidi1ControllerMax
                       : static_cast<float>(ump.words[1] >> 16) / kMidi2VelocityMax;
  } else if (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    value.input = ControllerInput::kControlChange;
    value.index = ump.note_number();
    value.norm = midi1 ? static_cast<float>(ump.data2_7bit()) / kMidi1ControllerMax
                       : static_cast<float>(ump.words[1]) / kMidi2FieldMax;
  } else if (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure)) {
    value.input = ControllerInput::kChannelPressure;
    value.norm = midi1 ? static_cast<float>(ump.note_number()) / kMidi1ControllerMax
                       : static_cast<float>(ump.words[1]) / kMidi2FieldMax;
  } else if (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure)) {
    value.input = ControllerInput::kPolyPressure;
    value.note = ump.note_number();
    value.norm = midi1 ? static_cast<float>(ump.data2_7bit()) / kMidi1ControllerMax
                       : static_cast<float>(ump.words[1]) / kMidi2FieldMax;
  } else if (ump.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    value.input = ControllerInput::kPitchBend;
    const uint32_t bend14 =
        (static_cast<uint32_t>(ump.data2_7bit()) << 7) | static_cast<uint32_t>(ump.note_number());
    value.norm = midi1 ? static_cast<float>(bend14) / kMidi1BendMax
                       : static_cast<float>(ump.words[1]) / kMidi2FieldMax;
  } else {
    return false;
  }
  *out = value;
  return true;
}

float controller_map_value(const ControllerBinding& binding, float norm) noexcept {
  float shaped = norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
  // Skipped rather than evaluated at 1.0: a linear binding must reproduce its
  // input exactly, and pow() is not obliged to be the identity there.
  if (binding.curve != 1.0f) shaped = std::pow(shaped, binding.curve);
  return binding.lo + (binding.hi - binding.lo) * shaped;
}

bool ControllerProfile::preset(std::string_view name, ControllerProfile* out) noexcept {
  if (out == nullptr) return false;
  for (const PresetEntry& entry : kPresets) {
    if (name != entry.name) continue;
    ControllerProfile built;
    if (!entry.build(&built)) return false;
    *out = built;
    return true;
  }
  return false;
}

size_t ControllerProfile::preset_count() noexcept { return std::size(kPresets); }

const char* ControllerProfile::preset_name_at(size_t index) noexcept {
  return index < std::size(kPresets) ? kPresets[index].name : nullptr;
}

bool ControllerProfile::bind(const ControllerBinding& binding) noexcept {
  if (binding.axis == ControllerAxis::kNone) return false;
  if (binding.input == ControllerInput::kPolyPressure &&
      !controller_axis_is_excitation(binding.axis)) {
    return false;
  }
  if (count_ >= kMaxControllerBindings) return false;
  bindings_[count_++] = binding;
  return true;
}

void ControllerProfile::clear() noexcept {
  bindings_ = {};
  count_ = 0;
}

size_t ControllerProfile::resolve(const Ump& ump, ControllerAxisValue* out,
                                  size_t cap) const noexcept {
  if (out == nullptr || cap == 0) return 0;
  ControllerInputValue in{};
  if (!controller_input_of(ump, &in)) return 0;
  size_t written = 0;
  for (size_t i = 0; i < count_ && written < cap; ++i) {
    const ControllerBinding& binding = bindings_[i];
    if (binding.input != in.input) continue;
    if (binding.input == ControllerInput::kControlChange && binding.index != in.index) continue;
    out[written].axis = binding.axis;
    out[written].value = controller_map_value(binding, in.norm);
    out[written].note = in.note;
    ++written;
  }
  return written;
}

}  // namespace sonare::midi
