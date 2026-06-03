#include "mir/key_context.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonare::mir {
namespace {

/// Number of pitch classes per octave (integer modulo space; distinct from the
/// float constants::kSemitonesPerOctave used in frequency math).
constexpr int kPcCount = 12;

/// Normalizes any integer into the pitch-class range 0..11.
uint8_t wrap_pc(int pc) {
  int m = pc % kPcCount;
  if (m < 0) m += kPcCount;
  return static_cast<uint8_t>(m);
}

/// Appends `pc` to `out` if not already present (keeps the set small + unique).
void add_pc(std::vector<uint8_t>& out, int pc) {
  const uint8_t v = wrap_pc(pc);
  if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
}

/// Chord-tone semitone offsets (from the root) for an arrangement chord quality.
/// Extensions on the ChordSymbol add further tones in derive_pitch_correction_target.
std::vector<int> quality_intervals(arrangement::ChordQuality quality) {
  switch (quality) {
    case arrangement::ChordQuality::kMajor:
      return {0, 4, 7};
    case arrangement::ChordQuality::kMinor:
      return {0, 3, 7};
    case arrangement::ChordQuality::kDiminished:
      return {0, 3, 6};
    case arrangement::ChordQuality::kAugmented:
      return {0, 4, 8};
    case arrangement::ChordQuality::kDominant:
      return {0, 4, 7, 10};
    case arrangement::ChordQuality::kHalfDiminished:
      return {0, 3, 6, 10};
    case arrangement::ChordQuality::kSuspended:
      return {0, 5, 7};  // sus4 by default; sus2 distinguished via extensions.
    case arrangement::ChordQuality::kUnknown:
    default:
      return {0};
  }
}

/// Extension scale-degree (e.g. 9, 11, 13, or 2 for sus2) -> semitone offset.
int extension_semitones(uint8_t degree) {
  switch (degree) {
    case 2:
      return 2;  // 9th folded into the octave, or sus2.
    case 4:
      return 5;  // 11th folded, or sus4 / add4.
    case 6:
      return 9;  // 13th folded, or add6.
    case 7:
      return 10;  // minor 7th by default (quality already sets maj7 cases).
    case 9:
      return 2;
    case 11:
      return 5;
    case 13:
      return 9;
    default:
      return -1;  // unknown degree: skip.
  }
}

/// Diatonic scale offsets (from the tonic) for an arrangement key mode.
std::vector<int> mode_scale(arrangement::KeyMode mode) {
  switch (mode) {
    case arrangement::KeyMode::kMajor:
      return {0, 2, 4, 5, 7, 9, 11};
    case arrangement::KeyMode::kMinor:  // natural minor
      return {0, 2, 3, 5, 7, 8, 10};
    case arrangement::KeyMode::kDorian:
      return {0, 2, 3, 5, 7, 9, 10};
    case arrangement::KeyMode::kPhrygian:
      return {0, 1, 3, 5, 7, 8, 10};
    case arrangement::KeyMode::kLydian:
      return {0, 2, 4, 6, 7, 9, 11};
    case arrangement::KeyMode::kMixolydian:
      return {0, 2, 4, 5, 7, 9, 10};
    case arrangement::KeyMode::kLocrian:
      return {0, 1, 3, 5, 6, 8, 10};
    case arrangement::KeyMode::kUnknown:
    default:
      return {};
  }
}

/// Converts a time in seconds to a PPQ position via a prepared tempo map.
double seconds_to_ppq(float seconds, const transport::TempoMap& tempo_map) {
  const double sr = tempo_map.sample_rate();
  const auto sample = static_cast<int64_t>(std::llround(static_cast<double>(seconds) * sr));
  return tempo_map.sample_to_ppq(sample);
}

}  // namespace

MappedQuality map_chord_quality(ChordQuality quality) {
  MappedQuality m;
  switch (quality) {
    case ChordQuality::Major:
      m.quality = arrangement::ChordQuality::kMajor;
      break;
    case ChordQuality::Minor:
      m.quality = arrangement::ChordQuality::kMinor;
      break;
    case ChordQuality::Diminished:
      m.quality = arrangement::ChordQuality::kDiminished;
      break;
    case ChordQuality::Augmented:
      m.quality = arrangement::ChordQuality::kAugmented;
      break;
    case ChordQuality::Dominant7:
      m.quality = arrangement::ChordQuality::kDominant;
      m.extensions = {7};
      break;
    case ChordQuality::Major7:
      m.quality = arrangement::ChordQuality::kMajor;
      m.extensions = {7};  // major-7th flavor carried as an extension degree.
      break;
    case ChordQuality::Minor7:
      m.quality = arrangement::ChordQuality::kMinor;
      m.extensions = {7};
      break;
    case ChordQuality::Sus2:
      m.quality = arrangement::ChordQuality::kSuspended;
      m.extensions = {2};
      break;
    case ChordQuality::Sus4:
      m.quality = arrangement::ChordQuality::kSuspended;
      m.extensions = {4};
      break;
    case ChordQuality::Add9:
      m.quality = arrangement::ChordQuality::kMajor;
      m.extensions = {9};
      break;
    case ChordQuality::MinorAdd9:
      m.quality = arrangement::ChordQuality::kMinor;
      m.extensions = {9};
      break;
    case ChordQuality::Dim7:
      m.quality = arrangement::ChordQuality::kDiminished;
      m.extensions = {7};
      break;
    case ChordQuality::HalfDim7:
      m.quality = arrangement::ChordQuality::kHalfDiminished;
      m.extensions = {7};
      break;
    case ChordQuality::Major9:
      m.quality = arrangement::ChordQuality::kMajor;
      m.extensions = {7, 9};
      break;
    case ChordQuality::Dominant9:
      m.quality = arrangement::ChordQuality::kDominant;
      m.extensions = {7, 9};
      break;
    case ChordQuality::Sus2Add4:
      m.quality = arrangement::ChordQuality::kSuspended;
      m.extensions = {2, 4};
      break;
    case ChordQuality::Unknown:
    default:
      m.quality = arrangement::ChordQuality::kUnknown;
      break;
  }
  return m;
}

arrangement::KeyMode map_key_mode(Mode mode) {
  switch (mode) {
    case Mode::Major:
      return arrangement::KeyMode::kMajor;
    case Mode::Minor:
      return arrangement::KeyMode::kMinor;
    case Mode::Dorian:
      return arrangement::KeyMode::kDorian;
    case Mode::Phrygian:
      return arrangement::KeyMode::kPhrygian;
    case Mode::Lydian:
      return arrangement::KeyMode::kLydian;
    case Mode::Mixolydian:
      return arrangement::KeyMode::kMixolydian;
    case Mode::Locrian:
      return arrangement::KeyMode::kLocrian;
    default:
      return arrangement::KeyMode::kUnknown;
  }
}

arrangement::HarmonicTimeline build_harmonic_timeline(const HarmonicAnalysisInput& input,
                                                      const transport::TempoMap& tempo_map) {
  arrangement::HarmonicTimeline timeline;

  // --- Key segments --------------------------------------------------------
  const bool have_segmented_keys =
      !input.keys.empty() && input.key_start_times.size() == input.keys.size();
  if (have_segmented_keys) {
    for (size_t i = 0; i < input.keys.size(); ++i) {
      arrangement::KeySegment seg;
      seg.start_ppq = seconds_to_ppq(input.key_start_times[i], tempo_map);
      seg.end_ppq =
          (i + 1 < input.keys.size())
              ? seconds_to_ppq(input.key_start_times[i + 1], tempo_map)
              : (input.chords.empty() ? seg.start_ppq
                                      : seconds_to_ppq(input.chords.back().end, tempo_map));
      seg.tonic_pc = static_cast<uint8_t>(static_cast<int>(input.keys[i].root));
      seg.mode = map_key_mode(input.keys[i].mode);
      timeline.keys.push_back(std::move(seg));
    }
  } else if (input.has_global_key && !input.chords.empty()) {
    arrangement::KeySegment seg;
    seg.start_ppq = seconds_to_ppq(input.chords.front().start, tempo_map);
    seg.end_ppq = seconds_to_ppq(input.chords.back().end, tempo_map);
    seg.tonic_pc = static_cast<uint8_t>(static_cast<int>(input.global_key.root));
    seg.mode = map_key_mode(input.global_key.mode);
    timeline.keys.push_back(std::move(seg));
  }

  // --- Chord segments ------------------------------------------------------
  auto key_tonic_at = [&](double ppq) -> int {
    const arrangement::KeySegment* k = timeline.key_at(ppq);
    return k ? static_cast<int>(k->tonic_pc) : -1;
  };

  int prev_tonic = -2;  // sentinel distinct from any pitch class or "unknown".
  for (const auto& c : input.chords) {
    arrangement::ChordSymbol sym;
    sym.start_ppq = seconds_to_ppq(c.start, tempo_map);
    sym.end_ppq = seconds_to_ppq(c.end, tempo_map);
    sym.root_pc = static_cast<uint8_t>(static_cast<int>(c.root));
    const MappedQuality mq = map_chord_quality(c.quality);
    sym.quality = mq.quality;
    sym.extensions = mq.extensions;
    if (c.bass != c.root) {
      sym.slash_bass_pc = static_cast<uint8_t>(static_cast<int>(c.bass));
    }
    // Mark a modulation boundary when the active key tonic changes here.
    const int tonic = key_tonic_at(sym.start_ppq);
    if (tonic >= 0 && tonic != prev_tonic && prev_tonic != -2) {
      sym.modulation_boundary = true;
    }
    if (tonic >= 0) prev_tonic = tonic;
    timeline.chords.push_back(std::move(sym));
  }

  return timeline;
}

PitchCorrectionTarget derive_pitch_correction_target(const arrangement::HarmonicTimeline& timeline,
                                                     double ppq) {
  PitchCorrectionTarget target;

  // Chord tones: root + quality intervals + extension degrees.
  const arrangement::ChordSymbol* chord = timeline.chord_at(ppq);
  if (chord != nullptr && chord->root_pc != arrangement::kUnknownPitchClass) {
    const int root = chord->root_pc;
    for (int iv : quality_intervals(chord->quality)) add_pc(target.chord_tones, root + iv);
    for (uint8_t deg : chord->extensions) {
      const int semis = extension_semitones(deg);
      if (semis >= 0) add_pc(target.chord_tones, root + semis);
    }
    std::sort(target.chord_tones.begin(), target.chord_tones.end());
  }

  // Scale tones: diatonic collection of the active key.
  const arrangement::KeySegment* key = timeline.key_at(ppq);
  if (key != nullptr && key->tonic_pc != arrangement::kUnknownPitchClass) {
    const int tonic = key->tonic_pc;
    for (int iv : mode_scale(key->mode)) add_pc(target.scale_tones, tonic + iv);
    std::sort(target.scale_tones.begin(), target.scale_tones.end());
  } else if (!target.chord_tones.empty()) {
    // No key context: fall back to the chord's own tones as the scale.
    target.scale_tones = target.chord_tones;
  }

  return target;
}

std::vector<SplitCandidate> onset_split_candidates(const std::vector<Onset>& onsets,
                                                   const transport::TempoMap& tempo_map,
                                                   double clip_start_ppq, double clip_end_ppq,
                                                   const OnsetSplitConfig& config) {
  std::vector<SplitCandidate> out;
  if (clip_end_ppq <= clip_start_ppq) return out;

  const double low = clip_start_ppq + config.edge_guard_ppq;
  const double high = clip_end_ppq - config.edge_guard_ppq;

  double last_accepted = -std::numeric_limits<double>::infinity();
  for (const auto& on : onsets) {
    if (on.strength < config.min_strength) continue;
    const double ppq = seconds_to_ppq(on.time, tempo_map);
    // Strictly inside the clip span (a split at the exact start/end is a no-op).
    if (ppq <= clip_start_ppq || ppq >= clip_end_ppq) continue;
    if (ppq < low || ppq > high) continue;
    if (!out.empty() && config.min_spacing_ppq > 0.0 &&
        (ppq - last_accepted) < config.min_spacing_ppq) {
      continue;
    }
    out.push_back(SplitCandidate{ppq, on.strength});
    last_accepted = ppq;
  }
  return out;
}

}  // namespace sonare::mir
