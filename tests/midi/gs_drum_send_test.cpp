/// @file gs_drum_send_test.cpp
/// @brief 41 m5 / m6 / m9: a drum note's send multiplicand against the bus it
/// scales, on both voice banks.
///
/// docs/gs.md, deliberate divergences: the three sends are multiplicands of what
/// the note sends into that unit rather than additions to it, and the same rule
/// covers all three. The properties asserted here are that rule's two halves —
/// the multiplicand at zero empties the bus however loud the part is, and a note
/// whose send comes from its part alone goes silent with the part — for the
/// SoundFont voices and the model-bank voices alike, since a parameter must not
/// depend on which bank answered.
///
/// Silence is asserted against a render that feeds the unit nothing at all
/// (no zone send, no part send), not against an arbitrary quiet threshold, and
/// every case is paired with a probe that must move the same bus, so an
/// inaudible stimulus cannot satisfy the equalities for the wrong reason.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
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

constexpr double kOutRate = 24000.0;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kNote = 38;
/// Long enough for the power-on 340 ms delay tap to land inside the render.
constexpr int kHeld = 6000;
constexpr int kTail = 12000;

/// One system effect: the part-level controller that feeds it and the drum
/// setup parameter nibble that scales the note's share of it.
struct Unit {
  const char* name;
  uint8_t part_cc;
  uint8_t param_nibble;
};
constexpr std::array<Unit, 3> kUnits = {{
    {"reverb", 91, 0x5},
    {"chorus", 93, 0x6},
    {"delay", 94, 0x9},
}};
constexpr size_t kReverb = 0;
constexpr size_t kChorus = 1;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Bank 128 program 0: a looped sine whose zone carries the send generators at
/// @p zone_reverb / @p zone_chorus (0.1% units). Both at zero is a kit that
/// sends nothing of its own, which is the case the part's send is the whole of.
std::shared_ptr<Sf2File> make_kit(int zone_reverb, int zone_chorus) {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.gens.push_back({16 /*reverbEffectsSend*/, static_cast<int16_t>(zone_reverb)});
  zone.gens.push_back({15 /*chorusEffectsSend*/, static_cast<int16_t>(zone_chorus)});
  zone.target = sine_id;
  const int inst = b.add_instrument("sineinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Kit", 128, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

/// One rendered stimulus. Indices into `part` / `mult` are kUnits' order.
struct Stimulus {
  /// No SoundFont at all, so the physical-model fallback voices answer.
  bool model_bank = false;
  int zone_reverb = 0;
  int zone_chorus = 0;
  std::array<uint8_t, 3> part = {0, 0, 0};
  /// 41 mn rr write; -1 leaves the parameter unwritten (the kit's own value).
  std::array<int, 3> mult = {-1, -1, -1};
};

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

void cc(Sf2Player& p, uint8_t channel, uint8_t controller, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

/// A framed Roland DT1 write of @p data at @p addr, with its checksum.
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

/// The 41 mn rr address for drum map 1 (whose nibble is zero), parameter
/// nibble @p param, drum note @p note.
uint32_t drum_addr(uint8_t param, uint8_t note) {
  return 0x410000u | (static_cast<uint32_t>(param) << 8) | note;
}

StereoRender render(const Stimulus& s) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = s.model_bank;
  // Offline: process() realises the pending GS state inline, the path a bounce
  // takes and the one handle_sysex feeds.
  cfg.realize_efx_inline = true;
  Sf2Player player(cfg);
  if (!s.model_bank) player.set_soundfont(make_kit(s.zone_reverb, s.zone_chorus));
  player.prepare(kOutRate, 256);
  // Every unit is driven from this stimulus alone: reverb powers on at 40, so
  // an unwritten controller would leave a second bus carrying the note.
  for (size_t u = 0; u < kUnits.size(); ++u) cc(player, kDrumChannel, kUnits[u].part_cc, s.part[u]);
  for (size_t u = 0; u < kUnits.size(); ++u) {
    if (s.mult[u] < 0) continue;
    const auto msg =
        dt1(drum_addr(kUnits[u].param_nibble, kNote), {static_cast<uint8_t>(s.mult[u] & 0x7F)});
    player.handle_sysex(msg.data(), msg.size());
  }

  StereoRender out;
  out.left.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  out.right.assign(static_cast<size_t>(kHeld + kTail), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, kNote, uint8_t{110})));
  player.process(chans, 2, kHeld);
  chans[0] += kHeld;
  chans[1] += kHeld;
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, kDrumChannel, kNote, uint8_t{0})));
  player.process(chans, 2, kTail);
  return out;
}

bool identical(const StereoRender& a, const StereoRender& b) {
  return a.left == b.left && a.right == b.right;
}

bool sounds(const StereoRender& a) {
  for (const float v : a.left) {
    if (v != 0.0f) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("a GS drum send multiplicand empties its bus at zero, on either bank",
          "[midi][synth][gs]") {
  for (const bool model_bank : {false, true}) {
    INFO((model_bank ? "model bank" : "SoundFont bank"));
    for (size_t u = 0; u < kUnits.size(); ++u) {
      INFO(kUnits[u].name);
      // Nothing reaches this unit: the kit sends nothing of its own and the
      // part sends nothing either. Anything else that equals it is bus-silent.
      Stimulus base;
      base.model_bank = model_bank;
      const StereoRender silent = render(base);
      REQUIRE(sounds(silent));

      Stimulus loud = base;
      loud.part[u] = 127;
      loud.mult[u] = 0x7F;
      const StereoRender loud_out = render(loud);
      CHECK_FALSE(identical(loud_out, silent));

      // 7F is the identity value the unwritten parameter is held at, so writing
      // it has to change nothing.
      Stimulus unwritten = loud;
      unwritten.mult[u] = -1;
      CHECK(identical(render(unwritten), loud_out));

      // The converse of the rule: the multiplicand at zero takes the note out
      // of the bus however loud its part is sending.
      Stimulus muted = loud;
      muted.mult[u] = 0x00;
      CHECK(identical(render(muted), silent));

      // And the rule itself, on a kit whose send is its part's alone.
      Stimulus part_zero = loud;
      part_zero.part[u] = 0;
      CHECK(identical(render(part_zero), silent));

      // A value between the two ends is neither of them: the multiplicand
      // attenuates rather than switching.
      Stimulus half = loud;
      half.mult[u] = 0x40;
      const StereoRender partial = render(half);
      CHECK_FALSE(identical(partial, silent));
      CHECK_FALSE(identical(partial, loud_out));
    }
  }
}

TEST_CASE("a GS drum send multiplicand scales the kit zone's own send too", "[midi][synth][gs]") {
  // The SoundFont zone's reverbEffectsSend / chorusEffectsSend is the second
  // thing a note sends, and the manual's multiplicand is over everything the
  // note sends rather than over one of its two sources. Delay has no zone
  // generator, which is why this case has only two units.
  for (const size_t u : {kReverb, kChorus}) {
    INFO(kUnits[u].name);
    Stimulus base;
    const StereoRender silent = render(base);
    REQUIRE(sounds(silent));

    Stimulus zone_only = base;
    if (u == kReverb) {
      zone_only.zone_reverb = 500;
    } else {
      zone_only.zone_chorus = 500;
    }
    CHECK_FALSE(identical(render(zone_only), silent));

    // Both sources at once, and the multiplicand has to take both.
    Stimulus both = zone_only;
    both.part[u] = 127;
    both.mult[u] = 0x00;
    CHECK(identical(render(both), silent));
  }
}
