#include "analysis/section_analyzer.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "feature/spectral.h"
#include "util/exception.h"

namespace sonare {

std::string Section::type_string() const { return section_type_to_string(type); }

char section_type_to_char(SectionType type) {
  switch (type) {
    case SectionType::Intro:
      return 'I';
    case SectionType::Verse:
      return 'A';
    case SectionType::PreChorus:
      return 'P';
    case SectionType::Chorus:
      return 'B';
    case SectionType::Bridge:
      return 'C';
    case SectionType::Instrumental:
      return 'S';
    case SectionType::Outro:
      return 'O';
    case SectionType::Unknown:
      return '?';
  }
  return '?';
}

std::string section_type_to_string(SectionType type) {
  switch (type) {
    case SectionType::Intro:
      return "Intro";
    case SectionType::Verse:
      return "Verse";
    case SectionType::PreChorus:
      return "Pre-Chorus";
    case SectionType::Chorus:
      return "Chorus";
    case SectionType::Bridge:
      return "Bridge";
    case SectionType::Instrumental:
      return "Instrumental";
    case SectionType::Outro:
      return "Outro";
    case SectionType::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

SectionAnalyzer::SectionAnalyzer(const Audio& audio, const SectionConfig& config)
    : audio_(audio), config_(config), sr_(audio.sample_rate()), hop_length_(config.hop_length) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  analyze();
}

void SectionAnalyzer::analyze() {
  // Compute RMS energy curve
  energy_curve_ = rms_energy(audio_, config_.n_fft, config_.hop_length);

  // Detect boundaries using BoundaryDetector
  BoundaryConfig boundary_config;
  boundary_config.n_fft = config_.n_fft;
  boundary_config.hop_length = config_.hop_length;
  boundary_config.threshold = config_.boundary_threshold;
  boundary_config.kernel_size = config_.kernel_size;
  boundary_config.peak_distance = config_.min_section_sec;

  BoundaryDetector detector(audio_, boundary_config);
  boundaries_ = detector.boundary_times();

  // Create sections from boundaries
  float audio_duration = audio_.duration();

  // Add start and end as implicit boundaries
  std::vector<float> all_boundaries;
  all_boundaries.push_back(0.0f);
  for (float b : boundaries_) {
    all_boundaries.push_back(b);
  }
  all_boundaries.push_back(audio_duration);

  // Create sections
  sections_.clear();
  for (size_t i = 0; i + 1 < all_boundaries.size(); ++i) {
    Section section;
    section.start = all_boundaries[i];
    section.end = all_boundaries[i + 1];
    section.energy_level = compute_section_energy(section.start, section.end);
    section.confidence = 0.5f;          // Will be updated by classification
    section.type = SectionType::Verse;  // Default, will be classified later

    // Only add if section is long enough
    if (section.duration() >= config_.min_section_sec * 0.5f) {
      sections_.push_back(section);
    }
  }

  // Classify sections
  classify_sections();
}

float SectionAnalyzer::compute_section_energy(float start, float end) const {
  if (energy_curve_.empty() || start >= end) {
    return 0.0f;
  }

  float hop_duration = static_cast<float>(hop_length_) / sr_;
  int start_frame = static_cast<int>(start / hop_duration);
  int end_frame = static_cast<int>(end / hop_duration);

  start_frame = std::max(0, start_frame);
  end_frame = std::min(static_cast<int>(energy_curve_.size()), end_frame);

  if (start_frame >= end_frame) {
    return 0.0f;
  }

  float sum = 0.0f;
  for (int i = start_frame; i < end_frame; ++i) {
    sum += energy_curve_[i];
  }

  return sum / (end_frame - start_frame);
}

void SectionAnalyzer::classify_sections() {
  if (sections_.empty()) {
    return;
  }

  // Normalize energy levels to [0, 1]
  float max_energy = 0.0f;
  for (const auto& section : sections_) {
    max_energy = std::max(max_energy, section.energy_level);
  }

  if (max_energy > 1e-6f) {
    for (auto& section : sections_) {
      section.energy_level /= max_energy;
    }
  }

  // Classify each section
  for (size_t i = 0; i < sections_.size(); ++i) {
    sections_[i].type = classify_section(static_cast<int>(i));
    sections_[i].confidence = 0.6f + 0.2f * sections_[i].energy_level;
  }
}

SectionType SectionAnalyzer::classify_section(int section_idx) const {
  if (section_idx < 0 || section_idx >= static_cast<int>(sections_.size())) {
    return SectionType::Verse;
  }

  const Section& section = sections_[section_idx];
  int n_sections = static_cast<int>(sections_.size());

  // Position-based heuristics
  bool is_first = (section_idx == 0);
  bool is_last = (section_idx == n_sections - 1);
  float relative_position = static_cast<float>(section_idx) / std::max(1, n_sections - 1);

  // Energy-based heuristics
  float energy = section.energy_level;
  bool is_high_energy = (energy > 0.7f);
  bool is_low_energy = (energy < 0.3f);

  // Duration heuristics
  float duration = section.duration();
  bool is_short = (duration < config_.min_section_sec);

  // Classification rules
  if (is_first && is_low_energy) {
    return SectionType::Intro;
  }

  if (is_last && is_low_energy) {
    return SectionType::Outro;
  }

  if (is_first && duration < 10.0f) {
    return SectionType::Intro;
  }

  if (is_last && duration < 10.0f) {
    return SectionType::Outro;
  }

  // High energy sections in the middle are likely choruses
  if (is_high_energy && !is_first && !is_last) {
    return SectionType::Chorus;
  }

  // Short sections before chorus-like sections might be pre-chorus
  if (is_short && section_idx + 1 < n_sections) {
    const Section& next = sections_[section_idx + 1];
    if (next.energy_level > section.energy_level + 0.2f) {
      return SectionType::PreChorus;
    }
  }

  // Low energy sections in the middle with high surrounding energy might be bridge
  if (is_low_energy && !is_first && !is_last) {
    bool high_neighbors = false;
    if (section_idx > 0 && sections_[section_idx - 1].energy_level > 0.6f) {
      high_neighbors = true;
    }
    if (section_idx + 1 < n_sections && sections_[section_idx + 1].energy_level > 0.6f) {
      high_neighbors = true;
    }
    if (high_neighbors) {
      return SectionType::Bridge;
    }
  }

  // Sections at roughly 1/3 and 2/3 position with moderate energy are likely verses
  if (relative_position > 0.1f && relative_position < 0.9f && !is_high_energy) {
    return SectionType::Verse;
  }

  // Default to verse
  return SectionType::Verse;
}

std::string SectionAnalyzer::form() const {
  std::ostringstream oss;
  for (const auto& section : sections_) {
    oss << section_type_to_char(section.type);
  }
  return oss.str();
}

Section SectionAnalyzer::section_at(float time) const {
  for (const auto& section : sections_) {
    if (time >= section.start && time < section.end) {
      return section;
    }
  }

  // Return empty section if not found
  Section empty;
  empty.type = SectionType::Verse;
  empty.start = 0.0f;
  empty.end = 0.0f;
  empty.energy_level = 0.0f;
  empty.confidence = 0.0f;
  return empty;
}

float SectionAnalyzer::duration() const {
  if (sections_.empty()) {
    return 0.0f;
  }
  return sections_.back().end;
}

std::vector<float> SectionAnalyzer::boundary_times() const { return boundaries_; }

}  // namespace sonare
