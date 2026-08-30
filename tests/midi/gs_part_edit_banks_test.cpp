/// @file gs_part_edit_banks_test.cpp
/// @brief The eight GS melodic part edits (40 1x 30 TONE MODIFY and the part
///        NRPNs it aliases) probed ONE AT A TIME on each of the two voice
///        banks: eight parameters times SoundFont and model.
///
/// The audibility gate writes the whole eight-byte block at once and asks only
/// whether the render moved, so seven of the eight could go dead and it would
/// still pass. That is the shape of defect this repository has already shipped
/// once — a per-note drum send reached one bank and not the other, and a
/// whole-block probe passed it — and it is what this file exists to catch: each
/// parameter has to move each bank on its own.
///
/// Both banks are probed because the two apply the edits through different code
/// (Sf2VoiceParams via apply_gs_part_params, GsPartMod via NativeSynthVoice::
/// start), and a parameter must not do something different because one of them
/// took the note.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

/// Part 1 = channel 0 = block nibble 1, so every write below is 40 11 30.
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kToneModifyLo = 0x30;
/// Each byte is read as `value - 64`, so 0x40 is the offset at which the
/// parameter is a no-op — which is what lets the other seven be held still.
constexpr uint8_t kCentre = 0x40;

/// Which voice bank answers the note.
enum class Bank : uint8_t {
  kSoundFont,  ///< The fixture preset answers, through apply_gs_part_params.
  kModel,      ///< No SoundFont at all, so the physical-model floor answers.
};

const char* bank_name(Bank bank) { return bank == Bank::kSoundFont ? "soundfont" : "model"; }

/// The program each bank is probed on. The SoundFont bank plays the fixture
/// preset at program 0. The model bank plays GM 82 (Lead 3, calliope), which is
/// the one program whose fallback patch can show all eight at once: a running
/// vibrato (22 cents at 6 Hz, so a rate, a depth and a delay edit all have a
/// depth to act on), a ladder filter at 2.6 kHz that is never bypassed, and a
/// full 35 / 200 / 0.9 / 180 amplitude envelope, so attack, decay and release
/// are each a segment the render crosses.
constexpr uint8_t kSoundFontProgram = 0;
constexpr uint8_t kModelProgram = 82;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A framed Roland DT1 write of @p data at 40 <block> <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t block, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, block, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + block + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Program 0: a square loop carrying vibrato behind an onset delay, a mid
/// filter and a full attack/decay/sustain/release, so each of the eight has
/// something of its own to move — a rate or a delay multiplies a depth, and
/// multiplying a depth of zero is inaudible.
std::shared_ptr<const Sf2File> fixture() {
  static std::shared_ptr<const Sf2File> cached = [] {
    Sf2Builder b;
    std::vector<float> square(128);
    for (size_t i = 0; i < square.size(); ++i) {
      double v = 0.0;
      for (int h = 1; h <= 9; h += 2) {
        v += std::sin(kTwoPi * h * static_cast<double>(i) / 64.0) / h;
      }
      square[i] = 0.6f * static_cast<float>(v);
    }
    const int sq_id = b.add_sample("square500", square, 32000, 60, 0, 128);

    Sf2Builder::ZoneSpec zone;
    zone.gens.push_back({54 /*sampleModes*/, 1});
    zone.gens.push_back({8 /*initialFilterFc*/, 8637});  // ~1.2 kHz
    zone.gens.push_back({6 /*vibLfoToPitch*/, 60});      // cents, so rate/delay bite
    zone.gens.push_back({23 /*delayVibLFO*/, -2000});    // ~0.32 s onset
    zone.gens.push_back({34 /*attackVolEnv*/, -2400});   // 0.25 s, so attack bites
    zone.gens.push_back({36 /*decayVolEnv*/, -1200});    // 0.5 s ...
    zone.gens.push_back({37 /*sustainVolEnv*/, 120});    // ... down to -12 dB, or
                                                         // scaling the decay time
                                                         // scales a flat segment
    zone.gens.push_back({38 /*releaseVolEnv*/, -1200});  // 0.5 s, so release bites
    zone.target = sq_id;
    const int inst = b.add_instrument("squareinst", {zone});

    Sf2Builder::ZoneSpec pz;
    pz.target = inst;
    b.add_preset("Square", 0, kSoundFontProgram, {pz});

    const auto bytes = b.build();
    auto sf2 = std::make_shared<Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    return std::shared_ptr<const Sf2File>(sf2);
  }();
  return cached;
}

/// Held long enough that the attack finishes and the decay is well under way
/// before note-off, and tailed long enough that the release is a segment rather
/// than a cut.
constexpr int kHeld = 36000;
constexpr int kTail = 36000;

using Render = std::vector<float>;

/// One note on @p bank, with @p block written to 40 11 30 first when it is
/// non-empty. Interleaved, so anything that moves either leg shows up.
Render render(Bank bank, const std::vector<uint8_t>& block) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  // Withholding the SoundFont rather than choosing a program it misses: a
  // preset the fixture does cover would answer from the wrong bank silently.
  cfg.synth_fallback = bank == Bank::kModel;
#if defined(SONARE_MIDI_WITH_FX)
  // The part edits are voice parameters; a reverb or delay tail only smears the
  // difference they make into the one place a threshold has to live.
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  if (bank == Bank::kSoundFont) player.set_soundfont(fixture());
  player.prepare(kOutRate, 256);

  const uint8_t program = bank == Bank::kModel ? kModelProgram : kSoundFontProgram;
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, program)));
  if (!block.empty()) {
    const std::vector<uint8_t> msg = dt1(kPartBlock, kToneModifyLo, block);
    REQUIRE(player.handle_sysex(msg.data(), msg.size()));
  }

  std::vector<float> left(kHeld + kTail, 0.0f);
  std::vector<float> right(kHeld + kTail, 0.0f);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  float* held[2] = {left.data(), right.data()};
  player.process(held, 2, kHeld);
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  float* tail[2] = {left.data() + kHeld, right.data() + kHeld};
  player.process(tail, 2, kTail);

  Render out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

double peak(const Render& r) {
  double out = 0.0;
  for (const float s : r) out = std::max(out, std::abs(static_cast<double>(s)));
  return out;
}

double max_difference(const Render& a, const Render& b) {
  REQUIRE(a.size() == b.size());
  double out = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    out = std::max(out, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
  }
  return out;
}

/// One of the eight, and the single byte that moves it.
struct PartEdit {
  const char* name;
  uint8_t index;  ///< Offset inside the 40 1x 30 block, which is address order.
  uint8_t value;  ///< The raw byte, read as `value - 64`.
};

/// The probe is the whole eight-byte TONE MODIFY block with seven bytes held at
/// centre rather than the single NRPN, because that is the form a GS file
/// actually writes and it exercises the block's address ordering as well as the
/// parameter; the NRPN path onto the same storage is already covered by the
/// alias test. The seven centre bytes being no-ops is not assumed — it is
/// asserted by the all-centre case below.
///
/// Every offset is +40 (0x68) except the cutoff, and +40 is far enough to be
/// unmistakable without pinning any parameter to the end of its range: it is
/// 5.7x on an envelope time, 1.8x on the vibrato rate, +120 cents of vibrato
/// depth and 4.0x on filter Q.
constexpr std::array<PartEdit, 8> kPartEdits = {{
    {"vibrato rate", 0, 0x68},
    {"vibrato depth", 1, 0x68},
    // Downward, which is the direction that colours whatever the patch's own
    // corner is. Upward moves both of these two as well (measured), but only
    // because neither patch starts wide open — on one that does, +6000 cents
    // is a corner already past Nyquist moved further past it, and the case
    // would go quiet for a reason that is not the wiring.
    {"TVF cutoff", 2, 0x18},
    {"TVF resonance", 3, 0x68},
    {"EG attack", 4, 0x68},
    {"EG decay", 5, 0x68},
    {"EG release", 6, 0x68},
    // Upward, because downward is exactly inert on the model bank, whose LFO
    // has no onset delay of its own: a shortening edit scales zero and stays
    // zero (gs_vib_delay_seconds), and the render comes back bit-identical.
    // Only a lengthening edit can give that LFO an onset.
    {"vibrato delay", 7, 0x68},
}};

/// The block that moves @p edit and nothing else.
std::vector<uint8_t> block_for(const PartEdit& edit) {
  std::vector<uint8_t> block(kPartEdits.size(), kCentre);
  block[edit.index] = edit.value;
  return block;
}

std::string case_text(const PartEdit& edit, Bank bank) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s, bank=%s (40 %02X %02X, byte %02X = offset %+d)", edit.name,
                bank_name(bank), static_cast<unsigned>(kPartBlock),
                static_cast<unsigned>(kToneModifyLo + edit.index),
                static_cast<unsigned>(edit.value), static_cast<int>(edit.value) - 64);
  return buf;
}

std::string measure_text(double delta, double base_peak) {
  char buf[112];
  std::snprintf(buf, sizeof(buf),
                "peak difference %.6g, %+.1f dB relative to the baseline peak %.6g", delta,
                20.0 * std::log10(std::max(delta, 1.0e-30) / base_peak), base_peak);
  return buf;
}

/// The difference a probe has to make, as a fraction of the baseline's own
/// peak. Four decades over single-precision rounding, so a denormal or a
/// reordered sum cannot reach it, and 35 dB under the closest of the sixteen
/// cases, so it is not tuned to any one of them.
constexpr double kMinRelativeDifference = 1.0e-3;

}  // namespace

TEST_CASE("each GS part edit moves the render on both voice banks", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const Render baseline = render(bank, {});
    const double base_peak = peak(baseline);
    // A silent baseline would score every parameter as inert for a reason that
    // is not the parameter's, so the whole file would pass vacuously.
    INFO("baseline, bank=" << bank_name(bank));
    REQUIRE(base_peak > 1.0e-3);

    for (const PartEdit& edit : kPartEdits) {
      const Render probed = render(bank, block_for(edit));
      const double delta = max_difference(baseline, probed);
      INFO(case_text(edit, bank) << "\n  " << measure_text(delta, base_peak));
      CHECK(delta > kMinRelativeDifference * base_peak);
    }
  }
}

TEST_CASE("a GS part-edit block at centre changes nothing on either bank", "[midi][synth][gs]") {
  // What makes the probes above single-parameter: each writes seven bytes it is
  // not testing, and they are only held still if centre is genuinely a no-op.
  // The block also has to survive the round trip bit-exactly, or a "difference"
  // upstream could be the write itself rather than the byte that moved.
  const std::vector<uint8_t> centred(kPartEdits.size(), kCentre);
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    INFO("bank=" << bank_name(bank));
    const Render baseline = render(bank, {});
    const Render centre = render(bank, centred);
    REQUIRE(peak(baseline) > 1.0e-3);
    CHECK(max_difference(baseline, centre) == 0.0);
  }
}
