/// @file gs_aftertouch_dest_test.cpp
/// @brief GS controller destinations from channel aftertouch (40 2x 2x): that
///        the pressure arrives, and that it reaches the same seven places the
///        modulation wheel reaches by the same route.
///
/// The block gives every source the same eleven destinations, so the claim
/// worth testing is not what each byte does — the modulation block's own tests
/// establish that — but that a source is only a position. A destination written
/// into the aftertouch block and driven by pressure has to render *identically*
/// to the same destination written into the modulation block and driven by the
/// wheel; anything else means a second conversion was written somewhere.
///
/// Both banks are exercised. The two clamps are checked as identities rather
/// than as inequalities: past its end of the range a summed destination is
/// pinned, so two sources at full render exactly as one at full does, and a
/// missing clamp shows as a difference rather than as a tolerance.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
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

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1. The controller-destination block for
/// that part, and the part block the neutralisers below are written into.
constexpr uint8_t kDestBlock = 0x21;
constexpr uint8_t kPartBlock = 0x11;
constexpr uint8_t kToneModifyVibDepth = 0x31;
/// The destination offsets, from the modulation source's base and the
/// aftertouch source's. The seven libsonare routes are +00 to +06.
constexpr uint8_t kModBase = 0x00;
constexpr uint8_t kCafBase = 0x20;
constexpr uint8_t kPitch = 0x00;
constexpr uint8_t kTvfCutoff = 0x01;
constexpr uint8_t kAmplitude = 0x02;
constexpr uint8_t kLfo1Rate = 0x03;
constexpr uint8_t kLfo1PitchDepth = 0x04;
constexpr uint8_t kLfo1TvfDepth = 0x05;
constexpr uint8_t kLfo1TvaDepth = 0x06;
constexpr uint8_t kModWheel = 1;
constexpr uint8_t kVolume = 7;

enum class Bank : uint8_t { kSoundFont, kModel };
enum class Source : uint8_t { kWheel, kAftertouch };

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = 0x40 + mid + lo;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// A looped sine held at one level: every destination here changes its pitch,
/// its brightness or its level, and none of those needs a richer source.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.target = sine_id;
  const int inst = b.add_instrument("sineinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Sine", 0, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

using Render = std::vector<float>;
using Writes = std::function<void(Sf2Player&)>;

constexpr int kFrames = 36000;

Render render(Bank bank, const Writes& before) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = bank == Bank::kModel;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  if (bank == Bank::kSoundFont) player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  if (before) before(player);

  std::vector<float> left(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, 60, 100)));
  float* out[2] = {left.data(), right.data()};
  player.process(out, 2, kFrames);
  return left;
}

double energy(const Render& r) {
  double sum = 0.0;
  for (const float v : r) sum += static_cast<double>(v) * static_cast<double>(v);
  return sum;
}

void write_dest(Sf2Player& p, uint8_t base, uint8_t dest, uint8_t value) {
  const std::vector<uint8_t> m = dt1(kDestBlock, static_cast<uint8_t>(base + dest), {value});
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

void pressure(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_channel_pressure(0, kChannel, value)));
}

void volume(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kVolume, value)));
}

/// What the two sources have to be made symmetric about before they can be
/// compared. The wheel powers on with 0A of LFO1 PITCH DEPTH and aftertouch
/// with none, so the wheel is stripped of it; and the LFO then has nothing
/// riding it, which would make an LFO RATE destination inaudible from either
/// source, so a static vibrato comes from TONE MODIFY (40 1x 31) — the one
/// place neither controller reaches.
void neutralise(Sf2Player& p) {
  write_dest(p, kModBase, kLfo1PitchDepth, 0x00);
  const std::vector<uint8_t> m = dt1(kPartBlock, kToneModifyVibDepth, {0x7F});
  p.handle_sysex(m.data(), m.size());
}

/// One destination, written into @p source's half of the block and driven by
/// that source alone.
Render from_source(Bank bank, Source source, uint8_t dest, uint8_t value) {
  return render(bank, [source, dest, value](Sf2Player& p) {
    neutralise(p);
    write_dest(p, source == Source::kWheel ? kModBase : kCafBase, dest, value);
    if (source == Source::kWheel) {
      wheel(p, 127);
    } else {
      pressure(p, 127);
    }
  });
}

struct Dest {
  uint8_t offset;
  /// The end of the range that shows on this fixture: 00 for the cutoff, whose
  /// other end raises an already-open filter, and the far end for the five
  /// whose range starts or centres at rest.
  uint8_t probe;
  /// The value `probe` is compared against with the source at rest. 00 for
  /// every destination that engages nothing at the note-on; the LFO TVF depth
  /// is the exception, because its 00 is the one value that leaves the part's
  /// filter disengaged and the engagement is audible without being the depth.
  uint8_t rest_peer;
  const char* name;
};

constexpr Dest kDests[] = {
    {kPitch, 0x58, 0x28, "40 2x 00/20 pitch"},
    {kTvfCutoff, 0x00, 0x7F, "40 2x 01/21 TVF cutoff"},
    {kAmplitude, 0x7F, 0x00, "40 2x 02/22 amplitude"},
    {kLfo1Rate, 0x7F, 0x00, "40 2x 03/23 LFO1 rate"},
    {kLfo1PitchDepth, 0x7F, 0x00, "40 2x 04/24 LFO1 pitch depth"},
    {kLfo1TvfDepth, 0x7F, 0x40, "40 2x 05/25 LFO1 TVF depth"},
    {kLfo1TvaDepth, 0x7F, 0x00, "40 2x 06/26 LFO1 TVA depth"},
};

}  // namespace

TEST_CASE("channel aftertouch reaches a part at all", "[midi][synth][gs]") {
  // Before it was received, every address in the aftertouch half of the block
  // was inert whatever a file wrote there. AMPLITUDE CONTROL at 00 is the
  // cheapest witness: full pressure against it is the whole of the part's gain,
  // so it has to land where taking CC7 to zero lands.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render plain = render(bank, nullptr);
    const Render muted = render(bank, [](Sf2Player& p) { volume(p, 0); });
    const Render pressed = render(bank, [](Sf2Player& p) {
      write_dest(p, kCafBase, kAmplitude, 0x00);
      pressure(p, 127);
    });
    REQUIRE(energy(plain) > 0.0);
    CHECK_FALSE(pressed == plain);
    CHECK(pressed == muted);
  }
}

TEST_CASE("an unpressed part reads nothing in the aftertouch block", "[midi][synth][gs]") {
  // A destination is a depth: it says what a source at full is worth, so with no
  // pressure on the part the two ends of every range have to render alike.
  // Compared end against end rather than against an untouched part because
  // writing a TVF CUTOFF destination engages the part's filter whatever the
  // controller is doing (gs_mod_cutoff_test.cpp), and that engagement is
  // audible without being the offset. Both ends engage it identically.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    for (const Dest& d : kDests) {
      INFO("destination: " << d.name);
      auto at = [bank, &d](uint8_t value, uint8_t press) {
        return render(bank, [&d, value, press](Sf2Player& p) {
          // The same neutralisers the source comparison needs, and for the
          // second of its two reasons: an LFO RATE destination with nothing
          // riding the LFO would satisfy the non-vacuity below by being dead.
          neutralise(p);
          write_dest(p, kCafBase, d.offset, value);
          if (press != 0) pressure(p, press);
        });
      };
      CHECK(at(d.rest_peer, 0) == at(d.probe, 0));
      // Non-vacuous: the same pair has to part company once the part is
      // pressed, or the range has no ends to have compared.
      CHECK_FALSE(at(d.rest_peer, 127) == at(d.probe, 127));
    }
  }
}

TEST_CASE("a destination does not care which source reaches it", "[midi][synth][gs]") {
  // The whole claim: a source is a position and nothing else. The same byte in
  // the aftertouch half at full pressure and in the modulation half at a full
  // wheel has to produce the identical render, or one of the two went through a
  // conversion the other did not.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render neutral = render(bank, neutralise);
    for (const Dest& d : kDests) {
      INFO("destination: " << d.name);
      const Render by_wheel = from_source(bank, Source::kWheel, d.offset, d.probe);
      const Render by_pressure = from_source(bank, Source::kAftertouch, d.offset, d.probe);
      // Non-vacuous: both have to have done something, or "identical" is only
      // saying that neither source was wired.
      CHECK_FALSE(by_wheel == neutral);
      CHECK(by_wheel == by_pressure);
    }
  }
}

TEST_CASE("two sources on one destination sum", "[midi][synth][gs]") {
  // AMPLITUDE CONTROL is the one whose sum is a number rather than a direction:
  // +98.4 % from each source is +196.9 % together, and the renders are exact
  // scalar multiples, so the energy ratio is that gain squared. On the
  // SoundFont bank, where a channel gain is the whole of what scales a render.
  const double one_gain = 1.0 + 63.0 / 64.0;
  const double two_gain = 1.0 + 2.0 * (63.0 / 64.0);
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const double neutral = energy(render(bank, neutralise));
    REQUIRE(neutral > 0.0);
    const double one = energy(from_source(bank, Source::kWheel, kAmplitude, 0x7F)) / neutral;
    const double two = energy(render(bank,
                                     [](Sf2Player& p) {
                                       neutralise(p);
                                       write_dest(p, kModBase, kAmplitude, 0x7F);
                                       write_dest(p, kCafBase, kAmplitude, 0x7F);
                                       wheel(p, 127);
                                       pressure(p, 127);
                                     })) /
                       neutral;
    INFO("one " << one << " want " << one_gain * one_gain << " / two " << two << " want "
                << two_gain * two_gain);
    CHECK(two > one);
    if (bank == Bank::kSoundFont) {
      CHECK(std::fabs(one - one_gain * one_gain) < 0.001 * one_gain * one_gain);
      CHECK(std::fabs(two - two_gain * two_gain) < 0.001 * two_gain * two_gain);
    }
  }
}

TEST_CASE("a summed amplitude stops at silence rather than inverting", "[midi][synth][gs]") {
  // Two sources each asking for -100 % sum to -200 %, which unfloored is a gain
  // of -1: the part at full level with its phase turned over. Floored, the
  // second source has nothing left to take, so the pair renders exactly as one
  // of them does — and that identity is what an unfloored gain would break.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render one = from_source(bank, Source::kWheel, kAmplitude, 0x00);
    const Render both = render(bank, [](Sf2Player& p) {
      neutralise(p);
      write_dest(p, kModBase, kAmplitude, 0x00);
      write_dest(p, kCafBase, kAmplitude, 0x00);
      wheel(p, 127);
      pressure(p, 127);
    });
    CHECK_FALSE(one == render(bank, neutralise));
    CHECK(both == one);
  }
}

TEST_CASE("a summed tremolo stops at full depth", "[midi][synth][gs]") {
  // The same shape one destination along: two full TVA depths sum to 2, where
  // the LFO's trough would take the amplitude through zero and back out
  // inverted. Clamped, the pair is the single depth exactly.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render one = from_source(bank, Source::kWheel, kLfo1TvaDepth, 0x7F);
    const Render both = render(bank, [](Sf2Player& p) {
      neutralise(p);
      write_dest(p, kModBase, kLfo1TvaDepth, 0x7F);
      write_dest(p, kCafBase, kLfo1TvaDepth, 0x7F);
      wheel(p, 127);
      pressure(p, 127);
    });
    CHECK_FALSE(one == render(bank, neutralise));
    CHECK(both == one);
  }
}
