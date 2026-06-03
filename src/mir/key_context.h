#pragma once

/// @file key_context.h
/// @brief Bridges offline key/chord analysis into an arrangement HarmonicTimeline
///        and derives pitch-correction targets and onset split candidates.
///
/// This is an OFFLINE, control-plane-only component (subsystem sonare::mir). It
/// never runs on the audio thread. It does NOT re-implement key/chord detection
/// or onset detection; it consumes the structs produced by the existing
/// analyzers (sonare::ChordAnalyzer, sonare::KeyAnalyzer, sonare::OnsetAnalyzer,
/// feature/onset) and:
///   1. builds a THIN arrangement::HarmonicTimeline (detected roots / qualities /
///      key segments), at the same chord-symbol granularity assist uses, so
///      the two round-trip through the SAME type;
///   2. derives a pitch-correction TARGET (scale / chord target pitch classes)
///      from the key/chord context at a given PPQ -- it returns target pitch
///      classes only and never performs correction (that is editing/ DSP);
///   3. turns onset markers into a list of candidate split PPQ positions for a
///      clip. These are CANDIDATES only: the caller issues the actual split via a
///      SplitClip command. The bridge never mutates a Project and never
///      auto-splits.
///
/// @section keyctx_time Time conversion
/// The analyzers report times in SECONDS. Conversion to PPQ (musical time) is
/// done deterministically through a prepared transport::TempoMap: seconds map to
/// samples via the map's sample rate, then samples to PPQ via sample_to_ppq().
/// The same input + tempo map always yields byte-identical PPQ positions.
///
/// @section keyctx_determinism Determinism
/// All functions are pure given their inputs. No clocks, no std::rand. The only
/// floating-point is IEEE arithmetic on the analyzer outputs.

#include <cstdint>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "arrangement/harmonic_timeline.h"
#include "transport/tempo_map.h"
#include "util/types.h"

namespace sonare::mir {

/// @brief Offline analysis output consumed by build_harmonic_timeline().
///
/// Plain values so callers can populate it from a real ChordAnalyzer /
/// KeyAnalyzer or a synthetic test fixture. Chord/key times are in SECONDS.
struct HarmonicAnalysisInput {
  /// Detected chord segments (root, quality, start/end seconds, confidence).
  std::vector<Chord> chords;
  /// Detected key segments over the timeline. When empty, a single global key
  /// (see `global_key`) spanning all chords is emitted.
  std::vector<Key> keys;
  /// Start time (seconds) of each entry in `keys`. Must match `keys` size when
  /// non-empty; ignored otherwise.
  std::vector<float> key_start_times;
  /// Global key used when `keys` is empty (0-length means "unknown key").
  Key global_key{PitchClass::C, Mode::Major, 0.0f};
  /// Whether a global key estimate is available (controls the fallback above).
  bool has_global_key = false;
};

/// @brief Maps an analysis-side sonare::ChordQuality onto the arrangement-side
///        chord-symbol quality plus its extension scale degrees.
///
/// E.g. Dominant9 -> {kDominant, extensions {7, 9}}. Deterministic and total.
struct MappedQuality {
  arrangement::ChordQuality quality = arrangement::ChordQuality::kUnknown;
  std::vector<uint8_t> extensions;
};
MappedQuality map_chord_quality(ChordQuality quality);

/// @brief Maps an analysis-side sonare::Mode onto the arrangement-side KeyMode.
arrangement::KeyMode map_key_mode(Mode mode);

/// @brief Builds a thin arrangement::HarmonicTimeline from offline analysis.
///
/// Chord/key times (seconds) are converted to PPQ via @p tempo_map (which must
/// be prepared). Chords are emitted in order at chord-symbol granularity; each
/// chord that begins inside a different key than its predecessor is marked as a
/// modulation boundary. Roman numerals / slash bass are left empty here (this is
/// the thin MIR side; assist fills the rich fields on the SAME type).
///
/// Deterministic: same input + tempo map -> identical timeline.
arrangement::HarmonicTimeline build_harmonic_timeline(const HarmonicAnalysisInput& input,
                                                      const transport::TempoMap& tempo_map);

/// @brief Pitch-correction target derived from harmonic context at a PPQ.
///
/// Returns target pitch classes only; it does NOT correct pitch. The caller
/// (editing/ pitch DSP) decides how to snap. `chord_tones` are the strongest
/// targets (the chord active at the PPQ); `scale_tones` are the diatonic
/// collection of the active key (a wider, weaker target set). Both are sorted
/// ascending and contain pitch classes in 0..11.
struct PitchCorrectionTarget {
  /// Chord-tone pitch classes (highest-priority snap targets). May be empty when
  /// no chord covers the PPQ.
  std::vector<uint8_t> chord_tones;
  /// Scale (diatonic) pitch classes of the active key. May be empty when no key
  /// covers the PPQ and no chord is available to seed a scale.
  std::vector<uint8_t> scale_tones;
};

/// @brief Derives a pitch-correction target from a HarmonicTimeline at @p ppq.
///
/// The chord active at @p ppq seeds `chord_tones` (root + quality intervals +
/// extensions). The key active at @p ppq seeds `scale_tones` (the diatonic
/// collection of tonic + mode). When no key covers @p ppq but a chord does, the
/// scale falls back to the chord's own tones. Deterministic.
PitchCorrectionTarget derive_pitch_correction_target(const arrangement::HarmonicTimeline& timeline,
                                                     double ppq);

/// @brief Configuration for onset -> split-candidate conversion.
struct OnsetSplitConfig {
  /// Drop onsets weaker than this strength (0 keeps all).
  float min_strength = 0.0f;
  /// Minimum spacing (PPQ) between accepted candidates; closer onsets after the
  /// first are dropped. 0 keeps all spacing.
  double min_spacing_ppq = 0.0;
  /// Drop candidates within this PPQ of the clip start or end (avoids zero-length
  /// split fragments). 0 keeps boundary-adjacent onsets.
  double edge_guard_ppq = 0.0;
};

/// @brief A candidate split position for a clip (PPQ, with source confidence).
///
/// CANDIDATE only: the caller issues the real edit via a SplitClip command.
/// `ppq` is an absolute timeline position inside the clip's [start, end) span.
struct SplitCandidate {
  double ppq = 0.0;
  float strength = 0.0f;
};

/// @brief Converts onset markers into split candidates for a clip span.
///
/// @param onsets Detected onsets (time in seconds, with strength).
/// @param tempo_map Prepared map for seconds -> PPQ conversion.
/// @param clip_start_ppq Clip start on the timeline (inclusive).
/// @param clip_end_ppq Clip end on the timeline (exclusive).
/// @param config Filtering knobs.
/// @return Candidates strictly inside (clip_start_ppq, clip_end_ppq), ascending
///         by PPQ. Deterministic.
std::vector<SplitCandidate> onset_split_candidates(const std::vector<Onset>& onsets,
                                                   const transport::TempoMap& tempo_map,
                                                   double clip_start_ppq, double clip_end_ppq,
                                                   const OnsetSplitConfig& config = {});

}  // namespace sonare::mir
