#pragma once

#include <cstdint>

/// @file
/// MIDI 1.0 control-change controller numbers, named after the specification.

namespace sonare::midi::cc {

inline constexpr uint8_t kBankSelectMsb = 0;
inline constexpr uint8_t kModulationWheel = 1;
inline constexpr uint8_t kBreathController = 2;
inline constexpr uint8_t kPortamentoTime = 5;
inline constexpr uint8_t kDataEntryMsb = 6;
inline constexpr uint8_t kChannelVolume = 7;
inline constexpr uint8_t kPan = 10;
inline constexpr uint8_t kExpression = 11;
inline constexpr uint8_t kGeneralPurpose1 = 16;
inline constexpr uint8_t kGeneralPurpose2 = 17;
inline constexpr uint8_t kBankSelectLsb = 32;
inline constexpr uint8_t kDataEntryLsb = 38;
inline constexpr uint8_t kDamperPedal = 64;
inline constexpr uint8_t kPortamentoOnOff = 65;
inline constexpr uint8_t kSostenuto = 66;
inline constexpr uint8_t kSoftPedal = 67;
/// Sound Controllers 2-9 (GS TONE MODIFY 1-8); 74 is Sound Controller 5 (brightness).
inline constexpr uint8_t kSoundController2 = 71;
inline constexpr uint8_t kSoundController3 = 72;
inline constexpr uint8_t kSoundController4 = 73;
inline constexpr uint8_t kSoundController5 = 74;
inline constexpr uint8_t kSoundController6 = 75;
inline constexpr uint8_t kSoundController7 = 76;
inline constexpr uint8_t kSoundController8 = 77;
inline constexpr uint8_t kSoundController9 = 78;
inline constexpr uint8_t kPortamentoControl = 84;
inline constexpr uint8_t kEffects1Depth = 91;  ///< Reverb send.
inline constexpr uint8_t kEffects3Depth = 93;  ///< Chorus send.
inline constexpr uint8_t kEffects4Depth = 94;  ///< GS delay send.
inline constexpr uint8_t kNrpnLsb = 98;
inline constexpr uint8_t kNrpnMsb = 99;
inline constexpr uint8_t kRpnLsb = 100;
inline constexpr uint8_t kRpnMsb = 101;
inline constexpr uint8_t kAllSoundOff = 120;
inline constexpr uint8_t kResetAllControllers = 121;
inline constexpr uint8_t kAllNotesOff = 123;
inline constexpr uint8_t kOmniModeOff = 124;
inline constexpr uint8_t kOmniModeOn = 125;
inline constexpr uint8_t kMonoModeOn = 126;
inline constexpr uint8_t kPolyModeOn = 127;

}  // namespace sonare::midi::cc
