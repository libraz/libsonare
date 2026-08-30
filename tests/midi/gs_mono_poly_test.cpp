/// @file gs_mono_poly_test.cpp
/// @brief GS MONO/POLY MODE (40 1x 13): the part plays one note at a time.
///
/// The parameter is 00 Mono / 01 Poly, default Poly. The map annotates the row
/// "(=CC# 126 01/CC# 127 00)" and those trailing bytes are the CCs' own data
/// bytes — Mono Mode On carries a voice count, Poly Mode On's data byte is
/// always 00 — not parameter values, so the annotation is an alias claim only.
///
/// Mono is not ASSIGN MODE SINGLE narrowed: a new note-on stops the previous
/// note even at a different note number, so every case here strikes distinct
/// notes. A rhythm part is exempt, and the guard reads the part's current
/// rhythm flag, so a part made rhythmic later stops being monophonic.
///
/// The alias is one storage location and not one behaviour: CC126/127 perform
/// All Notes Off as well and the SysEx write does not. The byte-equality case
/// therefore sets the mode before any note-on, where that All Notes Off is a
/// no-op, and the asymmetry is asserted on its own.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::kGsAssignModeSingle;
using sonare::midi::synth::kGsMonoPolyMono;
using sonare::midi::synth::kGsMonoPolyPoly;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr uint8_t kMonoPolyLo = 0x13;
constexpr uint8_t kAssignModeLo = 0x14;
constexpr uint8_t kRhythmPartLo = 0x15;
/// GS part blocks: block 1 = part 1 = channel 0, block 0 = part 10 = channel 9.
constexpr uint8_t kChannelA = 0;
constexpr uint8_t kBlockA = 0x11;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kDrumBlock = 0x10;

constexpr uint8_t kLimitedMulti = 0x01;

/// Three distinct notes on the fixture's one looped sine: root key 60 sounds
/// 1000 Hz, so the chord lands on 500 / 1000 / 2000 Hz.
constexpr uint8_t kLowNote = 48;
constexpr uint8_t kMidNote = 60;
constexpr uint8_t kHighNote = 72;
constexpr double kLowHz = 500.0;
constexpr double kMidHz = 1000.0;
constexpr double kHighHz = 2000.0;
constexpr uint8_t kVelocity = 100;
/// Note 48 in equal temperament, for the GM fallback voices, which are not
/// pitched by the fixture's sample.
constexpr double kLowFundamentalHz = 130.8127826502993;

/// Measured from the chord's first frame. 100 ms is 50 / 100 / 200 whole cycles
/// of the three bins, so they are exactly orthogonal, and it starts past the
/// 5 ms choke's ~15 ms envelope floor.
constexpr size_t kMeasureBegin = 4800;  // 100 ms
constexpr size_t kWindowLen = 4800;     // 100 ms
/// 700 ms: past the zone's own 150 ms release, so a note All Notes Off let go
/// of is gone here and a note still held is not.
constexpr size_t kGoneBegin = 33600;
constexpr int kChordFrames = 38400;  // 800 ms

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// A framed Roland DT1 write of @p value at 40 <mid> <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, uint8_t value) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo, value};
  const int sum = 0x40 + mid + lo + value;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

void write_sysex(Sf2Player& p, uint8_t block, uint8_t lo, uint8_t value) {
  const std::vector<uint8_t> msg = dt1(block, lo, value);
  p.handle_sysex(msg.data(), msg.size());
}

void write_mono_poly(Sf2Player& p, uint8_t block, uint8_t value) {
  write_sysex(p, block, kMonoPolyLo, value);
}

void note_on(Sf2Player& p, uint8_t channel, uint8_t note, uint8_t velocity) {
  p.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, velocity)));
}

void control_change(Sf2Player& p, uint8_t channel, uint8_t controller, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

/// One looped sine at root key 60, published as melodic program 0 and as the
/// bank-128 rhythm set, so the drum part sounds the same three bins a melodic
/// part does. A silent rhythm part would satisfy the exemption case by having
/// no chord to lose. The 150 ms volume release makes the CC's All Notes Off
/// measurable while staying far longer than the 5 ms choke fade.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> sine(96);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 32.0));
  }
  const int sine_id = b.add_sample("sine1k", sine, 32000, 60, 32, 96);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  // 2^(-3284/1200) s = 150 ms.
  zone.gens.push_back({38 /*releaseVolEnv*/, -3284});
  zone.target = sine_id;
  const int inst = b.add_instrument("sineinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Sine", 0, 0, {pz});
  b.add_preset("Kit", 128, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2PlayerConfig test_config() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  return cfg;
}

/// A player on the sine fixture, with the synth fallback off so every voice
/// counted below is a SoundFont voice.
std::unique_ptr<Sf2Player> make_sf_player() {
  Sf2PlayerConfig cfg = test_config();
  cfg.synth_fallback = false;
  auto p = std::make_unique<Sf2Player>(cfg);
  p->set_soundfont(make_fixture());
  p->prepare(kOutRate, 256);
  return p;
}

/// A player with no SoundFont, so every note resolves to the GM fallback bank.
std::unique_ptr<Sf2Player> make_fallback_player(uint8_t program) {
  Sf2PlayerConfig cfg = test_config();
  cfg.synth_fallback = true;
  auto p = std::make_unique<Sf2Player>(cfg);
  p->prepare(kOutRate, 256);
  p->on_event(0, event(sonare::midi::make_midi1_program_change(0, kChannelA, program)));
  return p;
}

struct Stereo {
  std::vector<float> left;
  std::vector<float> right;
};

Stereo render(Sf2Player& p, int frames) {
  Stereo out;
  out.left.assign(static_cast<size_t>(frames), 0.0f);
  out.right.assign(static_cast<size_t>(frames), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  p.process(chans, 2, frames);
  return out;
}

void append(Stereo& dst, const Stereo& src) {
  dst.left.insert(dst.left.end(), src.left.begin(), src.left.end());
  dst.right.insert(dst.right.end(), src.right.begin(), src.right.end());
}

bool identical(const Stereo& a, const Stereo& b) { return a.left == b.left && a.right == b.right; }

/// Amplitude at @p hz over [begin, begin+len) of @p buf. Every window below is
/// a whole number of cycles of all three chord bins, so they cannot leak.
double bin_amplitude(const std::vector<float>& buf, size_t begin, size_t len, double hz) {
  REQUIRE(begin + len <= buf.size());
  double re = 0.0;
  double im = 0.0;
  for (size_t i = 0; i < len; ++i) {
    const double phase = kTwoPi * hz * static_cast<double>(i) / kOutRate;
    re += static_cast<double>(buf[begin + i]) * std::cos(phase);
    im -= static_cast<double>(buf[begin + i]) * std::sin(phase);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(len);
}

double rms(const std::vector<float>& buf) {
  double acc = 0.0;
  for (const float s : buf) acc += static_cast<double>(s) * s;
  return std::sqrt(acc / static_cast<double>(buf.size()));
}

struct Bins {
  double low = 0.0;
  double mid = 0.0;
  double high = 0.0;
};

Bins chord_bins(const Stereo& s) {
  return {bin_amplitude(s.left, kMeasureBegin, kWindowLen, kLowHz),
          bin_amplitude(s.left, kMeasureBegin, kWindowLen, kMidHz),
          bin_amplitude(s.left, kMeasureBegin, kWindowLen, kHighHz)};
}

/// Three simultaneous note-ons, high note last, then one render. Distinct note
/// numbers on purpose: a same-note chord would collapse under ASSIGN MODE
/// SINGLE too and prove nothing about mono.
Stereo strike_chord(Sf2Player& p, uint8_t channel) {
  note_on(p, channel, kLowNote, kVelocity);
  note_on(p, channel, kMidNote, kVelocity);
  note_on(p, channel, kHighNote, kVelocity);
  return render(p, kChordFrames);
}

Stereo chord_render(uint8_t channel, const std::function<void(Sf2Player&)>& setup) {
  auto p = make_sf_player();
  if (setup) setup(*p);
  return strike_chord(*p, channel);
}

/// A strike, a same-note retrigger and a second note, each followed by a
/// render. The retrigger is what ASSIGN MODE SINGLE acts on, so this sequence
/// separates SINGLE from LIMITED-MULTI whenever mono is not already chok-
/// ing the whole part.
Stereo retrigger_render(bool mono, uint8_t assign_mode) {
  auto p = make_sf_player();
  if (mono) write_mono_poly(*p, kBlockA, kGsMonoPolyMono);
  write_sysex(*p, kBlockA, kAssignModeLo, assign_mode);
  note_on(*p, kChannelA, kMidNote, kVelocity);
  Stereo out = render(*p, 9600);
  note_on(*p, kChannelA, kMidNote, kVelocity);
  append(out, render(*p, 9600));
  note_on(*p, kChannelA, kHighNote, kVelocity);
  append(out, render(*p, 19200));
  return out;
}

}  // namespace

TEST_CASE("the default MONO/POLY MODE is Poly and sounds the whole chord", "[midi][synth][gs]") {
  // The anchor for every mono case below: a fixture that cannot sound three
  // notes at once satisfies them all for the wrong reason.
  auto p = make_sf_player();
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyPoly);

  const Bins b = chord_bins(strike_chord(*p, kChannelA));
  CHECK(p->active_voice_count() == 3);
  CHECK(b.low > 0.05);
  CHECK(b.mid > 0.05);
  CHECK(b.high > 0.05);
}

TEST_CASE("Mono leaves only the last of three distinct notes", "[midi][synth][gs]") {
  const Bins poly = chord_bins(chord_render(kChannelA, nullptr));
  REQUIRE(poly.low > 0.05);
  REQUIRE(poly.mid > 0.05);

  auto p = make_sf_player();
  write_mono_poly(*p, kBlockA, kGsMonoPolyMono);
  REQUIRE(p->mono_poly(kChannelA) == kGsMonoPolyMono);

  const Bins b = chord_bins(strike_chord(*p, kChannelA));
  CHECK(p->active_voice_count() == 1);
  // Audio, not just bookkeeping: the two earlier notes' own frequencies are
  // gone while their keys are still down, so nothing but the choke ended them.
  CHECK(b.low < 0.001 * poly.low);
  CHECK(b.mid < 0.001 * poly.mid);
  CHECK(b.high > 0.9 * poly.high);
}

TEST_CASE("CC126 and CC127 write the storage location the SysEx writes", "[midi][synth][gs]") {
  auto p = make_sf_player();
  REQUIRE(p->mono_poly(kChannelA) == kGsMonoPolyPoly);
  // Both directions through one location. CC126's data byte is a voice count,
  // so any value of it still means mono.
  control_change(*p, kChannelA, 126, 1);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyMono);
  write_mono_poly(*p, kBlockA, kGsMonoPolyPoly);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyPoly);
  control_change(*p, kChannelA, 126, 4);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyMono);
  control_change(*p, kChannelA, 127, 0);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyPoly);
  write_mono_poly(*p, kBlockA, kGsMonoPolyMono);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyMono);
  control_change(*p, kChannelA, 127, 0);
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyPoly);

  // The mode is set before any note-on, which makes the CC's All Notes Off a
  // no-op and leaves the two writes byte-comparable.
  const Stereo poly = chord_render(kChannelA, nullptr);
  const Stereo cc_mono =
      chord_render(kChannelA, [](Sf2Player& q) { control_change(q, kChannelA, 126, 1); });
  const Stereo sysex_mono =
      chord_render(kChannelA, [](Sf2Player& q) { write_mono_poly(q, kBlockA, kGsMonoPolyMono); });
  const Stereo cc_poly =
      chord_render(kChannelA, [](Sf2Player& q) { control_change(q, kChannelA, 127, 0); });
  CHECK(identical(cc_mono, sysex_mono));
  CHECK(identical(cc_poly, poly));
  // Not vacuous: the two writes are a real behavioural difference apart.
  CHECK_FALSE(identical(cc_mono, poly));
}

TEST_CASE("CC126 performs All Notes Off and the SysEx write does not", "[midi][synth][gs]") {
  // The one place the alias is not an alias, and it is correct behaviour: the
  // CC carries All Notes Off, the parameter write carries only the parameter.
  auto by_cc = make_sf_player();
  note_on(*by_cc, kChannelA, kMidNote, kVelocity);
  const Stereo before = render(*by_cc, 9600);
  const double baseline = bin_amplitude(before.left, kMeasureBegin, kWindowLen, kMidHz);
  REQUIRE(baseline > 0.05);
  control_change(*by_cc, kChannelA, 126, 1);
  const Stereo cc_after = render(*by_cc, kChordFrames);
  CHECK(by_cc->active_voice_count() == 0);
  CHECK(bin_amplitude(cc_after.left, kGoneBegin, kWindowLen, kMidHz) < 0.001 * baseline);

  auto by_sysex = make_sf_player();
  note_on(*by_sysex, kChannelA, kMidNote, kVelocity);
  render(*by_sysex, 9600);
  write_mono_poly(*by_sysex, kBlockA, kGsMonoPolyMono);
  const Stereo sysex_after = render(*by_sysex, kChordFrames);
  REQUIRE(by_sysex->mono_poly(kChannelA) == kGsMonoPolyMono);
  CHECK(by_sysex->active_voice_count() == 1);
  CHECK(bin_amplitude(sysex_after.left, kGoneBegin, kWindowLen, kMidHz) > 0.9 * baseline);
}

TEST_CASE("a rhythm part ignores Mono", "[midi][synth][gs]") {
  // Positive control on a melodic part: the same write is monophonic there, so
  // the two rhythm rows below cannot pass by mono being inert everywhere.
  const Bins melodic = chord_bins(
      chord_render(kChannelA, [](Sf2Player& q) { write_mono_poly(q, kBlockA, kGsMonoPolyMono); }));
  CHECK(melodic.low < 0.001);
  CHECK(melodic.high > 0.05);

  auto drum = make_sf_player();
  write_mono_poly(*drum, kDrumBlock, kGsMonoPolyMono);
  REQUIRE(drum->mono_poly(kDrumChannel) == kGsMonoPolyMono);
  const Bins kit = chord_bins(strike_chord(*drum, kDrumChannel));
  CHECK(drum->active_voice_count() == 3);
  CHECK(kit.low > 0.05);
  CHECK(kit.mid > 0.05);
  CHECK(kit.high > 0.05);

  // The guard reads the part's current rhythm flag, so USE FOR RHYTHM PART
  // arriving after the mono write still takes the part out of mono.
  auto made_rhythm = make_sf_player();
  write_mono_poly(*made_rhythm, kBlockA, kGsMonoPolyMono);
  write_sysex(*made_rhythm, kBlockA, kRhythmPartLo, 0x01);
  const Bins after = chord_bins(strike_chord(*made_rhythm, kChannelA));
  CHECK(made_rhythm->active_voice_count() == 3);
  CHECK(after.low > 0.05);
  CHECK(after.mid > 0.05);
  CHECK(after.high > 0.05);
}

TEST_CASE("Mono subsumes ASSIGN MODE SINGLE", "[midi][synth][gs]") {
  // Mono already stops everything the part is sounding, so the assign mode is
  // never consulted and the two renders are the same bytes.
  CHECK(identical(retrigger_render(true, kGsAssignModeSingle),
                  retrigger_render(true, kLimitedMulti)));
  // Not vacuous: without mono the same two assign modes render differently.
  CHECK_FALSE(identical(retrigger_render(false, kGsAssignModeSingle),
                        retrigger_render(false, kLimitedMulti)));
}

TEST_CASE("an out-of-range MONO/POLY value is ignored rather than clamped", "[midi][synth][gs]") {
  // 02 and up are outside the row's 00-01. The standing rule is that the
  // parameter stays where it was rather than landing on the nearest end.
  for (const uint8_t value : {uint8_t{0x02}, uint8_t{0x7F}}) {
    INFO("value=" << int(value));
    auto from_poly = make_sf_player();
    write_mono_poly(*from_poly, kBlockA, value);
    CHECK(from_poly->mono_poly(kChannelA) == kGsMonoPolyPoly);

    auto from_mono = make_sf_player();
    write_mono_poly(*from_mono, kBlockA, kGsMonoPolyMono);
    write_mono_poly(*from_mono, kBlockA, value);
    CHECK(from_mono->mono_poly(kChannelA) == kGsMonoPolyMono);
  }

  // A clamp to 01 would have turned the mono part poly, so the audio says it
  // too rather than only the accessor.
  const Stereo mono =
      chord_render(kChannelA, [](Sf2Player& q) { write_mono_poly(q, kBlockA, kGsMonoPolyMono); });
  const Stereo rejected = chord_render(kChannelA, [](Sf2Player& q) {
    write_mono_poly(q, kBlockA, kGsMonoPolyMono);
    write_mono_poly(q, kBlockA, 0x02);
  });
  CHECK(identical(mono, rejected));
}

TEST_CASE("GS Reset restores Poly", "[midi][synth][gs]") {
  static constexpr uint8_t kGsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                         0x00, 0x7F, 0x00, 0x41, 0xF7};
  auto p = make_sf_player();
  write_mono_poly(*p, kBlockA, kGsMonoPolyMono);
  write_mono_poly(*p, kDrumBlock, kGsMonoPolyMono);
  REQUIRE(p->mono_poly(kChannelA) == kGsMonoPolyMono);
  REQUIRE(p->handle_sysex(kGsReset, sizeof(kGsReset)));
  CHECK(p->mono_poly(kChannelA) == kGsMonoPolyPoly);
  CHECK(p->mono_poly(kDrumChannel) == kGsMonoPolyPoly);

  // Behavioural, not just the accessor: the chord sounds again.
  const Bins b = chord_bins(strike_chord(*p, kChannelA));
  CHECK(p->active_voice_count() == 3);
  CHECK(b.low > 0.05);
  CHECK(b.mid > 0.05);
  CHECK(b.high > 0.05);
}

TEST_CASE("Mono reaches the GM fallback voices", "[midi][synth][gs]") {
  // Program 19 (church organ) sustains while the key is down, so a surviving
  // voice stays countable instead of decaying away on its own. Its own release
  // runs past a second, which is why the choke must not use it: the count at
  // 30 ms is what separates the two.
  constexpr uint8_t kChurchOrgan = 19;
  constexpr int kFadeFrames = 1440;     // 30 ms, past the choke's ~15 ms floor
  constexpr int kSettleFrames = 72000;  // 1.5 s
  constexpr int kTailFrames = 9600;     // 200 ms, measured

  auto poly = make_fallback_player(kChurchOrgan);
  note_on(*poly, kChannelA, kLowNote, kVelocity);
  note_on(*poly, kChannelA, kMidNote, kVelocity);
  note_on(*poly, kChannelA, kHighNote, kVelocity);
  render(*poly, kFadeFrames);
  REQUIRE(poly->active_voice_count() == 3);
  render(*poly, kSettleFrames);
  const Stereo poly_tail = render(*poly, kTailFrames);
  REQUIRE(poly->active_voice_count() == 3);

  auto mono = make_fallback_player(kChurchOrgan);
  write_mono_poly(*mono, kBlockA, kGsMonoPolyMono);
  note_on(*mono, kChannelA, kLowNote, kVelocity);
  note_on(*mono, kChannelA, kMidNote, kVelocity);
  note_on(*mono, kChannelA, kHighNote, kVelocity);
  render(*mono, kFadeFrames);
  CHECK(mono->active_voice_count() == 1);
  render(*mono, kSettleFrames);
  const Stereo mono_tail = render(*mono, kTailFrames);
  CHECK(mono->active_voice_count() == 1);
  // Audio as well as bookkeeping: the low note's own fundamental is missing
  // from the sustained tail, which a voice count alone would not say. Total RMS
  // cannot carry this — three model voices partially cancel.
  const double poly_low = bin_amplitude(poly_tail.left, 0, kTailFrames, kLowFundamentalHz);
  const double mono_low = bin_amplitude(mono_tail.left, 0, kTailFrames, kLowFundamentalHz);
  CHECK(rms(mono_tail.left) > 0.0);
  CHECK(poly_low > 3.0 * mono_low);
}
