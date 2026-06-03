#include "midi/program_map.h"

#include <cstddef>

namespace sonare::midi {
namespace {

// --- General MIDI Level 1: 128 instrument names, indexed by 0-based program. --
constexpr std::array<std::string_view, 128> kGmInstruments = {{
    // Piano (0..7)
    "Acoustic Grand Piano",
    "Bright Acoustic Piano",
    "Electric Grand Piano",
    "Honky-tonk Piano",
    "Electric Piano 1",
    "Electric Piano 2",
    "Harpsichord",
    "Clavi",
    // Chromatic Percussion (8..15)
    "Celesta",
    "Glockenspiel",
    "Music Box",
    "Vibraphone",
    "Marimba",
    "Xylophone",
    "Tubular Bells",
    "Dulcimer",
    // Organ (16..23)
    "Drawbar Organ",
    "Percussive Organ",
    "Rock Organ",
    "Church Organ",
    "Reed Organ",
    "Accordion",
    "Harmonica",
    "Tango Accordion",
    // Guitar (24..31)
    "Acoustic Guitar (nylon)",
    "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)",
    "Electric Guitar (clean)",
    "Electric Guitar (muted)",
    "Overdriven Guitar",
    "Distortion Guitar",
    "Guitar harmonics",
    // Bass (32..39)
    "Acoustic Bass",
    "Electric Bass (finger)",
    "Electric Bass (pick)",
    "Fretless Bass",
    "Slap Bass 1",
    "Slap Bass 2",
    "Synth Bass 1",
    "Synth Bass 2",
    // Strings (40..47)
    "Violin",
    "Viola",
    "Cello",
    "Contrabass",
    "Tremolo Strings",
    "Pizzicato Strings",
    "Orchestral Harp",
    "Timpani",
    // Ensemble (48..55)
    "String Ensemble 1",
    "String Ensemble 2",
    "SynthStrings 1",
    "SynthStrings 2",
    "Choir Aahs",
    "Voice Oohs",
    "Synth Voice",
    "Orchestra Hit",
    // Brass (56..63)
    "Trumpet",
    "Trombone",
    "Tuba",
    "Muted Trumpet",
    "French Horn",
    "Brass Section",
    "SynthBrass 1",
    "SynthBrass 2",
    // Reed (64..71)
    "Soprano Sax",
    "Alto Sax",
    "Tenor Sax",
    "Baritone Sax",
    "Oboe",
    "English Horn",
    "Bassoon",
    "Clarinet",
    // Pipe (72..79)
    "Piccolo",
    "Flute",
    "Recorder",
    "Pan Flute",
    "Blown Bottle",
    "Shakuhachi",
    "Whistle",
    "Ocarina",
    // Synth Lead (80..87)
    "Lead 1 (square)",
    "Lead 2 (sawtooth)",
    "Lead 3 (calliope)",
    "Lead 4 (chiff)",
    "Lead 5 (charang)",
    "Lead 6 (voice)",
    "Lead 7 (fifths)",
    "Lead 8 (bass + lead)",
    // Synth Pad (88..95)
    "Pad 1 (new age)",
    "Pad 2 (warm)",
    "Pad 3 (polysynth)",
    "Pad 4 (choir)",
    "Pad 5 (bowed)",
    "Pad 6 (metallic)",
    "Pad 7 (halo)",
    "Pad 8 (sweep)",
    // Synth Effects (96..103)
    "FX 1 (rain)",
    "FX 2 (soundtrack)",
    "FX 3 (crystal)",
    "FX 4 (atmosphere)",
    "FX 5 (brightness)",
    "FX 6 (goblins)",
    "FX 7 (echoes)",
    "FX 8 (sci-fi)",
    // Ethnic (104..111)
    "Sitar",
    "Banjo",
    "Shamisen",
    "Koto",
    "Kalimba",
    "Bag pipe",
    "Fiddle",
    "Shanai",
    // Percussive (112..119)
    "Tinkle Bell",
    "Agogo",
    "Steel Drums",
    "Woodblock",
    "Taiko Drum",
    "Melodic Tom",
    "Synth Drum",
    "Reverse Cymbal",
    // Sound Effects (120..127)
    "Guitar Fret Noise",
    "Breath Noise",
    "Seashore",
    "Bird Tweet",
    "Telephone Ring",
    "Helicopter",
    "Applause",
    "Gunshot",
}};

constexpr std::array<std::string_view, 16> kGmFamilies = {{
    "Piano",
    "Chromatic Percussion",
    "Organ",
    "Guitar",
    "Bass",
    "Strings",
    "Ensemble",
    "Brass",
    "Reed",
    "Pipe",
    "Synth Lead",
    "Synth Pad",
    "Synth Effects",
    "Ethnic",
    "Percussive",
    "Sound Effects",
}};

// --- GM percussion (channel 10) note -> name, notes 35..81. ------------------
constexpr uint8_t kDrumNoteLow = 35;
constexpr uint8_t kDrumNoteHigh = 81;
constexpr std::array<std::string_view, kDrumNoteHigh - kDrumNoteLow + 1> kGmDrums = {{
    "Acoustic Bass Drum",  // 35
    "Bass Drum 1",         // 36
    "Side Stick",          // 37
    "Acoustic Snare",      // 38
    "Hand Clap",           // 39
    "Electric Snare",      // 40
    "Low Floor Tom",       // 41
    "Closed Hi Hat",       // 42
    "High Floor Tom",      // 43
    "Pedal Hi-Hat",        // 44
    "Low Tom",             // 45
    "Open Hi-Hat",         // 46
    "Low-Mid Tom",         // 47
    "Hi-Mid Tom",          // 48
    "Crash Cymbal 1",      // 49
    "High Tom",            // 50
    "Ride Cymbal 1",       // 51
    "Chinese Cymbal",      // 52
    "Ride Bell",           // 53
    "Tambourine",          // 54
    "Splash Cymbal",       // 55
    "Cowbell",             // 56
    "Crash Cymbal 2",      // 57
    "Vibraslap",           // 58
    "Ride Cymbal 2",       // 59
    "Hi Bongo",            // 60
    "Low Bongo",           // 61
    "Mute Hi Conga",       // 62
    "Open Hi Conga",       // 63
    "Low Conga",           // 64
    "High Timbale",        // 65
    "Low Timbale",         // 66
    "High Agogo",          // 67
    "Low Agogo",           // 68
    "Cabasa",              // 69
    "Maracas",             // 70
    "Short Whistle",       // 71
    "Long Whistle",        // 72
    "Short Guiro",         // 73
    "Long Guiro",          // 74
    "Claves",              // 75
    "Hi Wood Block",       // 76
    "Low Wood Block",      // 77
    "Mute Cuica",          // 78
    "Open Cuica",          // 79
    "Mute Triangle",       // 80
    "Open Triangle",       // 81
}};

// --- GM2 percussion sets. -----------------------------------------------------
// Each GM2 percussion set is selected by a bank LSB on the GM2 percussion bank
// (bank MSB 0x78). A set differs from the Standard GM drum map only at a small
// number of notes; this tabulates ONLY those re-voiced notes, and any note not
// listed for a set falls back to the Standard GM name (kGmDrums). The Standard
// set (LSB 0) has no overrides.
struct Gm2DrumSetInfo {
  uint8_t bank_lsb;
  std::string_view name;
};
constexpr std::array<Gm2DrumSetInfo, 9> kGm2DrumSets = {{
    {0, "Standard"},
    {8, "Room"},
    {16, "Power"},
    {24, "Electronic"},
    {25, "Analog"},
    {32, "Jazz"},
    {40, "Brush"},
    {48, "Orchestra"},
    {56, "SFX"},
}};

struct Gm2DrumOverride {
  uint8_t bank_lsb;
  uint8_t note;
  std::string_view name;
};
// Canonical per-set note re-voicings from the GM2 specification's percussion
// sets. (Only the notes that differ from the Standard set are listed.)
constexpr std::array<Gm2DrumOverride, 40> kGm2DrumOverrides = {{
    // Room set (8)
    {8, 41, "Room Low Tom 2"},
    {8, 43, "Room Low Tom 1"},
    {8, 45, "Room Mid Tom 2"},
    {8, 47, "Room Mid Tom 1"},
    {8, 48, "Room Hi Tom 2"},
    {8, 50, "Room Hi Tom 1"},
    // Power set (16)
    {16, 36, "Power Kick Drum"},
    {16, 38, "Power Snare Drum"},
    {16, 41, "Power Low Tom 2"},
    {16, 43, "Power Low Tom 1"},
    {16, 45, "Power Mid Tom 2"},
    {16, 47, "Power Mid Tom 1"},
    {16, 48, "Power Hi Tom 2"},
    {16, 50, "Power Hi Tom 1"},
    // Electronic set (24)
    {24, 36, "Electric Bass Drum"},
    {24, 38, "Electric Snare 1"},
    {24, 40, "Electric Snare 2"},
    {24, 41, "Electric Low Tom 2"},
    {24, 43, "Electric Low Tom 1"},
    {24, 45, "Electric Mid Tom 2"},
    {24, 47, "Electric Mid Tom 1"},
    {24, 48, "Electric Hi Tom 2"},
    {24, 50, "Electric Hi Tom 1"},
    {24, 52, "Reverse Cymbal"},
    // Analog set (25)
    {25, 36, "Analog Bass Drum"},
    {25, 37, "Analog Rim Shot"},
    {25, 38, "Analog Snare 1"},
    {25, 40, "Analog Snare 2"},
    {25, 42, "Analog Closed Hi-Hat 1"},
    {25, 44, "Analog Closed Hi-Hat 2"},
    {25, 46, "Analog Open Hi-Hat"},
    {25, 56, "Analog Cowbell"},
    {25, 75, "Analog Claves"},
    // Jazz set (32)
    {32, 36, "Jazz Kick Drum"},
    // Brush set (40)
    {40, 38, "Brush Tap"},
    {40, 39, "Brush Slap"},
    {40, 40, "Brush Swirl"},
    // Orchestra set (48)
    {48, 38, "Concert Snare Drum"},
    {48, 40, "Concert Snare Drum"},
    {48, 41, "Timpani F"},
}};

// --- Standard MIDI controller names. Index = CC number; empty = undefined. ---
constexpr std::array<std::string_view, 128> kCcNames = [] {
  std::array<std::string_view, 128> n{};
  n[0] = "Bank Select (MSB)";
  n[1] = "Modulation Wheel (MSB)";
  n[2] = "Breath Controller (MSB)";
  n[4] = "Foot Controller (MSB)";
  n[5] = "Portamento Time (MSB)";
  n[6] = "Data Entry (MSB)";
  n[7] = "Channel Volume (MSB)";
  n[8] = "Balance (MSB)";
  n[10] = "Pan (MSB)";
  n[11] = "Expression (MSB)";
  n[12] = "Effect Control 1 (MSB)";
  n[13] = "Effect Control 2 (MSB)";
  n[16] = "General Purpose 1 (MSB)";
  n[17] = "General Purpose 2 (MSB)";
  n[18] = "General Purpose 3 (MSB)";
  n[19] = "General Purpose 4 (MSB)";
  n[32] = "Bank Select (LSB)";
  n[33] = "Modulation Wheel (LSB)";
  n[34] = "Breath Controller (LSB)";
  n[36] = "Foot Controller (LSB)";
  n[37] = "Portamento Time (LSB)";
  n[38] = "Data Entry (LSB)";
  n[39] = "Channel Volume (LSB)";
  n[40] = "Balance (LSB)";
  n[42] = "Pan (LSB)";
  n[43] = "Expression (LSB)";
  n[64] = "Sustain Pedal";
  n[65] = "Portamento On/Off";
  n[66] = "Sostenuto";
  n[67] = "Soft Pedal";
  n[68] = "Legato Footswitch";
  n[69] = "Hold 2";
  n[70] = "Sound Variation";
  n[71] = "Timbre / Harmonic Intensity";
  n[72] = "Release Time";
  n[73] = "Attack Time";
  n[74] = "Brightness";
  n[75] = "Decay Time";
  n[76] = "Vibrato Rate";
  n[77] = "Vibrato Depth";
  n[78] = "Vibrato Delay";
  n[84] = "Portamento Control";
  n[91] = "Reverb Send Level";
  n[92] = "Tremolo Depth";
  n[93] = "Chorus Send Level";
  n[94] = "Celeste Depth";
  n[95] = "Phaser Depth";
  n[96] = "Data Increment";
  n[97] = "Data Decrement";
  n[98] = "NRPN (LSB)";
  n[99] = "NRPN (MSB)";
  n[100] = "RPN (LSB)";
  n[101] = "RPN (MSB)";
  n[120] = "All Sound Off";
  n[121] = "Reset All Controllers";
  n[122] = "Local Control On/Off";
  n[123] = "All Notes Off";
  n[124] = "Omni Mode Off";
  n[125] = "Omni Mode On";
  n[126] = "Mono Mode On";
  n[127] = "Poly Mode On";
  return n;
}();

// --- MIDI 2.0 registered per-note controller names. -------------------------
constexpr std::array<std::string_view, 128> kPerNoteControllerNames = [] {
  std::array<std::string_view, 128> n{};
  n[1] = "Modulation";
  n[2] = "Breath";
  n[3] = "Pitch 7.25";
  n[7] = "Volume";
  n[8] = "Balance";
  n[10] = "Pan";
  n[11] = "Expression";
  n[70] = "Sound Variation";
  n[71] = "Timbre / Harmonic Intensity";
  n[72] = "Release Time";
  n[73] = "Attack Time";
  n[74] = "Brightness";
  n[75] = "Decay Time";
  n[76] = "Vibrato Rate";
  n[77] = "Vibrato Depth";
  n[78] = "Vibrato Delay";
  return n;
}();

// --- GM2 melodic variation table: (program, bank_lsb) -> name. ---------------
// GM2 keeps the 128 GM Level 1 names at bank LSB 0 (bank MSB 0x79) and assigns
// variation sounds at higher bank LSBs. This tabulates the canonical GM2
// "Instrument Sound Set" variations; queries that miss fall back to the base GM
// Level 1 name for the program.
struct Gm2Variation {
  uint8_t program;
  uint8_t bank_lsb;
  std::string_view name;
};
constexpr std::array<Gm2Variation, 127> kGm2Variations = {{
    // Piano
    {0, 1, "Acoustic Grand Piano (wide)"},
    {0, 2, "Acoustic Grand Piano (dark)"},
    {1, 1, "Bright Acoustic Piano (wide)"},
    {2, 1, "Electric Grand Piano (wide)"},
    {3, 1, "Honky-tonk Piano (wide)"},
    {4, 1, "Detuned Electric Piano 1"},
    {4, 2, "Electric Piano 1 (velocity mix)"},
    {4, 3, "60's Electric Piano"},
    {5, 1, "Detuned Electric Piano 2"},
    {5, 2, "Electric Piano 2 (velocity mix)"},
    {5, 3, "EP Legend"},
    {5, 4, "EP Phase"},
    {6, 1, "Harpsichord (octave mix)"},
    {6, 2, "Harpsichord (wide)"},
    {6, 3, "Harpsichord (with key off)"},
    {7, 1, "Clavi (wide)"},
    // Chromatic Percussion
    {8, 1, "Celesta"},
    {9, 1, "Glockenspiel"},
    {11, 1, "Vibraphone (wide)"},
    {12, 1, "Marimba (wide)"},
    {14, 1, "Church Bell"},
    {14, 2, "Carillon"},
    // Organ
    {16, 1, "Detuned Organ 1"},
    {16, 2, "60's Organ 1"},
    {16, 3, "Organ 4"},
    {17, 1, "Detuned Organ 2"},
    {17, 2, "Organ 5"},
    {18, 1, "Rock Organ"},
    {19, 1, "Church Organ 2"},
    {19, 2, "Church Organ 3"},
    {20, 1, "Reed Organ"},
    {21, 1, "French Accordion"},
    {23, 1, "Bandoneon"},
    // Guitar
    {24, 1, "Ukulele"},
    {24, 2, "Acoustic Guitar (nylon + key off)"},
    {24, 3, "Acoustic Guitar (nylon 2)"},
    {25, 1, "12-String Guitar"},
    {25, 2, "Mandolin"},
    {25, 3, "Steel Guitar with Body Sound"},
    {26, 1, "Hawaiian Guitar"},
    {27, 1, "Chorus Guitar"},
    {28, 1, "Funk Guitar"},
    {28, 2, "Funk Guitar 2"},
    {30, 1, "Guitar Pinch"},
    {31, 1, "Guitar Feedback"},
    // Bass
    {32, 1, "Acoustic Bass"},
    {33, 1, "Finger Slap Bass"},
    {35, 1, "Fretless Bass"},
    {36, 1, "Slap Bass 1"},
    {38, 1, "Synth Bass 3 (resonance)"},
    {38, 2, "Clavi Bass"},
    {38, 3, "Hammer"},
    {39, 1, "Synth Bass 4 (attack)"},
    {39, 2, "Synth Bass (rubber)"},
    {39, 3, "Attack Pulse"},
    // Strings & Orchestral
    {40, 1, "Violin (slow attack)"},
    {42, 1, "Cello"},
    {44, 1, "Tremolo Strings"},
    {45, 1, "Pizzicato Strings"},
    {46, 1, "Yang Qin"},
    {47, 1, "Timpani"},
    {48, 1, "Strings and Brass"},
    {48, 2, "60's Strings"},
    {49, 1, "Slow String Ensemble"},
    {50, 1, "Synth Strings 3"},
    {51, 1, "Synth Strings 2"},
    // Choir / Voice
    {52, 1, "Choir Aahs 2"},
    {54, 1, "Synth Voice"},
    {54, 2, "Analog Voice"},
    {55, 1, "Orchestra Hit 2"},
    {55, 2, "Impact"},
    // Brass
    {56, 1, "Trumpet (dark)"},
    {57, 1, "Trombone 2"},
    {57, 2, "Bright Trombone"},
    {58, 1, "Tuba"},
    {59, 1, "Muted Trumpet 2"},
    {60, 1, "French Horn 2 (warm)"},
    {61, 1, "Brass Section 2"},
    {62, 1, "Synth Brass 3"},
    {62, 2, "Analog Synth Brass 1"},
    {62, 3, "Jump Brass"},
    {63, 1, "Synth Brass 4"},
    {63, 2, "Analog Synth Brass 2"},
    // Reed
    {64, 1, "Soprano Sax"},
    {65, 1, "Alto Sax"},
    {66, 1, "Tenor Sax"},
    {67, 1, "Baritone Sax"},
    {68, 1, "Oboe"},
    {71, 1, "Clarinet"},
    // Pipe
    {72, 1, "Piccolo"},
    {73, 1, "Flute"},
    {75, 1, "Pan Flute"},
    // Synth Lead
    {80, 1, "Square Wave"},
    {80, 2, "Sine Wave"},
    {81, 1, "Saw Wave"},
    {81, 2, "Doctor Solo"},
    {81, 3, "Natural Lead"},
    {81, 4, "Sequenced Saw"},
    {82, 1, "Calliope Lead"},
    {83, 1, "Chiff Lead"},
    {84, 1, "Charang Lead"},
    {84, 2, "Wire Lead"},
    {85, 1, "Voice Lead"},
    {86, 1, "Fifths Lead"},
    {87, 1, "Bass and Lead"},
    {87, 2, "Soft Wrl"},
    // Synth Pad
    {88, 1, "New Age Pad"},
    {89, 1, "Warm Pad"},
    {89, 2, "Sine Pad"},
    {90, 1, "Polysynth Pad"},
    {91, 1, "Space Voice Pad"},
    {91, 2, "Itopia"},
    {92, 1, "Bowed Glass Pad"},
    {93, 1, "Metal Pad"},
    {94, 1, "Halo Pad"},
    {95, 1, "Sweep Pad"},
    // Synth Effects
    {96, 1, "Rain (FX)"},
    {97, 1, "Soundtrack (FX)"},
    {98, 1, "Crystal (FX)"},
    {98, 2, "Synth Mallet"},
    {99, 1, "Atmosphere (FX)"},
    {100, 1, "Brightness (FX)"},
    {101, 1, "Goblins (FX)"},
    {102, 1, "Echo Drops (FX)"},
    {102, 2, "Echo Bell"},
    {102, 3, "Echo Pan"},
    {103, 1, "Sci-Fi (FX)"},
}};

}  // namespace

std::string_view gm_instrument_name(uint8_t program) noexcept {
  if (program >= kGmInstruments.size()) return {};
  return kGmInstruments[program];
}

int gm_program_for_name(std::string_view name) noexcept {
  if (name.empty()) return -1;
  for (size_t i = 0; i < kGmInstruments.size(); ++i) {
    if (kGmInstruments[i] == name) return static_cast<int>(i);
  }
  return -1;
}

int gm_family_first_program(uint8_t family) noexcept {
  if (family >= kGmFamilies.size()) return -1;
  return static_cast<int>(family) * 8;
}

std::string_view gm_family_name(uint8_t family) noexcept {
  if (family >= kGmFamilies.size()) return {};
  return kGmFamilies[family];
}

std::string_view gm2_instrument_name(uint8_t bank_lsb, uint8_t program) noexcept {
  if (program >= kGmInstruments.size()) return {};
  if (bank_lsb != 0) {
    for (const auto& v : kGm2Variations) {
      if (v.program == program && v.bank_lsb == bank_lsb) return v.name;
    }
  }
  // Fall back to the base GM Level 1 name (bank LSB 0 == GM compatible).
  return kGmInstruments[program];
}

std::string_view gm_drum_name(uint8_t note) noexcept {
  if (note < kDrumNoteLow || note > kDrumNoteHigh) return {};
  return kGmDrums[note - kDrumNoteLow];
}

int gm_drum_note_for_name(std::string_view name) noexcept {
  if (name.empty()) return -1;
  for (size_t i = 0; i < kGmDrums.size(); ++i) {
    if (kGmDrums[i] == name) return static_cast<int>(i) + kDrumNoteLow;
  }
  return -1;
}

std::string_view gm2_drum_name(uint8_t bank_lsb, uint8_t note) noexcept {
  if (note < kDrumNoteLow || note > kDrumNoteHigh) return {};
  if (bank_lsb != 0) {
    for (const auto& o : kGm2DrumOverrides) {
      if (o.bank_lsb == bank_lsb && o.note == note) return o.name;
    }
  }
  // Notes a set does not re-voice (and the Standard set) use the GM Level 1 name.
  return kGmDrums[note - kDrumNoteLow];
}

std::string_view gm2_drum_set_name(uint8_t bank_lsb) noexcept {
  for (const auto& s : kGm2DrumSets) {
    if (s.bank_lsb == bank_lsb) return s.name;
  }
  return {};
}

Ump remap_drum_note(const Ump& ump, const DrumMapOverride& map) noexcept {
  const UmpMessageType mt = ump.message_type();
  if (mt != UmpMessageType::kMidi1ChannelVoice && mt != UmpMessageType::kMidi2ChannelVoice) {
    return ump;
  }
  const uint8_t status = ump.status_nibble();
  const bool note_addressed = status == static_cast<uint8_t>(UmpStatus::kNoteOn) ||
                              status == static_cast<uint8_t>(UmpStatus::kNoteOff) ||
                              status == static_cast<uint8_t>(UmpStatus::kPolyPressure);
  if (!note_addressed) return ump;
  const uint8_t note = ump.note_number();
  const uint8_t mapped = map.map_note(note);
  if (mapped == note) return ump;
  // Rewrite the note number (word[0] bits 8..14); identical field for 1.0 / 2.0.
  Ump out = ump;
  out.words[0] = (out.words[0] & ~(0x7Fu << 8u)) | (static_cast<uint32_t>(mapped & 0x7Fu) << 8u);
  return out;
}

std::string_view cc_name(uint8_t controller) noexcept {
  if (controller >= kCcNames.size()) return {};
  return kCcNames[controller];
}

int cc_index_for_name(std::string_view name) noexcept {
  if (name.empty()) return -1;
  for (size_t i = 0; i < kCcNames.size(); ++i) {
    if (!kCcNames[i].empty() && kCcNames[i] == name) return static_cast<int>(i);
  }
  return -1;
}

std::string_view per_note_controller_name(uint8_t index) noexcept {
  if (index >= kPerNoteControllerNames.size()) return {};
  return kPerNoteControllerNames[index];
}

BankProgramMessages program_to_messages(uint8_t group, uint8_t channel,
                                        const ProgramSelection& selection) noexcept {
  BankProgramMessages out;
  out.messages[0] = make_midi1_control_change(group, channel, 0, selection.bank_msb);
  out.messages[1] = make_midi1_control_change(group, channel, 32, selection.bank_lsb);
  out.messages[2] = make_midi1_program_change(group, channel, selection.program);
  out.count = 3;
  return out;
}

bool ProgramState::observe(const Ump& ump) noexcept {
  const UmpMessageType mt = ump.message_type();
  if (mt != UmpMessageType::kMidi1ChannelVoice && mt != UmpMessageType::kMidi2ChannelVoice) {
    return false;
  }
  const uint8_t status = ump.status_nibble();
  if (status == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t controller = static_cast<uint8_t>((ump.words[0] >> 8) & 0x7Fu);
    const uint8_t value = mt == UmpMessageType::kMidi2ChannelVoice
                              ? scale_cc_32_to_7(ump.words[1])
                              : static_cast<uint8_t>(ump.words[0] & 0x7Fu);
    if (controller == 0) {
      pending_bank_msb = value;
    } else if (controller == 32) {
      pending_bank_lsb = value;
    }
    return false;
  }
  if (status == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    ProgramSelection next;
    if (mt == UmpMessageType::kMidi2ChannelVoice) {
      const bool bank_valid = (ump.words[0] & 0x01u) != 0;
      next.bank_msb =
          bank_valid ? static_cast<uint8_t>((ump.words[1] >> 8) & 0x7Fu) : pending_bank_msb;
      next.bank_lsb = bank_valid ? static_cast<uint8_t>(ump.words[1] & 0x7Fu) : pending_bank_lsb;
      next.program = static_cast<uint8_t>((ump.words[1] >> 24) & 0x7Fu);
      if (bank_valid) {
        pending_bank_msb = next.bank_msb;
        pending_bank_lsb = next.bank_lsb;
      }
    } else {
      next.bank_msb = pending_bank_msb;
      next.bank_lsb = pending_bank_lsb;
      next.program = static_cast<uint8_t>((ump.words[0] >> 8) & 0x7Fu);
    }
    const bool changed = !program_seen || next != current;
    current = next;
    program_seen = true;
    return changed;
  }
  return false;
}

}  // namespace sonare::midi
