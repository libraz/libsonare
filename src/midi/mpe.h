#pragma once

/// @file mpe.h
/// @brief MIDI Polyphonic Expression zones: which channels carry one note each,
///        and what a channel-addressed value currently means for a note on one.
///
/// Specification: MIDI Association / AMEI document M1-100-UM version 1.1. Every
/// section number quoted here and in the implementation is that document's.
///
/// Layering: depends on note_tracking.h and nothing else -- not even on the UMP
/// decoder. It names channels and zones, never voices and never engines, and it
/// takes already-decoded values so the instrument keeps its one dispatch point.
///
/// Threading: unlike ControllerProfile, this is configured by a MIDI message
/// rather than by the host -- support for the MPE Configuration Message is
/// mandatory (2.2.1) -- so it mutates on the AUDIO thread. Fixed size, no
/// allocation, no lock, no clock, no random. A reconfiguration obliges the
/// caller to stop the sounding notes on the channels that moved (2.2.3);
/// apply_mcm() reports which those are rather than doing it, because the notes
/// belong to the instrument.

#include <cstddef>
#include <cstdint>

#include "midi/note_tracking.h"

namespace sonare::midi {

/// The two zones the spec allows, named by the end of the channel space each
/// grows from: Lower is manager channel 1 with members counting up from 2,
/// Upper is manager channel 16 with members counting down from 15 (2.2.1).
enum class MpeZone : uint8_t { kLower = 0, kUpper = 1 };
inline constexpr size_t kMpeZoneCount = 2;

/// Channel numbers of the two manager channels, zero-based as the rest of this
/// library spells a MIDI channel.
inline constexpr uint8_t kMpeLowerManagerChannel = 0;
inline constexpr uint8_t kMpeUpperManagerChannel = 15;

/// Pitch Bend Sensitivity a receiver installs when it accepts an MCM (2.2.5).
/// The manager keeps an ordinary wheel range; a member channel carries one
/// note's own bend and gets four octaves for it.
inline constexpr float kMpeManagerBendSemitones = 2.0f;
inline constexpr float kMpeMemberBendSemitones = 48.0f;
/// The widest RPN 0 may ask for afterwards (2.2.5).
inline constexpr float kMpeMaxBendSemitones = 96.0f;

/// What a channel is inside the zone model. Unassigned channels "remain
/// available for conventional use" (2.2.1) and this class says nothing about
/// them.
enum class MpeChannelRole : uint8_t { kUnassigned = 0, kManager = 1, kMember = 2 };

/// The three dimensions MPE carries per note (2.1).
enum class MpeDimension : uint8_t { kBend = 0, kPressure = 1, kTimbre = 2 };
inline constexpr size_t kMpeDimensionCount = 3;

/// The controller the third dimension is carried on (2.2.8). Which axis it
/// reaches is a receiver's own decision; that it arrives here is not.
inline constexpr uint8_t kMpeTimbreCc = 74;

/// Mode 3 is what a receiver shall default to; Mode 4 makes each member channel
/// monophonic and is optional -- "MPE Devices are not required to support MIDI
/// Mode 4" (2.2.4).
enum class MpeMidiMode : uint8_t { kPoly = 3, kMono = 4 };

/// A message the zone model requires the receiver to ignore on some channels.
/// The table is Appendix E Table 5; the prose is at the section cited on each.
enum class MpeIgnorable : uint8_t {
  kPolyKeyPressure = 0,  ///< Prohibited on a member channel (2.2.7)
  kModeMessage = 1,      ///< CC#126 / #127, prohibited on a manager channel (2.2.4.3)
  kBankSelect = 2,       ///< CC#0 / #32, prohibited on a member channel in Mode 3
  kProgramChange = 3,    ///< Ignored on a member channel in Mode 3 (2.3.3)
};

/// One note a channel-addressed value could reach, in the order the notes
/// started.
struct MpeNote {
  uint8_t note = 0;
  /// False once its Note Off has arrived and only a pedal or a release tail
  /// keeps it sounding. Such a note is never selected, because per-note control
  /// "shall not affect a note after the Note Off message has been received"
  /// (2.4) -- which is why kLastNote and kAllNotes do not reach a sustaining
  /// note here even though the implementation this rule was modelled on lets
  /// them: that latitude is not the receiver's to take under MPE.
  bool active = true;
  /// Written by mpe_select_notes, which overwrites whatever it held.
  bool selected = false;
};

/// Marks which of @p notes a channel-addressed value reaches under @p mode and
/// returns how many were marked. @p notes is oldest-first, which is the only
/// order that can answer kLastNote. Selection is written back into each entry
/// rather than into a parallel array, so a caller sizing one buffer has sized
/// all of them.
size_t mpe_select_notes(NoteTracking mode, MpeNote* notes, size_t count) noexcept;

/// The zone configuration and the per-channel dimension values that go with it.
class MpeState {
 public:
  // -- configuration, by message -------------------------------------------

  /// Applies an MPE Configuration Message (2.2.1). @p manager_channel is 0 or
  /// 15 and @p member_count is 0..15, zero deactivating that zone. Returns
  /// false and changes nothing otherwise: "All other values are invalid and
  /// should be ignored".
  ///
  /// Writes into @p out_reconfigured the bitmask of channels that entered or
  /// left a zone, which the caller owes a stop of every sounding note and a
  /// controller reset on (2.2.3). A caller that ignores it leaves the hanging
  /// notes the section exists to prevent.
  bool apply_mcm(uint8_t manager_channel, uint8_t member_count,
                 uint16_t* out_reconfigured) noexcept;

  /// Pitch Bend Sensitivity from RPN 0 (2.2.5). On a member channel it applies
  /// to every member of that zone, which the spec requires rather than permits:
  /// "Member Channels within the same Zone shall not have different Pitch Bend
  /// Sensitivity values." Returns false for a channel in no zone -- that one is
  /// ordinary MIDI and its range stays the instrument's own.
  bool apply_bend_sensitivity(uint8_t channel, float semitones) noexcept;

  /// Switches the zone containing @p channel between Mode 3 and Mode 4. Refused
  /// on a manager channel, where the mode messages are prohibited, and on a
  /// channel in no zone (2.2.4.3).
  bool apply_midi_mode(uint8_t channel, MpeMidiMode mode) noexcept;

  // -- tracking, by message ------------------------------------------------

  /// Records a dimension's new value on @p channel, which is kept even while the
  /// channel is silent: a receiver "shall continue to track" these so the next
  /// note on that channel takes them as its initial state (2.2.6 - 2.2.8).
  void track_bend(uint8_t channel, uint16_t bend14) noexcept;
  void track_pressure(uint8_t channel, uint8_t value) noexcept;
  void track_timbre(uint8_t channel, uint8_t value) noexcept;

  /// Returns every channel to its post-MCM state without changing the zones --
  /// what 2.2.3 asks of the controls while the caller stops the notes.
  void reset_controls(uint16_t channels) noexcept;
  /// Drops both zones and every tracked value.
  void reset() noexcept;

  // -- reading -------------------------------------------------------------

  /// True once at least one zone is configured, which is what turns MPE Mode on
  /// (2.2.1). While false every accessor below reports an unassigned channel and
  /// the instrument behaves as it did before any MCM arrived.
  bool active() const noexcept { return zones_[0].active || zones_[1].active; }

  MpeChannelRole role(uint8_t channel) const noexcept;
  /// Which zone @p channel is in. Undefined for an unassigned channel, so ask
  /// role() first.
  MpeZone zone_of(uint8_t channel) const noexcept;
  MpeMidiMode midi_mode(MpeZone zone) const noexcept;
  uint8_t member_count(MpeZone zone) const noexcept;
  float bend_sensitivity(uint8_t channel) const noexcept;

  /// Whether the receiver must ignore @p what arriving on @p channel.
  bool ignores(uint8_t channel, MpeIgnorable what) const noexcept;

  /// Combined manager + member value of one dimension for a note on @p channel.
  ///
  /// The spec requires the combination -- "shall combine such data meaningfully
  /// and separately for each Sounding Note" -- and leaves the method to the
  /// manufacturer (2.2.6 - 2.2.8). Bend sums the two in semitones, which is
  /// Appendix C.5's worked example; pressure and timbre add and clamp, which is
  /// the first of Appendix D's listed strategies and the one that reads the
  /// manager value as the bias that section calls it.
  ///
  /// A manager value nothing has reached contributes nothing rather than
  /// contributing a zero or a centre, so a zone whose manager is silent leaves
  /// its members' values untouched.
  float bend_semitones(uint8_t channel) const noexcept;
  uint8_t pressure(uint8_t channel) const noexcept;
  uint8_t timbre(uint8_t channel) const noexcept;

  /// The manager's bend alone, in semitones. This is the one per-note dimension
  /// that keeps acting on a note after its Note Off: manager bend "applies to
  /// every Sounding Note within the Zone, even those that have passed into their
  /// Note Off phase" (A.4.1), while the two below it do not.
  float manager_bend_semitones(uint8_t channel) const noexcept;

  /// Whether a controller has reached @p dimension on @p channel or on its
  /// manager. An untouched dimension is absent rather than zero, the same rule
  /// ControllerAxisState carries, so a patch's own voicing stands until a
  /// controller actually arrives.
  bool has(uint8_t channel, MpeDimension dimension) const noexcept;

 private:
  struct Zone {
    bool active = false;
    uint8_t member_count = 0;
    float manager_bend = kMpeManagerBendSemitones;
    float member_bend = kMpeMemberBendSemitones;
    MpeMidiMode mode = MpeMidiMode::kPoly;
  };

  struct Channel {
    uint16_t bend14 = 8192;
    uint8_t pressure = 0;
    uint8_t timbre = 0;
    uint8_t present = 0;
  };

  /// Bitmask of the channels @p zone occupies, manager included, or 0 when it
  /// is inactive.
  static uint16_t zone_mask(MpeZone zone, uint8_t member_count) noexcept;
  /// Every channel under MPE control, and the active zones' managers alone.
  uint16_t occupied_mask() const noexcept;
  uint16_t manager_mask() const noexcept;
  /// Manager channel of the zone @p channel belongs to. Ask role() first.
  uint8_t manager_of(uint8_t channel) const noexcept;

  Zone zones_[kMpeZoneCount]{};
  Channel channels_[16]{};
};

}  // namespace sonare::midi
