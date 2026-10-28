#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "editing/voice_changer/pitch_target.h"
#include "util/constants.h"

namespace {

using namespace sonare::editing::voice_changer;

constexpr double kSr = 48000.0;
constexpr int kHop = 96;         // 2 ms
constexpr int kEstPerBlock = 5;  // 10 ms blocks
constexpr int kBlock = kHop * kEstPerBlock;

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }
double midi_hz(double m) { return 440.0 * std::exp2((m - 69.0) / 12.0); }

/// Drives a controller with synthetic estimates, 5 per block, advancing centre_sample by one hop.
struct Rig {
  PitchTargetController c;
  std::int64_t centre = 0;
  PitchCorrectionConfig pc;
  HarmonyConfig hm;
  MidiConfig midi;
  float semitones = 0.0f;
  float ff = 1.0f;
  FormantMode fm = FormantMode::Relative;

  Rig() {
    c.prepare(kSr);
    hm.detune_cents = 0.0f;
    hm.delay_ms = 12.0f;
  }
  void apply() { c.set_config(pc, hm, midi, semitones, ff, fm); }

  VoiceTargets block(double hz, bool voiced = true) {
    F0Estimate e[kEstPerBlock];
    for (auto& x : e) {
      x = F0Estimate{static_cast<float>(hz), 0.9f, voiced, centre};
      centre += kHop;
    }
    return c.update(e, kEstPerBlock, kBlock);
  }
  VoiceTargets run(double hz, double seconds) {
    VoiceTargets t;
    const int n = static_cast<int>(seconds * kSr / kBlock);
    for (int i = 0; i < n; ++i) t = block(hz);
    return t;
  }
  void note_on(int n) { c.apply_note({NoteEvent::Kind::On, n, 1.0f}); }
  void note_off(int n) { c.apply_note({NoteEvent::Kind::Off, n, 0.0f}); }
};

double out_f0(const VoiceTarget& v, double f_in) {
  return std::pow(f_in * v.ratio, 1.0 - v.absolute_weight) *
         std::pow(v.target_hz, v.absolute_weight);
}

}  // namespace

// Fails on a stub: update() returns count = 0 and every field defaulted.
TEST_CASE("off mode follows the input with ratio 2^(s/12) and weight 0", "[voice_target]") {
  Rig r;
  r.semitones = 3.0f;
  r.apply();
  VoiceTargets t = r.run(220.0, 0.05);
  REQUIRE(t.count == 1);
  CHECK(t.voices[0].active);
  CHECK(t.voices[0].absolute_weight == 0.0f);
  CHECK(std::fabs(cents(t.voices[0].ratio, std::exp2(3.0 / 12.0))) < 0.01);
  CHECK(t.voices[0].gain == 1.0f);
  CHECK(t.voices[0].delay_ms == 0.0f);
}

// Fails on a stub: no weight, no target.
TEST_CASE("auto mode: weight is amount and the target is the scale note times s",
          "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 0.0f;
  r.pc.amount = 0.5f;
  r.semitones = 2.0f;
  r.apply();
  VoiceTargets t = r.run(222.0, 0.05);
  REQUIRE(t.count == 1);
  CHECK(t.voices[0].absolute_weight == 0.5f);
  CHECK(std::fabs(cents(t.voices[0].target_hz, 220.0 * std::exp2(2.0 / 12.0))) < 0.05);
  CHECK(std::fabs(cents(t.voices[0].ratio, std::exp2(2.0 / 12.0))) < 0.01);
}

// Auto pitch convergence: the first voiced block of a +40 cent tone sits on the scale note.
TEST_CASE("auto mode converges within 20 ms at speed 0", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 0.0f;
  r.pc.amount = 1.0f;
  r.apply();
  VoiceTargets t = r.block(220.0 * std::exp2(40.0 / 1200.0));  // one 10 ms block
  CHECK(t.voices[0].absolute_weight == 1.0f);
  CHECK(std::fabs(cents(t.voices[0].target_hz, 220.0)) < 5.0);
  t = r.block(220.0 * std::exp2(40.0 / 1200.0));
  CHECK(std::fabs(cents(t.voices[0].target_hz, 220.0)) < 5.0);
}

// Fails on a stub: speed_ms smoothing absent, so the target jumps or never exists.
TEST_CASE("auto mode smooths the target with a one-pole in the log domain", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 100.0f;
  r.apply();
  const double a3 = 220.0, b3 = midi_hz(59.0);
  r.run(a3, 0.2);
  // B3 is the next scale note; the switch happens after the 30 ms hold, then the one-pole runs.
  VoiceTargets t = r.run(b3, 0.13);
  const double moved = cents(t.voices[0].target_hz, a3) / cents(b3, a3);
  CHECK(moved > 0.4);
  CHECK(moved < 0.85);
  t = r.run(b3, 1.5);
  CHECK(std::fabs(cents(t.voices[0].target_hz, b3)) < 5.0);
}

// Auto pitch switching count over the 0.1 Hz glide with vibrato, hysteresis on and off.
TEST_CASE("auto mode switches at most twice per round trip; plain snapping switches >= 8",
          "[voice_target]") {
  // Midpoint between C4 (60) and D4 (62) is 61; glide +-60 cent, vibrato +-10 cent at 5.5 Hz.
  // The control is checked for both vibrato phases so the count does not depend on the phase.
  for (int phase = 0; phase < 2; ++phase) {
    Rig r;
    r.pc.mode = PitchCorrectionMode::Auto;
    r.pc.speed_ms = 0.0f;
    r.apply();
    const int n_est = static_cast<int>(10.0 * kSr / kHop);
    std::vector<double> midis(n_est);
    for (int i = 0; i < n_est; ++i) {
      const double t = i * static_cast<double>(kHop) / kSr;
      const double arg = 2.0 * sonare::constants::kPiD * 5.5 * t;
      midis[i] = 61.0 + 0.6 * std::sin(2.0 * sonare::constants::kPiD * 0.1 * t) +
                 0.1 * (phase == 0 ? std::sin(arg) : std::cos(arg));
    }
    int switches = 0, plain = 0;
    double prev_target = 0.0;
    int prev_note = -1;
    std::int64_t centre = 0;
    for (int b = 0; b + kEstPerBlock <= n_est; b += kEstPerBlock) {
      F0Estimate e[kEstPerBlock];
      for (int k = 0; k < kEstPerBlock; ++k) {
        e[k] = F0Estimate{static_cast<float>(midi_hz(midis[b + k])), 0.9f, true, centre};
        centre += kHop;
        const int note = midis[b + k] < 61.0 ? 60 : 62;  // plain nearest scale note
        if (prev_note >= 0 && note != prev_note) ++plain;
        prev_note = note;
      }
      const VoiceTargets t = r.c.update(e, kEstPerBlock, kBlock);
      if (prev_target > 0.0 && std::fabs(cents(t.voices[0].target_hz, prev_target)) > 1.0) {
        ++switches;
      }
      prev_target = t.voices[0].target_hz;
    }
    INFO("phase=" << phase << " switches=" << switches << " plain=" << plain);
    CHECK(switches <= 2);
    CHECK(plain >= 8);
  }
}

// Fails on a stub: no robot target.
TEST_CASE("robot picks the tonic nearest the first voiced f0", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Robot;
  r.apply();
  VoiceTargets t = r.run(200.0, 0.02);  // midi 55.2: C4 (60) is nearer than C3 (48)
  CHECK(t.voices[0].absolute_weight == 1.0f);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(60.0))) < 0.01);

  Rig low;
  low.pc.mode = PitchCorrectionMode::Robot;
  low.apply();
  t = low.run(140.0, 0.02);  // midi 49.5: C3
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(48.0))) < 0.01);
}

TEST_CASE("robot tonic uses the key root, the minor tonic, and ignores key_root when chromatic",
          "[voice_target]") {
  Rig minor;
  minor.pc.mode = PitchCorrectionMode::Robot;
  minor.pc.scale = ScaleKind::NaturalMinor;
  minor.pc.key_root = 9;  // A
  minor.apply();
  VoiceTargets t = minor.run(200.0, 0.02);
  CHECK(std::fabs(cents(t.voices[0].target_hz, 220.0)) < 0.01);

  Rig chroma;
  chroma.pc.mode = PitchCorrectionMode::Robot;
  chroma.pc.scale = ScaleKind::Chromatic;
  chroma.pc.key_root = 7;
  chroma.apply();
  t = chroma.run(200.0, 0.02);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(60.0))) < 0.01);
}

// Fails on a stub: no 1 s centre, no hysteresis.
TEST_CASE("robot tonic needs the 1 s centre 300 cent past the octave midpoint to move",
          "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Robot;
  r.apply();
  r.run(200.0, 0.1);  // initial C4
  // 400 Hz is midi 67.2: past the C4/C5 midpoint (66) but inside the 300 cent band.
  VoiceTargets t = r.run(400.0, 6.0);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(60.0))) < 0.01);
  // 500 Hz is midi 71.2: more than 9 semitones from C4 once the centre settles.
  t = r.run(500.0, 6.0);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(72.0))) < 0.01);
  // The centre is a 1 s average: a short excursion does not move the tonic back.
  t = r.run(200.0, 0.2);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(72.0))) < 0.01);
}

TEST_CASE("robot_octave shifts the tonic by an octave", "[voice_target]") {
  Rig up;
  up.pc.mode = PitchCorrectionMode::Robot;
  up.pc.robot_octave = 1;
  up.apply();
  CHECK(std::fabs(cents(up.run(200.0, 0.02).voices[0].target_hz, midi_hz(72.0))) < 0.01);
  Rig down;
  down.pc.mode = PitchCorrectionMode::Robot;
  down.pc.robot_octave = -1;
  down.apply();
  CHECK(std::fabs(cents(down.run(200.0, 0.02).voices[0].target_hz, midi_hz(48.0))) < 0.01);
}

// +-50 cent vibrato leaves the robot output flat; semitones applies to the target.
TEST_CASE("robot output f0 is steady under vibrato and carries the semitone shift",
          "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Robot;
  r.semitones = -5.0f;
  r.apply();
  std::vector<double> out;
  std::int64_t centre = 0;
  const int n_est = static_cast<int>(2.0 * kSr / kHop);
  for (int b = 0; b + kEstPerBlock <= n_est; b += kEstPerBlock) {
    F0Estimate e[kEstPerBlock];
    double f_last = 0.0;
    for (int k = 0; k < kEstPerBlock; ++k) {
      const double t = (b + k) * static_cast<double>(kHop) / kSr;
      f_last = 220.0 * std::exp2(0.5 * std::sin(2.0 * sonare::constants::kPiD * 5.5 * t) / 12.0);
      e[k] = F0Estimate{static_cast<float>(f_last), 0.9f, true, centre};
      centre += kHop;
    }
    const VoiceTargets t = r.c.update(e, kEstPerBlock, kBlock);
    out.push_back(1200.0 * std::log2(out_f0(t.voices[0], f_last)));
  }
  double mean = 0.0;
  for (double v : out) mean += v;
  mean /= out.size();
  double var = 0.0;
  for (double v : out) var += (v - mean) * (v - mean);
  CHECK(std::sqrt(var / out.size()) <= 3.0);
  CHECK(std::fabs(cents(std::exp2(mean / 1200.0), midi_hz(60.0) * std::exp2(-5.0 / 12.0))) < 0.01);
}

// Harmony by scale degree from the lead's target.
TEST_CASE("harmony voices step by scale degree from the lead target", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 0.0f;
  r.hm.voices = 3;
  r.hm.steps[0] = 2;
  r.hm.steps[1] = 4;
  r.hm.steps[2] = -3;
  r.hm.level = 0.6f;
  r.hm.lead_level = 0.25f;
  r.apply();
  VoiceTargets t = r.run(midi_hz(60.0), 0.05);  // C4
  REQUIRE(t.count == 4);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(60.0))) < 5.0);
  CHECK(std::fabs(cents(t.voices[1].target_hz, midi_hz(64.0))) < 5.0);  // E4
  CHECK(std::fabs(cents(t.voices[2].target_hz, midi_hz(67.0))) < 5.0);  // G4
  CHECK(std::fabs(cents(t.voices[3].target_hz, midi_hz(55.0))) < 5.0);  // G3
  CHECK(std::fabs(cents(t.voices[1].ratio, std::exp2(4.0 / 12.0))) < 0.01);
  CHECK(std::fabs(cents(t.voices[2].ratio, std::exp2(7.0 / 12.0))) < 0.01);
  CHECK(std::fabs(cents(t.voices[3].ratio, std::exp2(-5.0 / 12.0))) < 0.01);
  CHECK(t.voices[0].gain == 0.25f);
  CHECK(t.voices[1].gain == 0.6f);
  CHECK(t.voices[1].delay_ms == 12.0f);
  CHECK(t.voices[0].delay_ms == 0.0f);
  for (int i = 0; i < 4; ++i) CHECK(t.voices[i].active);

  // Minor key: a third above A3 is C4.
  Rig m;
  m.pc.mode = PitchCorrectionMode::Auto;
  m.pc.speed_ms = 0.0f;
  m.pc.scale = ScaleKind::NaturalMinor;
  m.pc.key_root = 9;
  m.hm.voices = 1;
  m.hm.steps[0] = 2;
  m.apply();
  t = m.run(220.0, 0.05);
  CHECK(std::fabs(cents(t.voices[1].target_hz, midi_hz(60.0))) < 5.0);
}

TEST_CASE("harmony in off mode steps from the snapped input", "[voice_target]") {
  Rig r;
  r.hm.voices = 2;
  r.hm.steps[0] = 2;
  r.hm.steps[1] = 4;
  r.semitones = 1.0f;
  r.apply();
  VoiceTargets t = r.run(midi_hz(60.2), 0.05);  // snaps to C4
  REQUIRE(t.count == 3);
  CHECK(t.voices[1].absolute_weight == 0.0f);
  CHECK(std::fabs(cents(t.voices[1].ratio, std::exp2(5.0 / 12.0))) < 0.01);  // +4 semitones, +1
  CHECK(std::fabs(cents(t.voices[2].ratio, std::exp2(8.0 / 12.0))) < 0.01);  // +7 semitones, +1
}

TEST_CASE("chromatic scale ignores key_root and snaps to semitones", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 0.0f;
  r.pc.scale = ScaleKind::Chromatic;
  r.pc.key_root = 5;
  r.apply();
  VoiceTargets t = r.run(midi_hz(61.1), 0.05);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(61.0))) < 0.01);
}

// Absolute weights per mode, with semitones applied in every mode.
TEST_CASE("weights per mode", "[voice_target]") {
  Rig r;
  r.apply();
  CHECK(r.run(220.0, 0.02).voices[0].absolute_weight == 0.0f);
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.amount = 0.8f;
  r.apply();
  CHECK(std::fabs(r.run(220.0, 0.02).voices[0].absolute_weight - 0.8f) < 1e-6f);
  r.pc.mode = PitchCorrectionMode::Robot;
  r.apply();
  CHECK(r.run(220.0, 0.02).voices[0].absolute_weight == 1.0f);
  r.pc.mode = PitchCorrectionMode::Off;
  r.apply();
  r.note_on(60);
  CHECK(r.run(220.0, 0.02).voices[0].absolute_weight == 1.0f);
}

// Formant resampling factor per formant mode.
TEST_CASE("formant_alpha follows the formant mode", "[voice_target]") {
  Rig abs_rig;
  abs_rig.pc.mode = PitchCorrectionMode::Robot;
  abs_rig.ff = 1.2f;
  abs_rig.fm = FormantMode::Absolute;
  abs_rig.apply();
  CHECK(abs_rig.run(200.0, 0.02).voices[0].formant_alpha == 1.2f);

  Rig rel;
  rel.pc.mode = PitchCorrectionMode::Robot;
  rel.ff = 1.2f;
  rel.apply();
  const VoiceTargets t = rel.run(200.0, 0.02);
  CHECK(std::fabs(cents(t.voices[0].formant_alpha, midi_hz(60.0) / 200.0 * 1.2)) < 0.05);

  Rig off;
  off.semitones = 12.0f;
  off.ff = 0.9f;
  off.apply();
  CHECK(std::fabs(cents(off.run(200.0, 0.02).voices[0].formant_alpha, 2.0 * 0.9)) < 0.01);
}

// Detune formula: sigma = (+1,-1,+1), phi = (0, 2pi/3, 4pi/3), 0.7 Hz, lead c = 0.
TEST_CASE("harmony detune follows the documented formula", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Robot;
  r.hm.voices = 3;
  r.hm.steps[0] = 0;
  r.hm.steps[1] = 0;
  r.hm.steps[2] = 0;
  r.hm.detune_cents = 10.0f;
  r.apply();
  const double sigma[3] = {1.0, -1.0, 1.0};
  const double phi[3] = {0.0, 2.0 * sonare::constants::kPiD / 3.0,
                         4.0 * sonare::constants::kPiD / 3.0};
  double t_s = 0.0;
  for (int blk = 0; blk < 40; ++blk) {
    const VoiceTargets t = r.block(220.0);
    CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(60.0))) < 0.001);
    for (int j = 0; j < 3; ++j) {
      const double c = sigma[j] * 10.0 *
                       (0.5 + 0.5 * std::sin(2.0 * sonare::constants::kPiD * 0.7 * t_s + phi[j]));
      CHECK(std::fabs(cents(t.voices[j + 1].target_hz, midi_hz(60.0)) - c) < 0.01);
    }
    t_s += kBlock / kSr;
  }
}

// MIDI-held voice count and assignment.
TEST_CASE("held notes become the voices, newest as the lead", "[voice_target]") {
  Rig r;
  r.hm.voices = 3;
  r.hm.level = 0.5f;
  r.hm.lead_level = 0.9f;
  r.semitones = 2.0f;
  r.apply();
  r.note_on(60);
  VoiceTargets t = r.block(300.0);
  CHECK(t.count == 1);
  CHECK(t.voices[0].gain == 1.0f);
  r.note_on(64);
  r.note_on(67);
  t = r.block(300.0);
  REQUIRE(t.count == 3);
  const double s = std::exp2(2.0 / 12.0);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(67.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[1].target_hz, midi_hz(64.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[2].target_hz, midi_hz(60.0) * s)) < 0.01);
  CHECK(t.voices[0].gain == 0.9f);
  CHECK(t.voices[1].gain == 0.5f);
  CHECK(t.voices[1].delay_ms == 12.0f);
  for (int i = 0; i < 3; ++i) CHECK(t.voices[i].absolute_weight == 1.0f);

  r.note_on(72);
  t = r.block(300.0);
  REQUIRE(t.count == 4);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(72.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[3].target_hz, midi_hz(60.0) * s)) < 0.01);

  // A fifth note drops the oldest; a repeated note moves to the front.
  r.note_on(74);
  r.note_on(64);
  t = r.block(300.0);
  REQUIRE(t.count == 4);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(64.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[1].target_hz, midi_hz(74.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[2].target_hz, midi_hz(72.0) * s)) < 0.01);
  CHECK(std::fabs(cents(t.voices[3].target_hz, midi_hz(67.0) * s)) < 0.01);

  // Releasing an unheld note changes nothing.
  r.note_off(10);
  CHECK(r.block(300.0).count == 4);
}

TEST_CASE("voice count is min(held, 1 + harmony.voices)", "[voice_target]") {
  Rig r;
  r.hm.voices = 1;
  r.apply();
  for (int n : {60, 62, 64, 65}) r.note_on(n);
  VoiceTargets t = r.block(200.0);
  REQUIRE(t.count == 2);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(65.0))) < 0.01);
  CHECK(std::fabs(cents(t.voices[1].target_hz, midi_hz(64.0))) < 0.01);

  Rig mono;
  mono.hm.voices = 0;
  mono.apply();
  for (int n : {60, 62, 64}) mono.note_on(n);
  t = mono.block(200.0);
  CHECK(t.count == 1);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(64.0))) < 0.01);
}

// Held MIDI notes: each voice sits on its note.
TEST_CASE("three voices with three and four held notes sit on the notes", "[voice_target]") {
  Rig r;
  r.hm.voices = 3;
  r.hm.lead_level = 0.0f;
  r.apply();
  const int notes[4] = {48, 52, 55, 59};
  for (int i = 0; i < 3; ++i) r.note_on(notes[i]);
  VoiceTargets t = r.block(200.0);
  REQUIRE(t.count == 3);
  for (int i = 0; i < 3; ++i) {
    CHECK(std::fabs(cents(t.voices[i].target_hz, midi_hz(notes[2 - i]))) < 5.0);
  }
  r.note_on(notes[3]);
  t = r.block(200.0);
  REQUIRE(t.count == 4);
  for (int i = 0; i < 4; ++i) {
    CHECK(std::fabs(cents(t.voices[i].target_hz, midi_hz(notes[3 - i]))) < 5.0);
  }
}

TEST_CASE("midi.release hold keeps the last note, input returns to the mode", "[voice_target]") {
  Rig hold;
  hold.pc.mode = PitchCorrectionMode::Robot;
  hold.midi.release = MidiRelease::Hold;
  hold.apply();
  hold.run(200.0, 0.02);
  hold.note_on(67);
  hold.note_on(69);
  hold.note_off(69);
  hold.note_off(67);
  VoiceTargets t = hold.block(200.0);
  REQUIRE(t.count == 1);
  CHECK(t.voices[0].absolute_weight == 1.0f);
  CHECK(std::fabs(cents(t.voices[0].target_hz, midi_hz(67.0))) < 0.01);  // last released
  // A new press replaces the held note.
  hold.note_on(72);
  CHECK(std::fabs(cents(hold.block(200.0).voices[0].target_hz, midi_hz(72.0))) < 0.01);

  Rig in;
  in.pc.mode = PitchCorrectionMode::Off;
  in.midi.release = MidiRelease::Input;
  in.apply();
  in.run(200.0, 0.02);
  in.note_on(67);
  CHECK(in.block(200.0).voices[0].absolute_weight == 1.0f);
  in.note_off(67);
  t = in.block(200.0);
  CHECK(t.voices[0].absolute_weight == 0.0f);
  CHECK(std::fabs(cents(t.voices[0].ratio, 1.0)) < 0.01);
}

TEST_CASE("all_notes_off clears the held notes", "[voice_target]") {
  Rig r;
  r.midi.release = MidiRelease::Hold;
  r.apply();
  r.note_on(60);
  r.note_on(64);
  r.c.all_notes_off();
  const VoiceTargets t = r.block(200.0);
  CHECK(t.voices[0].absolute_weight == 0.0f);
}

TEST_CASE("unvoiced blocks hold the previous target", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Auto;
  r.pc.speed_ms = 0.0f;
  r.apply();
  const VoiceTargets a = r.run(230.0, 0.05);
  VoiceTargets b;
  for (int i = 0; i < 20; ++i) b = r.block(0.0, false);
  CHECK(b.voices[0].target_hz == a.voices[0].target_hz);
  CHECK(b.voices[0].absolute_weight == a.voices[0].absolute_weight);
}

TEST_CASE("robot has no absolute target before the first voiced estimate", "[voice_target]") {
  Rig r;
  r.pc.mode = PitchCorrectionMode::Robot;
  r.apply();
  const VoiceTargets t = r.block(0.0, false);
  CHECK(t.count == 1);
  CHECK(t.voices[0].absolute_weight == 0.0f);
  CHECK(t.voices[0].target_hz == 0.0f);
}
