#pragma once

/// @file section_analyzer.h
/// @brief Section analysis for detecting song structure.

#include <string>
#include <vector>

#include "analysis/boundary_detector.h"
#include "core/audio.h"
#include "util/types.h"

namespace sonare {

/// @brief Detected section with classification.
struct Section {
  SectionType type;    ///< Section type (Intro, Verse, Chorus, etc.)
  float start;         ///< Start time in seconds
  float end;           ///< End time in seconds
  float energy_level;  ///< Relative energy level [0, 1]
  float confidence;    ///< Classification confidence [0, 1]

  /// @brief Returns section type as string.
  std::string type_string() const;

  /// @brief Returns duration in seconds.
  float duration() const { return end - start; }
};

/// @brief Configuration for section analysis.
struct SectionConfig {
  int n_fft = 2048;                 ///< FFT size
  int hop_length = 512;             ///< Hop length
  float min_section_sec = 4.0f;     ///< Minimum section duration
  float boundary_threshold = 0.3f;  ///< Boundary detection threshold
  int kernel_size = 64;             ///< Checkerboard kernel size
};

/// @brief Section analyzer for detecting song structure.
/// @details Combines boundary detection with energy analysis to classify
/// sections into types like Intro, Verse, Chorus, Bridge, and Outro.
class SectionAnalyzer {
 public:
  /// @brief Constructs section analyzer from audio.
  /// @param audio Input audio
  /// @param config Section analysis configuration
  explicit SectionAnalyzer(const Audio& audio, const SectionConfig& config = SectionConfig());

  /// @brief Constructs section analyzer from pre-computed boundaries.
  /// @param audio Input audio for energy analysis
  /// @param boundaries Pre-computed boundary times in seconds
  /// @param config Section analysis configuration
  SectionAnalyzer(const Audio& audio, const std::vector<float>& boundaries,
                  const SectionConfig& config = SectionConfig());

  /// @brief Returns detected sections.
  const std::vector<Section>& sections() const { return sections_; }

  /// @brief Returns the song form as a string (e.g., "IABABCAB").
  std::string form() const;

  /// @brief Returns number of detected sections.
  size_t count() const { return sections_.size(); }

  /// @brief Returns section at a specific time.
  /// @param time Time in seconds
  /// @return Section at the given time
  Section section_at(float time) const;

  /// @brief Returns total duration in seconds.
  float duration() const;

  /// @brief Returns section boundaries in seconds.
  std::vector<float> boundary_times() const;

 private:
  void analyze();
  void classify_sections();
  SectionType classify_section(int section_idx) const;
  float compute_section_energy(float start, float end) const;

  std::vector<Section> sections_;
  std::vector<float> energy_curve_;
  std::vector<float> boundaries_;
  Audio audio_;
  SectionConfig config_;
  int sr_;
  int hop_length_;
};

/// @brief Converts section type to single character for form notation.
char section_type_to_char(SectionType type);

/// @brief Converts section type to string.
std::string section_type_to_string(SectionType type);

}  // namespace sonare
