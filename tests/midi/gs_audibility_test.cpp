/// @file gs_audibility_test.cpp
/// @brief The gate behind GsLevel: every row of kGsAddressTable is probed with a
///        value it accepts, and what the render and the player's mirrors do with
///        it has to be what the row's level promised.
///
/// docs/gs.md verifies kAudible by "changing the value produces a measurable
/// difference", and until this file nothing checked it. The corpus census counts
/// an address as answered once it has a row, so a row that decodes, stores its
/// byte and is then read by nobody looks identical to one the audio depends on.
/// The check runs in both directions — a row below kAudible that does move the
/// audio is the same kind of defect — and it walks the table itself rather than
/// a hand-written list, so a row added later is covered without anyone
/// remembering to add a case.
///
/// The build must carry the FX suite and the insert factory: the system-effect
/// rows are audible only inside SONARE_MIDI_WITH_FX and the EFX rows only once
/// a chain builds, so a build without either fails here rather than passing
/// those rows dry.
///
/// Deliberately untagged at about six seconds, past the threshold where a case
/// takes [.][slow]. That tag would take it out of the default run, and a gate
/// that has to be asked for is most of the way to not existing — which is the
/// state this file was written to end. Probing both voice banks is what costs
/// the time and it is not optional: probing one is how a parameter reaching
/// only half the voices passed.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
#include "mastering/api/insert_factory.h"
#endif

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::gs_param_name;
using sonare::midi::synth::gs_part_block_to_channel;
using sonare::midi::synth::GsAddressEntry;
using sonare::midi::synth::GsLevel;
using sonare::midi::synth::GsParam;
using sonare::midi::synth::kGsAddressTable;

/// GS part-parameter block 1, which gs_part_block_to_channel maps to channel 0 —
/// the melodic part the stimulus sustains. Block 0 would be channel 9, the
/// rhythm part, where the key-shift and mono/poly rows are exempt by the manual.
constexpr uint8_t kMelodicBlock = 1;

const char* level_name(GsLevel level) {
  switch (level) {
    case GsLevel::kAudible:
      return "AUDIBLE";
    case GsLevel::kState:
      return "STATE";
    case GsLevel::kAccept:
      return "ACCEPT";
    case GsLevel::kIgnore:
      return "IGNORE";
  }
  return "?";
}

std::string addr_text(uint32_t addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02X %02X %02X", static_cast<unsigned>((addr >> 16) & 0xFFu),
                static_cast<unsigned>((addr >> 8) & 0xFFu), static_cast<unsigned>(addr & 0xFFu));
  return buf;
}

std::string bytes_text(const std::vector<uint8_t>& data) {
  std::string out;
  for (const uint8_t b : data) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>(b));
    if (!out.empty()) out += ' ';
    out += buf;
  }
  return out;
}

/// A framed Roland DT1 write of @p data at @p addr, with the checksum.
std::vector<uint8_t> dt1(uint32_t addr, const std::vector<uint8_t>& data) {
  const uint8_t a0 = static_cast<uint8_t>((addr >> 16) & 0x7Fu);
  const uint8_t a1 = static_cast<uint8_t>((addr >> 8) & 0x7Fu);
  const uint8_t a2 = static_cast<uint8_t>(addr & 0x7Fu);
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = a0 + a1 + a2;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// The state a row needs before its value can move anything.
enum class Setup : uint8_t {
  kNone,
  kEfxTypeOnly,   ///< An EFX type is selected; no part is routed through it.
  kEfxRouteOnly,  ///< Part 1 is routed; no type is selected.
  kEfxActive,     ///< Both, so an insertion chain is built and the part bussed.
  kEqLifted,      ///< Both master-EQ bands off 0 dB, where the EQ is a no-op.
  kVolumeCut,     ///< MASTER VOLUME pulled down, for the reset-command rows.
  kBendApplied,   ///< The melodic part bent fully up, where a bend range scales.
  kModWheelUp,    ///< The melodic part's CC1 raised, where a mod depth scales.
  /// The rhythm part's SECOND note put in a group, where an assign group only
  /// says something once two notes are in one.
  kDrumGroupPeer,
  /// The rhythm part switched to a user drum set, which is the only state in
  /// which anything reads the 21 dn rr block.
  kUserDrumSet,
  /// That, and the set's SECOND note put in a group — what the drum setup's
  /// assign-group row needs, needed again one layer down.
  kUserDrumSetGroupPeer,
};

const char* setup_name(Setup setup) {
  switch (setup) {
    case Setup::kNone:
      return "none";
    case Setup::kEfxTypeOnly:
      return "efx-type";
    case Setup::kEfxRouteOnly:
      return "efx-route";
    case Setup::kEfxActive:
      return "efx-active";
    case Setup::kEqLifted:
      return "eq-lifted";
    case Setup::kVolumeCut:
      return "volume-cut";
    case Setup::kBendApplied:
      return "bend-applied";
    case Setup::kModWheelUp:
      return "mod-wheel-up";
    case Setup::kDrumGroupPeer:
      return "drum-group-peer";
    case Setup::kUserDrumSet:
      return "user-drum-set";
    case Setup::kUserDrumSetGroupPeer:
      return "user-drum-set-group-peer";
  }
  return "?";
}

/// The other note the stimulus strikes, and the one a drum-setup row uses when
/// it needs a second note to say anything.
constexpr uint32_t kProbeDrumPeerNote = 42;

std::vector<std::vector<uint8_t>> setup_writes(Setup setup) {
  // Overdrive (01 10) is a type gs_efx_insert_chain realises as one stage, so a
  // chain builds and the part is bussed.
  const std::vector<uint8_t> type = dt1(0x400300, {0x01, 0x10});
  const std::vector<uint8_t> route = dt1(0x404022u | (kMelodicBlock << 8), {0x01});
  switch (setup) {
    case Setup::kNone:
      return {};
    case Setup::kEfxTypeOnly:
      return {type};
    case Setup::kEfxRouteOnly:
      return {route};
    case Setup::kEfxActive:
      return {type, route};
    case Setup::kEqLifted:
      // +12 dB low, -12 dB high, both corners left at their defaults so a FREQ
      // probe is the only thing that moves them.
      return {dt1(0x400200, {0x00, 0x4C, 0x00, 0x34})};
    case Setup::kVolumeCut:
      return {dt1(0x400004, {0x20})};
    case Setup::kDrumGroupPeer:
      // The stimulus's second drum note, put in the group the probe writes, so
      // the probe's note has something to be choked by.
      return {dt1(0x410300u | kProbeDrumPeerNote, {0x7F})};
    case Setup::kUserDrumSetGroupPeer:
      // The same peer the drum setup's group row needs, written into the set
      // rather than into the map so the set's own storage is what is probed.
      return {dt1(0x210300u | kProbeDrumPeerNote, {0x7F})};
    case Setup::kBendApplied:
    case Setup::kModWheelUp:
    case Setup::kUserDrumSet:
      return {};
  }
  return {};
}

Setup setup_for(const GsAddressEntry& row) {
  switch (row.param) {
    // The EFX parameters and sends read nothing until a chain exists.
    case GsParam::kEfxParameter:
    case GsParam::kEfxSendToReverb:
    case GsParam::kEfxSendToChorus:
    case GsParam::kEfxSendToDelay:
    case GsParam::kEfxControlSource1:
    case GsParam::kEfxControlDepth1:
    case GsParam::kEfxControlSource2:
    case GsParam::kEfxControlDepth2:
    case GsParam::kEfxSendEqSwitch:
      return Setup::kEfxActive;
    // The type is the probe, so the setup supplies only the routing.
    case GsParam::kEfxType:
      return Setup::kEfxRouteOnly;
    // The routing is the probe, so the setup supplies only the type.
    case GsParam::kPartEfxAssign:
      return Setup::kEfxTypeOnly;
    // A corner or a per-part bypass means nothing while both bands are flat —
    // the state that makes the power-on EQ bit-exact.
    case GsParam::kEqLowFreq:
    case GsParam::kEqHighFreq:
    case GsParam::kPartEqSwitch:
      return Setup::kEqLifted;
    // The two reset commands accept their default and nothing else, so they are
    // probed as commands: perturb an audible parameter, then write the address
    // and require the render to move back.
    case GsParam::kSystemModeSet:
    case GsParam::kModeSet:
      return Setup::kVolumeCut;
    // A bend range scales the bend, so it says nothing while the wheel is
    // centred — which is where the stimulus leaves it.
    case GsParam::kPartBendPitchControl:
      return Setup::kBendApplied;
    // Likewise anything the wheel scales, which says nothing at CC1 zero.
    case GsParam::kPartModLfo1PitchDepth:
    case GsParam::kPartModTvfCutoff:
    case GsParam::kPartModLfo1Rate:
    case GsParam::kPartModLfo1TvaDepth:
      return Setup::kModWheelUp;
    // A group is a relation, so one note in it chokes nothing.
    case GsParam::kDrumAssignGroup:
      return Setup::kDrumGroupPeer;
    // A user drum set is stored whether or not anything plays it; the rhythm
    // part has to have selected it before a note reads one.
    case GsParam::kUserDrumSourceProgram:
    case GsParam::kUserDrumSourceNote:
    case GsParam::kUserDrumPlayNote:
    case GsParam::kUserDrumLevel:
    case GsParam::kUserDrumPanpot:
    case GsParam::kUserDrumReverbSend:
    case GsParam::kUserDrumChorusSend:
    case GsParam::kUserDrumRxNoteOn:
    case GsParam::kUserDrumDelaySend:
      return Setup::kUserDrumSet;
    // And a group still needs its peer, one layer further down.
    case GsParam::kUserDrumAssignGroup:
      return Setup::kUserDrumSetGroupPeer;
    default:
      return Setup::kNone;
  }
}

/// The bytes a row is probed with, and why the generic rule did not pick them.
struct Probe {
  std::vector<uint8_t> bytes;
  const char* why = nullptr;  ///< Non-null when this overrides the generic rule.
};

/// Generic rule: a value inside [lo, hi] that differs from the row's default,
/// preferring hi. A multi-byte row takes the whole run at that value, so the
/// last address of the run is written as well as the first.
Probe probe_for(const GsAddressEntry& row) {
  switch (row.param) {
    case GsParam::kMasterTune:
      // The four nibbles are one 0018-07E8 word, so 0F in all four asks for
      // +6451 cents — far outside the range the consumer bounds. The probe is
      // the manual's own maximum, 07E8 = +100 cents.
      return {{0x00, 0x07, 0x0E, 0x08},
              "the row bounds a nibble; the aggregate word has its own range"};
    case GsParam::kEfxType:
      // 7F 7F is a type no adapter realises, so no chain is built and the part
      // is never bussed. 01 10 is Overdrive, which realises.
      return {{0x01, 0x10}, "the generic value selects a type nothing realises"};
    case GsParam::kPartModTvfCutoff:
      // hi opens the filter, and the stimulus zone's is already open: +9450
      // cents onto a cutoff above the band leaves every partial where it was.
      // lo is the same edit downwards, which the same zone can show.
      return {{0x00}, "hi raises a cutoff that is already above the signal"};
    case GsParam::kPartAssignMode:
      // 01 LIMITED-MULTI and 02 FULL-MULTI are one behaviour here and only 00
      // SINGLE branches (docs/gs.md), so hi is inert by design.
      return {{0x00}, "hi is deliberately the same behaviour as the default"};
    default:
      break;
  }
  const uint8_t value = row.hi != row.def ? row.hi : row.lo;
  return {std::vector<uint8_t>(row.size, value), nullptr};
}

/// The note the rhythm part sounds first, and the one a drum-setup row is probed
/// on: an edit to any other note is inaudible however well the row works.
constexpr uint32_t kProbeDrumNote = 38;

/// The concrete address a row is probed at: a part row resolves its block nibble
/// to the melodic part the stimulus plays, and a drum-setup row resolves to map
/// 1 (the zero-based nibble 0, which channel 9 powers on reading) and a note the
/// stimulus strikes. A channel-nibble row (00 01 xx) is probed at channel 0,
/// which its base address already carries.
uint32_t probe_address(const GsAddressEntry& row) {
  if (row.mask == 0x000F00u) return row.addr | (static_cast<uint32_t>(kMelodicBlock) << 8);
  if (row.mask == 0x00F07Fu) return row.addr | kProbeDrumNote;
  // A whole-block row is probed at its part page rather than at mid byte 00, so
  // the run writes what a file addressing that block actually writes.
  if (row.mask == 0x007F00u) return row.addr | (uint32_t{0x11} << 8);
  return row.addr;
}

}  // namespace

#if !defined(SONARE_MIDI_WITH_FX) || !defined(SONARE_WITH_MASTERING)

TEST_CASE("the GS audibility gate needs the FX suite and the insert factory", "[midi][synth][gs]") {
  FAIL(
      "built without SONARE_MIDI_WITH_FX / SONARE_WITH_MASTERING: the system-effect and EFX rows "
      "cannot be probed, so passing here would prove nothing");
}

#else

namespace {

using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsSystemEffects;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

/// Half the usual rate, which halves the work: every effect parameter the table
/// reaches is a time or a frequency in physical units, so the timing the probes
/// depend on is unchanged and the whole gate stays inside the default run.
constexpr double kOutRate = 24000.0;
constexpr double kTwoPi = 6.28318530717958647692;
/// Four quarter-second segments: the notes start, one is retriggered, they are
/// released, and the last quarter is nothing but the reverb / chorus / delay
/// tails the effect rows move.
///
/// One second is the shortest window that reveals every row, and the binding
/// constraint is the delay: its feedback and its macro only separate on the
/// SECOND repeat of the power-on 340 ms tap. At two thirds of a second both read
/// as inert — a stimulus failure that looks exactly like an unread row.
constexpr int kSegment = 6000;
constexpr int kFrames = 4 * kSegment;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Bank 0 program 0 is a looped sine (a sustaining melodic voice); bank 128
/// program 0 is a one-shot burst, which is what channel 9 resolves to.
///
/// The burst runs several hundred milliseconds rather than the tens a kit piece
/// would: the stimulus strikes its two drum notes a quarter of a second apart,
/// and a choke between them is only visible while the first is still sounding.
std::shared_ptr<const Sf2File> fixture() {
  static std::shared_ptr<const Sf2File> cached = [] {
    Sf2Builder b;
    std::vector<float> sine(96);
    for (size_t i = 0; i < sine.size(); ++i) {
      sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
    }
    const int sine_id = b.add_sample("sine", sine, 32000, 60, 32, 96);
    std::vector<float> burst(12288);
    for (size_t i = 0; i < burst.size(); ++i) {
      const float envl = 1.0f - static_cast<float>(i) / static_cast<float>(burst.size());
      burst[i] = envl * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 7.0));
    }
    const int burst_id = b.add_sample("burst", burst, 48000, 60, 0, 12288);

    Sf2Builder::ZoneSpec looped;
    looped.gens.push_back({54 /*sampleModes*/, 1});
    looped.target = sine_id;
    const int sine_inst = b.add_instrument("sine", {looped});
    Sf2Builder::ZoneSpec oneshot;
    // The kit's own reverb and chorus sends, at half scale, so the 41 m5 /
    // 41 m6 rows have a send to scale whatever the part is sending.
    oneshot.gens.push_back({16 /*reverbEffectsSend*/, 500});
    oneshot.gens.push_back({15 /*chorusEffectsSend*/, 500});
    oneshot.target = burst_id;
    const int burst_inst = b.add_instrument("burst", {oneshot});

    Sf2Builder::ZoneSpec pz;
    pz.target = sine_inst;
    b.add_preset("Sine", 0, 0, {pz});
    pz.target = burst_inst;
    // A second melodic program, for the same reason the second kit below
    // exists: a row that chooses a PROGRAM has nothing to choose with one.
    b.add_preset("Burst", 0, 127, {pz});
    b.add_preset("Kit", 128, 0, {pz});
    // A second rhythm kit, voiced by the looped sine so it cannot be mistaken
    // for the first. resolve_preset falls every rhythm program back to program
    // 0, so with one kit a row that chooses BETWEEN kits has nothing to choose
    // and reads as inert on this bank however well it works.
    pz.target = sine_inst;
    b.add_preset("Kit 2", 128, 127, {pz});

    const auto bytes = b.build();
    auto sf2 = std::make_shared<Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    return std::shared_ptr<const Sf2File>(sf2);
  }();
  return cached;
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

void cc(Sf2Player& p, uint8_t channel, uint8_t controller, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

void note_on(Sf2Player& p, uint8_t channel, uint8_t note, uint8_t velocity) {
  p.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, velocity)));
}

void note_off(Sf2Player& p, uint8_t channel, uint8_t note) {
  p.on_event(0, event(sonare::midi::make_midi1_note_off(0, channel, note, 0)));
}

/// The part of a setup that is channel messages rather than SysEx. A row whose
/// value only scales a controller reads as inert until that controller is off
/// its neutral position, and setup_writes goes through handle_sysex, which a
/// channel message is not.
void setup_channel_state(Sf2Player& p, Setup setup) {
  // Fully up in both cases, on the melodic part the stimulus plays: the widest
  // separation between one depth and another.
  const uint8_t ch = gs_part_block_to_channel(kMelodicBlock);
  if (setup == Setup::kBendApplied) {
    p.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, ch, 16383)));
  } else if (setup == Setup::kModWheelUp) {
    cc(p, ch, 1, 127);
  } else if (setup == Setup::kUserDrumSet || setup == Setup::kUserDrumSetGroupPeer) {
    // Set 1, on the rhythm part the stimulus strikes. With nothing written to
    // the set this sounds the Standard kit, which is what the part was already
    // playing, so the baseline it renders is the probe's only difference.
    p.on_event(0, event(sonare::midi::make_midi1_program_change(
                      0, 9, sonare::midi::synth::kGsUserDrumSetProgram)));
  }
}

/// One stimulus, able to reveal every kind of row: a melodic part sustaining a
/// chord (mono/poly, assign mode, tuning, level, pan), a rhythm part sounding
/// drum notes, a retrigger of a held note (SINGLE assign mode), a release well
/// before the end so the effect tails stand alone, and non-zero chorus and delay
/// sends — both power on at zero, so without them neither unit carries signal
/// and no chorus or delay row could move anything.
///
/// Which voice bank answers the stimulus. A parameter must not do something
/// different because one of them took the note (docs/gs.md, the ASSIGN MODE
/// bullet), so every row is probed on both — and a row is only AUDIBLE if it
/// moves both. Probing one alone is how the drum delay multiplicand shipped
/// reaching the SoundFont voices and not the model ones: it moved the render,
/// so the gate passed it, and half of it did nothing.
enum class Bank : uint8_t {
  kSoundFont,  ///< The fixture's presets answer every note.
  kModel,      ///< No SoundFont at all, so the physical-model floor answers.
};

const char* bank_name(Bank bank) { return bank == Bank::kSoundFont ? "soundfont" : "model"; }

/// A row known to reach one bank and not the other, with what is missing. The
/// gate requires each of these to STILL fail, so an entry cannot outlive the
/// gap it excuses — the discipline the parity allowlist has, for the same
/// reason: a stale entry keeps blessing a defect nobody re-examined.
struct BankGap {
  GsParam param;
  Bank silent;
  const char* why;
};

/// Empty, and worth keeping so: every row this gate probes reaches both banks.
constexpr std::array<BankGap, 0> kBankGaps = {};

const BankGap* bank_gap_for(const GsAddressEntry& row, Bank bank) {
  for (const BankGap& gap : kBankGaps) {
    if (gap.param == row.param && gap.silent == bank) return &gap;
  }
  return nullptr;
}

/// @param probe  DT1 messages under test; empty renders the baseline.
/// @param accepted  Set false when the player refused one of them.
StereoRender render(Bank bank, Setup setup, const std::vector<std::vector<uint8_t>>& probe,
                    bool* accepted = nullptr) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  // Offline: process() realises a pending EFX chain and system-effect state
  // inline, which is the path a bounce takes and the only one handle_sysex
  // feeds for the 40 01 / 40 02 / 40 03 blocks.
  cfg.realize_efx_inline = true;
  cfg.synth_fallback = bank == Bank::kModel;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player(cfg);
  // Withholding the SoundFont rather than choosing programs it misses: a preset
  // the fixture does cover would answer from the wrong bank without saying so.
  if (bank == Bank::kSoundFont) player.set_soundfont(fixture());
  player.prepare(kOutRate, 256);

  for (const uint8_t channel : {uint8_t{0}, uint8_t{9}}) {
    cc(player, channel, 93, 64);  // chorus send
    cc(player, channel, 94, 64);  // delay send
  }
  for (const std::vector<uint8_t>& msg : setup_writes(setup)) {
    REQUIRE(player.handle_sysex(msg.data(), msg.size()));
  }
  setup_channel_state(player, setup);
  for (const std::vector<uint8_t>& msg : probe) {
    const bool ok = player.handle_sysex(msg.data(), msg.size());
    if (accepted != nullptr && !ok) *accepted = false;
  }

  StereoRender out;
  out.left.assign(static_cast<size_t>(kFrames), 0.0f);
  out.right.assign(static_cast<size_t>(kFrames), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};

  note_on(player, 0, 48, 100);
  note_on(player, 0, 55, 100);
  note_on(player, 0, 60, 100);
  note_on(player, 9, 38, 110);
  player.process(chans, 2, kSegment);

  chans[0] += kSegment;
  chans[1] += kSegment;
  note_on(player, 0, 60, 100);  // retrigger: SINGLE stops the held one
  note_on(player, 9, 42, 110);
  player.process(chans, 2, kSegment);

  chans[0] += kSegment;
  chans[1] += kSegment;
  note_off(player, 0, 48);
  note_off(player, 0, 55);
  note_off(player, 0, 60);
  player.process(chans, 2, 2 * kSegment);
  return out;
}

double peak(const StereoRender& r) {
  double out = 0.0;
  for (const float s : r.left) out = std::max(out, std::abs(static_cast<double>(s)));
  for (const float s : r.right) out = std::max(out, std::abs(static_cast<double>(s)));
  return out;
}

double max_difference(const StereoRender& a, const StereoRender& b) {
  double out = 0.0;
  for (size_t i = 0; i < a.left.size(); ++i) {
    out = std::max(out, std::abs(static_cast<double>(a.left[i]) - b.left[i]));
    out = std::max(out, std::abs(static_cast<double>(a.right[i]) - b.right[i]));
  }
  return out;
}

/// Whether the probe reached a mirror the player exposes.
enum class Mirror : uint8_t {
  kUnmodelled,  ///< The block this row sits in has no accessor of this shape.
  kStored,      ///< The write moved the mirror.
  kNotStored,   ///< The write left it untouched.
};

/// Which of those @p probe produced. This is the kState half of the gate and the
/// kAccept / kIgnore half at once, without a per-row field table: the two blocks
/// that hold the rows in question both have an accessor, and the comparison is
/// over the whole struct.
///
/// Modelled for the patch-common / system-effect (40 01 xx) and EFX (40 03 xx)
/// blocks only — the master, part and EQ mirrors have no accessor of this shape,
/// and no row outside kAudible sits in them.
Mirror mirror_state(const GsAddressEntry& row, const std::vector<uint8_t>& probe) {
  const uint32_t block = row.addr & 0xFFFF00u;
  if (block != 0x400100u && block != 0x400300u) return Mirror::kUnmodelled;
  Sf2PlayerConfig cfg;
  cfg.realize_efx_inline = true;
  cfg.synth_fallback = false;
  Sf2Player player(cfg);
  for (const std::vector<uint8_t>& msg : setup_writes(setup_for(row))) {
    player.handle_sysex(msg.data(), msg.size());
  }
  const GsSystemEffects before_fx = player.gs_system_effects();
  const GsEfx before_efx = player.gs_efx();
  player.handle_sysex(probe.data(), probe.size());
  const bool fx_moved =
      std::memcmp(&before_fx, &player.gs_system_effects(), sizeof(GsSystemEffects)) != 0;
  const GsEfx& after_efx = player.gs_efx();
  const bool efx_moved = before_efx.type != after_efx.type ||
                         before_efx.params != after_efx.params ||
                         before_efx.send_reverb != after_efx.send_reverb ||
                         before_efx.send_chorus != after_efx.send_chorus ||
                         before_efx.send_delay != after_efx.send_delay;
  return (fx_moved || efx_moved) ? Mirror::kStored : Mirror::kNotStored;
}

std::string row_text(const GsAddressEntry& row, const Probe& probe, Setup setup, Bank bank) {
  std::string out = addr_text(probe_address(row)) + "  " + gs_param_name(row.param) + "  " +
                    level_name(row.level) + "  bank=" + bank_name(bank) +
                    "  probe=" + bytes_text(probe.bytes);
  if (setup != Setup::kNone) out += "  setup=" + std::string(setup_name(setup));
  if (probe.why != nullptr) out += "\n      override: " + std::string(probe.why);
  return out;
}

}  // namespace

TEST_CASE("every GS address row keeps the promise its level makes", "[midi][synth][gs]") {
  // A chain that never builds would make every EFX row read as unheard for a
  // reason that is not the table's.
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  // Every part row is probed on this block, so the mapping is checked once
  // rather than per row.
  REQUIRE(gs_part_block_to_channel(kMelodicBlock) == 0);

  std::map<std::pair<Bank, Setup>, StereoRender> baselines;
  auto baseline = [&baselines](Bank bank, Setup setup) -> const StereoRender& {
    const std::pair<Bank, Setup> key{bank, setup};
    auto it = baselines.find(key);
    if (it == baselines.end()) it = baselines.emplace(key, render(bank, setup, {})).first;
    return it->second;
  };

  struct Finding {
    uint32_t addr;
    std::string text;
    double relative;
  };
  std::vector<Finding> unheard;  ///< AUDIBLE, and the render did not move.
  std::vector<Finding> heard;    ///< Not AUDIBLE, and the render moved.
  std::vector<Finding> unheld;   ///< STATE, and nothing stored the byte.
  std::vector<Finding> held;     ///< ACCEPT / IGNORE, and something stored it.
  std::vector<Finding> faint;
  std::vector<Finding> unapplied;   ///< AUDIBLE / STATE, and no apply layer took it.
  std::vector<Finding> applied;     ///< ACCEPT / IGNORE, and one did.
  std::vector<Finding> fixed_gaps;  ///< A kBankGaps entry whose gap is closed.
  int probed = 0;

  for (const GsAddressEntry& row : kGsAddressTable) {
    // Only a fixed address, a part block, a channel nibble, a drum map with or
    // without a note, or a whole-block mid byte appears in the table today. An
    // EFX-unit (40 3u) row would need its own resolution and must not fall
    // through to an untested one.
    REQUIRE((row.mask == 0 || row.mask == 0x000F00u || row.mask == 0x00000Fu ||
             row.mask == 0x00F07Fu || row.mask == 0x00F000u || row.mask == 0x007F00u));

    const Probe probe = probe_for(row);
    const Setup setup = setup_for(row);
    const std::vector<uint8_t> message = dt1(probe_address(row), probe.bytes);
    bool accepted = true;
    ++probed;

    for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
      const StereoRender& base = baseline(bank, setup);
      const double base_peak = peak(base);
      // A silent baseline scores every row as unheard for a reason that is not
      // the row's.
      INFO(row_text(row, probe, setup, bank));
      REQUIRE(base_peak > 1.0e-4);

      const StereoRender probed_render = render(bank, setup, {message}, &accepted);
      const double delta = max_difference(base, probed_render);
      const double relative = delta / base_peak;
      const Finding finding{probe_address(row), row_text(row, probe, setup, bank), relative};

      const BankGap* gap = bank_gap_for(row, bank);
      if (gap != nullptr) {
        // The entry expires with the gap: once the row does reach this bank it
        // has to be deleted, or the gate says so here.
        if (delta != 0.0) {
          fixed_gaps.push_back({finding.addr, finding.text + "\n      excused: " + gap->why, 0.0});
        }
        continue;
      }

      if (row.level == GsLevel::kAudible) {
        if (delta == 0.0) {
          unheard.push_back(finding);
        } else if (relative < 1.0e-4) {
          // Audible in the sense that something moved, but 80 dB under the
          // render's own peak — worth naming rather than passing without
          // comment.
          faint.push_back(finding);
        }
      } else if (delta != 0.0) {
        heard.push_back(finding);
      }
    }

    // handle_sysex answers "an apply layer took this", which the top two levels
    // owe and the bottom two owe the negation of — a discarded byte that an
    // apply layer took is not discarded. Being decoded rather than counted as
    // unknown is the separate, weaker claim all four make, asserted below.
    // Neither this nor the mirror depends on which bank sounded the note.
    const Finding finding{probe_address(row), row_text(row, probe, setup, Bank::kSoundFont), 0.0};
    const bool wants_apply = row.level == GsLevel::kAudible || row.level == GsLevel::kState;
    if (wants_apply && !accepted) unapplied.push_back(finding);
    if (!wants_apply && accepted) applied.push_back(finding);

    // The rest is the STATE / ACCEPT / IGNORE split, which AUDIBLE has no part
    // in; whether the audio moved was settled per bank above.
    if (row.level == GsLevel::kAudible) continue;

    const Mirror mirror = mirror_state(row, message);
    if (row.level == GsLevel::kState) {
      // "Received and held": a byte nothing stores is not held, whatever the
      // row says. Unmodelled blocks hold no STATE row, so this is not a hole.
      if (mirror != Mirror::kStored) unheld.push_back(finding);
    } else if (mirror == Mirror::kStored) {
      // The converse: a discarded byte that turns out to be stored is STATE.
      held.push_back(finding);
    }
  }

  auto by_address = [](const Finding& a, const Finding& b) { return a.addr < b.addr; };
  for (std::vector<Finding>* list :
       {&unheard, &heard, &unheld, &held, &faint, &unapplied, &applied}) {
    std::sort(list->begin(), list->end(), by_address);
  }

  for (const Finding& f : faint) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", 20.0 * std::log10(f.relative));
    WARN("GS row moved the render by only " << buf << " dB under its peak: " << f.text);
  }

  // One scoped INFO rather than a run of UNSCOPED_INFO, which the first
  // assertion to report would consume and leave the rest of the CHECKs bare.
  std::string report = "probed " + std::to_string(probed) + " rows";
  auto add = [&report](const char* what, const std::vector<Finding>& list) {
    if (list.empty()) return;
    report += "\n" + std::to_string(list.size()) + " row(s) " + what + ':';
    for (const Finding& f : list) report += "\n  " + f.text;
  };
  add("claiming AUDIBLE with no render difference", unheard);
  add("moving the render without claiming AUDIBLE", heard);
  add("claiming STATE with nothing holding the byte", unheld);
  add("claiming ACCEPT / IGNORE while the byte is held", held);
  add("claiming AUDIBLE / STATE that no apply layer took", unapplied);
  add("claiming ACCEPT / IGNORE that an apply layer took", applied);
  add("excused as reaching one bank only, and now reaching both — delete the entry", fixed_gaps);
  INFO(report);
  CHECK(unheard.empty());
  CHECK(heard.empty());
  CHECK(unheld.empty());
  CHECK(held.empty());
  CHECK(unapplied.empty());
  CHECK(applied.empty());
  CHECK(fixed_gaps.empty());
}

#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("a kAudible GS row whose range admits only its default has no probe value",
          "[midi][synth][gs]") {
  // Such a row cannot be probed by changing its value, because there is no other
  // value to change to. Both of these are commands rather than parameters — a
  // write at all is what acts — so the loop above probes them by perturbing
  // MASTER VOLUME first and requiring the write to undo it. The set is asserted
  // so a third row of this shape cannot arrive unnoticed.
  std::vector<std::string> found;
  for (const GsAddressEntry& row : kGsAddressTable) {
    if (row.level != GsLevel::kAudible) continue;
    if (row.lo != row.hi || row.lo != row.def) continue;
    found.push_back(addr_text(row.addr) + " " + gs_param_name(row.param));
  }
  const std::vector<std::string> expected{"00 00 7F kSystemModeSet", "40 00 7F kModeSet"};
  for (const std::string& row : found) UNSCOPED_INFO("  " << row);
  CHECK(found == expected);
}

TEST_CASE("a GS row below AUDIBLE is still received", "[midi][synth][gs]") {
  // All four levels begin with "received", and the three below AUDIBLE are the
  // ones where nothing downstream would notice if the decode had stopped
  // claiming the address: the byte goes nowhere either way, so only the decoder
  // itself can say the write was understood rather than counted as unknown.
  size_t rows = 0;
  for (const GsAddressEntry& row : kGsAddressTable) {
    if (row.level == GsLevel::kAudible) continue;
    ++rows;
    const uint8_t value = row.hi != row.def ? row.hi : row.lo;
    const std::vector<uint8_t> msg = dt1(row.addr, {value});
    sonare::midi::synth::GsWrite write;
    uint32_t unknown = 0;
    INFO(addr_text(row.addr) << " " << gs_param_name(row.param) << " " << level_name(row.level));
    REQUIRE(sonare::midi::synth::gs_decode_sysex(msg.data(), msg.size(), &write, 1, &unknown) == 1);
    CHECK(write.param == row.param);
    CHECK(write.value == value);
    CHECK(unknown == 0);
  }
  CHECK(rows > 0);
}
