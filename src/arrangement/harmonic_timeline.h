#pragma once

/// @file harmonic_timeline.h
/// @brief First-class harmonic context type (key + chord-symbol timeline).
///
/// This file finalizes the chord-symbol harmony seed that the project model
/// (edit_model.h ProjectAnnotation) introduced. The harmonic representation is
/// factored out into a dedicated, documented value type so that it has a single
/// source of truth and is shared verbatim by:
///   - MIR analysis (sonare::mir builds a THIN timeline: detected roots /
///     qualities / key segments from the existing key/chord analyzers), and
///   - assist generation (which writes RICH progressions with extensions,
///     slash bass, Roman numerals / harmonic function).
/// Both round-trip through the SAME type, at CHORD-SYMBOL granularity (not a
/// bare interval list), so a thin MIR timeline and a rich assist timeline are
/// interchangeable and losslessly preserved by the project serializer.
///
/// The type is pure value data: control-thread only, no I/O, no system headers,
/// no RT object pointers. The core never interprets the musical meaning of an
/// extension number or a Roman-numeral string; it stores and round-trips them.
/// The compiler / RT path do NOT read the harmonic timeline.

#include <cstdint>
#include <string>
#include <vector>

namespace sonare::arrangement {

/// @brief Pitch-class sentinel meaning "unset / unknown" (valid PCs are 0..11).
inline constexpr uint8_t kUnknownPitchClass = 255;

// ===========================================================================
// Chord symbol
// ===========================================================================

/// @brief Chord quality at symbol granularity.
///
/// This is the ARRANGEMENT-side quality enum (distinct from the analysis-side
/// sonare::ChordQuality in util/types.h, which carries a different, detector-
/// oriented set). MIR maps detector qualities onto this enum; assist modules
/// author it directly. Forward-compatible: append new enumerators at the end
/// and never renumber (serialization stability).
enum class ChordQuality : uint32_t {
  kUnknown = 0,
  kMajor = 1,
  kMinor = 2,
  kDiminished = 3,
  kAugmented = 4,
  kDominant = 5,        ///< Dominant-family (e.g. dom7, dom9); extensions list refines it.
  kHalfDiminished = 6,  ///< m7b5.
  kSuspended = 7,       ///< sus2 / sus4 (the specific suspension lives in extensions).
};

/// @brief A single chord symbol over a PPQ span.
///
/// Tensions/extensions are stored as scale-degree numbers (e.g. {7, 9, 11, 13},
/// or 2 / 4 to disambiguate a suspension) plus an optional `add` flag implied by
/// the host/assist convention. The core does not interpret them musically; it
/// preserves the list verbatim. A span with end_ppq <= start_ppq is empty.
struct ChordSymbol {
  /// Span on the timeline (PPQ). end_ppq <= start_ppq is treated as empty.
  double start_ppq = 0.0;
  double end_ppq = 0.0;

  /// Root pitch class 0..11 (C=0). kUnknownPitchClass (255) = unknown.
  uint8_t root_pc = kUnknownPitchClass;
  ChordQuality quality = ChordQuality::kUnknown;
  /// Extension/tension scale degrees (e.g. {7, 9, 13}). Owned value data.
  std::vector<uint8_t> extensions;
  /// Optional slash-bass pitch class 0..11 (kUnknownPitchClass = none).
  uint8_t slash_bass_pc = kUnknownPitchClass;

  /// Optional Roman numeral / harmonic function text (e.g. "V7/ii"). Empty if
  /// unset. Owned; the core does not parse it.
  std::string roman_numeral;
  /// True when this chord begins a modulation (key change) boundary.
  bool modulation_boundary = false;

  /// @brief Returns the chord length in PPQ (0 when empty).
  double length_ppq() const noexcept { return end_ppq > start_ppq ? end_ppq - start_ppq : 0.0; }
  /// @brief Whether `ppq` falls in [start_ppq, end_ppq).
  bool contains(double ppq) const noexcept { return ppq >= start_ppq && ppq < end_ppq; }
};

// ===========================================================================
// Key segment
// ===========================================================================

/// @brief Mode of a key segment. Forward-compatible (append-only).
enum class KeyMode : uint32_t {
  kUnknown = 0,
  kMajor = 1,
  kMinor = 2,
  /// Modal keys. The tonic pitch class still applies; the mode disambiguates the
  /// diatonic collection used for pitch-correction targets.
  kDorian = 3,
  kPhrygian = 4,
  kLydian = 5,
  kMixolydian = 6,
  kLocrian = 7,
};

/// @brief A key (tonic + mode) over a PPQ span; the boundary is a modulation.
struct KeySegment {
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  /// Tonic pitch class 0..11 (kUnknownPitchClass = unknown).
  uint8_t tonic_pc = kUnknownPitchClass;
  KeyMode mode = KeyMode::kUnknown;

  /// @brief Whether `ppq` falls in [start_ppq, end_ppq).
  bool contains(double ppq) const noexcept { return ppq >= start_ppq && ppq < end_ppq; }
};

// ===========================================================================
// HarmonicTimeline
// ===========================================================================

/// @brief Key + chord-symbol timeline: the first-class harmonic context.
///
/// A sequence of key segments (with modulation boundaries) and a parallel
/// sequence of chord segments at symbol granularity. Segments are expected to be
/// non-overlapping and sorted by start_ppq, but the type does not enforce this;
/// the query helpers return the LAST matching segment so a caller that appends
/// in time order gets stable results. This is the same type MIR fills (thin) and
/// assist fills (rich), and the project serializer round-trips.
struct HarmonicTimeline {
  /// Key segments over the timeline (modulation boundaries between them).
  std::vector<KeySegment> keys;
  /// Chord-symbol progression over the timeline.
  std::vector<ChordSymbol> chords;

  /// @brief Returns the chord active at `ppq`, or nullptr if none covers it.
  const ChordSymbol* chord_at(double ppq) const noexcept {
    const ChordSymbol* found = nullptr;
    for (const auto& c : chords) {
      if (c.contains(ppq)) found = &c;
    }
    return found;
  }

  /// @brief Returns the key active at `ppq`, or nullptr if none covers it.
  const KeySegment* key_at(double ppq) const noexcept {
    const KeySegment* found = nullptr;
    for (const auto& k : keys) {
      if (k.contains(ppq)) found = &k;
    }
    return found;
  }

  /// @brief True when there are no key and no chord segments.
  bool empty() const noexcept { return keys.empty() && chords.empty(); }
};

}  // namespace sonare::arrangement
