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
#include <string_view>
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
using sonare::midi::synth::NativeSynthPatch;
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

TEST_CASE("Bank Select LSB selects the GS tone map", "[midi][synth][gm]") {
  using sonare::midi::synth::gs_tone_map_from_lsb;
  using sonare::midi::synth::GsToneMap;
  // The three maps an SC-88Pro-class module addresses through CC#32.
  REQUIRE(gs_tone_map_from_lsb(1) == GsToneMap::kSc55);
  REQUIRE(gs_tone_map_from_lsb(2) == GsToneMap::kSc88);
  REQUIRE(gs_tone_map_from_lsb(3) == GsToneMap::kSc88Pro);
  // Unset, and every value that names no map, read as the module's own default:
  // a module that never saw the message is already playing that map, so an
  // unrecognised one must sound it rather than nothing.
  REQUIRE(gs_tone_map_from_lsb(0) == GsToneMap::kModuleDefault);
  for (int lsb = 4; lsb < 128; ++lsb) {
    INFO("bank LSB " << lsb);
    REQUIRE(gs_tone_map_from_lsb(static_cast<uint8_t>(lsb)) == GsToneMap::kModuleDefault);
  }
}

TEST_CASE("the tone map is read from CC#32 only for a GS part", "[midi][synth][gm]") {
  using sonare::midi::Gm2Bank;
  using sonare::midi::synth::gs_effective_tone_map;
  using sonare::midi::synth::GsToneMap;
  // A GS part: the LSB is the map select.
  REQUIRE(gs_effective_tone_map(0, 1) == GsToneMap::kSc55);
  REQUIRE(gs_effective_tone_map(8, 3) == GsToneMap::kSc88Pro);
  // GM2 gives the same controller a different meaning under its own bank MSBs
  // (a melodic variation number, a percussion set), so it must not be read as a
  // map there — a GM2 file selecting variation 2 is not asking for the SC-88 map.
  REQUIRE(gs_effective_tone_map(static_cast<uint8_t>(Gm2Bank::kMelodic), 2) ==
          GsToneMap::kModuleDefault);
  REQUIRE(gs_effective_tone_map(static_cast<uint8_t>(Gm2Bank::kPercussion), 1) ==
          GsToneMap::kModuleDefault);
}

TEST_CASE("a drum kit resolves only in the tone maps that define it", "[midi][synth][gm]") {
  using sonare::midi::synth::gs_map_reaches;
  using sonare::midi::synth::GsToneMap;
  constexpr GsToneMap kMaps[] = {GsToneMap::kModuleDefault, GsToneMap::kSc55, GsToneMap::kSc88,
                                 GsToneMap::kSc88Pro};

  // Every kit resolves in the map that introduced it and in every later one, and
  // in none before it. The kits an older map has no entry for fall back to
  // Standard, which is what a module plays for a kit it does not have.
  for (const auto& kit : kGsDrumKits) {
    for (const GsToneMap map : kMaps) {
      INFO(kit.name << " in map " << static_cast<int>(map));
      const bool reaches = gs_map_reaches(map, kit.since);
      REQUIRE(gm_fallback_drum_kit(kit.program, map) == (reaches ? kit.index : uint8_t{0}));
      REQUIRE(gs_drum_kit_name(kit.program, map).empty() == !reaches);
    }
  }

  // The concrete cases the numbering turns on: the SC-88Pro map splits the
  // SC-88 map's combined TR-808/909 set in two, so TR-909 exists only there,
  // while TR-808 has been in every map since the SC-55.
  REQUIRE(gs_drum_kit_name(30, GsToneMap::kSc88Pro) == "TR-909");
  REQUIRE(gs_drum_kit_name(30, GsToneMap::kSc88).empty());
  REQUIRE(gs_drum_kit_name(30, GsToneMap::kSc55).empty());
  REQUIRE(gs_drum_kit_name(25, GsToneMap::kSc55) == "TR-808");
  // Dance arrived with the SC-88 map.
  REQUIRE(gs_drum_kit_name(26, GsToneMap::kSc88) == "Dance");
  REQUIRE(gs_drum_kit_name(26, GsToneMap::kSc55).empty());
  // The SC-55 map's CM-64/32L set sits alone at the top of the program range.
  REQUIRE(gs_drum_kit_name(127, GsToneMap::kSc55) == "CM-64/32L");
}

TEST_CASE("GS drum kits past the SC-55 set are voiced apart from Standard",
          "[midi][synth][sf2][gm]") {
  // Kit indices are what the voicing switch reads, so a kit added to the table
  // without a voicing would silently play Standard. Render the kick through each
  // kit and require every set that claims its own kick to differ from Standard.
  // The sets left out are the ones that deliberately keep the Standard voicing:
  // SFX, Rhythm FX, Rhythm FX 2 and Cymbal & Claps replace pieces the kick is
  // not among.
  constexpr uint8_t kVoicedApart[] = {
      1,    // Standard 2
      2,    // Standard 3
      9,    // Hip Hop
      10,   // Jungle
      11,   // Techno
      26,   // Dance
      27,   // CR-78
      28,   // TR-606
      29,   // TR-707
      30,   // TR-909
      49,   // Ethnic
      50,   // Kick & Snare
      52,   // Asia
      127,  // CM-64/32L
  };
  Sf2Player standard_player = make_fallback_player();
  const std::vector<float> standard = select_and_play(standard_player, kDrumChannel, 0, 0, 0, 36);
  REQUIRE(peak(standard) > 0.0f);
  for (const uint8_t program : kVoicedApart) {
    INFO("rhythm program " << static_cast<int>(program) << " (" << gs_drum_kit_name(program)
                           << ")");
    Sf2Player player = make_fallback_player();
    const std::vector<float> kit = select_and_play(player, kDrumChannel, 0, 0, program, 36);
    REQUIRE(peak(kit) > 0.0f);
    REQUIRE(kit != standard);
  }

  // The same program under a map that does not define its kit plays Standard,
  // sample for sample.
  Sf2Player sc55_909 = make_fallback_player();
  REQUIRE(select_and_play(sc55_909, kDrumChannel, 0, 1, 30, 36) == standard);
}

TEST_CASE("a GS variation bank reaches its own patch and falls back to the capital",
          "[midi][synth][gm]") {
  using sonare::midi::synth::gm_fallback_patch;
  // The GS variation tones the model floor voices apart from their capital,
  // written out here as an independent statement of the contract. `gs` is the
  // Bank Select MSB number a Roland module uses and `gm2` the bank LSB number
  // GM2 gives the same tone (0 = GM2 gives that number to a DIFFERENT tone, so
  // only the GS address may resolve). `voice` names the patch the tone must land
  // on: two tones sharing a name must share one patch and two that differ must
  // not, which is what catches a table row pointed at the wrong member.
  struct Variation {
    uint8_t program;
    uint8_t gs;
    uint8_t gm2;
    const char* voice;
    const char* what;
  };
  constexpr Variation kVariations[] = {
      // The three grands above program 0 are one patch in this synth, so their
      // wide voicings are one patch too.
      {0, 8, 1, "piano_wide", "Piano 1w"},
      {1, 8, 1, "piano_wide", "Piano 2w"},
      {2, 8, 1, "piano_wide", "Piano 3w"},
      {3, 8, 1, "piano_wide", "HonkyTonk w"},
      {0, 16, 2, "piano_dark", "Piano 1d"},
      {4, 8, 1, "ep_detuned_1", "Detuned EP1"},
      {4, 16, 2, "ep_velocity_1", "E.Piano 1v"},
      {4, 24, 3, "ep_sixties", "60's E.Piano"},
      {5, 8, 1, "ep_detuned_2", "Detuned EP2"},
      {5, 16, 2, "ep_velocity_2", "E.Piano 2v"},
      {6, 8, 1, "hps_octave", "Coupled Hps."},
      {6, 16, 2, "hps_wide", "Harpsi.w"},
      {6, 24, 3, "hps_keyoff", "Harpsi.o"},
      {11, 8, 1, "vibraphone_wide", "Vib.w"},
      {12, 8, 1, "marimba_wide", "Marimba w"},
      {14, 8, 1, "church_bell", "Church Bell"},
      {14, 9, 2, "carillon", "Carillon"},
      {16, 8, 1, "organ_detuned_1", "Detuned Or1"},
      {16, 16, 2, "organ_sixties", "60's Organ1"},
      {16, 32, 3, "organ_4", "Organ 4"},
      {17, 8, 1, "organ_detuned_2", "Detuned Or2"},
      {17, 32, 2, "organ_5", "Organ 5"},
      {19, 8, 1, "church_organ_flutes", "Church Org.2"},
      {19, 16, 2, "church_organ_full", "Church Org.3"},
      {21, 8, 0, "accordion_italian", "Accordion It"},
      {24, 8, 1, "ukulele", "Ukulele"},
      {24, 16, 2, "nylon_keyoff", "Nylon Gt.o"},
      {25, 8, 1, "twelve_string", "12-str.Gt"},
      {25, 16, 2, "mandolin", "Mandolin"},
      {40, 8, 1, "violin_slow", "Slow Violin"},
  };

  std::vector<const void*> voiced;
  for (const Variation& v : kVariations) {
    INFO(v.what);
    const NativeSynthPatch& capital = gm_fallback_patch(0, v.program);
    const NativeSynthPatch& tone = gm_fallback_patch(v.gs, v.program);
    // The variation is a patch of its own, not the capital handed back.
    REQUIRE(&tone != &capital);
    // Both numbering schemes address one voice, so a file written for GS and one
    // written for GM2 sound the same tone rather than two near neighbours.
    if (v.gm2 != 0) REQUIRE(&gm_fallback_patch(v.gm2, v.program) == &tone);
    voiced.push_back(&tone);
  }
  // Tones share a patch exactly when they are meant to: a row pointed at the
  // wrong member would otherwise be silent, whichever way it went wrong.
  for (size_t i = 0; i < voiced.size(); ++i) {
    for (size_t j = i + 1; j < voiced.size(); ++j) {
      INFO(kVariations[i].what << " vs " << kVariations[j].what);
      const bool same_voice =
          std::string_view(kVariations[i].voice) == std::string_view(kVariations[j].voice);
      REQUIRE((voiced[i] == voiced[j]) == same_voice);
    }
  }

  // GM2 assigns program 21 bank LSB 1 to the FRENCH accordion — the dry tuning
  // the capital already voices — so that address must NOT reach the Italian
  // musette the GS variation selects.
  REQUIRE(&gm_fallback_patch(1, 21) == &gm_fallback_patch(0, 21));

  // Every bank the table does not list resolves to the capital tone. That is the
  // GS rule for a variation a module does not have, and it is what keeps a
  // file written for a larger module audible on this one.
  for (int program = 0; program < 128; ++program) {
    const auto prog = static_cast<uint8_t>(program);
    const NativeSynthPatch& capital = gm_fallback_patch(0, prog);
    for (int bank = 1; bank < 128; ++bank) {
      bool listed = false;
      for (const Variation& v : kVariations) {
        if (v.program != prog) continue;
        if (bank == v.gs || (v.gm2 != 0 && bank == v.gm2)) listed = true;
      }
      if (listed) continue;
      INFO("program " << program << " bank " << bank);
      REQUIRE(&gm_fallback_patch(static_cast<uint16_t>(bank), prog) == &capital);
    }
  }
}

TEST_CASE("a GS variation bank sounds different from its capital tone", "[midi][synth][sf2][gm]") {
  // The address checks above prove the routing; this proves the routing reaches
  // audio, once per engine a variation is voiced on — a delta written into a
  // section the engine never reads would pass every address check in silence.
  struct Case {
    uint8_t program;
    uint8_t variation;
    const char* what;
  };
  constexpr Case kCases[] = {
      {4, 8, "Detuned EP1 (FM)"},
      {6, 8, "Coupled Hps. (Karplus-Strong)"},
      {14, 8, "Church Bell (modal)"},
      {16, 8, "Detuned Or1 (drawbar)"},
      {19, 8, "Church Org.2 (pipe)"},
      {21, 8, "Accordion It (free reed)"},
      {25, 8, "12-str.Gt (Karplus-Strong)"},
      {40, 8, "Slow Violin (bowed string)"},
      {0, 16, "Piano 1d (waveguide piano)"},
  };
  for (const Case& c : kCases) {
    INFO(c.what);
    Sf2Player capital_player = make_fallback_player();
    Sf2Player variation_player = make_fallback_player();
    const std::vector<float> capital = select_and_play(capital_player, 0, 0, 0, c.program);
    const std::vector<float> variation =
        select_and_play(variation_player, 0, c.variation, 0, c.program);
    REQUIRE(peak(capital) > 0.0f);
    REQUIRE(peak(variation) > 0.0f);
    REQUIRE(capital != variation);
  }

  // The GM2 form of a tone is the same audio, not merely the same family.
  Sf2Player gs_form = make_fallback_player();
  Sf2Player gm2_form = make_fallback_player();
  REQUIRE(select_and_play(gm2_form, 0, 0x79, 1, 25) == select_and_play(gs_form, 0, 8, 0, 25));

  // A map that predates a tone plays the capital instead. Every tone voiced so
  // far is an SC-55 one, so the SC-55 map must reach all of them — the gate
  // must not be silently rejecting tones it should pass.
  Sf2Player sc55_map = make_fallback_player();
  Sf2Player default_map = make_fallback_player();
  REQUIRE(select_and_play(sc55_map, 0, 8, 1, 25) == select_and_play(default_map, 0, 8, 0, 25));
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

TEST_CASE("MIDI 2.0 note-on velocity uses the canonical 16-to-7-bit conversion",
          "[midi][synth][sf2][gm]") {
  // This is the conversion shared by NativeSynth and Sf2Player. Keep an
  // independent reference here so the helper's low-velocity clamp cannot drift
  // back to the plain (note-off-producing) 16 -> 7-bit down-scale.
  const auto canonical = [](uint16_t velocity16) {
    const uint8_t velocity7 = static_cast<uint8_t>(velocity16 >> 9u);
    return velocity16 != 0 && velocity7 == 0 ? uint8_t{1} : velocity7;
  };
  constexpr uint16_t boundaries[] = {0, 1, 511, 512, 65535};

  for (const uint16_t velocity16 : boundaries) {
    INFO("velocity16 " << velocity16);
    REQUIRE(sonare::midi::scale_note_on_velocity_16_to_7(velocity16) == canonical(velocity16));
  }
  for (uint32_t raw_velocity = 0; raw_velocity <= 0xFFFFu; ++raw_velocity) {
    const uint16_t velocity16 = static_cast<uint16_t>(raw_velocity);
    REQUIRE(sonare::midi::scale_note_on_velocity_16_to_7(velocity16) == canonical(velocity16));
  }

  // The boundary cases that produce an audible note must also be bit-identical
  // to the equivalent MIDI 1.0 event on both canonical consumers.
  for (const uint16_t velocity16 : {uint16_t{1}, uint16_t{511}, uint16_t{512}, uint16_t{65535}}) {
    INFO("velocity16 " << velocity16);
    const uint8_t velocity7 = sonare::midi::scale_note_on_velocity_16_to_7(velocity16);

    NativeSynth gm_midi2 = make_gm_synth();
    NativeSynth gm_midi1 = make_gm_synth();
    gm_midi2.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, velocity16)));
    gm_midi1.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, velocity7)));
    REQUIRE(render(gm_midi2, 2048) == render(gm_midi1, 2048));

    Sf2Player sf2_midi2 = make_fallback_player();
    Sf2Player sf2_midi1 = make_fallback_player();
    sf2_midi2.on_event(0, event(sonare::midi::make_midi2_note_on(0, 0, 60, velocity16)));
    sf2_midi1.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, velocity7)));
    REQUIRE(render(sf2_midi2, 2048) == render(sf2_midi1, 2048));
  }
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
