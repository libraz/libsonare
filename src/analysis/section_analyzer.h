#pragma once

/// @file section_analyzer.h
/// @brief Section analysis for detecting song structure.

#include <array>
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

/// @brief Section analyzer producing a heuristic song-structure estimate.
/// @details Combines boundary detection with energy / chroma / vocal-band
/// analysis to split the track and classify each segment into Intro, Verse,
/// Chorus, Bridge, Instrumental and Outro.
///
/// @warning This is a fixed-threshold heuristic, NOT a trained structure
/// detector. The boundary positions are generally usable, but the *labels*
/// (Verse vs Chorus vs Bridge, etc.) are best-effort and will be wrong on many
/// real songs, especially material that does not follow a conventional
/// pop/verse-chorus form; a track with no detected boundaries collapses to a
/// single whole-track "Verse". Treat @ref form / per-section @ref Section::type
/// as hints, not ground truth. For downstream algorithms prefer the raw signals
/// — @ref boundary_times (segment boundaries) and @ref section_self_similarity
/// (the chroma cosine self-similarity matrix) — and apply your own thresholds.
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

  /// @brief Returns the raw section-level self-similarity matrix.
  /// @details Row-major @c count() x @c count() matrix of chroma cosine
  /// similarities in [0, 1]; entry (i, j) is the similarity between section i and
  /// section j (the diagonal is ~1). This is the unthresholded signal that the
  /// built-in labeller consumes — exposed so callers can apply their own
  /// repetition thresholds / clustering instead of the fixed heuristic. Returns
  /// an empty vector when there are no sections.
  std::vector<float> section_self_similarity() const;

 private:
  void analyze();
  void classify_sections();
  float compute_section_energy(float start, float end) const;

  /// @brief Per-section descriptor used for self-similarity classification.
  struct SectionDescriptor {
    std::array<float, 12> chroma{};  ///< Mean (L2-normalized) chroma vector
    float energy = 0.0f;             ///< Mean RMS energy [0, 1] after normalization
    float vocal_likelihood = 0.0f;   ///< Estimated vocal presence [0, 1]
  };

  /// @brief Builds per-section chroma / energy / vocal-likelihood descriptors.
  /// @details Computes a chromagram and spectrogram once, then aggregates the
  /// per-frame features inside each section's time span.
  std::vector<SectionDescriptor> build_descriptors() const;

  /// @brief Computes the section-level self-similarity matrix (cosine of chroma).
  std::vector<float> self_similarity(const std::vector<SectionDescriptor>& descriptors) const;

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
