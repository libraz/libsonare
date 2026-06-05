#pragma once

/// @file analysis_json.h
/// @brief Canonical JSON serializer for the unified AnalysisResult.
///
/// This is the single source of truth for the field schema returned by the
/// one-shot analyze() API across every binding. The flat C-ABI struct
/// (SonareAnalysisResult) only carries bpm/key/beats; the rich result
/// (chords, sections, timbre, dynamics, rhythm, melody, form) is exposed to
/// C/Python/Node via sonare_analyze_json, which serializes through this helper.
/// The WASM native object (analysisResultToVal) mirrors the same field names.

#include <string>
#include <vector>

#include "analysis/music_analyzer.h"

namespace sonare {

/// @brief Serializes a complete AnalysisResult to a camelCase JSON object.
/// @details Schema (every field always present):
///   {
///     "bpm": number, "bpmConfidence": number,
///     "key": {root, mode, confidence, name, shortName},
///     "timeSignature": {numerator, denominator, confidence},
///     "beats": [{time, strength}],
///     "chords": [{root, bass, quality, start, end, confidence, name}],
///     "sections": [{type, start, end, energyLevel, confidence, name}],
///     "timbre": {brightness, warmth, density, roughness, complexity},
///     "dynamics": {dynamicRangeDb, peakDb, rmsDb, crestFactor,
///                  loudnessRangeDb, isCompressed},
///     "rhythm": {timeSignature, syncopation, grooveType,
///                patternRegularity, tempoStability},
///     "melody": {pitchRangeOctaves, pitchStability, meanFrequency,
///                vibratoRate, pitches: [{time, frequency, confidence}]},
///     "form": string
///   }
/// Non-finite floats are emitted as null (handled by the JSON dumper).
std::string analysis_result_to_json(const AnalysisResult& result);

/// @brief Canonical field paths for the unified AnalysisResult schema.
/// @details Array item fields use [] (for example beats[].time). Keep the JSON
/// serializer and WASM native object in parity by testing both surfaces against
/// this list.
const std::vector<std::string>& analysis_result_schema_paths();

}  // namespace sonare
