/// @file gs_cc_rpn_test.cpp
/// @brief The GS controllers an SC-88Pro receives that are not part of the
///        NRPN/SysEx layer: CC5/CC65/CC84 portamento, RPN 00 01 Master Fine
///        Tuning, RPN 00 02 Master Coarse Tuning and RPN 7F 7F Null.
///
/// GS is a control protocol here, not a sound to imitate: there is no reference
/// recording, so every case asserts direction, monotonicity or a boundary, and
/// none asserts an absolute timbre.

#include <catch2/catch_approx.hpp>
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

using Catch::Approx;
using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Program 0: a pure sine loop, 500 Hz at root key 60, no filter generator.
/// A sine keeps zero-crossing pitch tracking exact at every note in the range
/// these cases sweep, which a harmonic-rich sample does not.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;

  std::vector<float> sine(128);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.6f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 64.0));
  }
  // Two whole periods over the loop, so the seam is continuous.
  const int sine_id = b.add_sample("sine500", sine, 32000, 60, 0, 128);

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

Sf2Player make_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;  // keep the pitch trace dry
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  return player;
}

std::vector<float> render(Sf2Player& player, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, num_samples);
  return left;
}

void cc(Sf2Player& player, uint8_t channel, uint8_t controller, uint8_t value) {
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

void note_on(Sf2Player& player, uint8_t channel, uint8_t note) {
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, 127)));
}

void note_off(Sf2Player& player, uint8_t channel, uint8_t note) {
  player.on_event(0, event(sonare::midi::make_midi1_note_off(0, channel, note, 0)));
}

/// Selects RPN (@p msb, @p lsb) on @p channel.
void select_rpn(Sf2Player& player, uint8_t channel, uint8_t msb, uint8_t lsb) {
  cc(player, channel, 101, msb);
  cc(player, channel, 100, lsb);
}

/// Mean frequency over [@p from, @p to) from rising zero crossings.
double frequency_in(const std::vector<float>& buf, size_t from, size_t to) {
  to = std::min(to, buf.size());
  double first = -1.0;
  double last = -1.0;
  int cycles = 0;
  for (size_t i = from + 1; i < to; ++i) {
    if (buf[i - 1] >= 0.0f || buf[i] < 0.0f) continue;
    const double frac =
        static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
    const double t = static_cast<double>(i - 1) + frac;
    if (first < 0.0) {
      first = t;
    } else {
      last = t;
      ++cycles;
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kOutRate * static_cast<double>(cycles) / (last - first);
}

/// A part's note-on frequency spread, for the vibrato positive control.
double frequency_spread(const std::vector<float>& buf, size_t from) {
  double min_hz = 1e9;
  double max_hz = 0.0;
  double prev = -1.0;
  for (size_t i = from + 1; i < buf.size(); ++i) {
    if (buf[i - 1] >= 0.0f || buf[i] < 0.0f) continue;
    const double frac =
        static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
    const double t = static_cast<double>(i - 1) + frac;
    if (prev >= 0.0 && t > prev) {
      const double hz = kOutRate / (t - prev);
      min_hz = std::min(min_hz, hz);
      max_hz = std::max(max_hz, hz);
    }
    prev = t;
  }
  return max_hz > 0.0 ? max_hz - min_hz : 0.0;
}

/// Sets up @p player with note 60 as the part's previous note, leaving it
/// silent and ready for the note-on under test.
void seed_previous_note(Sf2Player& player) {
  note_on(player, 0, 60);
  render(player, 9600);
  note_off(player, 0, 60);
  render(player, 2400);
}

/// Renders one target note-on after @p arm has sent the portamento controllers.
template <typename Arm>
std::vector<float> glide_render(const Arm& arm, uint8_t target, int num_samples) {
  Sf2Player player = make_player();
  seed_previous_note(player);
  arm(player);
  note_on(player, 0, target);
  return render(player, num_samples);
}

constexpr uint8_t kNoteC4 = 60;   // 500 Hz on this fixture
constexpr uint8_t kNoteC5 = 72;   // 1000 Hz
constexpr uint8_t kNoteC3 = 48;   // 250 Hz
constexpr uint8_t kNoteG3 = 55;   // ~374 Hz; the CC84 source, distinct from all
constexpr size_t kEarly = 1200;   // first 25 ms of the target note
constexpr size_t kSettled = 500;  // ms after which a CC5=20 glide has landed

}  // namespace

TEST_CASE("portamento at its GS default leaves the render bit-identical", "[midi][sf2][gs]") {
  // A new knob that does not reproduce the baseline exactly at its no-op value
  // makes every later measurement artifact, so this is the first case.
  const auto two_notes = [](Sf2Player& player) {
    note_on(player, 0, kNoteC4);
    std::vector<float> out = render(player, 9600);
    note_off(player, 0, kNoteC4);
    note_on(player, 0, kNoteC5);
    const std::vector<float> tail = render(player, 9600);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
  };

  Sf2Player clean = make_player();
  const std::vector<float> baseline = two_notes(clean);

  SECTION("the portamento controllers at their power-on values") {
    Sf2Player player = make_player();
    cc(player, 0, 5, 0);   // portamento time, GS default
    cc(player, 0, 65, 0);  // portamento off, GS default
    REQUIRE(two_notes(player) == baseline);
  }

  SECTION("a portamento time with the switch off") {
    Sf2Player player = make_player();
    cc(player, 0, 5, 90);
    REQUIRE(two_notes(player) == baseline);
  }

  SECTION("the switch on with the default time") {
    Sf2Player player = make_player();
    cc(player, 0, 65, 127);
    REQUIRE(two_notes(player) == baseline);
  }

  SECTION("master tuning written back to its centre") {
    Sf2Player player = make_player();
    select_rpn(player, 0, 0, 1);
    cc(player, 0, 6, 0x40);
    cc(player, 0, 38, 0x00);
    select_rpn(player, 0, 0, 2);
    cc(player, 0, 6, 0x40);
    REQUIRE(two_notes(player) == baseline);
  }
}

TEST_CASE("CC65 glides the next note from the previous one", "[midi][sf2][gs]") {
  const std::vector<float> gliding = glide_render(
      [](Sf2Player& p) {
        cc(p, 0, 5, 20);
        cc(p, 0, 65, 127);
      },
      kNoteC5, 28800);
  // It starts at the previous note (500 Hz) rather than its own (1000 Hz)...
  REQUIRE(frequency_in(gliding, 0, kEarly) < 700.0);
  // ...and lands on its own pitch once the glide time has elapsed.
  const size_t settled = static_cast<size_t>(kSettled * kOutRate / 1000.0);
  REQUIRE(frequency_in(gliding, settled, gliding.size()) == Approx(1000.0).margin(10.0));

  // Without CC65 the same note starts on pitch.
  const std::vector<float> plain =
      glide_render([](Sf2Player& p) { cc(p, 0, 5, 20); }, kNoteC5, kEarly);
  REQUIRE(frequency_in(plain, 0, kEarly) == Approx(1000.0).margin(10.0));
}

TEST_CASE("CC5 portamento time is monotone in the controller", "[midi][sf2][gs]") {
  // A fixed window part-way through the glide: the longer the time, the further
  // the pitch still is from the target it is climbing to.
  const auto pitch_at_125ms = [](uint8_t time) {
    const std::vector<float> out = glide_render(
        [time](Sf2Player& p) {
          cc(p, 0, 5, time);
          cc(p, 0, 65, 127);
        },
        kNoteC5, 9600);
    return frequency_in(out, 4800, 7200);
  };
  const double fast = pitch_at_125ms(20);
  const double medium = pitch_at_125ms(40);
  const double slow = pitch_at_125ms(60);
  REQUIRE(fast > medium);
  REQUIRE(medium > slow);
  REQUIRE(slow > 500.0);  // still above the 500 Hz source it left
}

TEST_CASE("CC84 portamento control arms exactly one note-on", "[midi][sf2][gs]") {
  SECTION("it glides with CC65 off") {
    const std::vector<float> out = glide_render(
        [](Sf2Player& p) {
          cc(p, 0, 5, 20);
          cc(p, 0, 84, kNoteC3);  // source note two octaves below the target
        },
        kNoteC5, kEarly);
    REQUIRE(frequency_in(out, 0, kEarly) < 500.0);
  }

  SECTION("the note after it does not glide") {
    Sf2Player player = make_player();
    seed_previous_note(player);
    cc(player, 0, 5, 20);
    cc(player, 0, 84, kNoteC3);
    note_on(player, 0, kNoteC5);
    render(player, 28800);
    note_off(player, 0, kNoteC5);
    render(player, 2400);
    note_on(player, 0, kNoteC5);
    const std::vector<float> second = render(player, kEarly);
    REQUIRE(frequency_in(second, 0, kEarly) == Approx(1000.0).margin(10.0));
  }

  SECTION("its source note outranks the previous note") {
    // With CC65 on, the previous note (500 Hz) would put the start near 600 Hz
    // in this window; CC84's own source (250 Hz) puts it near 360 Hz.
    const std::vector<float> out = glide_render(
        [](Sf2Player& p) {
          cc(p, 0, 5, 20);
          cc(p, 0, 65, 127);
          cc(p, 0, 84, kNoteC3);
        },
        kNoteC5, kEarly);
    REQUIRE(frequency_in(out, 0, kEarly) < 450.0);
  }
}

TEST_CASE("portamento reaches the synth fallback voices too", "[midi][sf2][gs]") {
  // The fallback floor installs the glide on a NativeSynthVoice rather than an
  // Sf2Voice, which is its own call site. A modelled voice carries vibrato and
  // drift, so this asserts the direction of travel only.
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  const auto run = [&cfg](bool glide) {
    Sf2Player player(cfg);  // no SoundFont: every note takes the fallback bank
    player.prepare(kOutRate, 256);
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 80)));  // square lead
    seed_previous_note(player);
    cc(player, 0, 5, 40);
    if (glide) cc(player, 0, 65, 127);
    note_on(player, 0, kNoteC5);
    return render(player, 48000);
  };
  const std::vector<float> gliding = run(true);
  const std::vector<float> plain = run(false);
  const double early_glide = frequency_in(gliding, 0, 2400);
  const double early_plain = frequency_in(plain, 0, 2400);
  const double late_glide = frequency_in(gliding, 33600, 48000);
  const double late_plain = frequency_in(plain, 33600, 48000);
  INFO("early " << early_glide << " vs " << early_plain << ", late " << late_glide << " vs "
                << late_plain);
  REQUIRE(early_glide < 0.75 * early_plain);                // it starts below its own pitch
  REQUIRE(late_glide == Approx(late_plain).epsilon(0.05));  // and ends on it
}

TEST_CASE("the portamento controllers combine pairwise", "[midi][sf2][gs]") {
  // Four two-level factors — CC65 state, CC84 armed, CC5 time, note interval —
  // covered pairwise in five rows. The predicate under test is that a glide
  // happens exactly when a source is armed (CC84, else CC65 with a previous
  // note) AND the time is non-zero AND the source differs from the target.
  struct Row {
    bool cc65;
    bool cc84;
    uint8_t time;
    bool up;
    bool glides;
  };
  const Row rows[] = {
      {false, false, 0, true, false},  // nothing armed, no time
      {false, true, 20, false, true},  // CC84 alone arms it
      {true, false, 20, false, true},  // CC65 alone arms it
      {true, true, 0, false, false},   // armed twice over, but the time is 0
      {true, true, 20, true, true},    // both, gliding upward
  };
  for (const Row& row : rows) {
    const uint8_t target = row.up ? kNoteC5 : kNoteC3;
    const double target_hz = row.up ? 1000.0 : 250.0;
    const std::vector<float> out = glide_render(
        [&row](Sf2Player& p) {
          cc(p, 0, 5, row.time);
          if (row.cc65) cc(p, 0, 65, 127);
          if (row.cc84) cc(p, 0, 84, kNoteG3);
        },
        target, kEarly);
    const double start_hz = frequency_in(out, 0, kEarly);
    INFO("cc65=" << row.cc65 << " cc84=" << row.cc84 << " time=" << int(row.time)
                 << " up=" << row.up << " start=" << start_hz);
    if (row.glides) {
      // The glide source is always at least a fourth away from the target, so a
      // 15% band round the target is a wide margin either way.
      REQUIRE(std::fabs(start_hz - target_hz) > 0.15 * target_hz);
    } else {
      REQUIRE(start_hz == Approx(target_hz).margin(0.01 * target_hz));
    }
  }
}

TEST_CASE("RPN 00 01 master fine tuning round-trips and detunes", "[midi][sf2][gs]") {
  SECTION("the 14-bit value round-trips at both ends and the centre") {
    Sf2Player player = make_player();
    REQUIRE(player.pitch_fine_tune(0) == 8192);  // power-on centre
    REQUIRE(player.master_tune_cents(0) == 0.0f);

    select_rpn(player, 0, 0, 1);
    cc(player, 0, 6, 0x00);
    cc(player, 0, 38, 0x00);
    REQUIRE(player.pitch_fine_tune(0) == 0);
    REQUIRE(player.master_tune_cents(0) == Approx(-100.0f));

    cc(player, 0, 6, 0x7F);
    cc(player, 0, 38, 0x7F);
    REQUIRE(player.pitch_fine_tune(0) == 16383);
    REQUIRE(player.master_tune_cents(0) == Approx(99.988f).margin(0.01));

    cc(player, 0, 6, 0x40);
    cc(player, 0, 38, 0x00);
    REQUIRE(player.pitch_fine_tune(0) == 8192);
    REQUIRE(player.master_tune_cents(0) == 0.0f);

    // The two data-entry halves write one storage location, not two: an LSB on
    // its own moves only the bottom seven bits.
    cc(player, 0, 38, 0x01);
    REQUIRE(player.pitch_fine_tune(0) == 8193);
  }

  SECTION("it is per part") {
    Sf2Player player = make_player();
    select_rpn(player, 3, 0, 1);
    cc(player, 3, 6, 0x00);
    REQUIRE(player.pitch_fine_tune(3) == 0);
    REQUIRE(player.pitch_fine_tune(0) == 8192);
  }

  SECTION("a full-scale detune moves the rendered pitch by a semitone") {
    const auto tuned_hz = [](uint8_t msb) {
      Sf2Player player = make_player();
      select_rpn(player, 0, 0, 1);
      cc(player, 0, 6, msb);
      cc(player, 0, 38, 0x00);
      note_on(player, 0, kNoteC4);
      const std::vector<float> out = render(player, 9600);
      return frequency_in(out, 0, out.size());
    };
    REQUIRE(tuned_hz(0x7F) == Approx(529.7).margin(1.0));  // +100 cents
    REQUIRE(tuned_hz(0x40) == Approx(500.0).margin(1.0));  // centre
    REQUIRE(tuned_hz(0x00) == Approx(471.9).margin(1.0));  // -100 cents
  }
}

TEST_CASE("RPN 00 02 master coarse tuning round-trips and clamps", "[midi][sf2][gs]") {
  SECTION("the 7-bit MSB round-trips and clamps to +-24 semitones") {
    Sf2Player player = make_player();
    REQUIRE(player.pitch_coarse_tune(0) == 0);

    select_rpn(player, 0, 0, 2);
    cc(player, 0, 6, 0x4C);  // 64 + 12
    REQUIRE(player.pitch_coarse_tune(0) == 12);
    REQUIRE(player.master_tune_cents(0) == Approx(1200.0f));

    cc(player, 0, 6, 0x34);  // 64 - 12
    REQUIRE(player.pitch_coarse_tune(0) == -12);

    cc(player, 0, 6, 0x58);  // 64 + 24, the top of the defined range
    REQUIRE(player.pitch_coarse_tune(0) == 24);
    cc(player, 0, 6, 0x7F);  // past it: clamped, not ignored
    REQUIRE(player.pitch_coarse_tune(0) == 24);

    cc(player, 0, 6, 0x28);  // 64 - 24
    REQUIRE(player.pitch_coarse_tune(0) == -24);
    cc(player, 0, 6, 0x00);
    REQUIRE(player.pitch_coarse_tune(0) == -24);

    // The manual defines coarse tuning as MSB only; the LSB is not a second
    // half of it and must leave the value alone.
    cc(player, 0, 6, 0x40);
    cc(player, 0, 38, 0x7F);
    REQUIRE(player.pitch_coarse_tune(0) == 0);
    REQUIRE(player.master_tune_cents(0) == 0.0f);
  }

  SECTION("an octave up doubles the rendered pitch") {
    Sf2Player player = make_player();
    select_rpn(player, 0, 0, 2);
    cc(player, 0, 6, 0x4C);  // +12 semitones
    note_on(player, 0, kNoteC4);
    const std::vector<float> out = render(player, 9600);
    REQUIRE(frequency_in(out, 0, out.size()) == Approx(1000.0).margin(2.0));
  }

  SECTION("fine and coarse tuning add") {
    Sf2Player player = make_player();
    select_rpn(player, 0, 0, 2);
    cc(player, 0, 6, 0x4C);  // +1200 cents
    select_rpn(player, 0, 0, 1);
    cc(player, 0, 6, 0x00);
    cc(player, 0, 38, 0x00);  // -100 cents
    REQUIRE(player.master_tune_cents(0) == Approx(1100.0f));
  }
}

TEST_CASE("RPN 7F 7F Null discards later data entry", "[midi][sf2][gs]") {
  SECTION("a selected RPN is deselected") {
    Sf2Player player = make_player();
    select_rpn(player, 0, 0, 1);
    select_rpn(player, 0, 0x7F, 0x7F);
    cc(player, 0, 6, 0x00);
    cc(player, 0, 38, 0x00);
    REQUIRE(player.pitch_fine_tune(0) == 8192);
  }

  SECTION("the bend range is left alone too") {
    // RPN 00 00 is the one RPN that was already implemented; a Null between the
    // selection and the data entry has to protect it as well.
    const auto bent_hz = [](bool null_first) {
      Sf2Player player = make_player();
      select_rpn(player, 0, 0, 0);
      if (null_first) select_rpn(player, 0, 0x7F, 0x7F);
      cc(player, 0, 6, 12);  // 12 semitones of bend range
      player.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));
      note_on(player, 0, kNoteC4);
      const std::vector<float> out = render(player, 9600);
      return frequency_in(out, 0, out.size());
    };
    REQUIRE(bent_hz(false) == Approx(1000.0).margin(5.0));  // the write landed
    REQUIRE(bent_hz(true) == Approx(561.2).margin(2.0));    // default 200 cents
  }

  SECTION("a selected NRPN is deselected as well") {
    // Asserted against the render rather than the NRPN's own value, so the case
    // says only that nothing was applied whatever the NRPN would have done.
    const auto vibrato_render = [](bool select_nrpn, bool null_after) {
      Sf2Player player = make_player();
      if (select_nrpn) {
        cc(player, 0, 99, 0x01);
        cc(player, 0, 98, 0x09);  // vibrato depth
      }
      if (null_after) select_rpn(player, 0, 0x7F, 0x7F);
      cc(player, 0, 6, 127);
      note_on(player, 0, kNoteC4);
      return render(player, 48000);
    };
    const std::vector<float> clean = vibrato_render(false, false);
    const std::vector<float> applied = vibrato_render(true, false);
    const std::vector<float> nulled = vibrato_render(true, true);
    // Positive control: without the Null the NRPN reaches the voice.
    REQUIRE(frequency_spread(applied, 9600) > 20.0);
    REQUIRE(applied != clean);
    // With it, the data entry is discarded.
    REQUIRE(nulled == clean);
  }

  SECTION("a later NRPN LSB does not re-arm the deselected MSB") {
    // What Null is for in real files is misfire protection, so the NRPN MSB has
    // to go with the selection: leaving it behind would let a lone CC98 later
    // fire the parameter the file had already cancelled.
    const auto vibrato_render = [](bool null_between) {
      Sf2Player player = make_player();
      cc(player, 0, 99, 0x01);
      cc(player, 0, 98, 0x09);
      if (null_between) select_rpn(player, 0, 0x7F, 0x7F);
      cc(player, 0, 98, 0x09);  // LSB only; the MSB is never resent
      cc(player, 0, 6, 127);
      note_on(player, 0, kNoteC4);
      return render(player, 48000);
    };
    REQUIRE(frequency_spread(vibrato_render(false), 9600) > 20.0);
    REQUIRE(frequency_spread(vibrato_render(true), 9600) < 1.0);
  }

  SECTION("data entry with nothing selected changes nothing") {
    Sf2Player quiet = make_player();
    note_on(quiet, 0, kNoteC4);
    const std::vector<float> baseline = render(quiet, 9600);

    Sf2Player player = make_player();
    cc(player, 0, 6, 0x7F);
    cc(player, 0, 38, 0x7F);
    REQUIRE(player.pitch_fine_tune(0) == 8192);
    REQUIRE(player.pitch_coarse_tune(0) == 0);
    note_on(player, 0, kNoteC4);
    REQUIRE(render(player, 9600) == baseline);
  }
}

TEST_CASE("Reset All Controllers turns portamento off", "[midi][sf2][gs]") {
  // RP-015 lists Portamento On/Off among the controllers it clears; the time is
  // a setting and survives.
  Sf2Player player = make_player();
  seed_previous_note(player);
  cc(player, 0, 5, 20);
  cc(player, 0, 65, 127);
  cc(player, 0, 84, kNoteC3);
  cc(player, 0, 121, 0);  // reset all controllers
  note_on(player, 0, kNoteC5);
  const std::vector<float> out = render(player, kEarly);
  REQUIRE(frequency_in(out, 0, kEarly) == Approx(1000.0).margin(10.0));
}
