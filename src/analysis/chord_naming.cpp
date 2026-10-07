#include <string>
#include <utility>
#include <vector>

#include "analysis/chord_analyzer.h"

namespace sonare {

std::string Chord::to_string() const {
  if (quality == ChordQuality::Unknown) {
    return "N.C.";
  }
  std::string name = pitch_class_to_string(root);
  switch (quality) {
    case ChordQuality::Major:
      break;  // Major is implied
    case ChordQuality::Minor:
      name += "m";
      break;
    case ChordQuality::Diminished:
      name += "dim";
      break;
    case ChordQuality::Augmented:
      name += "aug";
      break;
    case ChordQuality::Dominant7:
      name += "7";
      break;
    case ChordQuality::Major7:
      name += "maj7";
      break;
    case ChordQuality::Minor7:
      name += "m7";
      break;
    case ChordQuality::Sus2:
      name += "sus2";
      break;
    case ChordQuality::Sus4:
      name += "sus4";
      break;
    case ChordQuality::Unknown:
      name += "?";
      break;
    case ChordQuality::Add9:
      name += "add9";
      break;
    case ChordQuality::MinorAdd9:
      name += "madd9";
      break;
    case ChordQuality::Dim7:
      name += "dim7";
      break;
    case ChordQuality::HalfDim7:
      name += "m7b5";
      break;
    case ChordQuality::Major9:
      name += "maj9";
      break;
    case ChordQuality::Dominant9:
      name += "9";
      break;
    case ChordQuality::Sus2Add4:
      name += "sus2add4";
      break;
    case ChordQuality::Major6:
      name += "6";
      break;
    case ChordQuality::Minor6:
      name += "m6";
      break;
    case ChordQuality::MinorMajor7:
      name += "mM7";
      break;
    case ChordQuality::Dominant7Sus4:
      name += "7sus4";
      break;
    case ChordQuality::Dominant11:
      name += "11";
      break;
    case ChordQuality::Dominant13:
      name += "13";
      break;
    case ChordQuality::Dominant7b9:
      name += "7b9";
      break;
    case ChordQuality::Dominant7s9:
      name += "7#9";
      break;
  }
  if (bass != root) {
    name += "/";
    name += pitch_class_to_string(bass);
  }
  return name;
}

std::string chord_symbol(int root, int quality) {
  if (root < 0 || root > 11 || quality < 0 || quality >= kChordQualityCount) {
    return "N.C.";
  }
  Chord chord{};
  chord.root = static_cast<PitchClass>(root);
  chord.quality = static_cast<ChordQuality>(quality);
  chord.bass = chord.root;
  return chord.to_string();
}

std::string ChordAnalyzer::progression_pattern() const {
  if (chords_.empty()) return "";

  std::string out;
  bool first = true;

  for (const auto& chord : chords_) {
    if (!first) {
      out += " - ";
    }
    out += chord.to_string();
    first = false;
  }

  return out;
}

std::string ChordAnalyzer::chord_to_roman_numeral(const Chord& chord, PitchClass key_root,
                                                  Mode mode) {
  if (chord.quality == ChordQuality::Unknown) {
    return "N.C.";
  }
  // Calculate interval from key root
  int interval = (static_cast<int>(chord.root) - static_cast<int>(key_root) + 12) % 12;

  // Scale degree names
  // Major scale intervals: 0, 2, 4, 5, 7, 9, 11 (I, II, III, IV, V, VI, VII)
  // Natural minor intervals: 0, 2, 3, 5, 7, 8, 10 (i, ii, bIII, iv, v, bVI, bVII)
  static const std::vector<std::pair<int, std::string>> major_degrees = {
      {0, "I"}, {2, "II"}, {4, "III"}, {5, "IV"}, {7, "V"}, {9, "VI"}, {11, "VII"}};

  static const std::vector<std::pair<int, std::string>> minor_degrees = {
      {0, "I"}, {2, "II"}, {3, "III"}, {5, "IV"}, {7, "V"}, {8, "VI"}, {10, "VII"}};

  // Select scale based on mode
  const auto& scale_degrees = (mode == Mode::Minor) ? minor_degrees : major_degrees;

  // Find closest scale degree
  std::string numeral;
  bool is_chromatic = true;

  for (const auto& deg : scale_degrees) {
    if (deg.first == interval) {
      numeral = deg.second;
      is_chromatic = false;
      break;
    }
  }

  // Handle chromatic chords (flat/sharp relative to the current scale)
  if (is_chromatic) {
    // For minor mode, check if the chord is on a major scale degree (raised)
    // For major mode, check if the chord is on a minor scale degree (lowered)
    const auto& other_degrees = (mode == Mode::Minor) ? major_degrees : minor_degrees;

    // Prefer an exact parallel-mode degree (bIII/bVI/bVII in major) over an
    // enharmonic sharp spelling relative to the diatonic scale.
    for (const auto& deg : other_degrees) {
      if (deg.first == interval) {
        numeral = (mode == Mode::Minor) ? deg.second : "b" + deg.second;
        is_chromatic = false;
        break;
      }
    }

    // Otherwise spell accidentals relative to the current scale, preferring
    // flats before sharps for the conventional Roman-numeral representation.
    if (numeral.empty()) {
      for (const auto& deg : scale_degrees) {
        if ((deg.first - 1 + 12) % 12 == interval) {
          numeral = "b" + deg.second;
          break;
        }
      }
    }
    if (numeral.empty()) {
      for (const auto& deg : scale_degrees) {
        if ((deg.first + 1) % 12 == interval) {
          numeral = "#" + deg.second;
          break;
        }
      }
    }
  }

  if (numeral.empty()) {
    numeral = "?";
  }

  // Adjust case based on chord quality. The triad base comes from the interval
  // table, so a minor-third quality added later is lower-cased without this
  // list having to be extended.
  const ChordQuality roman_triad_base = chord_quality_triad_base(chord.quality);
  bool is_minor_chord =
      (roman_triad_base == ChordQuality::Minor || roman_triad_base == ChordQuality::Diminished);

  if (is_minor_chord) {
    // Convert to lowercase
    for (char& c : numeral) {
      if (c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
      }
    }
  }

  // Add quality suffix
  switch (chord.quality) {
    case ChordQuality::Diminished:
      numeral += "°";
      break;
    case ChordQuality::Dim7:
      numeral += "°7";
      break;
    case ChordQuality::HalfDim7:
      numeral += "ø7";
      break;
    case ChordQuality::Augmented:
      numeral += "+";
      break;
    case ChordQuality::Dominant7:
      numeral += "7";
      break;
    case ChordQuality::Major7:
      numeral += "maj7";
      break;
    case ChordQuality::Minor7:
      numeral += "7";
      break;
    case ChordQuality::Add9:
    case ChordQuality::MinorAdd9:
      numeral += "add9";
      break;
    case ChordQuality::Major9:
      numeral += "maj9";
      break;
    case ChordQuality::Dominant9:
      numeral += "9";
      break;
    case ChordQuality::Sus2:
      numeral += "sus2";
      break;
    case ChordQuality::Sus4:
      numeral += "sus4";
      break;
    case ChordQuality::Sus2Add4:
      numeral += "sus2add4";
      break;
    case ChordQuality::Major6:
    case ChordQuality::Minor6:
      numeral += "6";
      break;
    case ChordQuality::MinorMajor7:
      numeral += "M7";
      break;
    case ChordQuality::Dominant7Sus4:
      numeral += "7sus4";
      break;
    case ChordQuality::Dominant11:
      numeral += "11";
      break;
    case ChordQuality::Dominant13:
      numeral += "13";
      break;
    case ChordQuality::Dominant7b9:
      numeral += "7b9";
      break;
    case ChordQuality::Dominant7s9:
      numeral += "7#9";
      break;
    default:
      break;
  }

  return numeral;
}

std::vector<std::string> ChordAnalyzer::functional_analysis(PitchClass key_root, Mode mode) const {
  std::vector<std::string> result;
  result.reserve(chords_.size());

  for (const auto& chord : chords_) {
    result.push_back(chord_to_roman_numeral(chord, key_root, mode));
  }

  return result;
}

}  // namespace sonare
