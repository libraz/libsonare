#include "analysis/key_analyzer.h"

#include <algorithm>
#include <cmath>

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

  // Compute correlation with all 24 keys
  for (int root = 0; root < 12; ++root) {
    PitchClass pc = static_cast<PitchClass>(root);

    // Major key
    auto major_profile = get_major_profile(pc, config_.profile_type);
    float major_corr = profile_correlation(mean_chroma_, major_profile);

    KeyCandidate major_candidate;
    major_candidate.key.root = pc;
    major_candidate.key.mode = Mode::Major;
    major_candidate.key.confidence = 0.0f;  // Set later
    major_candidate.correlation = major_corr;
    candidates_.push_back(major_candidate);

    // Minor key
    auto minor_profile = get_minor_profile(pc, config_.profile_type);
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

}  // namespace sonare
