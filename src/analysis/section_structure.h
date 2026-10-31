#pragma once

/// @file section_structure.h
/// @brief Song-structure analysis on a beat grid, as pure functions.
/// @details The stages @ref SectionAnalyzer runs, exposed one by one so each can be tested on
/// hand-built input. Frame features from the shared STFT are averaged into one column per beat
/// (gaps and the stretches outside the beats filled at the median spacing); boundaries are the
/// checkerboard-novelty peaks over those columns; sections are compared as sequences along the
/// diagonals of a smoothed column self-similarity matrix, grouped by complete linkage, and
/// labelled by a fixed rule table. Windows are held in seconds and converted to columns at the
/// track's median column spacing, so a doubled or halved beat tempo only changes the grid's
/// resolution. Every function here is deterministic and keeps no state.

#include <cstdint>
#include <limits>
#include <vector>

#include "analysis/boundary_detector.h"
#include "analysis/section_analyzer.h"
#include "core/audio.h"
#include "feature/chroma.h"
#include "util/types.h"

namespace sonare {

class BeatAnalyzer;
class Spectrogram;

namespace section_structure {

/// Fewer beats than this cannot carry a grid; the fixed fallback grid is used instead.
inline constexpr int kMinGridBeats = 8;
/// Column spacing of the fallback grid, in seconds.
inline constexpr float kFallbackGridSec = 0.5f;
/// Meter confidence below which downbeats are not trusted to place boundaries.
inline constexpr float kDownbeatConfidence = 0.5f;
/// Column count past which adjacent columns are merged pairwise (the full SSM and its smoothed
/// copy then stay within 32 MB).
inline constexpr int kMaxColumns = 2048;
/// Checkerboard half-width per second of minimum section length.
inline constexpr float kKernelHalfPerSection = 2.0f;
/// Raw-MFCC distance either side of a candidate below which the timbre has not changed.
/// Disabled: a steady chord drifts by up to 31.2 against 45.6 for the weakest true boundary of
/// the synthetic test song, short of a 1.5x margin either side of any floor.
inline constexpr float kMinTimbreContrast = std::numeric_limits<float>::infinity();
/// Chroma cosine distance either side below which the harmony has not changed: midpoint of
/// steady material (0.0009) and the synthetic song's weakest true boundary (0.0330).
inline constexpr float kMinHarmonyContrast = 0.017f;
/// Pair score at or above which two sections are the same material: the best balanced accuracy
/// (0.776) over 790 annotated Harmonix section pairs; the synthetic song's pairs split at
/// 0.53 / 0.76 either side of it.
inline constexpr float kRepeatScore = 0.60f;
/// Shorter-over-longer length ratio below which a section pair scores 0.
inline constexpr float kMinLengthRatio = 0.25f;
/// Alignment slack, in seconds, when matching a section inside another.
inline constexpr float kAlignSlackSec = 1.0f;
/// Two adjacent same-group sections merge only while together at most this times the group's
/// longest other occurrence.
inline constexpr float kMergeLengthRatio = 1.25f;
/// An edge section at most this times the Chorus mean length counts as short.
inline constexpr float kEdgeLengthRatio = 0.75f;

/// Which feature blocks the column vectors carry. Only accuracy measurement selects anything but
/// @ref FeatureSet::kBoth.
enum class FeatureSet { kBoth, kTimbreOnly, kHarmonyOnly };

/// Per-frame features on the analysis STFT's frame grid; per-coefficient arrays are row-major
/// ([coefficient * n_frames + frame]).
struct FrameFeatures {
  int n_frames = 0;
  float frame_sec = 0.0f;     ///< Hop duration
  float duration_sec = 0.0f;  ///< Analysis audio duration
  std::vector<float> mfcc;    ///< MFCC c1..c13 (13 x n_frames)
  std::vector<float> chroma;  ///< 12 x n_frames
  std::vector<float> rms;     ///< Frame RMS
  std::vector<float> frame_times;
  std::vector<float> vocal_band_energy;  ///< Spectral energy in the vocal band
  std::vector<float> total_energy;       ///< Spectral energy over all bins
  std::vector<float> tonality;           ///< 1 - spectral flatness
};

/// Number of MFCC coefficients kept per frame (c1..c13; c0 tracks level and is dropped).
inline constexpr int kFrameMfcc = 13;
/// Number of chroma bins per frame.
inline constexpr int kFrameChroma = 12;

/// The column time axis: one entry per column, each column spanning up to the next column's
/// start or, for the last, the end of the audio.
struct ColumnGrid {
  std::vector<float> times;    ///< Column start times, ascending; the first is 0
  std::vector<int> downbeats;  ///< Column indices that open a bar, ascending
  bool downbeats_reliable = false;
  float duration_sec = 0.0f;
  float column_sec = 0.0f;  ///< Median column spacing

  int size() const { return static_cast<int>(times.size()); }
  /// Start time of @p column, or the audio end for @c size().
  float time(int column) const;
  /// Whole columns in @p seconds at the median spacing, rounded, never negative.
  int to_columns(float seconds) const;
};

/// Per-column means before any comparison: raw MFCC c1..c13, L2-normalized chroma, and whether
/// the column is audible (silent columns hold zeros).
struct ColumnMeans {
  int n_columns = 0;
  std::vector<float> mfcc;       ///< n_columns x 13
  std::vector<float> chroma;     ///< n_columns x 12
  std::vector<uint8_t> audible;  ///< n_columns
};

/// Absolute change at each column between the audible columns of the windows either side.
/// 0 where the truncated kernel does not reach; +inf where only one side is audible.
struct ColumnContrast {
  std::vector<float> timbre;   ///< Euclidean distance of the mean raw MFCC
  std::vector<float> harmony;  ///< Cosine distance of the mean chroma
};

/// Column features: @c n_columns vectors of @c n_dims values, one column after another.
struct ColumnFeatures {
  int n_dims = 0;
  int n_columns = 0;
  std::vector<float> values;
};

/// A section as a half-open column range.
struct Span {
  int begin = 0;
  int end = 0;
  int length() const { return end - begin; }
};

/// Pairwise section scores with the alignment that produced each.
struct PairScores {
  int count = 0;
  std::vector<float> scores;  ///< count x count, symmetric, 1 on the diagonal
  /// count x count: entry (a, b) is where the shorter of the two starts inside the longer,
  /// relative to the longer's start, signed so that (a, b) = -(b, a).
  std::vector<int> offsets;

  float score(int a, int b) const { return scores[static_cast<size_t>(a * count + b)]; }
  int offset(int a, int b) const { return offsets[static_cast<size_t>(a * count + b)]; }
};

/// Sections after repetition analysis, with the scores between the final sections.
struct Repetition {
  std::vector<Span> spans;
  std::vector<int> groups;
  PairScores scores;
};

/// Per-section evidence the labelling rules read.
struct LabelInput {
  int group = 0;
  float length_sec = 0.0f;
  float energy = 0.0f;  ///< Median frame RMS, unnormalized
  float vocal_likelihood = 0.0f;
};

/// A function label and its confidence.
struct Label {
  SectionType type = SectionType::Unknown;
  float confidence = 0.0f;
};

/// The analysis outcome.
struct StructureResult {
  std::vector<Section> sections;
  std::vector<float> boundary_times;  ///< Detected, downbeat-snapped, before the length merge
  std::vector<float> pair_scores;     ///< sections x sections, row-major
};

/// The chroma geometry (and through it the STFT) section analysis runs at @p sample_rate.
ChromaConfig section_chroma_config(const SectionConfig& config, int sample_rate);

/// A grid from tracked beats, trusting the downbeats only above @ref kDownbeatConfidence.
SectionGrid make_section_grid(const BeatAnalyzer& beats);

/// Frame features of @p audio (already at the analysis rate) read off its STFT @p spec.
FrameFeatures frame_features(const Audio& audio, const Spectrogram& spec,
                             const SectionConfig& config);

/// Column axis for @p grid over @p duration_sec, filled where the beats leave gaps, replaced by
/// a fixed grid with too few beats, and halved until it has at most @ref kMaxColumns columns.
/// @param frame_sec Analysis hop; beats closer than this to the previous one are absorbed.
ColumnGrid column_grid(const SectionGrid& grid, float duration_sec, float frame_sec);

/// Per-column frame means; a column whose mean RMS carries no evidence is silent.
ColumnMeans column_means(const FrameFeatures& frames, const ColumnGrid& grid);

/// Column vectors: MFCC centred over the audible columns, each block L2-normalized and the two
/// blocks weighted equally; silent columns stay zero.
ColumnFeatures column_features(const ColumnMeans& means, FeatureSet features = FeatureSet::kBoth);

/// Contrast at every column over windows of the kernel's (truncated) half-width.
ColumnContrast column_contrast(const ColumnMeans& means, int kernel_size);

/// Column self-similarity S = F^T F, row-major N x N.
std::vector<float> column_ssm(const ColumnFeatures& features);

/// Diagonal smoothing: entry (i, j) is the mean of S(i+k, j+k) over the 0 <= k < @p length
/// terms that stay inside the matrix.
std::vector<float> smooth_diagonal(const std::vector<float>& ssm, int n, int length);

/// Sequence-match score of every section pair on the smoothed matrix.
/// @param slack Alignment slack in columns on either side of each candidate offset.
PairScores pair_scores(const std::vector<Span>& spans, const std::vector<float>& smoothed, int n,
                       int slack);

/// Joins adjacent sections a, b (left to right, once) when some third section x of about their
/// combined length matches both, with b's match following a's inside x.
std::vector<Span> join_split_sections(std::vector<Span> spans, const std::vector<float>& smoothed,
                                      const ColumnGrid& grid, float tau);

/// Complete-linkage groups at distance 1 - score, merged while within 1 - @p tau; numbered by
/// first appearance.
std::vector<int> group_sections(const PairScores& scores, float tau);

/// Merges adjacent same-group sections when the group occurs elsewhere and the merged length is
/// within @ref kMergeLengthRatio of its longest other occurrence. Groups are not recomputed.
void merge_adjacent(std::vector<Span>& spans, std::vector<int>& groups, const ColumnGrid& grid);

/// Join, grouping, adjacent merge and the one-group collapse, with scores of the final spans.
Repetition resolve_repetition(std::vector<Span> spans, const std::vector<float>& smoothed,
                              const ColumnGrid& grid, float tau = kRepeatScore);

/// Strongest boundary subset whose sections all reach @p min_section_sec, less half of
/// @p column_sec: boundaries sit on column times, so a shortfall under the grid's resolution
/// does not remove a section.
std::vector<Boundary> merge_short_sections(const std::vector<Boundary>& boundaries,
                                           float duration_sec, float min_section_sec,
                                           float column_sec);

/// Moves each boundary onto a downbeat at most one column away, keeping the stronger of two
/// that land on one column. A no-op unless the grid's downbeats are reliable.
std::vector<Boundary> snap_to_downbeats(const std::vector<Boundary>& boundaries,
                                        const ColumnGrid& grid);

/// Function labels from the rule table. @p scores holds the sections' pair scores.
std::vector<Label> label_sections(const std::vector<LabelInput>& sections,
                                  const PairScores& scores);

/// Runs every stage above on @p frames laid on @p grid.
StructureResult analyze_structure(const FrameFeatures& frames, const SectionGrid& grid,
                                  const SectionConfig& config,
                                  FeatureSet features = FeatureSet::kBoth);

}  // namespace section_structure
}  // namespace sonare
