#include "midi/assist/modules/music_theory.h"

#include <algorithm>
#include <array>

namespace sonare::midi::assist::theory {

namespace {

using arrangement::ChordQuality;
using arrangement::KeyMode;
using arrangement::kUnknownPitchClass;

constexpr int kPitchClasses = 12;
constexpr int kMaxMidiNote = 127;

bool valid_pc(uint8_t pc) noexcept { return pc < kPitchClasses; }

void push_unique(std::vector<uint8_t>& out, int semitone_above_root, int root_pc) {
  const auto pc = static_cast<uint8_t>(((root_pc + semitone_above_root) % kPitchClasses +
                                        kPitchClasses) %
                                       kPitchClasses);
  if (std::find(out.begin(), out.end(), pc) == out.end()) out.push_back(pc);
}

/// Semitone a tension degree sits at, reduced into the octave. The seventh is
/// absent here because its size follows the chord family; the caller adds it.
int semitone_for_degree(uint8_t degree) noexcept {
  switch (degree) {
    case 2:
    case 9:
      return 2;
    case 4:
    case 11:
      return 5;
    case 6:
    case 13:
      return 9;
    default:
      return -1;
  }
}

}  // namespace

std::vector<uint8_t> scale_pitch_classes(uint8_t tonic_pc, KeyMode mode) {
  if (!valid_pc(tonic_pc)) return {};
  static constexpr std::array<int, 7> kIonian{0, 2, 4, 5, 7, 9, 11};
  static constexpr std::array<int, 7> kAeolian{0, 2, 3, 5, 7, 8, 10};
  static constexpr std::array<int, 7> kDorian{0, 2, 3, 5, 7, 9, 10};
  static constexpr std::array<int, 7> kPhrygian{0, 1, 3, 5, 7, 8, 10};
  static constexpr std::array<int, 7> kLydian{0, 2, 4, 6, 7, 9, 11};
  static constexpr std::array<int, 7> kMixolydian{0, 2, 4, 5, 7, 9, 10};
  static constexpr std::array<int, 7> kLocrian{0, 1, 3, 5, 6, 8, 10};

  const std::array<int, 7>* degrees = nullptr;
  switch (mode) {
    case KeyMode::kMajor:
      degrees = &kIonian;
      break;
    case KeyMode::kMinor:
      degrees = &kAeolian;
      break;
    case KeyMode::kDorian:
      degrees = &kDorian;
      break;
    case KeyMode::kPhrygian:
      degrees = &kPhrygian;
      break;
    case KeyMode::kLydian:
      degrees = &kLydian;
      break;
    case KeyMode::kMixolydian:
      degrees = &kMixolydian;
      break;
    case KeyMode::kLocrian:
      degrees = &kLocrian;
      break;
    case KeyMode::kUnknown:
      return {};
  }
  std::vector<uint8_t> out;
  out.reserve(degrees->size());
  for (const int degree : *degrees) push_unique(out, degree, tonic_pc);
  return out;
}

std::vector<uint8_t> chord_pitch_classes(const arrangement::ChordSymbol& chord) {
  if (!valid_pc(chord.root_pc)) return {};
  const int root = chord.root_pc;
  std::vector<uint8_t> out;
  out.reserve(6);

  // The seventh's size is the family's, which is why it is decided here rather
  // than in the degree table.
  int seventh = -1;
  switch (chord.quality) {
    case ChordQuality::kMajor:
      push_unique(out, 0, root);
      push_unique(out, 4, root);
      push_unique(out, 7, root);
      seventh = 11;
      break;
    case ChordQuality::kMinor:
      push_unique(out, 0, root);
      push_unique(out, 3, root);
      push_unique(out, 7, root);
      seventh = 10;
      break;
    case ChordQuality::kDiminished:
      push_unique(out, 0, root);
      push_unique(out, 3, root);
      push_unique(out, 6, root);
      seventh = 9;  // the diminished seventh
      break;
    case ChordQuality::kAugmented:
      push_unique(out, 0, root);
      push_unique(out, 4, root);
      push_unique(out, 8, root);
      seventh = 10;
      break;
    case ChordQuality::kDominant:
      push_unique(out, 0, root);
      push_unique(out, 4, root);
      push_unique(out, 7, root);
      seventh = 10;
      break;
    case ChordQuality::kHalfDiminished:
      push_unique(out, 0, root);
      push_unique(out, 3, root);
      push_unique(out, 6, root);
      push_unique(out, 10, root);
      seventh = 10;
      break;
    case ChordQuality::kSuspended: {
      push_unique(out, 0, root);
      push_unique(out, 7, root);
      // The specific suspension lives in the extensions; sus4 when neither is
      // named, which is what a bare "sus" spells.
      const bool sus2 = std::find(chord.extensions.begin(), chord.extensions.end(),
                                  static_cast<uint8_t>(2)) != chord.extensions.end();
      push_unique(out, sus2 ? 2 : 5, root);
      seventh = 10;
      break;
    }
    case ChordQuality::kUnknown:
      return {};
  }

  for (const uint8_t degree : chord.extensions) {
    if (degree == 7) {
      if (seventh >= 0) push_unique(out, seventh, root);
      continue;
    }
    const int semitone = semitone_for_degree(degree);
    if (semitone >= 0) push_unique(out, semitone, root);
  }
  if (valid_pc(chord.slash_bass_pc)) push_unique(out, 0, chord.slash_bass_pc);
  return out;
}

int interval_class(int note_a, int note_b) noexcept {
  int distance = (note_a - note_b) % kPitchClasses;
  if (distance < 0) distance += kPitchClasses;
  return distance > 6 ? kPitchClasses - distance : distance;
}

float interval_class_roughness(int interval_class_value) noexcept {
  static constexpr std::array<float, 7> kRoughness{0.0f, 1.0f, 0.6f, 0.2f, 0.15f, 0.05f, 0.8f};
  if (interval_class_value < 0 || interval_class_value > 6) return 1.0f;
  return kRoughness[static_cast<size_t>(interval_class_value)];
}

bool admits(const std::vector<uint8_t>& pitch_classes, int midi_note) noexcept {
  if (pitch_classes.empty()) return true;
  if (midi_note < 0 || midi_note > kMaxMidiNote) return false;
  const auto pc = static_cast<uint8_t>(midi_note % kPitchClasses);
  return std::find(pitch_classes.begin(), pitch_classes.end(), pc) != pitch_classes.end();
}

int transpose_scale_steps(const std::vector<uint8_t>& pitch_classes, int midi_note,
                          int steps) noexcept {
  if (pitch_classes.empty()) return -1;
  const int start = nearest_admitted(pitch_classes, midi_note);
  if (start < 0) return -1;

  std::vector<uint8_t> sorted(pitch_classes);
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  const auto size = static_cast<int>(sorted.size());

  const auto pc = static_cast<uint8_t>(start % kPitchClasses);
  const auto found = std::find(sorted.begin(), sorted.end(), pc);
  if (found == sorted.end()) return -1;
  const int index = static_cast<int>(found - sorted.begin());

  // Split the move into whole turns of the set and a remainder, so the octave
  // follows from the number of turns rather than from comparing pitch classes.
  int target = index + steps;
  int octave_shift = 0;
  while (target < 0) {
    target += size;
    --octave_shift;
  }
  while (target >= size) {
    target -= size;
    ++octave_shift;
  }

  const int start_octave = start / kPitchClasses;
  const int note = (start_octave + octave_shift) * kPitchClasses + sorted[static_cast<size_t>(target)];
  if (note < 0 || note > kMaxMidiNote) return -1;
  return note;
}

int nearest_admitted(const std::vector<uint8_t>& pitch_classes, int midi_note) noexcept {
  if (pitch_classes.empty()) return midi_note;
  if (admits(pitch_classes, midi_note)) return midi_note;
  // Half an octave reaches every pitch class, so a wider search can only find a
  // note the first six offsets already passed.
  for (int offset = 1; offset <= 6; ++offset) {
    const int lower = midi_note - offset;
    if (lower >= 0 && admits(pitch_classes, lower)) return lower;
    const int upper = midi_note + offset;
    if (upper <= kMaxMidiNote && admits(pitch_classes, upper)) return upper;
  }
  return -1;
}

}  // namespace sonare::midi::assist::theory
