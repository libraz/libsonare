#pragma once

/// @file controller_profile.h
/// @brief How a device spells a gesture -> which expression axis it means.
///
/// A wind controller sends breath on CC2, or on CC11, or on channel aftertouch,
/// or on two of them at once, and the next instrument spells it differently
/// again. The profile is the one place that mapping lives, so an engine axis is
/// reached by what a gesture means rather than by the controller number that
/// happened to carry it.
///
/// Layering: depends on midi/ump only. It names axes, never engines — the synth
/// owns the axis -> engine binding (synth/excitation_axes.h) and nothing here
/// knows a synthesis mode. It is also not midi/cc_map.h: that one targets
/// automation parameter ids on the control thread, while an axis is evaluated
/// per voice per sample.
///
/// Threading / RT contract
///  - CONTROL thread: preset() / bind() / clear() mutate the binding table.
///  - AUDIO thread: resolve() is a pure scan of the fixed-capacity table — no
///    allocation, no lock, no mutation.
///
/// Determinism: no clock and no random; a message resolves from its own bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "midi/note_tracking.h"
#include "midi/ump.h"

namespace sonare::midi {

/// Input-side identity: how the device spells the gesture. Aftertouch, velocity
/// and bend sit in the same enumeration as a controller number because a device
/// assigns them to the same slot a CC would take.
enum class ControllerInput : uint8_t {
  kControlChange = 0,  ///< ControllerBinding::index is the CC number (0..127)
  kChannelPressure = 1,
  kPolyPressure = 2,  ///< per note; the note number is dynamic, not bound
  kPitchBend = 3,
  kVelocity = 4,  ///< note-on velocity
};

/// Output-side meaning. One entry per destination the synth already owns, and
/// grown only with them: a modulation macro (growl, vibrato as a gesture) is a
/// shape over an axis rather than an axis, and does not belong here.
enum class ControllerAxis : uint8_t {
  kNone = 0,
  kExcitation = 1,    ///< -> ModDestination::kExcitationForce, [0,1]
  kPosition = 2,      ///< -> kExcitationPosition, [0,1]
  kBrightness = 3,    ///< -> kExcitationBrightness, [0,1]
  kMorph = 4,         ///< -> kSpectrumMorph, [0,1]
  kLoudness = 5,      ///< -> the shared expression VCA, a gain factor
  kPitchCents = 6,    ///< -> pitch offset, cents
  kVibratoDepth = 7,  ///< -> kVibratoDepthCents, cents at full LFO
};

/// Number of ControllerAxis values including kNone, so an array indexed by the
/// enum covers it.
inline constexpr size_t kControllerAxisCount = 8;

/// True for the four axes an engine's own exciter reads. The other three are
/// channel-level state here, which is what decides where a per-note value can
/// land (see ControllerProfile::bind).
constexpr bool controller_axis_is_excitation(ControllerAxis axis) noexcept {
  return axis == ControllerAxis::kExcitation || axis == ControllerAxis::kPosition ||
         axis == ControllerAxis::kBrightness || axis == ControllerAxis::kMorph;
}

/// One device gesture bound to one axis. Binding the same input twice with
/// different axes is how a single gesture reaches two of them.
struct ControllerBinding {
  ControllerInput input = ControllerInput::kControlChange;
  /// CC number for kControlChange. Ignored by every other input, whose identity
  /// is the message status alone.
  uint8_t index = 0;
  ControllerAxis axis = ControllerAxis::kNone;
  /// Where the input's full deflection lands on the axis, in the axis's own
  /// unit — normalized [0,1] for the excitation axes and loudness, cents for
  /// pitch and vibrato depth. lo > hi inverts.
  float lo = 0.0f;
  float hi = 1.0f;
  /// Exponent applied to the normalized input before the range maps it. The
  /// default is linear and deliberately so: every wind controller already
  /// applies the curve its player chose, and a second one on this side bends a
  /// gesture the player has already shaped.
  float curve = 1.0f;
};

/// Fixed capacity: bind() refuses rather than growing, so resolve() can scan the
/// table from the audio thread.
inline constexpr size_t kMaxControllerBindings = 32;

/// A resolved value that belongs to the whole channel rather than to one note.
inline constexpr uint8_t kControllerAnyNote = 0xFFu;

/// One axis value a message produced.
struct ControllerAxisValue {
  ControllerAxis axis = ControllerAxis::kNone;
  /// Already mapped through the binding's curve and range: the axis's own unit.
  float value = 0.0f;
  /// The note this value belongs to, or kControllerAnyNote for the channel.
  uint8_t note = kControllerAnyNote;
};

/// What a message carries, before any binding is consulted.
struct ControllerInputValue {
  ControllerInput input = ControllerInput::kControlChange;
  /// CC number for kControlChange, 0 otherwise.
  uint8_t index = 0;
  uint8_t channel = 0;
  /// The note for kPolyPressure, kControllerAnyNote otherwise.
  uint8_t note = kControllerAnyNote;
  /// Deflection in [0,1]. Pitch bend is centred at 0.5 rather than signed, so
  /// one range spelling serves every input and polarity stays in lo/hi.
  float norm = 0.0f;
};

/// Decodes @p ump into the input it carries. Returns false for a message that is
/// not one of the five inputs. A MIDI 2.0 message resolves at its own width, so
/// the profile does not narrow a gesture the protocol sent at full resolution.
bool controller_input_of(const Ump& ump, ControllerInputValue* out) noexcept;

/// Maps a normalized deflection through one binding's curve and range.
float controller_map_value(const ControllerBinding& binding, float norm) noexcept;

/// The accumulated axis values of one channel. `present` is what keeps an axis
/// nobody bound from reading as zero: the patch's own voicing, the channel's own
/// pitch and a unit gain all stand until a controller actually reaches the axis.
struct ControllerAxisState {
  std::array<float, kControllerAxisCount> values{};
  uint32_t present = 0;

  bool has(ControllerAxis axis) const noexcept {
    return (present & (1u << static_cast<uint32_t>(axis))) != 0u;
  }
  void set(ControllerAxis axis, float value) noexcept {
    values[static_cast<size_t>(axis)] = value;
    present |= 1u << static_cast<uint32_t>(axis);
  }
  void reset() noexcept {
    values = {};
    present = 0;
  }
};

/// A device's spelling of the expression axes.
class ControllerProfile {
 public:
  // -- CONTROL thread ------------------------------------------------------

  /// Fills @p out with a named starting point. Returns false and leaves @p out
  /// untouched for a name that is not one of preset_name_at()'s — an unknown
  /// name resolving to a default would hand the caller a working profile with
  /// their device's spelling silently replaced.
  static bool preset(std::string_view name, ControllerProfile* out) noexcept;
  static size_t preset_count() noexcept;
  /// Name of preset @p index, or nullptr past the end.
  static const char* preset_name_at(size_t index) noexcept;

  /// Adds a binding. Returns false, adding nothing, when the table is full, when
  /// the axis is kNone, or when a kPolyPressure binding names an axis that is
  /// not an excitation axis: loudness, pitch and vibrato depth are channel-level
  /// state here, and a per-note value for them needs per-voice controller state
  /// this layer does not yet have. Refused rather than applied channel-wide,
  /// because a caller cannot tell a silently widened binding from one that took.
  bool bind(const ControllerBinding& binding) noexcept;
  void clear() noexcept;

  size_t binding_count() const noexcept { return count_; }
  const ControllerBinding& binding_at(size_t index) const noexcept { return bindings_[index]; }

  /// Which note a value addressed to the whole channel belongs to when several
  /// are sounding on it, per dimension. Set separately because the useful
  /// answers differ: pressure following the newest note while bend reaches
  /// every one is a real configuration, not a mistake. All three default to
  /// kLastNote. Read by the zone model (midi/mpe.h) and by nothing else --
  /// outside a zone a channel-addressed value is channel-wide by definition.
  NoteTracking pressure_tracking = NoteTracking::kLastNote;
  NoteTracking bend_tracking = NoteTracking::kLastNote;
  NoteTracking timbre_tracking = NoteTracking::kLastNote;

  /// Whether note-on velocity is expression. No fixed default is possible: a
  /// wind controller ships sending breath-derived velocity on one model and a
  /// constant on the next, so each preset states it. When false the synth takes
  /// every note at full scale and the bound axes carry the dynamics alone.
  bool velocity_meaningful = true;

  // -- AUDIO thread --------------------------------------------------------

  /// Resolves @p ump into 0..N axis values, writing at most @p cap of them and
  /// returning how many were written. A message produces at most one value per
  /// binding, so an @p out sized kMaxControllerBindings can never truncate.
  /// RT-safe: no allocation, no lock, no mutation.
  size_t resolve(const Ump& ump, ControllerAxisValue* out, size_t cap) const noexcept;

 private:
  std::array<ControllerBinding, kMaxControllerBindings> bindings_{};
  size_t count_ = 0;
};

}  // namespace sonare::midi
