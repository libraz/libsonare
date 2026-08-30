#pragma once

/// @file gs_address_table.h
/// @brief The GS address space as data: one row per address, carrying the level
///        it promises, the values it accepts and its reset default, plus the
///        SysEx frame and decode layer that walks it.
///
/// The table is the only owner of the map, so an address with no row is a
/// defect rather than a silence: a decoded byte matching nothing comes back as
/// GsParam::kUnknown and is counted instead of dropped (docs/gs.md).
///
/// Two tables, because the row count of the first is the number of promises
/// made. The main table holds defined addresses; the range table holds regions
/// the manual leaves undefined, which exist only so that a multi-byte write
/// running through them does not read as a gap.
///
/// A part, drum-map or EFX-unit number is not part of a parameter's identity —
/// it comes back on GsWrite::part — and neither is a position inside a
/// multi-byte parameter, which comes back on GsWrite::index.
///
/// RT contract: POD + pure functions, no allocation.

#include <array>
#include <cstddef>
#include <cstdint>

namespace sonare::midi::synth {

/// What an address promises (docs/gs.md). Every address carries exactly one.
enum class GsLevel : uint8_t {
  kAudible,  ///< Received, held, and reflected in the audio.
  kState,    ///< Received and held, not reflected in the audio.
  kAccept,   ///< Received and discarded.
  kIgnore,   ///< Deliberately not implemented; the row carries the reason.
};

/// Every parameter the table names, written once so the enum and the names
/// reported by gs_param_name() cannot disagree.
#define SONARE_GS_PARAMS(X) \
  X(kUnknown)               \
  X(kUndefined)             \
  X(kSystemModeSet)         \
  X(kChannelMsgRxPort)      \
  X(kMasterTune)            \
  X(kMasterVolume)          \
  X(kMasterKeyShift)        \
  X(kMasterPan)             \
  X(kModeSet)               \
  X(kPatchName)             \
  X(kReverbMacro)           \
  X(kReverbCharacter)       \
  X(kReverbPreLpf)          \
  X(kReverbLevel)           \
  X(kReverbTime)            \
  X(kReverbDelayFeedback)   \
  X(kReverbPredelay)        \
  X(kChorusMacro)           \
  X(kChorusPreLpf)          \
  X(kChorusLevel)           \
  X(kChorusFeedback)        \
  X(kChorusDelay)           \
  X(kChorusRate)            \
  X(kChorusDepth)           \
  X(kChorusSendToReverb)    \
  X(kChorusSendToDelay)     \
  X(kDelayMacro)            \
  X(kDelayPreLpf)           \
  X(kDelayTimeCenter)       \
  X(kDelayTimeRatioLeft)    \
  X(kDelayTimeRatioRight)   \
  X(kDelayLevelCenter)      \
  X(kDelayLevelLeft)        \
  X(kDelayLevelRight)       \
  X(kDelayLevel)            \
  X(kDelayFeedback)         \
  X(kDelaySendToReverb)     \
  X(kEqLowFreq)             \
  X(kEqLowGain)             \
  X(kEqHighFreq)            \
  X(kEqHighGain)            \
  X(kEfxType)               \
  X(kEfxParameter)          \
  X(kEfxSendToReverb)       \
  X(kEfxSendToChorus)       \
  X(kEfxSendToDelay)        \
  X(kEfxControlSource1)     \
  X(kEfxControlDepth1)      \
  X(kEfxControlSource2)     \
  X(kEfxControlDepth2)      \
  X(kEfxSendEqSwitch)       \
  X(kPartMonoPoly)          \
  X(kPartAssignMode)        \
  X(kUseForRhythmPart)      \
  X(kPartKeyShift)          \
  X(kPartLevel)             \
  X(kPartPanpot)            \
  X(kPartChorusSend)        \
  X(kPartReverbSend)        \
  X(kPartPitchFineTune)     \
  X(kPartDelaySend)         \
  X(kPartToneModify)        \
  X(kPartModDest)           \
  X(kPartModLfo1PitchDepth) \
  X(kPartBendDest)          \
  X(kPartBendPitchControl)  \
  X(kPartCafDest)           \
  X(kPartPafDest)           \
  X(kPartCc1Dest)           \
  X(kPartCc2Dest)           \
  X(kPartToneMapNumber)     \
  X(kPartToneMap0Number)    \
  X(kPartEqSwitch)          \
  X(kPartEfxAssign)         \
  X(kDrumPlayNote)          \
  X(kDrumLevel)             \
  X(kDrumAssignGroup)       \
  X(kDrumPanpot)            \
  X(kDrumReverbSend)        \
  X(kDrumChorusSend)        \
  X(kDrumDelaySend)         \
  X(kOppositeGroupBlock)

/// The parameter a decoded byte addresses. kUnknown means no table row claimed
/// the address; kUndefined means a range-table row did (an address the manual
/// leaves without a definition).
enum class GsParam : uint16_t {
#define SONARE_GS_PARAM_ENUMERATOR(name) name,
  SONARE_GS_PARAMS(SONARE_GS_PARAM_ENUMERATOR)
#undef SONARE_GS_PARAM_ENUMERATOR
};

/// The enumerator name of @p param, for coverage reports and test diagnostics.
const char* gs_param_name(GsParam param) noexcept;

/// One defined address (or one run of them). Addresses are the 24-bit GS
/// address packed as (hi << 16) | (mid << 8) | lo.
struct GsAddressEntry {
  /// Base address, with every variable nibble zeroed.
  uint32_t addr;
  /// The variable nibbles: part or EFX unit (0x000F00), drum map (0x00F000),
  /// drum note (0x00007F). Zero for a fixed address.
  uint32_t mask;
  GsParam param;
  GsLevel level;
  /// Consecutive addresses the row owns: one parameter spread over `size`
  /// bytes (MASTER TUNE) or `size` one-byte parameters of the same kind (EFX
  /// PARAMETER 1-20). GsWrite::index tells them apart either way.
  uint8_t size;
  /// Accepted value range, inclusive. A value outside it is ignored, never
  /// clamped (gs_value_in_range).
  uint8_t lo, hi;
  /// Reset default of the row's FIRST byte. A multi-byte row whose bytes do not
  /// share one default (MASTER TUNE, `00 04 00 00`) carries only that first
  /// byte here; the rest belongs to the reset implementation.
  uint8_t def;
  /// Required for kIgnore and kAccept, nullptr otherwise — an exclusion without
  /// a reason is not allowed.
  const char* why;
};

// A bound rather than an equality: the pointer is 4 bytes on wasm32 and 8 on a
// 64-bit host. The table grows towards the whole address space, so a field
// added here costs its own width once per row.
static_assert(sizeof(GsAddressEntry) <= 24, "GsAddressEntry grew");

/// A region the manual leaves undefined. Always kAccept and GsParam::kUndefined:
/// these rows keep the unknown counter meaningful, they are not promises.
struct GsAddressRange {
  uint32_t lo_addr;
  uint32_t hi_addr;
  /// Variable nibbles, as on GsAddressEntry. A gap inside a per-part block is
  /// one gap in sixteen places, and without this it would be a row per part.
  uint32_t mask;
  const char* why;
};

/// The defined addresses. Ascending by address; the row count is the number of
/// addresses the implementation has taken a position on.
inline constexpr std::array<GsAddressEntry, 105> kGsAddressTable = {{
    // System (00 00 xx / 00 01 xx).
    // SC-8850 takes 00 only and treats it as a GS reset: it has no Mode-2, so
    // the SC-88Pro's 01 falls outside the accepted range (docs/gs.md).
    {0x00007F, 0, GsParam::kSystemModeSet, GsLevel::kAudible, 1, 0x00, 0x00, 0x00, nullptr},
    {0x000100, 0x00000F, GsParam::kChannelMsgRxPort, GsLevel::kIgnore, 1, 0x00, 0x01, 0x00,
     "libsonare receives one port, so a channel has no second port to be assigned to"},
    {0x000110, 0x00000F, GsParam::kChannelMsgRxPort, GsLevel::kIgnore, 1, 0x00, 0x01, 0x01,
     "libsonare receives one port, so a channel has no second port to be assigned to"},

    // System parameters (40 00 xx).
    // MASTER TUNE is four nibbles making one 0018-07E8 word; the row bounds the
    // per-byte nibble, the aggregate range belongs to the consumer.
    {0x400000, 0, GsParam::kMasterTune, GsLevel::kAudible, 4, 0x00, 0x0F, 0x00, nullptr},
    {0x400004, 0, GsParam::kMasterVolume, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr},
    {0x400005, 0, GsParam::kMasterKeyShift, GsLevel::kAudible, 1, 0x28, 0x58, 0x40, nullptr},
    // 00 is out of range here: master pan has no random value, unlike a part's.
    {0x400006, 0, GsParam::kMasterPan, GsLevel::kAudible, 1, 0x01, 0x7F, 0x40, nullptr},
    {0x40007F, 0, GsParam::kModeSet, GsLevel::kAudible, 1, 0x00, 0x00, 0x00, nullptr},

    // Patch common (40 01 xx). PATCH NAME is one 16-byte ASCII field; the row
    // bounds a character and the index says which one.
    {0x400100, 0, GsParam::kPatchName, GsLevel::kAccept, 16, 0x20, 0x7F, 0x20,
     "a display string, and a renderer has nothing to display it on"},

    // Reverb (40 01 30-37). The defaults are the Hall 2 macro, so they and
    // kGsReverbMacros[4] in gs_system_effects.h say the same thing.
    //
    // kState from here to 40 01 5A marks a byte GsSystemEffects holds and the
    // effect bus never asks for: the bus takes ten fields off GsEffectsConfig,
    // and a parameter outside that set reaches the struct and stops. Raising one
    // to kAudible means giving the engine the control, not editing the row.
    {0x400130, 0, GsParam::kReverbMacro, GsLevel::kAudible, 1, 0x00, 0x07, 0x04, nullptr},
    {0x400131, 0, GsParam::kReverbCharacter, GsLevel::kAudible, 1, 0x00, 0x07, 0x04, nullptr},
    {0x400132, 0, GsParam::kReverbPreLpf, GsLevel::kState, 1, 0x00, 0x07, 0x00, nullptr},
    {0x400133, 0, GsParam::kReverbLevel, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    {0x400134, 0, GsParam::kReverbTime, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    // The one row that does not even reach GsEffectsConfig: the conversion exists
    // (gs_reverb_delay_feedback_coefficient) and has no caller.
    {0x400135, 0, GsParam::kReverbDelayFeedback, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x400137, 0, GsParam::kReverbPredelay, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},

    // Chorus (40 01 38-40). The defaults are the Chorus 3 macro.
    {0x400138, 0, GsParam::kChorusMacro, GsLevel::kAudible, 1, 0x00, 0x07, 0x02, nullptr},
    {0x400139, 0, GsParam::kChorusPreLpf, GsLevel::kState, 1, 0x00, 0x07, 0x00, nullptr},
    {0x40013A, 0, GsParam::kChorusLevel, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    {0x40013B, 0, GsParam::kChorusFeedback, GsLevel::kState, 1, 0x00, 0x7F, 0x08, nullptr},
    {0x40013C, 0, GsParam::kChorusDelay, GsLevel::kAudible, 1, 0x00, 0x7F, 0x50, nullptr},
    {0x40013D, 0, GsParam::kChorusRate, GsLevel::kAudible, 1, 0x00, 0x7F, 0x03, nullptr},
    {0x40013E, 0, GsParam::kChorusDepth, GsLevel::kAudible, 1, 0x00, 0x7F, 0x13, nullptr},
    // The unit-to-unit sends: the bus runs reverb, chorus and delay in parallel
    // off the part sends and does not feed one from another.
    {0x40013F, 0, GsParam::kChorusSendToReverb, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x400140, 0, GsParam::kChorusSendToDelay, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},

    // Delay (40 01 50-5A). The defaults are the Delay 1 macro.
    {0x400150, 0, GsParam::kDelayMacro, GsLevel::kAudible, 1, 0x00, 0x09, 0x00, nullptr},
    {0x400151, 0, GsParam::kDelayPreLpf, GsLevel::kState, 1, 0x00, 0x07, 0x00, nullptr},
    // 00 is out of range: the time table starts at 01 (0.1 ms).
    {0x400152, 0, GsParam::kDelayTimeCenter, GsLevel::kAudible, 1, 0x01, 0x73, 0x61, nullptr},
    // The three-tap delay is one tap here: both legs take the centre time and the
    // centre level, so the ratios and the per-leg levels have nothing to reach.
    {0x400153, 0, GsParam::kDelayTimeRatioLeft, GsLevel::kState, 1, 0x01, 0x78, 0x01, nullptr},
    {0x400154, 0, GsParam::kDelayTimeRatioRight, GsLevel::kState, 1, 0x01, 0x78, 0x01, nullptr},
    {0x400155, 0, GsParam::kDelayLevelCenter, GsLevel::kState, 1, 0x00, 0x7F, 0x7F, nullptr},
    {0x400156, 0, GsParam::kDelayLevelLeft, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x400157, 0, GsParam::kDelayLevelRight, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x400158, 0, GsParam::kDelayLevel, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    // 00-7F reads as -64..+63, so the default 50 is +16 and not the centre. The
    // signedness belongs to gs_delay_feedback_signed; the row bounds the byte.
    {0x400159, 0, GsParam::kDelayFeedback, GsLevel::kAudible, 1, 0x00, 0x7F, 0x50, nullptr},
    {0x40015A, 0, GsParam::kDelaySendToReverb, GsLevel::kState, 1, 0x00, 0x7F, 0x00, nullptr},

    // Master EQ (40 02 xx). The two FREQ addresses select one of two corners,
    // and the two GAIN addresses are 34-4C around a centred 40 (+-12 dB).
    {0x400200, 0, GsParam::kEqLowFreq, GsLevel::kAudible, 1, 0x00, 0x01, 0x00, nullptr},
    {0x400201, 0, GsParam::kEqLowGain, GsLevel::kAudible, 1, 0x34, 0x4C, 0x40, nullptr},
    {0x400202, 0, GsParam::kEqHighFreq, GsLevel::kAudible, 1, 0x00, 0x01, 0x00, nullptr},
    {0x400203, 0, GsParam::kEqHighGain, GsLevel::kAudible, 1, 0x34, 0x4C, 0x40, nullptr},

    // EFX (40 03 xx), insertion unit 0.
    {0x400300, 0, GsParam::kEfxType, GsLevel::kAudible, 2, 0x00, 0x7F, 0x00, nullptr},
    {0x400303, 0, GsParam::kEfxParameter, GsLevel::kAudible, 20, 0x00, 0x7F, 0x00, nullptr},
    {0x400317, 0, GsParam::kEfxSendToReverb, GsLevel::kAudible, 1, 0x00, 0x7F, 0x28, nullptr},
    {0x400318, 0, GsParam::kEfxSendToChorus, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x400319, 0, GsParam::kEfxSendToDelay, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00, nullptr},
    // The two control assignments let a controller move an EFX parameter while
    // the effect runs. The chain here is realised from its type and its twenty
    // parameters and is not re-parameterised afterwards, so there is nothing for
    // a source to drive; implementing them means giving the chain that hook.
    {0x40031B, 0, GsParam::kEfxControlSource1, GsLevel::kIgnore, 1, 0x00, 0x7F, 0x00,
     "the insertion chain is realised from its type and parameters and takes no live modulation"},
    {0x40031C, 0, GsParam::kEfxControlDepth1, GsLevel::kIgnore, 1, 0x00, 0x7F, 0x40,
     "the depth of a control source that has nothing to drive"},
    {0x40031D, 0, GsParam::kEfxControlSource2, GsLevel::kIgnore, 1, 0x00, 0x7F, 0x00,
     "the insertion chain is realised from its type and parameters and takes no live modulation"},
    {0x40031E, 0, GsParam::kEfxControlDepth2, GsLevel::kIgnore, 1, 0x00, 0x7F, 0x40,
     "the depth of a control source that has nothing to drive"},
    {0x40031F, 0, GsParam::kEfxSendEqSwitch, GsLevel::kIgnore, 1, 0x00, 0x01, 0x01,
     "one EQ stage, bypassed per part at 40 4x 20; an EFX return has no separate one to switch"},

    // Part parameters (40 1x xx).
    // 00 Mono / 01 Poly, and the same storage location CC126 and CC127 write.
    {0x401013, 0x000F00, GsParam::kPartMonoPoly, GsLevel::kAudible, 1, 0x00, 0x01, 0x01, nullptr},
    // 00 SINGLE / 01 LIMITED-MULTI / 02 FULL-MULTI. The default is 01 for every
    // part under the SC-8850 map; the per-part split the manual also prints
    // belongs to the SC-55 map, which is not the target.
    {0x401014, 0x000F00, GsParam::kPartAssignMode, GsLevel::kAudible, 1, 0x00, 0x02, 0x01, nullptr},
    // Part 10 powers on at 01 (drum map 1) and every other part at 00, which one
    // def cannot say; the row carries the melodic default and the reset owns the
    // exception.
    {0x401015, 0x000F00, GsParam::kUseForRhythmPart, GsLevel::kAudible, 1, 0x00, 0x02, 0x00,
     nullptr},
    // PITCH KEY SHIFT is its own parameter and not RPN 00 02's alias, however
    // exactly their ranges coincide: the map annotates every alias it has and
    // annotates this one with nothing (docs/gs.md).
    {0x401016, 0x000F00, GsParam::kPartKeyShift, GsLevel::kAudible, 1, 0x28, 0x58, 0x40, nullptr},
    // The rest of this block is the CC/SysEx/NRPN alias set (docs/gs.md): each
    // row writes the one storage location its controller already owns, so the
    // default here is the controller's own power-on value rather than a second.
    {0x401019, 0x000F00, GsParam::kPartLevel, GsLevel::kAudible, 1, 0x00, 0x7F, 0x64, nullptr},
    // 00 is RANDOM on the hardware and centre here — a deliberate divergence
    // (docs/gs.md), so it stays inside the accepted range rather than becoming
    // an out-of-range value the apply layer would drop.
    {0x40101C, 0x000F00, GsParam::kPartPanpot, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    {0x401021, 0x000F00, GsParam::kPartChorusSend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00, nullptr},
    // The reverb send's 28 is the GS power-on 40 the reset already installs.
    {0x401022, 0x000F00, GsParam::kPartReverbSend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x28, nullptr},
    // Two bytes making one 14-bit word centred on 40 00, which is RPN 00 01's
    // 8192. The row bounds a byte; index 0 is the MSB.
    {0x40102A, 0x000F00, GsParam::kPartPitchFineTune, GsLevel::kAudible, 2, 0x00, 0x7F, 0x40,
     nullptr},
    {0x40102C, 0x000F00, GsParam::kPartDelaySend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00, nullptr},
    // TONE MODIFY 1-8 in one row: same range, same default, and the index is
    // which of the eight (gs_apply_tone_modify).
    {0x401030, 0x000F00, GsParam::kPartToneModify, GsLevel::kAudible, 8, 0x00, 0x7F, 0x40, nullptr},

    // Controller destinations (40 2x xx): six sources, each with the same eleven
    // destinations, at +00 pitch / +01 TVF cutoff / +02 amplitude / +03-06 LFO1
    // rate, pitch, TVF, TVA / +07-0A the same four for LFO2. Rows are split where
    // the accepted range or the power-on default changes, not per destination.
    //
    // kAccept throughout: this is a modulation matrix and the engine routes its
    // controllers directly, so no byte here has a reader. Two of them name a
    // quantity that already exists under another address and are the ones worth
    // raising first (docs/gs.md's alias table).
    // Modulation as a source.
    {0x402000, 0x000F00, GsParam::kPartModDest, GsLevel::kAccept, 1, 0x28, 0x58, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402001, 0x000F00, GsParam::kPartModDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402004, 0x000F00, GsParam::kPartModLfo1PitchDepth, GsLevel::kAudible, 1, 0x00, 0x7F, 0x0A,
     nullptr},
    {0x402005, 0x000F00, GsParam::kPartModDest, GsLevel::kAccept, 2, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402007, 0x000F00, GsParam::kPartModDest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402008, 0x000F00, GsParam::kPartModDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    // Bend as a source.
    {0x402010, 0x000F00, GsParam::kPartBendPitchControl, GsLevel::kAudible, 1, 0x40, 0x58, 0x42,
     nullptr},
    {0x402011, 0x000F00, GsParam::kPartBendDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402014, 0x000F00, GsParam::kPartBendDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402017, 0x000F00, GsParam::kPartBendDest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402018, 0x000F00, GsParam::kPartBendDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    // CAf as a source.
    {0x402020, 0x000F00, GsParam::kPartCafDest, GsLevel::kAccept, 1, 0x28, 0x58, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402021, 0x000F00, GsParam::kPartCafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402024, 0x000F00, GsParam::kPartCafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402027, 0x000F00, GsParam::kPartCafDest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402028, 0x000F00, GsParam::kPartCafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    // PAf as a source.
    {0x402030, 0x000F00, GsParam::kPartPafDest, GsLevel::kAccept, 1, 0x28, 0x58, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402031, 0x000F00, GsParam::kPartPafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402034, 0x000F00, GsParam::kPartPafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402037, 0x000F00, GsParam::kPartPafDest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402038, 0x000F00, GsParam::kPartPafDest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    // CC1 as a source.
    {0x402040, 0x000F00, GsParam::kPartCc1Dest, GsLevel::kAccept, 1, 0x28, 0x58, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402041, 0x000F00, GsParam::kPartCc1Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402044, 0x000F00, GsParam::kPartCc1Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402047, 0x000F00, GsParam::kPartCc1Dest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402048, 0x000F00, GsParam::kPartCc1Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    // CC2 as a source.
    {0x402050, 0x000F00, GsParam::kPartCc2Dest, GsLevel::kAccept, 1, 0x28, 0x58, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402051, 0x000F00, GsParam::kPartCc2Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402054, 0x000F00, GsParam::kPartCc2Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402057, 0x000F00, GsParam::kPartCc2Dest, GsLevel::kAccept, 1, 0x00, 0x7F, 0x40,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},
    {0x402058, 0x000F00, GsParam::kPartCc2Dest, GsLevel::kAccept, 3, 0x00, 0x7F, 0x00,
     "recognised; the engine has no controller-destination matrix, so nothing reads it"},

    // Tone map (40 4x 00-01): which generation of the sound set a part plays
    // from. The maps themselves exist and are audible through Bank Select LSB;
    // what is missing is this route onto them. Ranges are the SC-8850's.
    {0x404000, 0x000F00, GsParam::kPartToneMapNumber, GsLevel::kAccept, 1, 0x00, 0x04, 0x00,
     "GsToneMap is selected by Bank Select LSB (gs_effective_tone_map); this route onto it is "
     "not wired yet"},
    {0x404001, 0x000F00, GsParam::kPartToneMap0Number, GsLevel::kAccept, 1, 0x01, 0x04, 0x04,
     "GsToneMap is selected by Bank Select LSB (gs_effective_tone_map); this route onto it is "
     "not wired yet"},

    // Part EQ switch (40 4x 20): whether the master EQ at 40 02 xx reaches this
    // part. Powers on ON, so a file that never writes it still gets the EQ.
    {0x404020, 0x000F00, GsParam::kPartEqSwitch, GsLevel::kAudible, 1, 0x00, 0x01, 0x01, nullptr},

    // Part EFX assign (40 4x 22).
    // The manual specifies 00 BYPASS / 01 EFX; 02-10 select insertion units 1-15,
    // the libsonare extension (docs/gs.md). Narrowing hi back to 01 to restore
    // spec compliance makes the extension unreachable.
    {0x404022, 0x000F00, GsParam::kPartEfxAssign, GsLevel::kAudible, 1, 0x00, 0x10, 0x00, nullptr},

    // Drum setup (41 mn rr): the five parameters the alias table reaches from
    // the NRPNs as well (docs/gs.md). The map nibble m is zero-based here — 41
    // 0n rr is MAP1 — where 40 1x 15's VALUE is one-based, and the note is the
    // whole low byte.
    //
    // The manual gives these no power-on value: a drum set change re-initialises
    // them to what the kit specifies. So each def is the value at which the
    // parameter changes nothing, which is what an unwritten one has to mean.
    // PLAY NOTE NUMBER's identity value is the address's own note, which one def
    // byte cannot carry; 00 stands in and the flag is what an unwritten note
    // reads, as for the five rows below.
    {0x410100, 0x00F07F, GsParam::kDrumPlayNote, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00, nullptr},
    {0x410200, 0x00F07F, GsParam::kDrumLevel, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr},
    // ASSIGN GROUP's identity is the kit's own group, so its def is a
    // placeholder for the same reason PLAY NOTE NUMBER's is. 00 is OFF, and a
    // note written to 00 is not an unwritten one.
    {0x410300, 0x00F07F, GsParam::kDrumAssignGroup, GsLevel::kAudible, 1, 0x00, 0x7F, 0x00,
     nullptr},
    // 00 is RANDOM at this address and hard left through NRPN 1C, the same split
    // 40 1x 1C and CC10 have; it stays in range and answers centre.
    {0x410400, 0x00F07F, GsParam::kDrumPanpot, GsLevel::kAudible, 1, 0x00, 0x7F, 0x40, nullptr},
    {0x410500, 0x00F07F, GsParam::kDrumReverbSend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr},
    {0x410600, 0x00F07F, GsParam::kDrumChorusSend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr},
    {0x410900, 0x00F07F, GsParam::kDrumDelaySend, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr},

    // The opposite group's blocks (50 ** ** / 51 ** **). An SC-88Pro address
    // reaching the other sixteen parts and their drum setup; the SC-8850 does
    // not have it, the port selecting the group instead, so 40 and 41 always
    // mean the receiving one (docs/gs.md). One row each rather than a mirror of
    // those two blocks: the statement is that a whole group is absent, and that
    // is one statement however many parameters sit inside it.
    {0x500000, 0x007F00, GsParam::kOppositeGroupBlock, GsLevel::kIgnore, 128, 0x00, 0x7F, 0x00,
     "libsonare receives one port, so there is no second group of parts to address"},
    {0x510000, 0x007F00, GsParam::kOppositeGroupBlock, GsLevel::kIgnore, 128, 0x00, 0x7F, 0x00,
     "libsonare receives one port, so there is no second group's drum setup to address"},
}};

/// The undefined regions covered so far.
inline constexpr std::array<GsAddressRange, 21> kGsUndefinedRanges = {{
    {0x400110, 0x40012F, 0,
     "no row between PATCH NAME and REVERB MACRO; the SC-55/SC-88 PARTIAL RESERVE the SC-8850 "
     "dropped arrives here"},
    {0x400136, 0x400136, 0, "no row between REVERB DELAY FEEDBACK and PREDELAY; a run crosses it"},
    {0x400141, 0x40014F, 0, "no row between the chorus block and DELAY MACRO; a run crosses it"},
    {0x40015B, 0x40017F, 0,
     "no row past DELAY SEND TO REVERB; a run from the delay block crosses it"},
    {0x400204, 0x40027F, 0, "no row past EQ HIGH GAIN; a run from the EQ block crosses it"},
    {0x400302, 0x400302, 0,
     "no row between EFX TYPE and PARAMETER 1; a run from the type crosses it"},
    {0x40031A, 0x40031A, 0, "no row between the EFX sends and CONTROL SOURCE 1; a run crosses it"},
    // Part block. The SC-8850 and SC-88Pro maps agree on 40 1x address for
    // address, so these four gaps are the manual's own and not a transcription
    // difference between the two.
    {0x401025, 0x401029, 0x000F00,
     "no row between RX BANK SELECT LSB and PITCH FINE TUNE; a run crosses it"},
    {0x40102D, 0x40102F, 0x000F00,
     "no row between DELAY SEND LEVEL and TONE MODIFY 1; a run crosses it"},
    {0x401038, 0x40103F, 0x000F00,
     "no row between TONE MODIFY 8 and SCALE TUNING C; a run crosses it"},
    {0x40104C, 0x40107F, 0x000F00,
     "no row past SCALE TUNING B; a run from the scale-tuning block crosses it"},
    // Controller-destination block. Each source owns +00-+0A and the manual
    // defines nothing between the last of its eleven and the next source.
    {0x40200B, 0x40200F, 0x000F00, "no row between the modulation and bend destinations"},
    {0x40201B, 0x40201F, 0x000F00, "no row between the bend and CAf destinations"},
    {0x40202B, 0x40202F, 0x000F00, "no row between the CAf and PAf destinations"},
    {0x40203B, 0x40203F, 0x000F00, "no row between the PAf and CC1 destinations"},
    {0x40204B, 0x40204F, 0x000F00, "no row between the CC1 and CC2 destinations"},
    {0x40205B, 0x40205F, 0x000F00, "no row past the CC2 destinations"},
    {0x402060, 0x40207F, 0x000F00, "no row past the controller-destination block"},
    // Tone map / EQ / EFX block. Real files write into the first of these.
    {0x404002, 0x40401F, 0x000F00, "no row between TONE MAP-0 NUMBER and the part EQ switch"},
    {0x404021, 0x404021, 0x000F00, "no row between the part EQ switch and the part EFX assign"},
    {0x404023, 0x40407F, 0x000F00, "no row past the part EFX assign"},
}};

// --- Frame layer ---

inline constexpr uint8_t kRolandManufacturerId = 0x41;
inline constexpr uint8_t kGsModelId = 0x42;  ///< GS. 0x45 is the SC-88 display.
inline constexpr uint8_t kGsCommandRq1 = 0x11;
inline constexpr uint8_t kGsCommandDt1 = 0x12;

/// A parsed Roland SysEx frame. @c data points into the caller's buffer and is
/// valid only as long as that buffer is.
struct GsFrame {
  bool valid = false;             ///< Well-formed Roland frame, checksum correct.
  uint8_t device = 0;             ///< Device ID as sent; 0x7F is broadcast.
  uint8_t model = 0;              ///< Model ID.
  uint8_t command = 0;            ///< kGsCommandDt1 / kGsCommandRq1.
  uint32_t addr = 0;              ///< 24-bit start address.
  const uint8_t* data = nullptr;  ///< First data byte.
  size_t len = 0;                 ///< Data bytes, checksum excluded.
};

/// Parses a Roland SysEx message, with or without its F0/F7 framing. Anything
/// that is not a Roland frame with a valid checksum — another manufacturer, a
/// truncated message, a data byte with its high bit set — comes back with
/// valid == false. Never crashes.
GsFrame gs_sysex_frame(const uint8_t* data, size_t size) noexcept;

/// The GS address @p n bytes past @p addr. Roland addresses carry at 0x80 per
/// byte, so a run starting at 40 01 7E reaches 40 02 00 on its third byte.
constexpr uint32_t gs_address_offset(uint32_t addr, uint32_t n) noexcept {
  const uint32_t flat =
      ((addr >> 16) & 0x7Fu) * 16384u + ((addr >> 8) & 0x7Fu) * 128u + (addr & 0x7Fu) + n;
  return (((flat / 16384u) & 0x7Fu) << 16) | (((flat / 128u) % 128u) << 8) | (flat % 128u);
}

// --- Lookup ---

/// The table row claiming @p addr, or nullptr. detail::gs_row_claims is the
/// matching rule, usable on a row the table does not hold yet.
const GsAddressEntry* gs_lookup_address(uint32_t addr) noexcept;

/// The undefined region containing @p addr, or nullptr.
const GsAddressRange* gs_lookup_range(uint32_t addr) noexcept;

/// True when @p value is one the row accepts. Out-of-range values are ignored
/// by the apply layer, never clamped.
constexpr bool gs_value_in_range(const GsAddressEntry& entry, uint8_t value) noexcept {
  return value >= entry.lo && value <= entry.hi;
}

/// GS part-parameter block nibble -> zero-based channel: block 0 = part 10
/// (channel 9), blocks 1..9 = parts 1..9, blocks A..F = parts 11..16.
constexpr uint8_t gs_part_block_to_channel(uint8_t block) noexcept {
  if (block == 0) return 9;
  if (block <= 9) return static_cast<uint8_t>(block - 1);
  return block;
}

/// The entity the variable nibble of @p addr selects under @p mask: a zero-based
/// part for the part blocks (40 1x / 40 2x / 40 4x), the raw nibble for an EFX
/// unit (40 3u) or a drum map (41 mn), and 0 when the row has no variable
/// nibble.
uint8_t gs_address_block_index(uint32_t addr, uint32_t mask) noexcept;

// --- Decode ---

/// One decoded data byte.
struct GsWrite {
  GsParam param = GsParam::kUnknown;
  uint8_t part = 0;   ///< gs_address_block_index of the address.
  uint8_t index = 0;  ///< Drum note / block number, or the byte offset in a
                      ///< multi-byte parameter.
  uint8_t value = 0;
  uint32_t addr = 0;  ///< The address this byte landed on.
};

/// Enumerates the writes @p frame carries, one per data byte, walking the main
/// table then the range table. Returns the number of writes the frame contains
/// — which may exceed @p capacity, in which case only the first @p capacity are
/// stored and the caller can see the truncation.
///
/// Only a valid DT1 frame addressed to the GS model produces writes: an RQ1
/// request and another Roland model are accepted and counted as neither writes
/// nor unknowns.
///
/// @param unknown_writes  Optional counter, incremented once per data byte no
///   table row claimed. Those bytes are still emitted, as GsParam::kUnknown.
size_t gs_decode_writes(const GsFrame& frame, GsWrite* out, size_t capacity,
                        uint32_t* unknown_writes) noexcept;

/// gs_sysex_frame followed by gs_decode_writes, so a caller holding a raw SysEx
/// payload does not re-derive the framing. Returns 0 for anything the frame
/// layer refuses.
size_t gs_decode_sysex(const uint8_t* data, size_t size, GsWrite* out, size_t capacity,
                       uint32_t* unknown_writes) noexcept;

// --- Table self-checks ---

namespace detail {

/// Low-byte extent a row covers, as [begin, end].
constexpr uint32_t gs_row_low_begin(const GsAddressEntry& e) noexcept { return e.addr & 0xFFu; }
constexpr uint32_t gs_row_low_end(const GsAddressEntry& e) noexcept {
  return (gs_row_low_begin(e) + e.size - 1u) | (e.mask & 0xFFu);
}

/// True when @p e claims @p addr. The one matching rule, shared by the lookup
/// and the table self-checks.
constexpr bool gs_row_claims(const GsAddressEntry& e, uint32_t addr) noexcept {
  if ((addr & ~e.mask & 0xFFFF00u) != (e.addr & 0xFFFF00u)) return false;
  const uint32_t low = addr & ~e.mask & 0xFFu;
  const uint32_t begin = gs_row_low_begin(e);
  return low >= begin && low < begin + e.size;
}

constexpr bool gs_rows_overlap(const GsAddressEntry& a, const GsAddressEntry& b) noexcept {
  if (((a.addr ^ b.addr) & ~(a.mask | b.mask) & 0xFFFF00u) != 0) return false;
  return gs_row_low_begin(a) <= gs_row_low_end(b) && gs_row_low_begin(b) <= gs_row_low_end(a);
}

constexpr bool gs_table_is_consistent() noexcept {
  for (size_t i = 0; i < kGsAddressTable.size(); ++i) {
    const GsAddressEntry& e = kGsAddressTable[i];
    const bool needs_why = e.level == GsLevel::kIgnore || e.level == GsLevel::kAccept;
    const bool has_why = e.why != nullptr && e.why[0] != '\0';
    if (needs_why != has_why) return false;
    if (e.size == 0) return false;
    if ((e.mask & 0xFFu) != 0 && e.size != 1) return false;  // a masked low byte indexes itself
    if ((e.addr & e.mask) != 0) return false;                // variable nibbles are zero in addr
    if (gs_row_low_end(e) > 0x7Fu) return false;             // a row never carries past its byte
    if (e.lo > e.hi || e.def < e.lo || e.def > e.hi) return false;
    if (i > 0 && kGsAddressTable[i - 1].addr >= e.addr) return false;
    for (size_t j = 0; j < i; ++j) {
      if (gs_rows_overlap(kGsAddressTable[j], e)) return false;
    }
  }
  return true;
}

/// Concrete regions a range row stands for: one per part block when its mask
/// carries the part nibble, otherwise the row itself.
constexpr uint32_t gs_range_block_count(const GsAddressRange& r) noexcept {
  return (r.mask & 0x000F00u) != 0 ? 16u : 1u;
}

constexpr bool gs_ranges_are_consistent() noexcept {
  for (size_t i = 0; i < kGsUndefinedRanges.size(); ++i) {
    const GsAddressRange& r = kGsUndefinedRanges[i];
    if (r.why == nullptr || r.why[0] == '\0') return false;
    if (r.lo_addr > r.hi_addr) return false;
    if ((r.lo_addr & r.mask) != 0 || (r.hi_addr & r.mask) != 0) return false;
    // One mid byte per range. A range crossing one would step through low bytes
    // above 7F, which are not GS addresses, and it is what lets the walk below
    // decide a row on the block base alone. Split such a range instead.
    if ((r.lo_addr & 0xFFFF00u) != (r.hi_addr & 0xFFFF00u)) return false;
    // Both checks below expand the mask into the blocks it reaches rather than
    // comparing masked bases: the variable nibble is the LOW nibble of the mid
    // byte, so clearing it in an unmasked address destroys which block that
    // address was in and a comparison across two different masks silently means
    // something else.
    const uint32_t blocks = gs_range_block_count(r);
    for (size_t j = 0; j < i; ++j) {
      const GsAddressRange& o = kGsUndefinedRanges[j];
      for (uint32_t b = 0; b < blocks; ++b) {
        for (uint32_t ob = 0; ob < gs_range_block_count(o); ++ob) {
          if ((r.lo_addr | (b << 8)) <= (o.hi_addr | (ob << 8)) &&
              (o.lo_addr | (ob << 8)) <= (r.hi_addr | (b << 8))) {
            return false;
          }
        }
      }
    }
    // Row-outermost, with the high-16 test lifted out of the address walk: a
    // row that cannot claim the block's base cannot claim anything in the
    // range, so all but a handful skip the walk entirely. The nesting is not
    // cosmetic — walking every (block, address, row) triple exhausts the
    // constexpr step budget once the table passes about eighty rows.
    for (const GsAddressEntry& e : kGsAddressTable) {
      for (uint32_t block = 0; block < blocks; ++block) {
        const uint32_t base = r.lo_addr | (block << 8);
        if ((base & ~e.mask & 0xFFFF00u) != (e.addr & 0xFFFF00u)) continue;
        for (uint32_t addr = r.lo_addr; addr <= r.hi_addr; ++addr) {
          if (gs_row_claims(e, addr | (block << 8))) return false;
        }
      }
    }
  }
  return true;
}

}  // namespace detail

static_assert(detail::gs_table_is_consistent(),
              "GS address table: a reason missing from a kIgnore/kAccept row or present on a row "
              "that takes none, a bad size/mask/range/default, or two rows claiming one address");
static_assert(detail::gs_ranges_are_consistent(),
              "GS undefined ranges: a row without a reason, an inverted range, or a range "
              "overlapping another range or a defined address");

}  // namespace sonare::midi::synth
