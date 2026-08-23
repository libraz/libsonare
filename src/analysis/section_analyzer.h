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
  int n_fft = 2048;      ///< FFT size
  int hop_length = 512;  ///< Hop length
  /// Minimum section duration in seconds. Shorter sections are merged into
  /// their neighbour; only a lone whole-track section may fall below it.
  float min_section_sec = 4.0f;
  float boundary_threshold = 0.3f;  ///< Boundary detection threshold
  int kernel_size = 64;             ///< Checkerboard kernel size
};

/// @brief Section analyzer producing a heuristic song-structure estimate.
/// @details Combines boundary detection with energy / chroma / vocal-band
/// analysis to split the track and classify each segment into Intro, Verse,
/// Chorus, Bridge, Instrumental and Outro. A segment that matches none of those
/// is reported as Unknown rather than folded into one of them, so a caller can
/// tell an identified section from an unidentified one.
///
/// @warning This is a fixed-threshold heuristic, NOT a trained structure
/// detector. The boundary positions are generally usable, but the *labels*
/// (Verse vs Chorus vs Bridge, etc.) are best-effort and will be wrong on many
/// real songs, especially material that does not follow a conventional
/// pop/verse-chorus form; a track with no detected boundaries collapses to a
/// single whole-track Unknown. Treat @ref form / per-section @ref Section::type
/// as hints, not ground truth. For downstream algorithms prefer the raw signals
/// — @ref boundary_times (segment boundaries) and @ref section_self_similarity
/// (the chroma cosine self-similarity matrix) — and apply your own thresholds.
///
/// Two guards keep it from inventing structure. Adjacent segments whose chroma
/// is indistinguishable are merged before anything is labelled, so a novelty
/// peak inside one continuous stretch of music does not split it. And when
/// nearly every pair of sections counts as a repetition of every other — which
/// is what uniform material looks like — repetition is treated as carrying no
/// information rather than as evidence for a verse/chorus alternation. Both
/// make the analyzer report Unknown where it previously named a section.
class SectionAnalyzer {
 public:
  /// @brief Constructs section analyzer from audio.
  /// @param audio Input audio; rates above 22.05 kHz are analyzed at 22.05 kHz
  /// so section results are stable across common source rates.
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
  void merge_short_sections();

  /// @brief Merges adjacent sections whose chroma content is indistinguishable.
  /// @details A novelty peak inside one continuous stretch of music splits it
  /// into segments that no descriptor can tell apart, which then read as
  /// repetitions of each other and drive a full song form out of material that
  /// never changed. Collapsing them first means the labeller sees the structure
  /// the audio has rather than the structure the peak picker found.
  void merge_indistinct_sections();

  /// @brief L2-normalized mean chroma of each current section.
  /// @details The harmonic half of @ref build_descriptors, computed on its own
  /// so the merge pass does not also pay for a spectrogram and a flatness curve
  /// that classification recomputes moments later.
  std::vector<std::array<float, 12>> section_mean_chromas() const;

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
