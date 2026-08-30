/// @file gs_assign_mode_test.cpp
/// @brief GS ASSIGN MODE (40 1x 14): SINGLE silences the note it retriggers.
///
/// SINGLE (00) chokes a sounding voice of the same note on the same part before
/// the new one allocates; the choke is a 5 ms fade rather than a kill, so a
/// retrigger cannot click. LIMITED-MULTI (01) and FULL-MULTI (02) are asserted
/// BIT-IDENTICAL here on purpose: on the hardware they differ only in how many
/// stale duplicates of a note survive before the machine steals, which is a
/// voice budget — a resource limit rather than a behaviour (docs/gs.md) — and
/// this synth has no such budget. A partial cap added later should turn that
/// case red and be re-argued, not diverge quietly.
///
/// The fixture splits one instrument by velocity into a 500 Hz and a 2000 Hz
/// looped sine, so one note number carries two separable timbres: the note is
/// struck loud, retriggered soft, and the first strike's own frequency bin says
/// whether it survived. Voice counts alone would not survive a refactor of the
/// pool bookkeeping.

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

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr uint8_t kSingle = 0x00;
constexpr uint8_t kLimitedMulti = 0x01;
constexpr uint8_t kFullMulti = 0x02;

constexpr uint8_t kAssignModeLo = 0x14;
/// GS part blocks: block 1 = part 1 = channel 0, block 0 = part 10 = channel 9.
constexpr uint8_t kChannelA = 0;
constexpr uint8_t kBlockA = 0x11;
constexpr uint8_t kChannelB = 1;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kDrumBlock = 0x10;

/// The two velocity layers of the fixture instrument, at note 60.
constexpr double kLoudHz = 500.0;
constexpr double kSoftHz = 2000.0;
constexpr uint8_t kLoudVel = 100;
constexpr uint8_t kSoftVel = 40;

/// 200 ms of sustain before the retrigger; the last 100 ms is the baseline.
constexpr int kPreFrames = 9600;
constexpr size_t kBaseBegin = 4800;
/// Windows measured from the retrigger frame. 96 frames is one 500 Hz cycle and
/// four 2000 Hz cycles, so every window length below separates the two exactly.
constexpr size_t kFadeLen = 96;
constexpr size_t kSettledBegin = 960;  // 20 ms: past the 5 ms choke's -80 dB floor
constexpr size_t kWindowLen = 4800;    // 100 ms
constexpr size_t kGoneBegin = 33600;   // 700 ms: past the zone's own 150 ms release
constexpr int kPostFrames = 38400;     // 800 ms

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

void write_assign_mode(Sf2Player& p, uint8_t block, uint8_t value) {
  const std::vector<uint8_t> msg = dt1(block, kAssignModeLo, value);
  p.handle_sysex(msg.data(), msg.size());
}

void note_on(Sf2Player& p, uint8_t channel, uint8_t note, uint8_t velocity) {
  p.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, velocity)));
}

/// One instrument, two velocity layers: loud plays a 500 Hz loop, soft a 2000 Hz
/// one, both at root key 60 with a 150 ms volume release. The release has to be
/// well longer than the 5 ms choke fade or the two are indistinguishable.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> loud(256);
  for (size_t i = 0; i < loud.size(); ++i) {
    loud[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 64.0));
  }
  std::vector<float> soft(96);
  for (size_t i = 0; i < soft.size(); ++i) {
    soft[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 16.0));
  }
  const int loud_id = b.add_sample("loud500", loud, 32000, 60, 64, 256);
  const int soft_id = b.add_sample("soft2k", soft, 32000, 60, 16, 96);

  // 2^(-3284/1200) s = 150 ms.
  const Sf2Builder::GenValue loop{54 /*sampleModes*/, 1};
  const Sf2Builder::GenValue release{38 /*releaseVolEnv*/, -3284};

  Sf2Builder::ZoneSpec loud_zone;
  loud_zone.vel_lo = 64;
  loud_zone.vel_hi = 127;
  loud_zone.gens = {loop, release};
  loud_zone.target = loud_id;

  Sf2Builder::ZoneSpec soft_zone;
  soft_zone.vel_lo = 0;
  soft_zone.vel_hi = 63;
  soft_zone.gens = {loop, release};
  soft_zone.target = soft_id;

  const int inst = b.add_instrument("split", {loud_zone, soft_zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Split", 0, 0, {pz});
  // Bank 128 so the rhythm part sounds the same two layers.
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

/// A player on the split fixture, with the synth fallback off so every active
/// voice counted below is a SoundFont voice.
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
  p->on_event(0, event(sonare::midi::make_midi1_program_change(0, kChannelB, program)));
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

bool identical(const Stereo& a, const Stereo& b) { return a.left == b.left && a.right == b.right; }

/// Amplitude at @p hz over [begin, begin+len) of @p buf. Every call site sizes
/// the window as a whole number of cycles of every frequency it compares, so
/// the bins are exactly orthogonal and one layer cannot leak into the other.
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

/// Strikes note 60 loud on @p channel, retriggers it soft, and returns the
/// render from the retrigger frame onward together with the loud layer's
/// pre-retrigger amplitude.
struct Retrigger {
  Stereo post;
  double baseline = 0.0;
  int voices_after = 0;
};

Retrigger strike_and_retrigger(Sf2Player& p, uint8_t channel) {
  note_on(p, channel, 60, kLoudVel);
  const Stereo pre = render(p, kPreFrames);
  Retrigger out;
  out.baseline = bin_amplitude(pre.left, kBaseBegin, kWindowLen, kLoudHz);
  note_on(p, channel, 60, kSoftVel);
  out.post = render(p, kPostFrames);
  out.voices_after = p.active_voice_count();
  return out;
}

/// A complete SINGLE-vs-default render used for the byte-equality cases: the
/// SysEx (when @p value is in range) precedes a strike and its retrigger.
Stereo full_render(int value) {
  auto p = make_sf_player();
  if (value >= 0) write_assign_mode(*p, kBlockA, static_cast<uint8_t>(value));
  note_on(*p, kChannelA, 60, kLoudVel);
  Stereo out = render(*p, kPreFrames);
  note_on(*p, kChannelA, 60, kSoftVel);
  const Stereo post = render(*p, kPostFrames);
  out.left.insert(out.left.end(), post.left.begin(), post.left.end());
  out.right.insert(out.right.end(), post.right.begin(), post.right.end());
  return out;
}

}  // namespace

TEST_CASE("the default ASSIGN MODE lets a repeated note stack", "[midi][synth][gs]") {
  // The anchor for every SINGLE case below: a fixture that cannot stack a note
  // satisfies them all for the wrong reason.
  auto p = make_sf_player();
  CHECK(p->assign_mode(kChannelA) == kLimitedMulti);

  const Retrigger r = strike_and_retrigger(*p, kChannelA);
  REQUIRE(r.baseline > 0.0);
  CHECK(r.voices_after == 2);
  // Both layers of the one note number are still sounding 700 ms on.
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kLoudHz) > 0.9 * r.baseline);
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kSoftHz) > 0.03 * r.baseline);
}

TEST_CASE("SINGLE silences the note it retriggers", "[midi][synth][gs]") {
  auto p = make_sf_player();
  write_assign_mode(*p, kBlockA, kSingle);
  REQUIRE(p->assign_mode(kChannelA) == kSingle);

  const Retrigger r = strike_and_retrigger(*p, kChannelA);
  REQUIRE(r.baseline > 0.0);
  CHECK(r.voices_after == 1);
  // Audio, not just bookkeeping: the first strike's own frequency is gone well
  // after its zone's 150 ms release would have finished either way, while the
  // retrigger's layer carries on.
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kLoudHz) < 0.001 * r.baseline);
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kSoftHz) > 0.03 * r.baseline);
}

TEST_CASE("the SINGLE choke is a fade rather than a cut", "[midi][synth][gs]") {
  auto p = make_sf_player();
  write_assign_mode(*p, kBlockA, kSingle);
  const Retrigger r = strike_and_retrigger(*p, kChannelA);
  REQUIRE(r.baseline > 0.0);

  const double first_ms = bin_amplitude(r.post.left, 0, kFadeLen, kLoudHz);
  const double second_ms = bin_amplitude(r.post.left, kFadeLen, kFadeLen, kLoudHz);
  const double settled = bin_amplitude(r.post.left, kSettledBegin, kWindowLen, kLoudHz);
  // Audible immediately after the retrigger and falling: a kill() would read
  // zero here, and the zone's own release would still be near the baseline at
  // 20 ms. The 5 ms fade reaches the envelope's -80 dB floor at ~15 ms.
  CHECK(first_ms > 0.2 * r.baseline);
  CHECK(second_ms > 0.03 * r.baseline);
  CHECK(second_ms < first_ms);
  CHECK(settled < 0.001 * r.baseline);
}

TEST_CASE("LIMITED-MULTI and FULL-MULTI render identically", "[midi][synth][gs]") {
  // One behaviour, deliberately. Their hardware difference is how many stale
  // duplicates survive before stealing, which is a voice budget this synth does
  // not impose; see the file header.
  const Stereo untouched = full_render(-1);
  const Stereo limited = full_render(kLimitedMulti);
  const Stereo full = full_render(kFullMulti);
  CHECK(identical(limited, full));
  CHECK(identical(untouched, limited));
  // Not vacuous: SINGLE is a different render through the same path.
  CHECK_FALSE(identical(untouched, full_render(kSingle)));
}

TEST_CASE("SINGLE reaches the GM fallback voices", "[midi][synth][gs]") {
  // Program 19 (church organ) sustains while the key is down, so a surviving
  // duplicate stays countable instead of decaying away on its own. Its own
  // release runs past a second, which is why the choke must not use it: the
  // fast-fade assertion below is what separates the two.
  constexpr uint8_t kChurchOrgan = 19;
  constexpr int kSettleFrames = 72000;  // 1.5 s
  constexpr int kTailFrames = 9600;     // 200 ms, measured
  constexpr int kFadeFrames = 1440;     // 30 ms, past the choke's ~15 ms floor

  auto stacked = make_fallback_player(kChurchOrgan);
  note_on(*stacked, kChannelA, 60, kLoudVel);
  render(*stacked, kPreFrames);
  REQUIRE(stacked->active_voice_count() == 1);
  note_on(*stacked, kChannelA, 60, kSoftVel);
  render(*stacked, kSettleFrames);
  const Stereo stacked_tail = render(*stacked, kTailFrames);
  REQUIRE(stacked->active_voice_count() == 2);

  auto single = make_fallback_player(kChurchOrgan);
  write_assign_mode(*single, kBlockA, kSingle);
  note_on(*single, kChannelA, 60, kLoudVel);
  render(*single, kPreFrames);
  REQUIRE(single->active_voice_count() == 1);
  note_on(*single, kChannelA, 60, kSoftVel);
  // The choked model voice is gone long before its patch release would have
  // ended it, which is the whole difference between choke_fast and choke.
  render(*single, kFadeFrames);
  CHECK(single->active_voice_count() == 1);
  render(*single, kSettleFrames);
  const Stereo single_tail = render(*single, kTailFrames);
  CHECK(single->active_voice_count() == 1);
  // Audio as well as bookkeeping: the retrigger is soft, so a surviving loud
  // duplicate dominates the tail. Measured ratio is ~5x.
  CHECK(rms(single_tail.left) > 0.0);
  CHECK(rms(stacked_tail.left) > 3.0 * rms(single_tail.left));
}

TEST_CASE("SINGLE is scoped to one channel and one note number", "[midi][synth][gs]") {
  auto p = make_sf_player();
  write_assign_mode(*p, kBlockA, kSingle);
  CHECK(p->assign_mode(kChannelB) == kLimitedMulti);

  // Note 48 is 250 Hz and note 72 is 1000 Hz on the loud layer; with 500 Hz and
  // the retrigger's 2000 Hz, all four sit on exact bins of a 100 ms window.
  note_on(*p, kChannelA, 48, kLoudVel);
  note_on(*p, kChannelB, 72, kLoudVel);
  note_on(*p, kChannelA, 60, kLoudVel);
  const Stereo pre = render(*p, kPreFrames);
  REQUIRE(p->active_voice_count() == 3);
  const double base_low = bin_amplitude(pre.left, kBaseBegin, kWindowLen, 250.0);
  const double base_high = bin_amplitude(pre.left, kBaseBegin, kWindowLen, 1000.0);
  const double base_target = bin_amplitude(pre.left, kBaseBegin, kWindowLen, kLoudHz);
  REQUIRE(base_low > 0.0);
  REQUIRE(base_high > 0.0);
  REQUIRE(base_target > 0.0);

  note_on(*p, kChannelA, 60, kSoftVel);
  const Stereo post = render(*p, kPostFrames);
  CHECK(p->active_voice_count() == 3);
  CHECK(bin_amplitude(post.left, kGoneBegin, kWindowLen, 250.0) > 0.9 * base_low);
  CHECK(bin_amplitude(post.left, kGoneBegin, kWindowLen, 1000.0) > 0.9 * base_high);
  CHECK(bin_amplitude(post.left, kGoneBegin, kWindowLen, kLoudHz) < 0.001 * base_target);
}

TEST_CASE("an out-of-range ASSIGN MODE is ignored rather than clamped", "[midi][synth][gs]") {
  // 03 is outside the row's 00-02. The standing rule is that it leaves the
  // parameter where it was rather than landing on the nearest end.
  auto from_default = make_sf_player();
  write_assign_mode(*from_default, kBlockA, 0x03);
  CHECK(from_default->assign_mode(kChannelA) == kLimitedMulti);

  auto from_single = make_sf_player();
  write_assign_mode(*from_single, kBlockA, kSingle);
  write_assign_mode(*from_single, kBlockA, 0x03);
  CHECK(from_single->assign_mode(kChannelA) == kSingle);

  // A clamp to 02 would have turned the SINGLE part multi, so the audio says it
  // too rather than only the accessor.
  auto rejected = make_sf_player();
  write_assign_mode(*rejected, kBlockA, kSingle);
  write_assign_mode(*rejected, kBlockA, 0x03);
  note_on(*rejected, kChannelA, 60, kLoudVel);
  Stereo out = render(*rejected, kPreFrames);
  note_on(*rejected, kChannelA, 60, kSoftVel);
  const Stereo post = render(*rejected, kPostFrames);
  out.left.insert(out.left.end(), post.left.begin(), post.left.end());
  out.right.insert(out.right.end(), post.right.begin(), post.right.end());
  CHECK(identical(out, full_render(kSingle)));
}

TEST_CASE("GS Reset restores the default ASSIGN MODE", "[midi][synth][gs]") {
  static constexpr uint8_t kGsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                         0x00, 0x7F, 0x00, 0x41, 0xF7};
  auto p = make_sf_player();
  write_assign_mode(*p, kBlockA, kSingle);
  write_assign_mode(*p, kDrumBlock, kSingle);
  REQUIRE(p->assign_mode(kChannelA) == kSingle);
  REQUIRE(p->handle_sysex(kGsReset, sizeof(kGsReset)));
  CHECK(p->assign_mode(kChannelA) == kLimitedMulti);
  CHECK(p->assign_mode(kDrumChannel) == kLimitedMulti);

  // Behavioural, not just the accessor: the note stacks again.
  const Retrigger r = strike_and_retrigger(*p, kChannelA);
  REQUIRE(r.baseline > 0.0);
  CHECK(r.voices_after == 2);
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kLoudHz) > 0.9 * r.baseline);
}

TEST_CASE("the rhythm part takes ASSIGN MODE too", "[midi][synth][gs]") {
  // The manual prints a drum-part exemption beside Key Shift and Mono/Poly and
  // none beside this parameter, so channel 10 answers it like any other part.
  auto stacked = make_sf_player();
  const Retrigger multi = strike_and_retrigger(*stacked, kDrumChannel);
  REQUIRE(multi.baseline > 0.0);
  REQUIRE(multi.voices_after == 2);

  auto p = make_sf_player();
  write_assign_mode(*p, kDrumBlock, kSingle);
  REQUIRE(p->assign_mode(kDrumChannel) == kSingle);
  const Retrigger r = strike_and_retrigger(*p, kDrumChannel);
  REQUIRE(r.baseline > 0.0);
  CHECK(r.voices_after == 1);
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kLoudHz) < 0.001 * r.baseline);
  CHECK(bin_amplitude(r.post.left, kGoneBegin, kWindowLen, kSoftHz) > 0.03 * r.baseline);
}
