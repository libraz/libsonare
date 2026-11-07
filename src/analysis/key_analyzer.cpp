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
constexpr int kRelativeKeyMinTonicRuns = 3;
constexpr int kRelativeKeyMinChordRoots = 3;
constexpr float kRelativeKeyMinTemplateConfidence = 0.8f;
constexpr float kDominantChordScoreWeight = 0.3f;

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

/// Repeated, well-matched tonic returns may resolve a chroma-ambiguous relative major/minor pair.
/// Adjacent repeats are one sustained run, so splitting a chord or placing N.C. between repeats
/// cannot manufacture tonic recurrences.
bool has_recurrent_relative_tonic(const Key& proposed, const Key& chroma_key,
                                  const std::vector<Chord>& all_chords,
                                  const std::vector<KeyCandidate>& chroma_candidates) {
  if ((proposed.mode != Mode::Major && proposed.mode != Mode::Minor) ||
      (chroma_key.mode != Mode::Major && chroma_key.mode != Mode::Minor) ||
      proposed.mode == chroma_key.mode) {
    return false;
  }

  const Key& minor = proposed.mode == Mode::Minor ? proposed : chroma_key;
  const Key& major = proposed.mode == Mode::Major ? proposed : chroma_key;
  if (static_cast<int>(major.root) != (static_cast<int>(minor.root) + 3) % 12) return false;

  bool proposed_was_scored = false;
  for (const KeyCandidate& candidate : chroma_candidates) {
    if (candidate.key.root == proposed.root && candidate.key.mode == proposed.mode) {
      proposed_was_scored = true;
      break;
    }
  }
  if (!proposed_was_scored) return false;

  struct HarmonicRun {
    PitchClass root;
    ChordQuality quality;
    double duration;
    double confidence_mass;
  };
  std::vector<HarmonicRun> runs;
  std::array<bool, 12> roots{};
  double total_duration = 0.0;
  double total_confidence_mass = 0.0;
  for (const Chord& chord : all_chords) {
    const double duration = static_cast<double>(chord.duration());
    if (chord.quality == ChordQuality::Unknown || !std::isfinite(duration) || duration <= 0.0) {
      continue;
    }
    if (!std::isfinite(chord.confidence)) return false;
    const ChordQuality quality = reduce_to_triad(chord.quality);
    const double confidence_mass = duration * chord.confidence;
    total_duration += duration;
    total_confidence_mass += confidence_mass;
    roots[static_cast<size_t>(chord.root)] = true;
    if (!runs.empty() && runs.back().root == chord.root && runs.back().quality == quality) {
      runs.back().duration += duration;
      runs.back().confidence_mass += confidence_mass;
    } else {
      runs.push_back({chord.root, quality, duration, confidence_mass});
    }
  }
  if (runs.empty() || total_duration <= 0.0) return false;

  int distinct_roots = 0;
  for (bool present : roots) distinct_roots += present ? 1 : 0;

  const ChordQuality tonic_quality =
      proposed.mode == Mode::Minor ? ChordQuality::Minor : ChordQuality::Major;

  std::vector<double> run_durations;
  run_durations.reserve(runs.size());
  for (const HarmonicRun& run : runs) run_durations.push_back(run.duration);
  std::sort(run_durations.begin(), run_durations.end());
  const size_t middle = run_durations.size() / 2;
  const double median_duration = run_durations.size() % 2 == 0
                                     ? 0.5 * (run_durations[middle - 1] + run_durations[middle])
                                     : run_durations[middle];
  if (runs.front().duration < median_duration) return false;

  const auto is_strong_run = [](const HarmonicRun& run) {
    return run.confidence_mass / run.duration >= kRelativeKeyMinTemplateConfidence;
  };
  const bool opening_tonic = runs.front().root == proposed.root &&
                             runs.front().quality == tonic_quality && is_strong_run(runs.front());
  double tonic_duration = 0.0;
  double tonic_confidence_mass = 0.0;
  for (const HarmonicRun& run : runs) {
    if (run.root == proposed.root && run.quality == tonic_quality) {
      tonic_duration += run.duration;
      tonic_confidence_mass += run.confidence_mass;
    }
  }
  int reliable_tonic_runs = 0;
  int distinct_reliable_roots = 0;
  std::array<bool, 12> reliable_roots{};
  bool has_previous_reliable_run = false;
  PitchClass previous_reliable_root = PitchClass::C;
  ChordQuality previous_reliable_quality = ChordQuality::Unknown;
  for (const HarmonicRun& run : runs) {
    if (!is_strong_run(run)) continue;
    reliable_roots[static_cast<size_t>(run.root)] = true;
    if (has_previous_reliable_run && previous_reliable_root == run.root &&
        previous_reliable_quality == run.quality) {
      continue;
    }
    has_previous_reliable_run = true;
    previous_reliable_root = run.root;
    previous_reliable_quality = run.quality;
    if (run.root == proposed.root && run.quality == tonic_quality) ++reliable_tonic_runs;
  }
  for (bool present : reliable_roots) distinct_reliable_roots += present ? 1 : 0;

  if (opening_tonic && tonic_duration > 0.0 &&
      tonic_confidence_mass / tonic_duration >= kRelativeKeyMinTemplateConfidence &&
      reliable_tonic_runs >= kRelativeKeyMinTonicRuns &&
      distinct_reliable_roots >= kRelativeKeyMinChordRoots) {
    return true;
  }

  if (proposed.mode != Mode::Minor || runs.size() < 2 ||
      total_confidence_mass / total_duration < kRelativeKeyMinTemplateConfidence ||
      distinct_roots < kRelativeKeyMinChordRoots) {
    return false;
  }
  const auto minor_v_root = static_cast<PitchClass>((static_cast<int>(proposed.root) + 7) % 12);
  if (runs[0].root != minor_v_root || runs[0].quality != ChordQuality::Minor ||
      !is_strong_run(runs[0]) || runs[1].root != proposed.root ||
      runs[1].quality != ChordQuality::Minor || !is_strong_run(runs[1])) {
    return false;
  }

  int strong_v_i_runs = 0;
  for (size_t index = 0; index + 1 < runs.size(); ++index) {
    const HarmonicRun& dominant = runs[index];
    const HarmonicRun& tonic = runs[index + 1];
    if (dominant.root == minor_v_root && dominant.quality == ChordQuality::Minor &&
        tonic.root == proposed.root && tonic.quality == ChordQuality::Minor &&
        is_strong_run(dominant) && is_strong_run(tonic)) {
      ++strong_v_i_runs;
      ++index;
    }
  }
  return strong_v_i_runs >= kRelativeKeyMinTonicRuns;
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

  // Detect a terminal cadence from the final two harmonic runs. N.C. and invalid
  // durations carry no harmonic duration, and splitting an extended chord must not
  // change the local cadence weight.
  PitchClass cadence_tonic = PitchClass::C;
  bool has_cadence = false;
  PitchClass minor_cadence_tonic = PitchClass::C;
  bool has_minor_cadence = false;
  float cadence_duration = 0.0f;
  std::array<float, 12> minor_v_i_durations{};
  struct HarmonicRun {
    PitchClass root;
    ChordQuality quality;
    float duration;
  };
  std::array<HarmonicRun, 2> terminal_runs{};
  size_t harmonic_run_count = 0;
  const auto record_minor_v_i = [&](const HarmonicRun& dominant, const HarmonicRun& tonic) {
    const int interval = (static_cast<int>(tonic.root) - static_cast<int>(dominant.root) + 12) % 12;
    if (interval == 5 && dominant.quality == ChordQuality::Minor &&
        tonic.quality == ChordQuality::Minor) {
      minor_v_i_durations[static_cast<size_t>(tonic.root)] += dominant.duration;
    }
  };
  for (const Chord& chord : chords) {
    const float duration = chord.duration();
    if (chord.quality == ChordQuality::Unknown || !std::isfinite(duration) || duration <= 0.0f) {
      continue;
    }
    const ChordQuality triad = reduce_to_triad(chord.quality);
    if (harmonic_run_count > 0 && terminal_runs[harmonic_run_count - 1].root == chord.root &&
        terminal_runs[harmonic_run_count - 1].quality == triad) {
      terminal_runs[harmonic_run_count - 1].duration += duration;
    } else {
      const HarmonicRun current{chord.root, triad, duration};
      if (harmonic_run_count == 1) {
        record_minor_v_i(terminal_runs[0], current);
        terminal_runs[1] = current;
        harmonic_run_count = 2;
      } else if (harmonic_run_count == 2) {
        record_minor_v_i(terminal_runs[1], current);
        terminal_runs[0] = terminal_runs[1];
        terminal_runs[1] = current;
      } else {
        terminal_runs[0] = current;
        harmonic_run_count = 1;
      }
    }
  }
  if (harmonic_run_count == terminal_runs.size()) {
    const HarmonicRun& dominant = terminal_runs[0];
    const HarmonicRun& tonic = terminal_runs[1];
    const int interval = (static_cast<int>(tonic.root) - static_cast<int>(dominant.root) + 12) % 12;
    if (interval == 5 && dominant.quality == ChordQuality::Major) {
      if (tonic.quality == ChordQuality::Major) {
        cadence_tonic = tonic.root;
        has_cadence = true;
      } else if (tonic.quality == ChordQuality::Minor) {
        minor_cadence_tonic = tonic.root;
        has_minor_cadence = true;
      }
      if (has_cadence || has_minor_cadence) {
        cadence_duration = dominant.duration + tonic.duration;
      }
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
          major_tonic_score += duration * kDominantChordScoreWeight;
        }
      }
    }

    // Add bonus for cadence detection (V-I at end of song)
    float cadence_bonus = 0.0f;
    if (has_cadence && cadence_tonic == key_root) {
      cadence_bonus = cadence_duration * 0.4f;
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
          if (triad == ChordQuality::Major) {
            minor_tonic_score += duration * kDominantChordScoreWeight;
          }
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
      minor_cadence_bonus = cadence_duration * 0.4f;
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

    const float natural_minor_dominant_score =
        minor_v_i_durations[static_cast<size_t>(minor_root)] * kDominantChordScoreWeight;
    float total_minor_score = minor_score + minor_tonic_score + natural_minor_dominant_score +
                              minor_cadence_bonus + minor_bookend_bonus + minor_first_chord_bonus;
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

  // Usually the chords may only choose among keys the chroma leaves ambiguous. A repeated opening
  // tonic and strong recurring harmony can resolve a relative major/minor pair that aggregate
  // chroma leaves numerically decisive but harmonically ambiguous.
  const float chord_posterior = key_posterior(chroma_candidates, chord_key);
  if (chord_posterior < kChordOverrideMinPosteriorRatio * chroma_key.confidence) {
    if (!has_recurrent_relative_tonic(chord_key, chroma_key, all_chords, chroma_candidates)) {
      return chroma_key;
    }
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
