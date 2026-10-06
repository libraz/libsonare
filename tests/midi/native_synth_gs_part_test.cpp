/// @file native_synth_gs_part_test.cpp
/// @brief The GS part edits (TONE MODIFY 1-8) as NativeSynth and the Sf2Player
///        fallback receive them: every entry point docs/gs.md lists reaches one
///        storage location, the system resets clear it, Rx.NRPN gates it, an
///        MPE zone keeps CC74 as its timbre dimension, and the release tail a
///        bounce reads covers an envelope-time edit once it has been received
///        rather than assuming the slowest one in advance.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

#include "midi/controller_profile.h"
#include "midi/midi_event.h"
#include "midi/synth/envelope.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::ControllerProfile;
using sonare::midi::MidiEvent;
using sonare::midi::synth::DahdsrEnvelope;
using sonare::midi::synth::gm_fallback_max_tail_samples;
using sonare::midi::synth::gs_time_scale;
using sonare::midi::synth::kGsPartOffsetMax;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::midi::synth::VaWaveform;
using sonare::test::event;
using sonare::test::render_stereo;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
/// GS part block of channel 0 (part 1) in the 40 1x xx address space.
constexpr uint8_t kPart1Block = 0x11;
constexpr uint8_t kRxNrpnLo = 0x0A;
const float kMaxScale = gs_time_scale(kGsPartOffsetMax);

/// One TONE MODIFY parameter's three entry points (docs/gs.md alias table).
struct ToneModifyAlias {
  const char* name;
  uint8_t cc;
  uint8_t nrpn_lsb;
  uint8_t sysex_lo;
};

constexpr ToneModifyAlias kAliases[] = {
    {"vibrato rate", 76, 0x08, 0x30},  {"vibrato depth", 77, 0x09, 0x31},
    {"vibrato delay", 78, 0x0A, 0x37}, {"TVF cutoff", 74, 0x20, 0x32},
    {"TVF resonance", 71, 0x21, 0x33}, {"EG attack", 73, 0x63, 0x34},
    {"EG decay", 75, 0x64, 0x35},      {"EG release", 72, 0x66, 0x36},
};

/// A framed Roland DT1 write of @p value at 40 <mid> <lo>, with the checksum.
std::vector<uint8_t> dt1(uint8_t mid, uint8_t lo, uint8_t value) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, mid, lo, value};
  const int sum = 0x40 + mid + lo + value;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

const std::vector<uint8_t> kGsReset{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                    0x00, 0x7F, 0x00, 0x41, 0xF7};
const std::vector<uint8_t> kGmSystemOn{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};

template <typename Instrument>
void send_sysex(Instrument& instrument, const std::vector<uint8_t>& payload) {
  MidiEvent ev;
  ev.ump = sonare::midi::make_sysex_handle(0, 1);
  ev.sysex_payload = payload.data();
  ev.sysex_payload_size = payload.size();
  instrument.on_event(0, ev);
}

template <typename Instrument>
void send_cc(Instrument& instrument, uint8_t channel, uint8_t controller, uint8_t value) {
  instrument.on_event(
      0, event(sonare::midi::make_midi1_control_change(0, channel, controller, value)));
}

template <typename Instrument>
void send_nrpn(Instrument& instrument, uint8_t lsb, uint8_t value, uint8_t channel = 0) {
  send_cc(instrument, channel, 99, 0x01);
  send_cc(instrument, channel, 98, lsb);
  send_cc(instrument, channel, 6, value);
}

NativeSynthConfig probe_config() {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.patch.waveform = VaWaveform::kSine;
  cfg.patch.gain = 0.8f;
  cfg.patch.amp_env.attack_ms = 40.0f;
  cfg.patch.amp_env.decay_ms = 180.0f;
  cfg.patch.amp_env.sustain = 0.55f;
  cfg.patch.amp_env.release_ms = 90.0f;
  cfg.patch.cutoff_hz = 2200.0f;
  cfg.patch.resonance_q = 0.9f;
  cfg.patch.lfo_rate_hz = 4.0f;
  cfg.patch.lfo_to_pitch_cents = 240.0f;
  return cfg;
}

struct PartRender {
  std::vector<float> onset;
  std::vector<float> tail;
};

/// Note 60 on @p channel after @p setup, held then released, both halves kept.
template <typename Setup>
PartRender render_probe(Setup setup, uint8_t channel = 0, bool silent_profile = false) {
  NativeSynth synth(probe_config());
  if (silent_profile) REQUIRE(synth.set_controller_profile(ControllerProfile{}));
  synth.prepare(kRate, kBlock);
  setup(synth);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, 60, 127)));
  PartRender out;
  out.onset = render_stereo(synth, 8192).left;
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, channel, 60, 0)));
  out.tail = render_stereo(synth, 16384).left;
  return out;
}

bool same(const PartRender& a, const PartRender& b) {
  return a.onset == b.onset && a.tail == b.tail;
}

NativeSynthConfig release_patch(float release_ms) {
  NativeSynthConfig cfg;
  cfg.patch.amp_env.sustain = 0.7f;
  cfg.patch.amp_env.release_ms = release_ms;
  return cfg;
}

int64_t tail_of(const NativeSynth& synth) { return synth.tail_samples(); }
int64_t tail_of(const Sf2Player& player) { return player.tail_samples(); }

Sf2Player make_fallback_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.prepare(kRate, kBlock);
  return player;
}

}  // namespace

TEST_CASE("every TONE MODIFY entry point reaches NativeSynth's one storage location",
          "[midi][synth][native-gs]") {
  const PartRender baseline = render_probe([](NativeSynth&) {});
  for (const ToneModifyAlias& alias : kAliases) {
    INFO(alias.name);
    const PartRender nrpn = render_probe([&](NativeSynth& s) { send_nrpn(s, alias.nrpn_lsb, 96); });
    REQUIRE_FALSE(same(nrpn, baseline));
    // The controller alias is compared with the profile silenced, because CC74
    // also reaches the default profile's brightness axis.
    const PartRender nrpn_quiet =
        render_probe([&](NativeSynth& s) { send_nrpn(s, alias.nrpn_lsb, 96); }, 0, true);
    const PartRender cc =
        render_probe([&](NativeSynth& s) { send_cc(s, 0, alias.cc, 96); }, 0, true);
    CHECK(same(cc, nrpn_quiet));
    const PartRender sysex =
        render_probe([&](NativeSynth& s) { send_sysex(s, dt1(kPart1Block, alias.sysex_lo, 96)); });
    CHECK(same(sysex, nrpn));
  }
}

TEST_CASE("GS Reset and GM System On clear NativeSynth's part edits", "[midi][synth][native-gs]") {
  const PartRender baseline = render_probe([](NativeSynth&) {});
  for (const std::vector<uint8_t>* reset : {&kGsReset, &kGmSystemOn}) {
    const PartRender cleared = render_probe([&](NativeSynth& s) {
      send_nrpn(s, 0x20, 96);
      send_sysex(s, *reset);
    });
    CHECK(same(cleared, baseline));
  }
}

TEST_CASE("NativeSynth honours Rx.NRPN as Sf2Player does", "[midi][synth][native-gs]") {
  const PartRender baseline = render_probe([](NativeSynth&) {});
  const PartRender direct = render_probe([](NativeSynth& s) { send_nrpn(s, 0x20, 96); });
  REQUIRE_FALSE(same(direct, baseline));

  // GM System On turns NRPN reception off and GS Reset turns it back on.
  const PartRender after_gm = render_probe([](NativeSynth& s) {
    send_sysex(s, kGmSystemOn);
    send_nrpn(s, 0x20, 96);
  });
  CHECK(same(after_gm, baseline));
  const PartRender after_gs = render_probe([](NativeSynth& s) {
    send_sysex(s, kGmSystemOn);
    send_sysex(s, kGsReset);
    send_nrpn(s, 0x20, 96);
  });
  CHECK(same(after_gs, direct));

  // 40 1x 0A RX NRPN closes the part's own switch.
  const PartRender switched_off = render_probe([](NativeSynth& s) {
    send_sysex(s, dt1(kPart1Block, kRxNrpnLo, 0));
    send_nrpn(s, 0x20, 96);
  });
  CHECK(same(switched_off, baseline));
  // The switch gates the NRPN only; the SysEx and controller aliases still land.
  const PartRender sysex_alias = render_probe([](NativeSynth& s) {
    send_sysex(s, dt1(kPart1Block, kRxNrpnLo, 0));
    send_sysex(s, dt1(kPart1Block, 0x32, 96));
  });
  CHECK(same(sysex_alias, direct));
}

TEST_CASE("an MPE zone keeps CC74 as its timbre dimension rather than TVF cutoff",
          "[midi][synth][native-gs][mpe]") {
  const auto zone = [](NativeSynth& s) {
    send_cc(s, 0, 101, 0);
    send_cc(s, 0, 100, 6);
    send_cc(s, 0, 6, 7);
  };
  const PartRender plain = render_probe(zone, 1, true);
  const PartRender timbre = render_probe(
      [&](NativeSynth& s) {
        zone(s);
        send_cc(s, 1, 74, 96);
      },
      1, true);
  CHECK(same(timbre, plain));
  // The other seven Sound Controllers keep their GS meaning inside a zone.
  const PartRender resonance = render_probe(
      [&](NativeSynth& s) {
        zone(s);
        send_cc(s, 1, 71, 96);
      },
      1, true);
  CHECK_FALSE(same(resonance, plain));
}

TEST_CASE("a Sound Controller the profile binds stays the profile's", "[midi][synth][native-gs]") {
  // The default profile binds CC74 to two excitation axes, which this patch's engine lacks.
  const PartRender baseline = render_probe([](NativeSynth&) {});
  const PartRender bound = render_probe([](NativeSynth& s) { send_cc(s, 0, 74, 96); });
  CHECK(same(bound, baseline));
  const PartRender unbound = render_probe([](NativeSynth& s) { send_cc(s, 0, 71, 96); });
  CHECK(same(unbound, render_probe([](NativeSynth& s) { send_nrpn(s, 0x21, 96); })));
}

TEST_CASE("GM static tails take no GS envelope-time offset", "[midi][synth][sf2][gs-tail]") {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  NativeSynth gm(cfg);
  gm.prepare(kRate, kBlock);
  Sf2Player fallback = make_fallback_player();
  const int64_t unscaled = gm_fallback_max_tail_samples(kRate, 1.0f, 1.0f, 1.0f);
  const int64_t scaled = gm_fallback_max_tail_samples(kRate, kMaxScale, kMaxScale, kMaxScale);
  for (const int64_t tail : {tail_of(gm), tail_of(fallback)}) {
    CHECK(tail >= unscaled);
    CHECK(tail < scaled);
  }
}

TEST_CASE("a received GS envelope-time edit raises NativeSynth's tail and never lowers it",
          "[midi][synth][native-gs][gs-tail]") {
  const int64_t base = DahdsrEnvelope::release_tail_samples(kRate, 100.0f);
  const int64_t scaled = DahdsrEnvelope::release_tail_samples(kRate, 100.0f * kMaxScale);

  NativeSynth synth(release_patch(100.0f));
  synth.prepare(kRate, kBlock);
  CHECK(tail_of(synth) == base);
  send_nrpn(synth, 0x66, 127);
  CHECK(tail_of(synth) >= scaled);
  const int64_t raised = tail_of(synth);
  send_nrpn(synth, 0x66, 64);
  CHECK(tail_of(synth) == raised);
  synth.reset();
  CHECK(tail_of(synth) == base);

  SECTION("controller alias") {
    send_cc(synth, 0, 72, 127);
    CHECK(tail_of(synth) >= scaled);
  }
  SECTION("SysEx alias") {
    send_sysex(synth, dt1(kPart1Block, 0x36, 127));
    CHECK(tail_of(synth) >= scaled);
  }
  SECTION("MIDI 2.0 assignable controller") {
    synth.on_event(
        0, event(sonare::midi::make_midi2_assignable_controller(0, 0, 0x01, 0x66, 0xFFFFFFFFu)));
    CHECK(tail_of(synth) >= scaled);
  }
  SECTION("decay offset on a zero-sustain envelope") {
    NativeSynthConfig cfg;
    cfg.patch.amp_env.sustain = 0.0f;
    cfg.patch.amp_env.decay_ms = 300.0f;
    cfg.patch.amp_env.release_ms = 20.0f;
    NativeSynth decaying(cfg);
    decaying.prepare(kRate, kBlock);
    send_nrpn(decaying, 0x64, 127);
    CHECK(tail_of(decaying) >= DahdsrEnvelope::release_tail_samples(kRate, 300.0f * kMaxScale));
  }
  SECTION("attack offset on a one-shot") {
    NativeSynthConfig cfg;
    cfg.patch.one_shot = true;
    cfg.patch.amp_env = {0.0f, 50.0f, 0.0f, 100.0f, 0.0f, 10.0f};
    NativeSynth one_shot(cfg);
    one_shot.prepare(kRate, kBlock);
    const int64_t unscaled = tail_of(one_shot);
    send_nrpn(one_shot, 0x63, 127);
    CHECK(tail_of(one_shot) >=
          DahdsrEnvelope::one_shot_tail_samples(kRate, cfg.patch.amp_env, kMaxScale, 1.0f));
    CHECK(tail_of(one_shot) > unscaled);
  }
}

TEST_CASE("a released voice outlives its unscaled tail only once the edit has raised it",
          "[midi][synth][native-gs][gs-tail]") {
  NativeSynth synth(release_patch(100.0f));
  synth.prepare(kRate, kBlock);
  send_nrpn(synth, 0x66, 127);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  render_stereo(synth, 8192);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  int64_t rendered = 0;
  while (synth.active_voice_count() > 0 && rendered <= tail_of(synth)) {
    render_stereo(synth, 2048);
    rendered += 2048;
  }
  CHECK(rendered > DahdsrEnvelope::release_tail_samples(kRate, 100.0f));
  CHECK(synth.active_voice_count() == 0);
}

TEST_CASE("a received GS envelope-time edit raises the Sf2Player fallback tail",
          "[midi][sf2][native-gs][gs-tail]") {
  const int64_t release_scaled = gm_fallback_max_tail_samples(kRate, 1.0f, 1.0f, kMaxScale);
  SECTION("NRPN") {
    Sf2Player player = make_fallback_player();
    REQUIRE(tail_of(player) < release_scaled);
    send_nrpn(player, 0x66, 127);
    CHECK(tail_of(player) >= release_scaled);
    const int64_t raised = tail_of(player);
    send_nrpn(player, 0x66, 64);
    CHECK(tail_of(player) == raised);
  }
  SECTION("controller alias") {
    Sf2Player player = make_fallback_player();
    send_cc(player, 0, 72, 127);
    CHECK(tail_of(player) >= release_scaled);
  }
  SECTION("SysEx alias") {
    Sf2Player player = make_fallback_player();
    send_sysex(player, dt1(kPart1Block, 0x36, 127));
    CHECK(tail_of(player) >= release_scaled);
  }
}
