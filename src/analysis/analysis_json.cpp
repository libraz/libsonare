#include "analysis/analysis_json.h"

#include "util/json.h"

namespace sonare {

namespace {

using sonare::util::json::Array;
using sonare::util::json::Object;
using sonare::util::json::Value;

Value time_signature_to_value(const TimeSignature& ts) {
  Object o;
  o["numerator"] = Value(ts.numerator);
  o["denominator"] = Value(ts.denominator);
  o["confidence"] = Value(ts.confidence);
  return Value(std::move(o));
}

}  // namespace

const std::vector<std::string>& analysis_result_schema_paths() {
  static const std::vector<std::string> paths = {
      "bpm",
      "bpmConfidence",
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
      "beats",
      "beats[].time",
      "beats[].strength",
      "chords",
      "chords[].root",
      "chords[].bass",
      "chords[].quality",
      "chords[].start",
      "chords[].end",
      "chords[].confidence",
      "chords[].name",
      "sections",
      "sections[].type",
      "sections[].start",
      "sections[].end",
      "sections[].energyLevel",
      "sections[].confidence",
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
      "dynamics.loudnessRangeDb",
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
      "melody.meanFrequency",
      "melody.vibratoRate",
      "melody.pitches",
      "melody.pitches[].time",
      "melody.pitches[].frequency",
      "melody.pitches[].confidence",
      "form",
  };
  return paths;
}

std::string analysis_result_to_json(const AnalysisResult& result) {
  Object root;
  root["bpm"] = Value(result.bpm);
  root["bpmConfidence"] = Value(result.bpm_confidence);

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

  // Beats (time + strength) — strength is dropped by the flat C struct.
  {
    Array beats;
    beats.reserve(result.beats.size());
    for (const auto& beat : result.beats) {
      Object b;
      b["time"] = Value(beat.time);
      b["strength"] = Value(beat.strength);
      beats.push_back(Value(std::move(b)));
    }
    root["beats"] = Value(std::move(beats));
  }

  // Chords
  {
    Array chords;
    chords.reserve(result.chords.size());
    for (const auto& chord : result.chords) {
      Object c;
      c["root"] = Value(static_cast<int>(chord.root));
      c["bass"] = Value(static_cast<int>(chord.bass));
      c["quality"] = Value(static_cast<int>(chord.quality));
      c["start"] = Value(chord.start);
      c["end"] = Value(chord.end);
      c["confidence"] = Value(chord.confidence);
      c["name"] = Value(chord.to_string());
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
      s["start"] = Value(section.start);
      s["end"] = Value(section.end);
      s["energyLevel"] = Value(section.energy_level);
      s["confidence"] = Value(section.confidence);
      s["name"] = Value(section.type_string());
      sections.push_back(Value(std::move(s)));
    }
    root["sections"] = Value(std::move(sections));
  }

  // Timbre
  {
    Object t;
    t["brightness"] = Value(result.timbre.brightness);
    t["warmth"] = Value(result.timbre.warmth);
    t["density"] = Value(result.timbre.density);
    t["roughness"] = Value(result.timbre.roughness);
    t["complexity"] = Value(result.timbre.complexity);
    root["timbre"] = Value(std::move(t));
  }

  // Dynamics fields shared by native and WASM unified results.
  {
    Object d;
    d["dynamicRangeDb"] = Value(result.dynamics.dynamic_range_db);
    d["peakDb"] = Value(result.dynamics.peak_db);
    d["rmsDb"] = Value(result.dynamics.rms_db);
    d["crestFactor"] = Value(result.dynamics.crest_factor);
    d["loudnessRangeDb"] = Value(result.dynamics.loudness_range_db);
    d["isCompressed"] = Value(result.dynamics.is_compressed);
    root["dynamics"] = Value(std::move(d));
  }

  // Rhythm fields shared by native and WASM unified results.
  {
    Object r;
    r["timeSignature"] = time_signature_to_value(result.rhythm.time_signature);
    r["syncopation"] = Value(result.rhythm.syncopation);
    r["grooveType"] = Value(result.rhythm.groove_type);
    r["patternRegularity"] = Value(result.rhythm.pattern_regularity);
    r["tempoStability"] = Value(result.rhythm.tempo_stability);
    root["rhythm"] = Value(std::move(r));
  }

  // Melody contour
  {
    Object m;
    m["pitchRangeOctaves"] = Value(result.melody.pitch_range_octaves);
    m["pitchStability"] = Value(result.melody.pitch_stability);
    m["meanFrequency"] = Value(result.melody.mean_frequency);
    m["vibratoRate"] = Value(result.melody.vibrato_rate);
    Array pitches;
    pitches.reserve(result.melody.pitches.size());
    for (const auto& point : result.melody.pitches) {
      Object p;
      p["time"] = Value(point.time);
      p["frequency"] = Value(point.frequency);
      p["confidence"] = Value(point.confidence);
      pitches.push_back(Value(std::move(p)));
    }
    m["pitches"] = Value(std::move(pitches));
    root["melody"] = Value(std::move(m));
  }

  root["form"] = Value(result.form);

  return sonare::util::json::dump(Value(std::move(root)));
}

}  // namespace sonare
