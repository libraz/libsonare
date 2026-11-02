#include "analysis/key_analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <limits>

#include "analysis/chord_analyzer.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "util/exception.h"
#include "util/insertion_sort.h"

namespace sonare {

namespace {

/// Chroma posterior, as a share of the chroma winner's, a chord-derived key needs before the chords
/// may choose it: within a factor of four, a correlation margin of about 0.11.
constexpr float kChordOverrideMinPosteriorRatio = 0.25f;

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
  insertion_sort(
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
  if (config.profile_type.has_value()) {
    return *config.profile_type;
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

void validate_chroma_config(const Audio& audio, const KeyConfig& config) {
  SONARE_CHECK_MSG(!config.tuning || is_valid_chroma_tuning(*config.tuning),
                   ErrorCode::InvalidParameter,
                   "KeyConfig: tuning must be finite and in [-0.5, 0.5)");
  SONARE_CHECK(config.high_pass_hz >= 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.high_pass_hz < static_cast<float>(audio.sample_rate()) * 0.5f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0 &&
                   config.hop_length <= INT_MAX / (2 * chord_constants::kChordChromaHopMultiplier),
               ErrorCode::InvalidParameter);
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

std::string key_name(int root, bool minor, bool short_form) {
  if (root < 0 || root > 11) {
    return "";
  }
  Key key{};
  key.root = static_cast<PitchClass>(root);
  key.mode = minor ? Mode::Minor : Mode::Major;
  return short_form ? key.to_short_string() : key.to_string();
}

KeyAnalyzer::KeyAnalyzer(const Audio& audio, const KeyConfig& requested) : config_(requested) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  validate_chroma_config(audio, config_);
  if (!config_.tuning) config_.tuning = measure_tuning(audio);

  ChordChromaConfig chroma_config;
  chroma_config.hop_length = config_.hop_length * chord_constants::kChordChromaHopMultiplier;
  chroma_config.tuning = *config_.tuning;
  chroma_config.use_hpss = config_.use_hpss;
  chroma_config.hpss_n_fft = config_.n_fft;
  chroma_config.hpss_hop_length = config_.hop_length;
  chroma_config.highpass_hz = config_.high_pass_hz;
  // The bass blend weighs every chord root alike and pulls the key towards the subdominant.
  chroma_config.bass_weighted = false;
  const Chroma chroma = chord_chroma(audio, chroma_config);

  // Frames of twice the chroma hop on the raw input; the pooling reads the shorter frame count.
  mean_chroma_ = config_.loudness_weighted
                     ? chroma.weighted_mean_energy(rms_energy(audio, 2 * chroma_config.hop_length,
                                                              chroma_config.hop_length))
                     : chroma.mean_energy();
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
  if (!config_.profile_type.has_value() && lowercase_ascii(config_.genre_hint) == "auto") {
    profile_type = select_auto_profile_type(mean_chroma_, config_, candidate_modes).profile_type;
  }

  // Compute correlation with all requested key/mode candidates using boosted profiles.
  candidates_ = build_key_candidates(mean_chroma_, candidate_modes, profile_type);

  if (candidates_.empty()) {
    key_.root = PitchClass::C;
    key_.mode = Mode::Major;
    key_.confidence = 0.0f;
    evidence_score_ = 0.0f;
    return;
  }

  // Evidence score: how strong the winning correlation is, blended with its
  // margin over the runner-up. It must not depend on how many candidates
  // were scored — a modal search over 84 candidates would otherwise look less
  // certain than a major/minor one over 24 on the very same audio.
  const float best_corr = candidates_[0].correlation;
  const float second_corr = candidates_.size() > 1 ? candidates_[1].correlation : 0.0f;
  const float gap = best_corr - second_corr;
  const float normalized_corr = (best_corr + 1.0f) / 2.0f;
  const float distinctiveness = std::min(gap / key_constants::kFullDistinctivenessGap, 1.0f);
  evidence_score_ = std::min(normalized_corr * 0.5f + distinctiveness * 0.5f, 1.0f);

  assign_posterior_confidences();
  key_ = candidates_[0].key;
}

void KeyAnalyzer::assign_posterior_confidences() {
  // A softmax over the candidate correlations. The reported confidence is then
  // a share of one analysis's total belief rather than an unbounded score, so
  // the number falls when a runner-up is close instead of saturating at 1.0 --
  // which is what a relative major and minor splitting the same chroma should
  // look like, and what the old formula could not express because its
  // distinctiveness term clipped at a fixed gap.
  //
  // Correlations are subtracted from the maximum before exponentiating, which
  // leaves the distribution unchanged and keeps exp() away from overflow.
  const float best_corr = candidates_[0].correlation;
  double total = 0.0;
  std::vector<double> weights(candidates_.size());
  for (size_t i = 0; i < candidates_.size(); ++i) {
    const double scaled = (static_cast<double>(candidates_[i].correlation) - best_corr) /
                          key_constants::kConfidenceTemperature;
    weights[i] = std::exp(scaled);
    total += weights[i];
  }
  // The best candidate always contributes exp(0), so the total is at least 1;
  // the guard is here only so a non-finite correlation cannot produce a NaN
  // confidence rather than because the branch is expected to run.
  if (!(total > 0.0) || !std::isfinite(total)) {
    for (auto& candidate : candidates_) {
      candidate.key.confidence = 0.0f;
    }
    return;
  }
  // On silence every candidate correlates to 0 and the distribution comes out
  // uniform: with 24 candidates each reports about 4%, which is the honest
  // reading of "no key stands out" rather than a mid-range score.
  for (size_t i = 0; i < candidates_.size(); ++i) {
    candidates_[i].key.confidence = static_cast<float>(weights[i] / total);
  }
}

std::vector<KeyCandidate> KeyAnalyzer::candidates(int top_n) const {
  const int n = std::clamp(top_n, 0, static_cast<int>(candidates_.size()));
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

namespace {

/// The chords that name a harmony: N.C. (Unknown, with a placeholder root)
/// carries no evidence for or against any key.
std::vector<Chord> recognized_chords(const std::vector<Chord>& chords) {
  std::vector<Chord> out;
  out.reserve(chords.size());
  for (const Chord& chord : chords) {
    if (chord.quality != ChordQuality::Unknown) out.push_back(chord);
  }
  return out;
}

/// Chord-progression score of every key, indexed [mode == Minor][root]; one scale for all 24.
using ChordKeyScores = std::array<std::array<float, 12>, 2>;

Key estimate_key_from_recognized_chords(const std::vector<Chord>& chords,
                                        ChordKeyScores* scores = nullptr);

}  // namespace

Key estimate_key_from_chords(const std::vector<Chord>& all_chords) {
  return estimate_key_from_recognized_chords(recognized_chords(all_chords));
}

namespace {

Key estimate_key_from_recognized_chords(const std::vector<Chord>& chords, ChordKeyScores* scores) {
  if (scores != nullptr) {
    for (auto& row : *scores) row.fill(0.0f);
  }
  if (chords.empty()) {
    return Key{PitchClass::C, Mode::Major, 0.0f};
  }

  float total_duration = 0.0f;
  for (const auto& chord : chords) {
    total_duration += std::max(0.0f, chord.duration());
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
    if (scores != nullptr) (*scores)[0][static_cast<size_t>(key_idx)] = total_major_score;
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
    if (scores != nullptr) (*scores)[1][static_cast<size_t>(minor_root)] = total_minor_score;
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

}  // namespace

float key_posterior(const std::vector<KeyCandidate>& candidates, const Key& key) {
  for (const KeyCandidate& candidate : candidates) {
    if (candidate.key.root == key.root && candidate.key.mode == key.mode) {
      return candidate.key.confidence;
    }
  }
  return 0.0f;
}

Key refine_key_with_chords(const std::vector<KeyCandidate>& chroma_candidates,
                           const std::vector<Chord>& all_chords) {
  if (chroma_candidates.empty()) {
    return Key{PitchClass::C, Mode::Major, 0.0f};
  }
  const Key chroma_key = std::max_element(chroma_candidates.begin(), chroma_candidates.end(),
                                          [](const KeyCandidate& a, const KeyCandidate& b) {
                                            return a.key.confidence < b.key.confidence;
                                          })
                             ->key;

  const std::vector<Chord> chords = recognized_chords(all_chords);
  if (chords.empty()) {
    return chroma_key;
  }
  ChordKeyScores scores{};
  Key chord_key = estimate_key_from_recognized_chords(chords, &scores);
  if (chord_key.root == chroma_key.root && chord_key.mode == chroma_key.mode) {
    return chroma_key;
  }

  // The chords may only choose among keys the chroma leaves ambiguous, and between the two they are
  // compared on the chord score alone.
  const float chord_posterior = key_posterior(chroma_candidates, chord_key);
  if (chord_posterior < kChordOverrideMinPosteriorRatio * chroma_key.confidence) {
    return chroma_key;
  }
  const auto chord_score = [&](const Key& key) {
    return scores[key.mode == Mode::Minor ? 1 : 0][static_cast<size_t>(key.root)];
  };
  if (chord_score(chord_key) > chord_score(chroma_key)) {
    chord_key.confidence = chord_posterior;
    return chord_key;
  }
  return chroma_key;
}

}  // namespace sonare
