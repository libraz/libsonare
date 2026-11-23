#include "analysis/analysis_json.h"

#include <cmath>
#include <cstdint>

#include "util/constants.h"
#include "util/json.h"

namespace sonare {

namespace {

using sonare::constants::kCentsPerSemitone;
using sonare::util::json::Array;
using sonare::util::json::Object;
using sonare::util::json::Value;

/// A value that was not measured (NaN) is an absent member; -inf stays and writes null.
void put_measured(Object& object, const char* key, float value) {
  if (!std::isnan(value)) object[key] = Value(value);
}

Value time_signature_to_value(const TimeSignature& ts) {
  Object o;
  o["numerator"] = Value(ts.numerator);
  o["denominator"] = Value(ts.denominator);
  o["confidence"] = Value(ts.confidence);
  return Value(std::move(o));
}

const char* bpm_candidate_relation_name(BpmCandidateRelation relation) {
  switch (relation) {
    case BpmCandidateRelation::Primary:
      return "primary";
    case BpmCandidateRelation::Half:
      return "half";
    case BpmCandidateRelation::Double:
      return "double";
    case BpmCandidateRelation::Other:
      return "other";
  }
  return "other";
}

}  // namespace

const std::vector<std::string>& analysis_result_schema_paths() {
  static const std::vector<std::string> paths = {
      "bpm",
      "bpmConfidence",
      "bpmCandidates",
      "bpmCandidates[].bpm",
      "bpmCandidates[].confidence",
      "bpmCandidates[].relation",
      "key",
      "key.root",
      "key.mode",
      "key.confidence",
      "key.name",
      "key.shortName",
      "timeSignature",
      "timeSignature.numerator",
      "timeSignature.denominator",
      "timeSignature.confidence",
      "timeSignatureCandidates",
      "timeSignatureCandidates[].numerator",
      "timeSignatureCandidates[].denominator",
      "timeSignatureCandidates[].confidence",
      "beats",
      "beats[].timeSec",
      "beats[].strength",
      "downbeats",
      "downbeats[].timeSec",
      "downbeats[].strength",
      "downbeatPhase",
      "beatObservations",
      "beatObservations.accent",
      "beatObservations.hasAttack",
      "beatObservations.lowFrequencyEnergy",
      "beatObservations.subdivisionAccent",
      "beatObservations.chordChange",
      "beatLocalBpm",
      "chords",
      "chords[].root",
      "chords[].bass",
      "chords[].quality",
      "chords[].startSec",
      "chords[].endSec",
      "chords[].durationSec",
      "chords[].confidence",
      "chords[].name",
      "chords[].romanNumeral",
      "sections",
      "sections[].type",
      "sections[].startSec",
      "sections[].endSec",
      "sections[].energyLevel",
      "sections[].confidence",
      "sections[].group",
      "sections[].name",
      "timbre",
      "timbre.brightness",
      "timbre.warmth",
      "timbre.density",
      "timbre.roughness",
      "timbre.complexity",
      "dynamics",
      "dynamics.dynamicRangeDb",
      "dynamics.peakDb",
      "dynamics.rmsDb",
      "dynamics.crestFactor",
      "dynamics.loudnessRangeLu",
      "dynamics.isCompressed",
      "rhythm",
      "rhythm.timeSignature",
      "rhythm.timeSignature.numerator",
      "rhythm.timeSignature.denominator",
      "rhythm.timeSignature.confidence",
      "rhythm.syncopation",
      "rhythm.grooveType",
      "rhythm.patternRegularity",
      "rhythm.tempoStability",
      "melody",
      "melody.pitchRangeOctaves",
      "melody.pitchStability",
      "melody.meanFrequencyHz",
      "melody.vibratoRateHz",
      "melody.pitches",
      "melody.pitches[].timeSec",
      "melody.pitches[].frequencyHz",
      "melody.pitches[].confidence",
      "form",
      "tuningCents",
  };
  return paths;
}

const std::vector<std::pair<std::string, std::string>>& analysis_result_null_means() {
  static const std::vector<std::pair<std::string, std::string>> means = {
      {"dynamics.dynamicRangeDb", "Infinity"},
      {"dynamics.peakDb", "-Infinity"},
      {"dynamics.rmsDb", "-Infinity"},
  };
  return means;
}

std::string analysis_result_to_json(const AnalysisResult& result) {
  Object root;
  put_measured(root, "bpm", result.bpm);
  root["bpmConfidence"] = Value(result.bpm_confidence);
  {
    Array candidates;
    candidates.reserve(result.bpm_candidates.size());
    for (const auto& candidate : result.bpm_candidates) {
      Object value;
      value["bpm"] = Value(candidate.value);
      value["confidence"] = Value(candidate.confidence);
      value["relation"] = Value(bpm_candidate_relation_name(candidate.relation));
      candidates.push_back(Value(std::move(value)));
    }
    root["bpmCandidates"] = Value(std::move(candidates));
  }

  // Key
  {
    Object key;
    key["root"] = Value(static_cast<int>(result.key.root));
    key["mode"] = Value(static_cast<int>(result.key.mode));
    key["confidence"] = Value(result.key.confidence);
    key["name"] = Value(result.key.to_string());
    key["shortName"] = Value(result.key.to_short_string());
    root["key"] = Value(std::move(key));
  }

  root["timeSignature"] = time_signature_to_value(result.time_signature);
  {
    Array candidates;
    candidates.reserve(result.time_signature_candidates.size());
    for (const auto& candidate : result.time_signature_candidates) {
      candidates.push_back(time_signature_to_value(candidate));
    }
    root["timeSignatureCandidates"] = Value(std::move(candidates));
  }

  // Beats (time + strength) — strength is dropped by the flat C struct.
  {
    Array beats;
    beats.reserve(result.beats.size());
    for (const auto& beat : result.beats) {
      Object b;
      b["timeSec"] = Value(beat.time);
      b["strength"] = Value(beat.strength);
      beats.push_back(Value(std::move(b)));
    }
    root["beats"] = Value(std::move(beats));
  }

  // Downbeats as the beat records they select, plus the meter phase they start from.
  {
    Array downbeats;
    downbeats.reserve(result.downbeat_indices.size());
    for (int index : result.downbeat_indices) {
      const auto& beat = result.beats.at(static_cast<size_t>(index));
      Object b;
      b["timeSec"] = Value(beat.time);
      b["strength"] = Value(beat.strength);
      downbeats.push_back(Value(std::move(b)));
    }
    root["downbeats"] = Value(std::move(downbeats));
  }
  root["downbeatPhase"] = Value(result.downbeat_phase);

  {
    const auto to_array = [](const std::vector<float>& values) {
      Array out;
      out.reserve(values.size());
      for (float value : values) {
        out.push_back(Value(value));
      }
      return out;
    };
    const auto to_bool_array = [](const std::vector<std::uint8_t>& values) {
      Array out;
      out.reserve(values.size());
      for (std::uint8_t value : values) {
        out.push_back(Value(value != 0));
      }
      return out;
    };
    Object observations;
    observations["accent"] = Value(to_array(result.beat_observations.accent));
    observations["hasAttack"] = Value(to_bool_array(result.beat_observations.has_attack));
    observations["lowFrequencyEnergy"] =
        Value(to_array(result.beat_observations.low_frequency_energy));
    observations["subdivisionAccent"] =
        Value(to_array(result.beat_observations.subdivision_accent));
    observations["chordChange"] = Value(to_array(result.beat_observations.chord_change));
    root["beatObservations"] = Value(std::move(observations));

    // Beat-indexed like the streams above, and likewise empty rather than absent
    // when it was not produced — here because the caller did not ask for it.
    root["beatLocalBpm"] = Value(to_array(result.beat_local_bpm));
  }

  // Chords
  {
    Array chords;
    chords.reserve(result.chords.size());
    for (size_t i = 0; i < result.chords.size(); ++i) {
      const Chord& chord = result.chords[i];
      Object c;
      c["root"] = Value(static_cast<int>(chord.root));
      c["bass"] = Value(static_cast<int>(chord.bass));
      c["quality"] = Value(static_cast<int>(chord.quality));
      c["startSec"] = Value(chord.start);
      c["endSec"] = Value(chord.end);
      c["durationSec"] = Value(chord.end - chord.start);
      c["confidence"] = Value(chord.confidence);
      c["name"] = Value(chord.to_string());
      c["romanNumeral"] = Value(
          i < result.chord_roman_numerals.size() ? result.chord_roman_numerals[i] : std::string());
      chords.push_back(Value(std::move(c)));
    }
    root["chords"] = Value(std::move(chords));
  }

  // Sections
  {
    Array sections;
    sections.reserve(result.sections.size());
    for (const auto& section : result.sections) {
      Object s;
      s["type"] = Value(static_cast<int>(section.type));
      s["startSec"] = Value(section.start);
      s["endSec"] = Value(section.end);
      s["energyLevel"] = Value(section.energy_level);
      s["confidence"] = Value(section.confidence);
      s["group"] = Value(section.group);
      s["name"] = Value(section.type_string());
      sections.push_back(Value(std::move(s)));
    }
    root["sections"] = Value(std::move(sections));
  }

  // Timbre
  {
    Object t;
    put_measured(t, "brightness", result.timbre.brightness);
    put_measured(t, "warmth", result.timbre.warmth);
    put_measured(t, "density", result.timbre.density);
    put_measured(t, "roughness", result.timbre.roughness);
    put_measured(t, "complexity", result.timbre.complexity);
    root["timbre"] = Value(std::move(t));
  }

  // Dynamics fields shared by native and WASM unified results.
  {
    Object d;
    put_measured(d, "dynamicRangeDb", result.dynamics.dynamic_range_db);
    put_measured(d, "peakDb", result.dynamics.peak_db);
    put_measured(d, "rmsDb", result.dynamics.rms_db);
    put_measured(d, "crestFactor", result.dynamics.crest_factor);
    put_measured(d, "loudnessRangeLu", result.dynamics.loudness_range_lu);
    d["isCompressed"] = Value(result.dynamics.is_compressed);
    root["dynamics"] = Value(std::move(d));
  }

  // Rhythm fields shared by native and WASM unified results.
  {
    Object r;
    r["timeSignature"] = time_signature_to_value(result.rhythm.time_signature);
    put_measured(r, "syncopation", result.rhythm.syncopation);
    r["grooveType"] = Value(result.rhythm.groove_type);
    put_measured(r, "patternRegularity", result.rhythm.pattern_regularity);
    put_measured(r, "tempoStability", result.rhythm.tempo_stability);
    root["rhythm"] = Value(std::move(r));
  }

  // Melody contour
  {
    Object m;
    put_measured(m, "pitchRangeOctaves", result.melody.pitch_range_octaves);
    put_measured(m, "pitchStability", result.melody.pitch_stability);
    put_measured(m, "meanFrequencyHz", result.melody.mean_frequency);
    put_measured(m, "vibratoRateHz", result.melody.vibrato_rate);
    Array pitches;
    pitches.reserve(result.melody.pitches.size());
    for (const auto& point : result.melody.pitches) {
      Object p;
      p["timeSec"] = Value(point.time);
      // An unvoiced point has no frequency; the key is left out.
      if (point.frequency > 0.0f) p["frequencyHz"] = Value(point.frequency);
      p["confidence"] = Value(point.confidence);
      pitches.push_back(Value(std::move(p)));
    }
    m["pitches"] = Value(std::move(pitches));
    root["melody"] = Value(std::move(m));
  }

  root["form"] = Value(result.form);
  root["tuningCents"] = Value(result.tuning * kCentsPerSemitone);

  return sonare::util::json::dump(Value(std::move(root)));
}

const std::vector<std::string>& meter_result_schema_paths() {
  static const std::vector<std::string> paths = {
      "timeSignature",
      "timeSignature.numerator",
      "timeSignature.denominator",
      "timeSignature.confidence",
      "downbeatPhase",
      "searched",
      "grouping",
      "candidateScores",
      "candidates",
      "candidates[].numerator",
      "candidates[].denominator",
      "candidates[].confidence",
      "unobservedBeats",
  };
  return paths;
}

std::string meter_result_to_json(const MeterResult& result) {
  Object root;
  root["timeSignature"] = time_signature_to_value(result.time_signature);
  root["downbeatPhase"] = Value(result.downbeat_phase);
  root["searched"] = Value(result.searched);

  Array grouping;
  grouping.reserve(result.grouping.size());
  for (int part : result.grouping) {
    grouping.push_back(Value(part));
  }
  root["grouping"] = Value(std::move(grouping));

  Array scores;
  scores.reserve(result.candidate_scores.size());
  for (float score : result.candidate_scores) {
    scores.push_back(Value(score));
  }
  root["candidateScores"] = Value(std::move(scores));

  Array candidates;
  candidates.reserve(result.candidates.size());
  for (const auto& candidate : result.candidates) {
    candidates.push_back(time_signature_to_value(candidate));
  }
  root["candidates"] = Value(std::move(candidates));

  Array unobserved_beats;
  unobserved_beats.reserve(result.unobserved_beats.size());
  for (int beat : result.unobserved_beats) {
    unobserved_beats.push_back(Value(beat));
  }
  root["unobservedBeats"] = Value(std::move(unobserved_beats));

  return sonare::util::json::dump(Value(std::move(root)));
}

}  // namespace sonare
