/// @file gm_gs_resolution_test.cpp
/// @brief The GM/GS/GM2 resolution rules that NativeSynth's GM mode and the
///        Sf2Player synth fallback must share: bank selection (plain GS
///        variation banks, GM2 melodic/percussion bank MSBs, GS rhythm parts),
///        the drum-kit program table, exclusive/mute group choking, the MIDI
///        2.0 banked program change and note-on velocity down-scale, and the
///        release tail a bounce derives its length from. The same MIDI stream
///        must select the same timbre and articulate the same way on both
///        instruments.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/midi_event.h"
#include "midi/program_map.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::Ump;
using sonare::midi::synth::gm_fallback_drum_kit;
using sonare::midi::synth::gs_drum_kit_name;
using sonare::midi::synth::gs_effective_bank;
using sonare::midi::synth::kDrumBank;
using sonare::midi::synth::kGsDrumKits;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;

constexpr double kOutRate = 48000.0;
constexpr uint8_t kDrumChannel = 9;

MidiEvent event(const Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

template <typename Instrument>
std::vector<float> render(Instrument& instrument, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  instrument.process(chans, 2, num_samples);
  // One interleaved buffer: an equality check then covers both legs, so a
  // difference that only shows up in the pan cannot slip through.
  std::vector<float> out;
  out.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out.push_back(left[i]);
    out.push_back(right[i]);
  }
  return out;
}

float peak(const std::vector<float>& buf, size_t from = 0) {
  float p = 0.0f;
  for (size_t i = from; i < buf.size(); ++i) p = std::max(p, std::fabs(buf[i]));
  return p;
}

NativeSynth make_gm_synth() {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  cfg.gain = 1.0f;
  cfg.polyphony = 8;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);
  return synth;
}

/// Sf2Player with no SoundFont: every note resolves through the GM fallback.
Sf2Player make_fallback_player() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  return player;
}

/// Selects (bank_msb, bank_lsb, program) on @p channel, sounds note 60 and
/// returns the render. Both instruments take the identical event stream.
template <typename Instrument>
std::vector<float> select_and_play(Instrument& instrument, uint8_t channel, uint8_t bank_msb,
                                   uint8_t bank_lsb, uint8_t program, uint8_t note = 60) {
  instrument.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 0, bank_msb)));
  instrument.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 32, bank_lsb)));
  instrument.on_event(0, event(sonare::midi::make_midi1_program_change(0, channel, program)));
  instrument.on_event(0, event(sonare::midi::make_midi1_note_on(0, channel, note, 110)));
  std::vector<float> out = render(instrument, 2048);
  instrument.on_event(0, event(sonare::midi::make_midi1_control_change(0, channel, 120, 0)));
  return out;
}

/// One bank-select form and the plain GS bank it must resolve to. Rendering
/// the two forms has to produce the same audio on an instrument that resolves
/// banks through the shared rule.
struct BankForm {
  uint8_t msb;
  uint8_t lsb;
  uint8_t program;
  uint8_t plain_bank;
  const char* what;
};

constexpr BankForm kBankForms[] = {
    {0x79, 0, 6, 0, "GM2 melodic bank MSB, capital tone"},
    {0x79, 1, 6, 1, "GM2 melodic bank MSB, harpsichord octave variation"},
    {0x79, 3, 6, 3, "GM2 melodic bank MSB, harpsichord key-off variation"},
    {0x79, 2, 6, 2, "GM2 melodic bank MSB, harpsichord wide variation"},
};

}  // namespace

TEST_CASE("gs_effective_bank is the one bank-resolution rule", "[midi][synth][gm]") {
  using sonare::midi::Gm2Bank;
  // A plain GS variation bank lives in the MSB; the LSB is ignored.
  REQUIRE(gs_effective_bank(0, 0, false) == 0);
  REQUIRE(gs_effective_bank(3, 7, false) == 3);
  // GM2 moves the melodic variation number into the LSB.
  REQUIRE(gs_effective_bank(static_cast<uint8_t>(Gm2Bank::kMelodic), 2, false) == 2);
  // Both routes to a rhythm part land on the drum bank, whatever the LSB says.
  REQUIRE(gs_effective_bank(static_cast<uint8_t>(Gm2Bank::kPercussion), 5, false) == kDrumBank);
  REQUIRE(gs_effective_bank(0, 0, true) == kDrumBank);
  REQUIRE(gs_effective_bank(static_cast<uint8_t>(Gm2Bank::kMelodic), 2, true) == kDrumBank);
}

TEST_CASE("NativeSynth and the SF2 fallback resolve bank-select forms alike",
          "[midi][synth][sf2][gm]") {
  // Equivalence per instrument: selecting a bank through its GM2 form must
  // sound exactly like selecting the plain GS bank it resolves to. Running the
  // same table on both instruments pins them to one rule without comparing
  // their (deliberately different) bus processing to each other.
  for (const BankForm& form : kBankForms) {
    INFO(form.what);
    NativeSynth gm_form = make_gm_synth();
    NativeSynth gm_plain = make_gm_synth();
    const std::vector<float> native_form =
        select_and_play(gm_form, 0, form.msb, form.lsb, form.program);
    const std::vector<float> native_plain =
        select_and_play(gm_plain, 0, form.plain_bank, 0, form.program);
    REQUIRE(peak(native_form) > 1.0e-4f);
    REQUIRE(native_form == native_plain);

    Sf2Player sf2_form = make_fallback_player();
    Sf2Player sf2_plain = make_fallback_player();
    const std::vector<float> fallback_form =
        select_and_play(sf2_form, 0, form.msb, form.lsb, form.program);
    const std::vector<float> fallback_plain =
        select_and_play(sf2_plain, 0, form.plain_bank, 0, form.program);
    REQUIRE(peak(fallback_form) > 1.0e-4f);
    REQUIRE(fallback_form == fallback_plain);
  }
}

TEST_CASE("the GM2 percussion bank reaches the drum map on both instruments",
          "[midi][synth][sf2][gm]") {
  using sonare::midi::Gm2Bank;
  const auto percussion_msb = static_cast<uint8_t>(Gm2Bank::kPercussion);
  // A melodic channel switched to the GM2 percussion bank must play the drum
  // note, i.e. sound like the same note on the drum channel rather than like
  // the melodic program it would otherwise resolve to.
  NativeSynth gm_bank = make_gm_synth();
  NativeSynth gm_drum_channel = make_gm_synth();
  NativeSynth gm_melodic = make_gm_synth();
  const std::vector<float> native_bank = select_and_play(gm_bank, 0, percussion_msb, 0, 0, 42);
  const std::vector<float> native_drums =
      select_and_play(gm_drum_channel, kDrumChannel, 0, 0, 0, 42);
  const std::vector<float> native_melodic = select_and_play(gm_melodic, 0, 0, 0, 0, 42);
  REQUIRE(peak(native_bank) > 1.0e-4f);
  REQUIRE(native_bank == native_drums);
  REQUIRE(native_bank != native_melodic);

  Sf2Player sf2_bank = make_fallback_player();
  Sf2Player sf2_drum_channel = make_fallback_player();
  const std::vector<float> fallback_bank = select_and_play(sf2_bank, 0, percussion_msb, 0, 0, 42);
  const std::vector<float> fallback_drums =
      select_and_play(sf2_drum_channel, kDrumChannel, 0, 0, 0, 42);
  REQUIRE(peak(fallback_bank) > 1.0e-4f);
  REQUIRE(fallback_bank == fallback_drums);
}

TEST_CASE("GM drum exclusive groups choke on both instruments", "[midi][synth][sf2][gm]") {
  // Closed hi-hat (42) and open hi-hat (46) share GM mute group 1: the closed
  // strike must cut the ringing open one. Measured as the open hi-hat's own
  // ring-out, so the test does not depend on the choking strike's level.
  //
  // The sends are zeroed first because choking a voice stops the voice, not the
  // ambience it already fed into the send returns. Sf2Player's fallback path
  // starts at the GS power-on reverb level, so its return tail would otherwise
  // dominate the measurement window and the property under test -- that the
  // VOICE was cut -- would be invisible behind it. The fallback send weighting
  // is multiplicative (see refresh_channel_mod), so CC 0 is fully dry.
  const auto open_hat_tail = [](auto& instrument, bool strike_closed) {
    for (uint8_t cc : {uint8_t{91}, uint8_t{93}, uint8_t{94}}) {
      instrument.on_event(0,
                          event(sonare::midi::make_midi1_control_change(0, kDrumChannel, cc, 0)));
    }
    instrument.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, 46, 110)));
    render(instrument, 2048);
    if (strike_closed) {
      instrument.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, 42, 110)));
    }
    // Let the closed hat's own short body decay before measuring, so what is
    // left is the open hat still ringing (or not).
    render(instrument, 9600);
    return peak(render(instrument, 4800));
  };

  NativeSynth gm_ringing = make_gm_synth();
  NativeSynth gm_choked = make_gm_synth();
  const float native_ringing = open_hat_tail(gm_ringing, false);
  const float native_choked = open_hat_tail(gm_choked, true);
  REQUIRE(native_ringing > 1.0e-4f);
  REQUIRE(native_choked < 0.2f * native_ringing);

  Sf2Player sf2_ringing = make_fallback_player();
  Sf2Player sf2_choked = make_fallback_player();
  const float fallback_ringing = open_hat_tail(sf2_ringing, false);
  const float fallback_choked = open_hat_tail(sf2_choked, true);
  REQUIRE(fallback_ringing > 1.0e-4f);
  REQUIRE(fallback_choked < 0.2f * fallback_ringing);
}

TEST_CASE("the MIDI 2.0 banked program change honours the bank-valid bit",
          "[midi][synth][sf2][gm]") {
  // Bank-valid set: the bank bytes apply, so the message selects the same voice
  // as the equivalent CC0/CC32 + program change. Bank-valid clear: the channel
  // keeps the bank it already had.
  const auto midi2_program_change = [](uint8_t channel, uint8_t program, bool bank_valid,
                                       uint8_t bank_msb, uint8_t bank_lsb) {
    Ump u;
    u.words[0] = (0x4u << 28) | (0xCu << 20) | (static_cast<uint32_t>(channel & 0x0Fu) << 16) |
                 (bank_valid ? 0x01u : 0x00u);
    u.words[1] = (static_cast<uint32_t>(program & 0x7Fu) << 24) |
                 (static_cast<uint32_t>(bank_msb & 0x7Fu) << 8) |
                 static_cast<uint32_t>(bank_lsb & 0x7Fu);
    return u;
  };

  const auto play_midi2 = [&](auto& instrument, bool bank_valid) {
    instrument.on_event(0, event(midi2_program_change(0, 6, bank_valid, 1, 0)));
    instrument.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
    std::vector<float> out = render(instrument, 2048);
    instrument.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    return out;
  };

  NativeSynth gm_banked = make_gm_synth();
  NativeSynth gm_unbanked = make_gm_synth();
  NativeSynth gm_reference = make_gm_synth();
  const std::vector<float> native_banked = play_midi2(gm_banked, true);
  const std::vector<float> native_unbanked = play_midi2(gm_unbanked, false);
  const std::vector<float> native_reference = select_and_play(gm_reference, 0, 1, 0, 6);
  REQUIRE(peak(native_banked) > 1.0e-4f);
  REQUIRE(native_banked == native_reference);    // bank 1 applied
  REQUIRE(native_unbanked != native_reference);  // bank left at 0

  Sf2Player sf2_banked = make_fallback_player();
  Sf2Player sf2_unbanked = make_fallback_player();
  Sf2Player sf2_reference = make_fallback_player();
  const std::vector<float> fallback_banked = play_midi2(sf2_banked, true);
  const std::vector<float> fallback_unbanked = play_midi2(sf2_unbanked, false);
  const std::vector<float> fallback_reference = select_and_play(sf2_reference, 0, 1, 0, 6);
  REQUIRE(peak(fallback_banked) > 1.0e-4f);
  REQUIRE(fallback_banked == fallback_reference);
  REQUIRE(fallback_unbanked != fallback_reference);
}

TEST_CASE("a quiet MIDI 2.0 note-on never down-scales into a note-off", "[midi][synth][sf2][gm]") {
  // The 16 -> 7 bit down-scale drops the low 9 bits, so a nonzero velocity
  // below 512 would land on 0 — a MIDI 1.0 note-off. Both instruments must
  // clamp it to 1 and sound the note.
  const auto midi2_note_on = [](uint8_t channel, uint8_t note, uint16_t velocity16) {
    Ump u;
    u.words[0] = (0x4u << 28) | (0x9u << 20) | (static_cast<uint32_t>(channel & 0x0Fu) << 16) |
                 (static_cast<uint32_t>(note & 0x7Fu) << 8);
    u.words[1] = static_cast<uint32_t>(velocity16) << 16;
    return u;
  };

  NativeSynth gm = make_gm_synth();
  gm.on_event(0, event(midi2_note_on(0, 60, 1)));
  REQUIRE(gm.active_voice_count() == 1);
  REQUIRE(peak(render(gm, 2048)) > 0.0f);

  Sf2Player fallback = make_fallback_player();
  fallback.on_event(0, event(midi2_note_on(0, 60, 1)));
  REQUIRE(fallback.active_voice_count() == 1);
  REQUIRE(peak(render(fallback, 2048)) > 0.0f);
}

TEST_CASE("the GS drum-kit table drives both the kit index and the kit name", "[midi][synth][gm]") {
  // The two lookups are derived from kGsDrumKits, so they cannot disagree —
  // assert that across the whole program range, not just the table entries.
  for (int program = 0; program < 128; ++program) {
    const auto p = static_cast<uint8_t>(program);
    INFO("bank-128 program " << program);
    const std::string_view name = gs_drum_kit_name(p);
    const uint8_t index = gm_fallback_drum_kit(p);
    bool in_table = false;
    for (const auto& kit : kGsDrumKits) {
      if (kit.program != p) continue;
      in_table = true;
      REQUIRE(name == kit.name);
      REQUIRE(index == kit.index);
    }
    if (!in_table) {
      // An unknown program is not a named kit, and plays the Standard kit.
      REQUIRE(name.empty());
      REQUIRE(index == 0);
    }
  }
  // The table itself must stay a bijection: no duplicate program or index.
  for (size_t i = 0; i < kGsDrumKits.size(); ++i) {
    for (size_t j = i + 1; j < kGsDrumKits.size(); ++j) {
      REQUIRE(kGsDrumKits[i].program != kGsDrumKits[j].program);
      REQUIRE(kGsDrumKits[i].index != kGsDrumKits[j].index);
    }
  }
}

TEST_CASE("NativeSynth sostenuto captures only the keys held at the press", "[midi][synth][gm]") {
  // A pedal that keeps sending values >= 64 must not capture notes struck
  // after the press: without a press-edge guard the second CC66 would capture
  // the later note and it would ignore its own note-off forever.
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  NativeSynth synth(cfg);
  synth.prepare(kOutRate, 256);

  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  render(synth, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 127)));
  // A later key, then a redundant pedal message while the pedal is still down.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 67, 110)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 100)));
  render(synth, 256);
  // Release the later key: it was never captured, so it must fall silent.
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 67, 0)));
  render(synth, synth.tail_samples() + 4800);
  REQUIRE(synth.active_voice_count() == 1);  // only the captured note is left

  // Release the captured note's own key. That is the defining behaviour: the
  // pedal holds it past its note-off, so it must still be sounding here.
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  render(synth, synth.tail_samples() + 4800);
  REQUIRE(synth.active_voice_count() == 1);

  // Lifting the pedal drops the capture, and the already-released key falls.
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 66, 0)));
  render(synth, synth.tail_samples() + 4800);
  REQUIRE(synth.active_voice_count() == 0);
}

TEST_CASE("both instruments report a tail that covers the GM fallback releases",
          "[midi][synth][sf2][gm]") {
  // The bounce derives its auto render length from MidiInstrument::tail_samples,
  // so both GM paths have to bound the slowest fallback release.
  const int64_t fallback_bound = sonare::midi::synth::DahdsrEnvelope::release_tail_samples(
      kOutRate, sonare::midi::synth::gm_fallback_max_release_ms());
  NativeSynth gm = make_gm_synth();
  Sf2Player fallback = make_fallback_player();
  REQUIRE(static_cast<int64_t>(gm.tail_samples()) >= fallback_bound);
  REQUIRE(static_cast<int64_t>(fallback.tail_samples()) >= fallback_bound);
}
