#pragma once

/// @file analysis_json.h
/// @brief Canonical JSON serializer for the unified AnalysisResult.
///
/// This is the single source of truth for the field schema returned by the
/// one-shot analyze() API across every binding. The full result
/// (tempo, key, beats, chords, sections, timbre, dynamics, rhythm, melody, form) is exposed to
/// C/Python/Node via sonare_analyze_json, which serializes through this helper.
/// The WASM native object (analysisResultToVal) mirrors the same field names.

#include <string>
#include <utility>
#include <vector>

#include "analysis/music_analyzer.h"

namespace sonare {

/// @brief Serializes a complete AnalysisResult to a camelCase JSON object.
/// @details The field set is @ref analysis_result_schema_paths. Every field is present except
///          `melody.pitches[].frequencyHz`, which is left out for an unvoiced point. Downbeats are
///          written as the beat records they index. Non-finite floats are emitted as null
///          (handled by the JSON dumper).
std::string analysis_result_to_json(const AnalysisResult& result);

/// @brief Canonical field paths for the unified AnalysisResult schema.
/// @details Array item fields use [] (for example beats[].timeSec). Keep the JSON
/// serializer and WASM native object in parity by testing both surfaces against
/// this list.
const std::vector<std::string>& analysis_result_schema_paths();
/// @brief The infinite end a null at each listed path of the same result stands for.
const std::vector<std::pair<std::string, std::string>>& analysis_result_null_means();

/// @brief Serializes a standalone meter estimate to a camelCase JSON object.
/// @details Schema (every field always present):
///   {
///     "timeSignature": {numerator, denominator, confidence},
///     "downbeatPhase": number,
///     "searched": bool,
///     "grouping": [number],
///     "candidateScores": [number],
///     "candidates": [{numerator, denominator, confidence}],
///     "unobservedBeats": [integer]
///   }
/// candidateScores is parallel to the requested candidate numerators, while
/// candidates is ordered by descending support, so the two do not index alike.
/// A candidateScores entry is standardized and may be negative: zero is the
/// level a numerator reaches on beats carrying no meter, and a score grows with
/// the square root of how many beats were scored, so scores from calls over
/// spans of different lengths are not comparable without normalizing for length.
/// grouping holds the beats per accent group within one bar and always sums to
/// timeSignature.numerator; a single entry means no internal division was
/// resolved.
/// unobservedBeats lists zero-based masked beat indices in ascending order.
/// It is an empty array when no per-beat energy was supplied.
/// searched is false when the beat series was too short to score any candidate,
/// in which case every other field carries the fixed fallback rather than a
/// measurement, timeSignature.confidence included, which is 0.
/// The name confidence covers two quantities: on timeSignature it is the margin
/// over the runner-up, on a candidates entry the candidate's share of the
/// summed support. They are not comparable.
std::string meter_result_to_json(const MeterResult& result);

/// @brief Canonical field paths for the standalone meter estimate schema.
const std::vector<std::string>& meter_result_schema_paths();

}  // namespace sonare
