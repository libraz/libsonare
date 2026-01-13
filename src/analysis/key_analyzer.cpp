#include "analysis/key_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "analysis/chord_analyzer.h"
#include "util/exception.h"

namespace sonare {

std::string Key::to_string() const {
  std::string root_name;
  switch (root) {
    case PitchClass::C:
      root_name = "C";
      break;
    case PitchClass::Cs:
      root_name = "C#";
      break;
    case PitchClass::D:
      root_name = "D";
      break;
    case PitchClass::Ds:
      root_name = "D#";
      break;
    case PitchClass::E:
      root_name = "E";
      break;
    case PitchClass::F:
      root_name = "F";
      break;
    case PitchClass::Fs:
      root_name = "F#";
      break;
    case PitchClass::G:
      root_name = "G";
      break;
    case PitchClass::Gs:
      root_name = "G#";
      break;
    case PitchClass::A:
      root_name = "A";
      break;
    case PitchClass::As:
      root_name = "A#";
      break;
    case PitchClass::B:
      root_name = "B";
      break;
  }

  return root_name + (mode == Mode::Major ? " major" : " minor");
}

std::string Key::to_short_string() const {
  std::string root_name;
  switch (root) {
    case PitchClass::C:
      root_name = "C";
      break;
    case PitchClass::Cs:
      root_name = "C#";
      break;
    case PitchClass::D:
      root_name = "D";
      break;
    case PitchClass::Ds:
      root_name = "D#";
      break;
    case PitchClass::E:
      root_name = "E";
      break;
    case PitchClass::F:
      root_name = "F";
      break;
    case PitchClass::Fs:
      root_name = "F#";
      break;
    case PitchClass::G:
      root_name = "G";
      break;
    case PitchClass::Gs:
      root_name = "G#";
      break;
    case PitchClass::A:
      root_name = "A";
      break;
    case PitchClass::As:
      root_name = "A#";
      break;
    case PitchClass::B:
      root_name = "B";
      break;
  }

  return root_name + (mode == Mode::Minor ? "m" : "");
}

KeyAnalyzer::KeyAnalyzer(const Audio& audio, const KeyConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute chroma
  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;

  Chroma chroma = Chroma::compute(audio, chroma_config);

  // Get mean chroma
  auto mean_energy = chroma.mean_energy();
  mean_chroma_ = mean_energy;

  analyze();
}

KeyAnalyzer::KeyAnalyzer(const Chroma& chroma, const KeyConfig& config) : config_(config) {
  SONARE_CHECK(!chroma.empty(), ErrorCode::InvalidParameter);

  auto mean_energy = chroma.mean_energy();
  mean_chroma_ = mean_energy;

  analyze();
}

KeyAnalyzer::KeyAnalyzer(const std::array<float, 12>& mean_chroma, const KeyConfig& config)
    : mean_chroma_(mean_chroma), config_(config) {
  analyze();
}

void KeyAnalyzer::analyze() {
  candidates_.clear();
  candidates_.reserve(24);

  // Create default multiplicative boosts for enhanced key detection
  KeyProfileBoosts major_boosts;
  major_boosts.tonic = key_constants::kMajorTonicBoost;
  major_boosts.third = key_constants::kMajorThirdBoost;
  major_boosts.fifth = key_constants::kMajorFifthBoost;
  major_boosts.seventh = 1.0f;  // No boost for major seventh

  KeyProfileBoosts minor_boosts;
  minor_boosts.tonic = key_constants::kMinorTonicBoost;
  minor_boosts.third = key_constants::kMinorThirdBoost;
  minor_boosts.fifth = key_constants::kMinorFifthBoost;
  minor_boosts.seventh = key_constants::kMinorSeventhBoost;

  // Compute correlation with all 24 keys using boosted profiles
  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    // Major key with boosted profile
    auto major_profile = get_boosted_major_profile(pc, major_boosts, config_.profile_type);
    major_profile = normalize_profile(major_profile);
    float major_corr = profile_correlation(mean_chroma_, major_profile);

    KeyCandidate major_candidate;
    major_candidate.key.root = pc;
    major_candidate.key.mode = Mode::Major;
    major_candidate.key.confidence = 0.0f;  // Set later
    major_candidate.correlation = major_corr;
    candidates_.push_back(major_candidate);

    // Minor key with boosted profile
    auto minor_profile = get_boosted_minor_profile(pc, minor_boosts, config_.profile_type);
    minor_profile = normalize_profile(minor_profile);
    float minor_corr = profile_correlation(mean_chroma_, minor_profile);

    KeyCandidate minor_candidate;
    minor_candidate.key.root = pc;
    minor_candidate.key.mode = Mode::Minor;
    minor_candidate.key.confidence = 0.0f;
    minor_candidate.correlation = minor_corr;
    candidates_.push_back(minor_candidate);
  }

  // Sort by correlation (descending)
  std::sort(
      candidates_.begin(), candidates_.end(),
      [](const KeyCandidate& a, const KeyCandidate& b) { return a.correlation > b.correlation; });

  // Compute confidence scores
  // Confidence based on how much the best correlation stands out
  if (!candidates_.empty()) {
    float best_corr = candidates_[0].correlation;
    float second_corr = candidates_.size() > 1 ? candidates_[1].correlation : 0.0f;

    // Confidence is based on the gap between best and second-best
    float gap = best_corr - second_corr;

    // Normalize correlation to [0, 1] range
    // Correlation is in [-1, 1], so shift and scale
    float normalized_corr = (best_corr + 1.0f) / 2.0f;

    // Combine correlation strength and distinctiveness
    float distinctiveness = std::min(gap / 0.2f, 1.0f);  // Gap of 0.2 = full confidence
    float confidence = normalized_corr * 0.5f + distinctiveness * 0.5f;

    for (auto& candidate : candidates_) {
      float rel_corr = (candidate.correlation + 1.0f) / 2.0f;
      candidate.key.confidence = rel_corr;
    }

    candidates_[0].key.confidence = std::min(confidence, 1.0f);
    key_ = candidates_[0].key;
  } else {
    key_.root = PitchClass::C;
    key_.mode = Mode::Major;
    key_.confidence = 0.0f;
  }
}

std::vector<KeyCandidate> KeyAnalyzer::candidates(int top_n) const {
  int n = std::min(top_n, static_cast<int>(candidates_.size()));
  return std::vector<KeyCandidate>(candidates_.begin(), candidates_.begin() + n);
}

Key detect_key(const Audio& audio, const KeyConfig& config) {
  KeyAnalyzer analyzer(audio, config);
  return analyzer.key();
}

namespace {

/// @brief Check if a chord is diatonic in a given major key.
/// @param chord_root Chord root pitch class
/// @param chord_quality Chord quality
/// @param key_root Key root pitch class
/// @return true if chord is diatonic
bool is_diatonic_major(PitchClass chord_root, ChordQuality chord_quality, PitchClass key_root) {
  int interval = (static_cast<int>(chord_root) - static_cast<int>(key_root) + 12) % 12;

  // Diatonic chords in major key:
  // I (0, Major), ii (2, Minor), iii (4, Minor), IV (5, Major),
  // V (7, Major), vi (9, Minor), vii° (11, Diminished)
  switch (interval) {
    case 0:
      return chord_quality == ChordQuality::Major;
    case 2:
      return chord_quality == ChordQuality::Minor;
    case 4:
      return chord_quality == ChordQuality::Minor;
    case 5:
      return chord_quality == ChordQuality::Major;
    case 7:
      return chord_quality == ChordQuality::Major;
    case 9:
      return chord_quality == ChordQuality::Minor;
    case 11:
      return chord_quality == ChordQuality::Diminished ||
             chord_quality == ChordQuality::Minor;  // Often used as minor
    default:
      return false;
  }
}

}  // namespace

Key estimate_key_from_chords(const std::vector<Chord>& chords) {
  if (chords.empty()) {
    return Key{PitchClass::C, Mode::Major, 0.0f};
  }

  // Count weighted occurrences of each root
  std::array<float, 12> root_weights = {};
  float total_duration = 0.0f;

  for (const auto& chord : chords) {
    float duration = chord.duration();
    root_weights[static_cast<int>(chord.root)] += duration;
    total_duration += duration;
  }

  // Detect common cadences (V-I resolution at the end)
  PitchClass cadence_tonic = PitchClass::C;
  bool has_cadence = false;

  if (chords.size() >= 2) {
    const Chord& last = chords.back();
    const Chord& second_last = chords[chords.size() - 2];

    // Check for V-I cadence (perfect cadence)
    int interval = (static_cast<int>(last.root) - static_cast<int>(second_last.root) + 12) % 12;
    if (interval == 5 && second_last.quality == ChordQuality::Major &&
        last.quality == ChordQuality::Major) {
      // Second last is V, last is I
      cadence_tonic = last.root;
      has_cadence = true;
    }
  }

  // Check if first and last chords are the same (common for pop songs)
  bool first_last_same = false;
  if (chords.size() >= 2) {
    first_last_same = (chords.front().root == chords.back().root &&
                       chords.front().quality == chords.back().quality);
  }

  // Score each possible key by how many chords are diatonic
  float best_score = -1.0f;
  PitchClass best_root = PitchClass::C;
  Mode best_mode = Mode::Major;

  for (int key_idx = 0; key_idx < 12; ++key_idx) {
    PitchClass key_root = static_cast<PitchClass>(key_idx);

    // Score for major key
    float major_score = 0.0f;
    float major_tonic_score = 0.0f;

    for (const auto& chord : chords) {
      float duration = chord.duration();
      if (is_diatonic_major(chord.root, chord.quality, key_root)) {
        major_score += duration;

        // Bonus for tonic chord
        if (chord.root == key_root && chord.quality == ChordQuality::Major) {
          major_tonic_score += duration * 0.5f;
        }
        // Bonus for dominant chord
        int interval = (static_cast<int>(chord.root) - key_idx + 12) % 12;
        if (interval == 7 && chord.quality == ChordQuality::Major) {
          major_tonic_score += duration * 0.3f;
        }
      }
    }

    // Add bonus for cadence detection (V-I at end of song)
    float cadence_bonus = 0.0f;
    if (has_cadence && cadence_tonic == key_root) {
      cadence_bonus = total_duration * 0.4f;  // Strong bonus for V-I cadence
    }

    // Add bonus if first and last chords are the tonic
    float bookend_bonus = 0.0f;
    if (first_last_same && chords.front().root == key_root &&
        chords.front().quality == ChordQuality::Major) {
      bookend_bonus = total_duration * 0.3f;
    }

    // Add bonus if first chord is the tonic (common in pop)
    float first_chord_bonus = 0.0f;
    if (!chords.empty() && chords.front().root == key_root &&
        chords.front().quality == ChordQuality::Major) {
      first_chord_bonus = total_duration * 0.15f;
    }

    float total_major_score =
        major_score + major_tonic_score + cadence_bonus + bookend_bonus + first_chord_bonus;
    if (total_major_score > best_score) {
      best_score = total_major_score;
      best_root = key_root;
      best_mode = Mode::Major;
    }

    // Score for relative minor (3 semitones down from major)
    // Minor key: i (Minor), ii° (Dim), III (Major), iv (Minor),
    //            v (Minor) or V (Major), VI (Major), VII (Major)
    PitchClass minor_root = static_cast<PitchClass>((key_idx + 9) % 12);  // Relative minor
    float minor_score = 0.0f;
    float minor_tonic_score = 0.0f;

    for (const auto& chord : chords) {
      float duration = chord.duration();
      int interval = (static_cast<int>(chord.root) - static_cast<int>(minor_root) + 12) % 12;

      bool is_diatonic_minor = false;
      switch (interval) {
        case 0:
          is_diatonic_minor = (chord.quality == ChordQuality::Minor);
          if (is_diatonic_minor) minor_tonic_score += duration * 0.5f;
          break;
        case 2:
          is_diatonic_minor = (chord.quality == ChordQuality::Diminished ||
                               chord.quality == ChordQuality::Minor);
          break;
        case 3:
          is_diatonic_minor = (chord.quality == ChordQuality::Major);
          break;
        case 5:
          is_diatonic_minor = (chord.quality == ChordQuality::Minor);
          break;
        case 7:
          is_diatonic_minor = (chord.quality == ChordQuality::Minor ||
                               chord.quality == ChordQuality::Major);  // Harmonic minor
          if (chord.quality == ChordQuality::Major) minor_tonic_score += duration * 0.3f;
          break;
        case 8:
          is_diatonic_minor = (chord.quality == ChordQuality::Major);
          break;
        case 10:
          is_diatonic_minor = (chord.quality == ChordQuality::Major);
          break;
        default:
          break;
      }

      if (is_diatonic_minor) {
        minor_score += duration;
      }
    }

    float total_minor_score = minor_score + minor_tonic_score;
    if (total_minor_score > best_score) {
      best_score = total_minor_score;
      best_root = minor_root;
      best_mode = Mode::Minor;
    }
  }

  // Calculate confidence based on how much of the progression is diatonic
  float confidence = (total_duration > 0.0f) ? (best_score / total_duration) : 0.0f;
  confidence = std::min(confidence, 1.0f);

  return Key{best_root, best_mode, confidence};
}

Key refine_key_with_chords(const Key& chroma_key, const std::vector<Chord>& chords) {
  if (chords.empty()) {
    return chroma_key;
  }

  Key chord_key = estimate_key_from_chords(chords);

  // If both methods agree, use that key with boosted confidence
  if (chroma_key.root == chord_key.root && chroma_key.mode == chord_key.mode) {
    float combined_confidence = std::min(
        (chroma_key.confidence + chord_key.confidence) / 2.0f + 0.1f, 1.0f);
    return Key{chroma_key.root, chroma_key.mode, combined_confidence};
  }

  // If chord-based detection has high confidence, prefer it
  // This helps when chroma-based detection is confused by strong subdominant
  if (chord_key.confidence > 0.7f && chord_key.confidence > chroma_key.confidence) {
    return chord_key;
  }

  // Check if one key is the relative major/minor of the other
  int interval = (static_cast<int>(chord_key.root) - static_cast<int>(chroma_key.root) + 12) % 12;
  bool are_relative = (interval == 3 || interval == 9);

  if (are_relative) {
    // For relative keys, prefer the one with higher confidence
    if (chord_key.confidence > chroma_key.confidence) {
      return chord_key;
    }
    return chroma_key;
  }

  // Check if chroma detected subdominant (F major) when actual key is tonic (C major)
  // This happens often with I-V-vi-IV progressions
  if (chroma_key.mode == Mode::Major && chord_key.mode == Mode::Major) {
    // If chord_key is a perfect fifth above chroma_key, chroma likely detected IV
    if (interval == 7) {
      // Chord detection says tonic is a fifth above what chroma detected
      // This suggests chroma detected the subdominant as tonic
      return chord_key;
    }
  }

  // Default: use chroma key if its confidence is reasonable
  if (chroma_key.confidence > 0.6f) {
    return chroma_key;
  }

  // Otherwise prefer chord-based detection
  return chord_key;
}

}  // namespace sonare
