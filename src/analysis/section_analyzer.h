#pragma once

/// @file section_analyzer.h
/// @brief Section analysis for detecting song structure.

#include <cstddef>
#include <string>
#include <vector>

#include "analysis/boundary_detector.h"
#include "core/audio.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/types.h"

namespace sonare {

class Spectrogram;

/// @brief Detected section with classification.
struct Section {
  SectionType type;    ///< Section type (Intro, Verse, Chorus, etc.)
  float start;         ///< Start time in seconds
  float end;           ///< End time in seconds
  float energy_level;  ///< Median frame RMS in the section over the loudest section's, [0, 1]
  float confidence;    ///< Classification confidence [0, 1]
  /// Repetition group: sections sharing a value are the same material. Numbered 0, 1, 2... in
  /// order of first appearance; a section that repeats nothing has a group of its own.
  int group = 0;

  /// @brief Returns section type as string.
  std::string type_string() const;

  /// @brief Returns duration in seconds.
  float duration() const { return end - start; }
};

/// @brief Default relative boundary threshold of section analysis. The synthetic test song
/// resolves all ten boundaries over 0.18-0.25; inside that, 0.22 is within 0.01 of the best mean
/// boundary hit rate (HR3F 0.500 vs 0.504 at 0.23) over 14 Harmonix tracks, nearest the centre.
inline constexpr float kSectionBoundaryThreshold = 0.22f;

/// @brief Configuration for section analysis.
struct SectionConfig {
  int n_fft = 2048;      ///< Window length in samples at 22050 Hz, rescaled to the analysis rate
  int hop_length = 512;  ///< Hop length in samples at the analysis rate
  /// Minimum section duration in seconds. It also sets the analysis scale: the novelty kernel
  /// spans twice this on each side of a boundary, and repetition is matched over runs this long.
  /// After downbeat snapping, endpoint-feasible boundaries are considered by decreasing novelty
  /// strength (ties go to earlier times); a boundary is kept when it leaves this floor between
  /// itself and every already-kept boundary or track edge, less half a grid column.
  float min_section_sec = 4.0f;
  float boundary_threshold = kSectionBoundaryThreshold;  ///< Boundary detection threshold
  /// Recording tuning offset in fractions of a semitone (estimate_tuning() unit) applied to the
  /// section chroma; 0 is concert A440.
  float tuning = 0.0f;
};

/// @brief Refuses a SectionConfig outside the argument domain every public surface shares.
/// @throws SonareException(InvalidParameter) unless n_fft and hop_length are positive and
///         min_section_sec is finite and non-negative.
inline void validate_section_config(const SectionConfig& config) {
  SONARE_CHECK_MSG(config.n_fft > 0 && config.hop_length > 0, ErrorCode::InvalidParameter,
                   "SectionConfig: n_fft and hop_length must be positive");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.min_section_sec),
                   ErrorCode::InvalidParameter,
                   "SectionConfig: min_section_sec must be finite and non-negative");
}

/// @brief Beat grid the section analysis is laid on.
struct SectionGrid {
  std::vector<float> beat_times;      ///< Beat times in seconds, ascending
  std::vector<int> downbeat_indices;  ///< Indices into @ref beat_times, strictly ascending
  /// Whether boundaries may be moved onto the downbeats; false leaves them where detected.
  bool downbeats_reliable = false;
};

/// @brief Refuses a SectionGrid that cannot describe a beat sequence.
/// @throws SonareException(InvalidParameter) when a beat time is non-finite, negative or below
///         its predecessor, or a downbeat index is out of range or not strictly ascending.
inline void validate_section_grid(const SectionGrid& grid) {
  for (size_t i = 0; i < grid.beat_times.size(); ++i) {
    SONARE_CHECK_MSG(numeric::finite_non_negative(grid.beat_times[i]) &&
                         (i == 0 || grid.beat_times[i] >= grid.beat_times[i - 1]),
                     ErrorCode::InvalidParameter,
                     "SectionGrid: beat_times must be finite, non-negative and ascending");
  }
  for (size_t i = 0; i < grid.downbeat_indices.size(); ++i) {
    const int index = grid.downbeat_indices[i];
    SONARE_CHECK_MSG(index >= 0 && static_cast<size_t>(index) < grid.beat_times.size() &&
                         (i == 0 || index > grid.downbeat_indices[i - 1]),
                     ErrorCode::InvalidParameter,
                     "SectionGrid: downbeat_indices must index beat_times in ascending order");
  }
}

/// @brief Section analyzer producing a rule-based song-structure estimate.
/// @details Features are averaged per beat (the grid's gaps and the stretches before the first
/// and after the last beat are filled at the median beat spacing). Boundaries come from a
/// checkerboard novelty over those beat columns, moved onto a nearby downbeat when the meter is
/// trusted. Sections are then compared as sequences on a diagonally smoothed self-similarity
/// matrix: sections that match form a repetition @ref Section::group, a section split in two is
/// rejoined when its halves line up with one whole occurrence elsewhere, and a function label is
/// read off the groups by a fixed rule table (the most repeated loud group is the Chorus, the
/// group covering most time the Verse, a group always leading into the Chorus a Pre-Chorus, and
/// so on). A segment no rule claims is Unknown.
///
/// @warning This is a fixed-rule heuristic, NOT a trained structure detector. The groups are
/// the measured part; the *labels* are an interpretation of them and will be wrong on material
/// that does not follow a conventional pop form. For downstream algorithms prefer
/// @ref Section::group, @ref boundary_times and @ref section_self_similarity.
///
/// Material that never changes is not given structure: when no boundary clears the novelty
/// floor, or every section falls into one group, the track is one Unknown section.
class SectionAnalyzer {
 public:
  /// @brief Constructs section analyzer from audio.
  /// @details Tracks the beats itself, without the chord-informed downbeat refinement
  /// @ref MusicAnalyzer applies, so its sections need not match that analyzer's.
  /// @param audio Input audio; rates above 22.05 kHz are analyzed at 22.05 kHz
  /// so section results are stable across common source rates.
  /// @param config Section analysis configuration
  explicit SectionAnalyzer(const Audio& audio, const SectionConfig& config = SectionConfig());

  /// @brief Constructs section analyzer from a beat grid and a pre-computed STFT.
  /// @param audio Input audio; resampled like the audio constructor's.
  /// @param grid Beats and downbeats of @p audio.
  /// @param spec STFT of the analysis-rate @p audio; its geometry is checked against @p config
  ///        and a mismatch throws, because a framing the features did not ask for reads section
  ///        spans off the wrong frames rather than failing.
  /// @param config Section analysis configuration
  /// @throws SonareException (InvalidParameter) if @p spec does not match or @p grid is invalid.
  SectionAnalyzer(const Audio& audio, const SectionGrid& grid, const Spectrogram& spec,
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

  /// @brief Returns detected boundaries in seconds, before the minimum-length merge.
  std::vector<float> boundary_times() const { return boundaries_; }

  /// @brief Returns the section-pair repetition scores.
  /// @details Row-major @c count() x @c count() matrix; entry (i, j) is how well the shorter of
  /// sections i and j matches the best-aligned stretch of the longer, as the mean of the
  /// diagonally smoothed beat self-similarity along that alignment. 1 on the diagonal, 0 for a
  /// pair whose lengths differ more than fourfold. This is the unthresholded signal the grouping
  /// consumes, so a caller can apply its own repetition threshold. Empty when there are no
  /// sections.
  std::vector<float> section_self_similarity() const { return pair_scores_; }

 private:
  void analyze(const SectionGrid& grid, const Spectrogram& spec);

  std::vector<Section> sections_;
  std::vector<float> pair_scores_;
  std::vector<float> boundaries_;
  Audio audio_;
  SectionConfig config_;
};

/// @brief Converts section type to single character for form notation.
char section_type_to_char(SectionType type);

/// @brief Converts section type to string.
std::string section_type_to_string(SectionType type);

}  // namespace sonare
