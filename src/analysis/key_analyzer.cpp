#include "analysis/key_analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>

#include "analysis/chord_analyzer.h"
#include "effects/hpss.h"
#include "feature/spectral.h"
#include "filters/iir.h"
#include "util/exception.h"

namespace sonare {

namespace {

constexpr float kHighpassFallbackCurrentConfidenceMax = 0.75f;
constexpr float kHighpassFallbackConfidenceMin = 0.50f;

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::vector<Mode> candidate_modes_for_config(const KeyConfig& config) {
  return config.modes.empty() ? std::vector<Mode>{Mode::Major, Mode::Minor} : config.modes;
}

struct KeyProfileMatch {
  KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler;
  float score = -std::numeric_limits<float>::infinity();
};

std::vector<KeyCandidate> build_key_candidates(const std::array<float, 12>& mean_chroma,
                                               const std::vector<Mode>& candidate_modes,
                                               KeyProfileType profile_type) {
  std::vector<KeyCandidate> candidates;
  candidates.reserve(12 * candidate_modes.size());

  KeyProfileBoosts major_boosts;
  major_boosts.tonic = key_constants::kMajorTonicBoost;
  major_boosts.third = key_constants::kMajorThirdBoost;
  major_boosts.fifth = key_constants::kMajorFifthBoost;
  major_boosts.seventh = 1.0f;

  KeyProfileBoosts minor_boosts;
  minor_boosts.tonic = key_constants::kMinorTonicBoost;
  minor_boosts.third = key_constants::kMinorThirdBoost;
  minor_boosts.fifth = key_constants::kMinorFifthBoost;
  minor_boosts.seventh = key_constants::kMinorSeventhBoost;

  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    for (Mode mode : candidate_modes) {
      const KeyProfileBoosts& boosts = (mode == Mode::Major) ? major_boosts : minor_boosts;
      // profile_correlation is Pearson, which is invariant to positive scaling and
      // offset of either operand. normalize_profile only rescales the profile by a
      // positive constant (1/sum), so it has no effect on the resulting correlation
      // and is intentionally omitted here.
      auto profile = get_boosted_mode_profile(pc, mode, boosts, profile_type);

      KeyCandidate candidate;
      candidate.key.root = pc;
      candidate.key.mode = mode;
      candidate.key.confidence = 0.0f;
      candidate.correlation = profile_correlation(mean_chroma, profile);
      candidates.push_back(candidate);
    }
  }

  // stable_sort so that equal correlations keep insertion order (root 0 = C,
  // Major first). On silence every candidate correlates to 0, and an unstable
  // sort would pick a platform-dependent winner (libstdc++ vs libc++); the
  // stable order yields the documented C-major fallback everywhere.
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const KeyCandidate& a, const KeyCandidate& b) { return a.correlation > b.correlation; });
  return candidates;
}

float profile_candidate_score(const std::vector<KeyCandidate>& candidates) {
  if (candidates.empty()) {
    return -std::numeric_limits<float>::infinity();
  }

  const float best_corr = candidates[0].correlation;
  const float second_corr = candidates.size() > 1 ? candidates[1].correlation : 0.0f;
  const float gap = best_corr - second_corr;
  return best_corr + 0.35f * gap;
}

KeyProfileMatch select_auto_profile_type(const std::array<float, 12>& mean_chroma,
                                         const KeyConfig& config,
                                         const std::vector<Mode>& candidate_modes) {
  const KeyProfileType profile_types[] = {KeyProfileType::KrumhanslSchmuckler,
                                          KeyProfileType::Shaath,
                                          KeyProfileType::FaraldoEDMT,
                                          KeyProfileType::FaraldoEDMA,
                                          KeyProfileType::FaraldoEDMM,
                                          KeyProfileType::BellmanBudge,
                                          KeyProfileType::Temperley};

  KeyProfileMatch ks_match;
  KeyProfileMatch best_match;
  for (KeyProfileType profile_type : profile_types) {
    const auto candidates = build_key_candidates(mean_chroma, candidate_modes, profile_type);
    KeyProfileMatch match;
    match.profile_type = profile_type;
    match.score = profile_candidate_score(candidates);
    if (profile_type == KeyProfileType::KrumhanslSchmuckler) {
      ks_match = match;
      best_match = match;
      continue;
    }
    if (match.score > best_match.score) {
      best_match = match;
    }
  }

  // Keep the historical KS behavior unless another profile gives clearly
  // stronger chroma evidence. This avoids turning uncertain MIR cases into
  // de-facto golden labels while still enabling auto profile selection.
  const float switch_margin = config.modes.size() > 2 ? 0.015f : 0.025f;
  if (best_match.profile_type != KeyProfileType::KrumhanslSchmuckler &&
      best_match.score >= ks_match.score + switch_margin) {
    return best_match;
  }
  return ks_match;
}

KeyProfileType resolve_profile_type(const KeyConfig& config) {
  if (config.profile_type != KeyProfileType::KrumhanslSchmuckler) {
    return config.profile_type;
  }

  const std::string hint = lowercase_ascii(config.genre_hint);
  if (hint == "edm" || hint == "electronic" || hint == "dance") {
    return KeyProfileType::FaraldoEDMA;
  }
  if (hint == "pop") {
    return KeyProfileType::Shaath;
  }
  if (hint == "classical") {
    return KeyProfileType::BellmanBudge;
  }
  if (hint == "jazz") {
    return KeyProfileType::Temperley;
  }

  return KeyProfileType::KrumhanslSchmuckler;
}

bool uses_auto_audio_candidates(const KeyConfig& config) {
  return lowercase_ascii(config.genre_hint) == "auto" && !config.use_hpss &&
         !config.loudness_weighted;
}

std::array<float, 12> compute_mean_chroma_for_audio(const Audio& audio, const KeyConfig& config,
                                                    bool use_hpss, bool loudness_weighted) {
  SONARE_CHECK(config.high_pass_hz >= 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.high_pass_hz < static_cast<float>(audio.sample_rate()) * 0.5f,
               ErrorCode::InvalidParameter);

  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;

  StftConfig stft_config = chroma_config.to_stft_config();
  Audio filtered_audio = audio;
  if (config.high_pass_hz > 0.0f) {
    const auto cascade = highpass_coeffs_4th(config.high_pass_hz, audio.sample_rate());
    filtered_audio = Audio::from_vector(apply_cascade_filtfilt(audio.data(), audio.size(), cascade),
                                        audio.sample_rate());
  }

  Audio analysis_audio =
      use_hpss ? harmonic(filtered_audio, HpssConfig(), stft_config) : filtered_audio;
  Chroma chroma = Chroma::compute(analysis_audio, chroma_config);

  if (loudness_weighted) {
    return chroma.weighted_mean_energy(rms_energy(analysis_audio, config.n_fft, config.hop_length));
  }
  return chroma.mean_energy();
}

float candidate_selection_score(const KeyAnalyzer& analyzer) { return analyzer.confidence(); }

bool should_use_harmonic_highpass_fallback(const KeyAnalyzer& current,
                                           const KeyAnalyzer& fallback) {
  if (current.key().root == fallback.key().root && current.key().mode == fallback.key().mode) {
    return false;
  }

  return current.confidence() <= kHighpassFallbackCurrentConfidenceMax &&
         fallback.confidence() >= kHighpassFallbackConfidenceMin;
}

}  // namespace

std::string Key::to_string() const {
  return std::string(pitch_class_name(root)) + " " + mode_name(mode);
}

std::string Key::to_short_string() const {
  if (mode == Mode::Major) {
    return std::string(pitch_class_name(root));
  }
  if (mode == Mode::Minor) {
    return std::string(pitch_class_name(root)) + "m";
  }
  return std::string(pitch_class_name(root)) + " " + mode_name(mode);
}

KeyAnalyzer::KeyAnalyzer(const Audio& audio, const KeyConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  if (uses_auto_audio_candidates(config)) {
    struct AudioCandidate {
      bool use_hpss;
      bool loudness_weighted;
      float selection_bias;
    };
    const AudioCandidate audio_candidates[] = {
        {false, false, 0.0f},
        {true, false, 0.0f},
        {false, true, 0.0f},
        // Harmonic loudness-weighted chroma is less overconfident on dense mixes,
        // so let it win close auto-profile decisions without forcing it globally.
        {true, true, 0.17f},
    };

    bool has_best = false;
    float best_score = -std::numeric_limits<float>::infinity();
    KeyAnalyzer best_analyzer(std::array<float, 12>{}, config);

    for (const auto& candidate : audio_candidates) {
      KeyConfig candidate_config = config;
      candidate_config.genre_hint = "auto";
      candidate_config.use_hpss = candidate.use_hpss;
      candidate_config.loudness_weighted = candidate.loudness_weighted;

      auto candidate_mean = compute_mean_chroma_for_audio(audio, config, candidate.use_hpss,
                                                          candidate.loudness_weighted);
      KeyAnalyzer analyzer(candidate_mean, candidate_config);
      const float score = candidate_selection_score(analyzer) + candidate.selection_bias;
      if (!has_best || score > best_score) {
        has_best = true;
        best_score = score;
        best_analyzer = analyzer;
      }
    }

    if (best_analyzer.confidence() <= kHighpassFallbackCurrentConfidenceMax) {
      KeyConfig fallback_config = config;
      fallback_config.genre_hint = "";
      fallback_config.use_hpss = true;
      fallback_config.loudness_weighted = false;
      fallback_config.high_pass_hz =
          fallback_config.high_pass_hz > 0.0f ? fallback_config.high_pass_hz : 60.0f;
      fallback_config.profile_type = KeyProfileType::KrumhanslSchmuckler;
      const auto fallback_mean = compute_mean_chroma_for_audio(audio, fallback_config, true, false);
      KeyAnalyzer fallback_analyzer(fallback_mean, fallback_config);
      if (should_use_harmonic_highpass_fallback(best_analyzer, fallback_analyzer)) {
        best_analyzer = fallback_analyzer;
      }
    }

    mean_chroma_ = best_analyzer.mean_chroma_;
    candidates_ = best_analyzer.candidates_;
    key_ = best_analyzer.key_;
    return;
  }

  mean_chroma_ =
      compute_mean_chroma_for_audio(audio, config, config.use_hpss, config.loudness_weighted);
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

  const std::vector<Mode> candidate_modes = candidate_modes_for_config(config_);
  KeyProfileType profile_type = resolve_profile_type(config_);
  if (config_.profile_type == KeyProfileType::KrumhanslSchmuckler &&
      lowercase_ascii(config_.genre_hint) == "auto") {
    profile_type = select_auto_profile_type(mean_chroma_, config_, candidate_modes).profile_type;
  }

  // Compute correlation with all requested key/mode candidates using boosted profiles.
  candidates_ = build_key_candidates(mean_chroma_, candidate_modes, profile_type);

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

    // Runner-up candidates report a raw-correlation confidence rescaled to [0, 1].
    for (auto& candidate : candidates_) {
      float rel_corr = (candidate.correlation + 1.0f) / 2.0f;
      candidate.key.confidence = rel_corr;
    }

    // Only the best candidate (== key()) carries the gap-based confidence that
    // blends correlation strength with distinctiveness from the runner-up. This is
    // intentionally on a different scale than the runner-up confidences above, so
    // consumers should not compare candidates_[0].confidence against the rest as a
    // ranking metric (candidates are already ordered by raw correlation).
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

/// @brief Reduce an extended/seventh chord quality to its underlying triad base.
///
/// Diatonic scoring only models the three triad qualities (Major/Minor/Diminished),
/// so a Imaj7 or V7 would otherwise score as non-diatonic and deflate the key
/// estimate on seventh-heavy jazz/pop material. Map each extended quality onto the
/// triad it is built on; qualities without a clear triad base (Sus*, Unknown,
/// Augmented) are passed through unchanged so they remain non-diatonic.
ChordQuality reduce_to_triad(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Dominant7:
    case ChordQuality::Major7:
    case ChordQuality::Add9:
    case ChordQuality::Major9:
    case ChordQuality::Dominant9:
      return ChordQuality::Major;
    case ChordQuality::Minor7:
    case ChordQuality::MinorAdd9:
      return ChordQuality::Minor;
    case ChordQuality::Dim7:
    case ChordQuality::HalfDim7:
      return ChordQuality::Diminished;
    default:
      return quality;
  }
}

/// @brief Check if a chord is diatonic in a given major key.
/// @param chord_root Chord root pitch class
/// @param chord_quality Chord quality
/// @param key_root Key root pitch class
/// @return true if chord is diatonic
bool is_diatonic_major(PitchClass chord_root, ChordQuality chord_quality, PitchClass key_root) {
  chord_quality = reduce_to_triad(chord_quality);
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
    float duration = std::max(0.0f, chord.duration());
    root_weights[static_cast<int>(chord.root)] += duration;
    total_duration += duration;
  }

  // Detect common cadences (V-I resolution at the end)
  PitchClass cadence_tonic = PitchClass::C;
  bool has_cadence = false;

  if (chords.size() >= 2) {
    const Chord& last = chords.back();
    const Chord& second_last = chords[chords.size() - 2];

    // Check for V-I cadence (perfect cadence). Reduce extended qualities to their
    // triad base so a V7-I (the canonical cadence) is recognized like a V-I.
    int interval = (static_cast<int>(last.root) - static_cast<int>(second_last.root) + 12) % 12;
    if (interval == 5 && reduce_to_triad(second_last.quality) == ChordQuality::Major &&
        reduce_to_triad(last.quality) == ChordQuality::Major) {
      // Second last is V, last is I
      cadence_tonic = last.root;
      has_cadence = true;
    }
  }

  // Detect a minor perfect cadence (V-i): a major-quality dominant resolving to
  // a minor-quality tonic. Tracked separately from the major V-I above so the
  // minor branch can receive the same cadence bonus the major branch gets,
  // rather than systematically losing close ties to its relative major.
  PitchClass minor_cadence_tonic = PitchClass::C;
  bool has_minor_cadence = false;
  if (chords.size() >= 2) {
    const Chord& last = chords.back();
    const Chord& second_last = chords[chords.size() - 2];
    int interval = (static_cast<int>(last.root) - static_cast<int>(second_last.root) + 12) % 12;
    if (interval == 5 && reduce_to_triad(second_last.quality) == ChordQuality::Major &&
        reduce_to_triad(last.quality) == ChordQuality::Minor) {
      minor_cadence_tonic = last.root;
      has_minor_cadence = true;
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
      float duration = std::max(0.0f, chord.duration());
      ChordQuality triad = reduce_to_triad(chord.quality);
      if (is_diatonic_major(chord.root, chord.quality, key_root)) {
        major_score += duration;

        // Bonus for tonic chord
        if (chord.root == key_root && triad == ChordQuality::Major) {
          major_tonic_score += duration * 0.5f;
        }
        // Bonus for dominant chord
        int interval = (static_cast<int>(chord.root) - key_idx + 12) % 12;
        if (interval == 7 && triad == ChordQuality::Major) {
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
        reduce_to_triad(chords.front().quality) == ChordQuality::Major) {
      bookend_bonus = total_duration * 0.3f;
    }

    // Add bonus if first chord is the tonic (common in pop)
    float first_chord_bonus = 0.0f;
    if (!chords.empty() && chords.front().root == key_root &&
        reduce_to_triad(chords.front().quality) == ChordQuality::Major) {
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
      float duration = std::max(0.0f, chord.duration());
      int interval = (static_cast<int>(chord.root) - static_cast<int>(minor_root) + 12) % 12;
      ChordQuality triad = reduce_to_triad(chord.quality);

      bool is_diatonic_minor = false;
      switch (interval) {
        case 0:
          is_diatonic_minor = (triad == ChordQuality::Minor);
          if (is_diatonic_minor) minor_tonic_score += duration * 0.5f;
          break;
        case 2:
          is_diatonic_minor = (triad == ChordQuality::Diminished || triad == ChordQuality::Minor);
          break;
        case 3:
          is_diatonic_minor = (triad == ChordQuality::Major);
          break;
        case 5:
          is_diatonic_minor = (triad == ChordQuality::Minor);
          break;
        case 7:
          is_diatonic_minor =
              (triad == ChordQuality::Minor || triad == ChordQuality::Major);  // Harmonic minor
          if (triad == ChordQuality::Major) minor_tonic_score += duration * 0.3f;
          break;
        case 8:
          is_diatonic_minor = (triad == ChordQuality::Major);
          break;
        case 10:
          is_diatonic_minor = (triad == ChordQuality::Major);
          break;
        default:
          break;
      }

      if (is_diatonic_minor) {
        minor_score += duration;
      }
    }

    // Apply the same cadence/bookend/first-chord bonuses as the major branch,
    // with minor-appropriate tonic-quality checks (a minor tonic is a Minor
    // triad, not Major). Without these the major branch wins close ties on
    // minor material that happens to share diatonic chords with its relative
    // major (analysis#5).
    float minor_cadence_bonus = 0.0f;
    if (has_minor_cadence && minor_cadence_tonic == minor_root) {
      minor_cadence_bonus = total_duration * 0.4f;
    }

    float minor_bookend_bonus = 0.0f;
    if (first_last_same && chords.front().root == minor_root &&
        reduce_to_triad(chords.front().quality) == ChordQuality::Minor) {
      minor_bookend_bonus = total_duration * 0.3f;
    }

    float minor_first_chord_bonus = 0.0f;
    if (!chords.empty() && chords.front().root == minor_root &&
        reduce_to_triad(chords.front().quality) == ChordQuality::Minor) {
      minor_first_chord_bonus = total_duration * 0.15f;
    }

    float total_minor_score = minor_score + minor_tonic_score + minor_cadence_bonus +
                              minor_bookend_bonus + minor_first_chord_bonus;
    if (total_minor_score > best_score) {
      best_score = total_minor_score;
      best_root = minor_root;
      best_mode = Mode::Minor;
    }
  }

  if (best_score <= 0.0f) {
    const ChordQuality triad = reduce_to_triad(chords.front().quality);
    const Mode fallback_mode = triad == ChordQuality::Minor ? Mode::Minor : Mode::Major;
    return Key{chords.front().root, fallback_mode, 0.0f};
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
    float combined_confidence =
        std::min((chroma_key.confidence + chord_key.confidence) / 2.0f + 0.1f, 1.0f);
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
