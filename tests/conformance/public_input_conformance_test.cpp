// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_voice_changer.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "c_api/synth_patch_fields.h"
#include "effects/silence.h"
#include "feature/cqt.h"
#include "util/json.h"

namespace {

using MarkerBytes = std::array<unsigned char, sizeof(SonareEngineMarker)>;

sonare::util::json::Value load_public_input_corpus() {
  std::ifstream input("tests/conformance/public_input_corpus.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

uint32_t corpus_marker_id(const sonare::util::json::Value& value) {
  if (value.is_string()) {
    REQUIRE(value.as_string() == "uint32_max");
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(value.as_number());
}

double corpus_ppq(const sonare::util::json::Value& value) {
  if (!value.is_string()) return value.as_number();
  if (value.as_string() == "nan") return std::numeric_limits<double>::quiet_NaN();
  REQUIRE(value.as_string() == "inf");
  return std::numeric_limits<double>::infinity();
}

#if defined(SONARE_WITH_MIXING)
float corpus_float(const sonare::util::json::Value& value) {
  if (!value.is_string()) return static_cast<float>(value.as_number());
  if (value.as_string() == "nan") return std::numeric_limits<float>::quiet_NaN();
  if (value.as_string() == "inf") return std::numeric_limits<float>::infinity();
  REQUIRE(value.as_string() == "neg_inf");
  return -std::numeric_limits<float>::infinity();
}
#endif  // defined(SONARE_WITH_MIXING)

std::vector<SonareEngineMarker> corpus_markers(const sonare::util::json::Value& value) {
  std::vector<SonareEngineMarker> markers;
  markers.reserve(value.size());
  for (const auto& item : value.as_array()) {
    SonareEngineMarker marker{};
    marker.id = corpus_marker_id(item["id"]);
    marker.ppq = corpus_ppq(item["ppq"]);
    const std::string& name = item["name"].as_string();
    std::strncpy(marker.name, name.c_str(), sizeof(marker.name) - 1);
    markers.push_back(marker);
  }
  return markers;
}

std::vector<MarkerBytes> marker_snapshot(SonareEngine* engine) {
  size_t count = 0;
  REQUIRE(sonare_engine_marker_count(engine, &count) == SONARE_OK);
  std::vector<MarkerBytes> snapshot(count);
  for (size_t i = 0; i < count; ++i) {
    SonareEngineMarker marker{};
    REQUIRE(sonare_engine_marker_by_index(engine, i, &marker) == SONARE_OK);
    std::memcpy(snapshot[i].data(), &marker, sizeof(marker));
  }
  return snapshot;
}

using CorpusJson = sonare::util::json::Value;
using CorpusJsonObject = sonare::util::json::Object;
using CorpusJsonArray = sonare::util::json::Array;

CorpusJson corpus_float_value(double value) {
  if (std::isnan(value)) return CorpusJson("NaN");
  if (std::isinf(value)) return CorpusJson(value > 0.0 ? "Infinity" : "-Infinity");
  return CorpusJson(value);
}

CorpusJson corpus_float_array(const float* values, size_t count) {
  CorpusJsonArray array;
  array.reserve(count);
  REQUIRE((values != nullptr || count == 0));
  for (size_t i = 0; i < count; ++i) {
    array.push_back(corpus_float_value(values[i]));
  }
  return CorpusJson(std::move(array));
}

void corpus_put_measured(CorpusJsonObject& object, const char* key, double value) {
  if (!std::isnan(value)) object.emplace(key, corpus_float_value(value));
}

CorpusJson corpus_object(CorpusJsonObject object) { return CorpusJson(std::move(object)); }

CorpusJson corpus_synth_enum_names(int kind) {
  const char* raw = sonare_synth_enum_names(kind);
  REQUIRE(raw != nullptr);
  const std::string joined(raw);
  CorpusJsonArray names;
  size_t start = 0;
  while (start < joined.size()) {
    const size_t end = joined.find('\n', start);
    names.emplace_back(joined.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return CorpusJson(std::move(names));
}

CorpusJson synth_enum_tables_corpus_result() {
  static constexpr std::array<std::pair<const char*, int>, 19> kFields{{
      {"engineModes", SONARE_SYNTH_ENUM_ENGINE_MODE},
      {"waveforms", SONARE_SYNTH_ENUM_OSC_WAVEFORM},
      {"builtinWaveforms", SONARE_SYNTH_ENUM_BUILTIN_WAVEFORM},
      {"filterModels", SONARE_SYNTH_ENUM_FILTER_MODEL},
      {"filterOutputs", SONARE_SYNTH_ENUM_FILTER_OUTPUT},
      {"bodyTypes", SONARE_SYNTH_ENUM_BODY_TYPE},
      {"modSources", SONARE_SYNTH_ENUM_MOD_SOURCE},
      {"modDestinations", SONARE_SYNTH_ENUM_MOD_DESTINATION},
      {"controllerInputs", SONARE_SYNTH_ENUM_CONTROLLER_INPUT},
      {"controllerAxes", SONARE_SYNTH_ENUM_CONTROLLER_AXIS},
      {"articulations", SONARE_SYNTH_ENUM_ARTICULATION},
      {"mpeDimensions", SONARE_SYNTH_ENUM_MPE_DIMENSION},
      {"noteTrackings", SONARE_SYNTH_ENUM_NOTE_TRACKING},
      {"sampleLoopModes", SONARE_SYNTH_ENUM_SAMPLE_LOOP_MODE},
      {"sampleKeyTracks", SONARE_SYNTH_ENUM_SAMPLE_KEY_TRACK},
      {"retriggers", SONARE_SYNTH_ENUM_RETRIGGER},
      {"partRigModes", SONARE_SYNTH_ENUM_PART_RIG_MODE},
      {"sampleDescLoopModes", SONARE_SYNTH_ENUM_SAMPLE_DESC_LOOP_MODE},
      {"lfoShapes", SONARE_SYNTH_ENUM_LFO_SHAPE},
  }};
  CorpusJsonObject result;
  for (const auto& [name, kind] : kFields) {
    result.emplace(name, corpus_synth_enum_names(kind));
  }
  return corpus_object(std::move(result));
}

std::string corpus_synth_enum_name(int ordinal, sonare::c_api::synth_names::NameTable names) {
  REQUIRE(ordinal >= 0);
  REQUIRE(static_cast<size_t>(ordinal) < names.count);
  return names.names[static_cast<size_t>(ordinal)];
}

CorpusJson synth_preset_patch_corpus_result(const SonareSynthPatch& patch) {
  using namespace sonare::c_api::synth_names;
  CorpusJsonObject result;
  if (patch.preset[0] != '\0') result.emplace("preset", CorpusJson(patch.preset));
  result.emplace("engineMode",
                 CorpusJson(corpus_synth_enum_name(patch.engine_mode, name_table(kEngineModes))));
  for (const auto& field : sonare_c_detail::kSynthPatchEnumFields) {
    result.emplace(field.name,
                   CorpusJson(corpus_synth_enum_name(patch.*field.member, field.values)));
  }
  for (const auto& field : sonare_c_detail::kSynthPatchNumericFields) {
    if (std::holds_alternative<float SonareSynthPatch::*>(field.member)) {
      result.emplace(field.name,
                     corpus_float_value(patch.*std::get<float SonareSynthPatch::*>(field.member)));
    } else {
      result.emplace(
          field.name,
          CorpusJson(static_cast<double>(patch.*std::get<int SonareSynthPatch::*>(field.member))));
    }
  }
  result.insert_or_assign("oneShot", CorpusJson(patch.one_shot != 0));
  CorpusJsonArray routings;
  REQUIRE(patch.num_mod_routings >= 0);
  REQUIRE(patch.num_mod_routings <= SONARE_SYNTH_PATCH_MOD_ROUTINGS);
  for (int i = 0; i < patch.num_mod_routings; ++i) {
    const auto& routing = patch.mod_routings[i];
    routings.emplace_back(corpus_object({
        {"source", CorpusJson(corpus_synth_enum_name(routing.source, name_table(kModSources)))},
        {"destination",
         CorpusJson(corpus_synth_enum_name(routing.destination, name_table(kModDestinations)))},
        {"depth", corpus_float_value(routing.depth)},
    }));
  }
  result.emplace("modRoutings", CorpusJson(std::move(routings)));
  return corpus_object(std::move(result));
}

CorpusJson read_corpus_json(const char* path) {
  std::ifstream input(path);
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

std::vector<float> result_corpus_fixture(const std::string& fixture, size_t frames) {
  if (fixture == "impulsive") {
    std::vector<float> input(512);
    for (size_t i = 0; i < input.size(); ++i) {
      input[i] = i == 64 || (i >= 256 && i < 260) ? 1.0f : (static_cast<int>(i % 23) - 11) / 128.0f;
    }
    return input;
  }
  if (fixture == "silence_excerpt") return std::vector<float>(8192);
  if (fixture.rfind("historical_", 0) == 0 || fixture == "meter_accents" ||
      fixture == "decay_envelope" || fixture == "lone_impulse") {
    static const auto signals = read_corpus_json("tests/conformance/public_result_signals.json");
    std::vector<float> samples;
    const auto& input =
        signals[fixture == "historical_excerpt" ? "historical_left" : fixture].as_array();
    const size_t count = fixture == "historical_excerpt" ? 8192 : input.size();
    REQUIRE(count <= input.size());
    for (size_t i = 0; i < count; ++i) {
      const auto& value = input[i];
      samples.push_back(static_cast<float>(value.as_number()));
    }
    return samples;
  }
  if (fixture == "empty" || fixture == "midi" || fixture == "tagged_wav") return {};
  std::vector<float> samples(frames, 0.0f);
  if (fixture == "silence" || fixture == "profile_silence") return samples;
  if (fixture != "tone" && fixture != "clipped" && fixture != "gapped") {
    FAIL("unknown one-shot result corpus fixture");
    return {};
  }
  for (size_t i = 0; i < frames; ++i) {
    if (fixture == "gapped" && (i < frames / 2 || i >= frames / 2 + 256)) continue;
    if (fixture == "clipped" && i % 109 < 8) {
      samples[i] = 1.0f;
      continue;
    }
    const double value = (static_cast<double>(static_cast<int>(i % 109) - 54) +
                          static_cast<double>(static_cast<int>(i % 47) - 23) * 0.5) /
                         (i < frames / 2 ? 512.0 : 256.0);
    samples[i] = static_cast<float>(value);
  }
  return samples;
}

CorpusJson boundaries_corpus_result(const SonareBoundaryResult& result) {
  CorpusJsonArray boundaries;
  boundaries.reserve(result.boundary_count);
  REQUIRE((result.boundaries != nullptr || result.boundary_count == 0));
  for (size_t i = 0; i < result.boundary_count; ++i) {
    const auto& boundary = result.boundaries[i];
    CorpusJsonObject item;
    item.emplace("timeSec", corpus_float_value(boundary.time_sec));
    item.emplace("frame", CorpusJson(static_cast<double>(boundary.frame)));
    item.emplace("strength", corpus_float_value(boundary.strength));
    boundaries.emplace_back(std::move(item));
  }
  CorpusJsonArray novelty;
  novelty.reserve(result.novelty_length);
  REQUIRE((result.novelty_curve != nullptr || result.novelty_length == 0));
  for (size_t i = 0; i < result.novelty_length; ++i) {
    novelty.push_back(corpus_float_value(result.novelty_curve[i]));
  }
  CorpusJsonObject out;
  out.emplace("boundaries", CorpusJson(std::move(boundaries)));
  out.emplace("noveltyCurve", corpus_float_array(result.novelty_curve, result.novelty_length));
  // The JS facade exposes a peak only when it can be used to restore the raw curve.
  if (!std::isnan(result.novelty_peak) && result.novelty_peak > 0.0f) {
    out.emplace("noveltyPeak", corpus_float_value(result.novelty_peak));
  }
  out.emplace("sampleRate", CorpusJson(static_cast<double>(result.sample_rate)));
  out.emplace("hopLength", CorpusJson(static_cast<double>(result.hop_length)));
  out.emplace("frameStride", CorpusJson(static_cast<double>(result.frame_stride)));
  return corpus_object(std::move(out));
}

CorpusJson clipping_corpus_result(const SonareClipDetection& result) {
  CorpusJsonObject out;
  out.emplace("clippedSamples", CorpusJson(static_cast<double>(result.clipped_samples)));
  out.emplace("clippedRatio", corpus_float_value(result.clipped_ratio));
  out.emplace("runCount", CorpusJson(static_cast<double>(result.run_count)));
  out.emplace("longestRunSamples", CorpusJson(static_cast<double>(result.longest_run_samples)));
  out.emplace("flatRunCount", CorpusJson(static_cast<double>(result.flat_run_count)));
  out.emplace("longestFlatRunSamples",
              CorpusJson(static_cast<double>(result.longest_flat_run_samples)));
  out.emplace("flatSampleCount", CorpusJson(static_cast<double>(result.flat_sample_count)));
  if (result.flat_run_count != 0) out.emplace("flatLevel", corpus_float_value(result.flat_level));
  return corpus_object(std::move(out));
}

CorpusJson dynamics_corpus_result(const SonareDynamicsResult& result) {
  CorpusJsonObject out;
  corpus_put_measured(out, "dynamicRangeDb", result.dynamic_range_db);
  corpus_put_measured(out, "peakDb", result.peak_db);
  corpus_put_measured(out, "rmsDb", result.rms_db);
  corpus_put_measured(out, "crestFactor", result.crest_factor);
  corpus_put_measured(out, "loudnessRangeLu", result.loudness_range_lu);
  out.emplace("isCompressed", CorpusJson(result.is_compressed != 0));
  out.emplace("loudnessTimesSec",
              corpus_float_array(result.loudness_times_sec, result.loudness_count));
  out.emplace("loudnessRmsDb", corpus_float_array(result.loudness_rms_db, result.loudness_count));
  return corpus_object(std::move(out));
}

CorpusJson timbre_corpus_result(const SonareTimbreResult& result) {
  CorpusJsonObject out;
  corpus_put_measured(out, "brightness", result.brightness);
  corpus_put_measured(out, "warmth", result.warmth);
  corpus_put_measured(out, "density", result.density);
  corpus_put_measured(out, "roughness", result.roughness);
  corpus_put_measured(out, "complexity", result.complexity);
  out.emplace("spectralCentroid",
              corpus_float_array(result.spectral_centroid, result.spectral_centroid_count));
  out.emplace("spectralFlatness",
              corpus_float_array(result.spectral_flatness, result.spectral_flatness_count));
  out.emplace("spectralRolloff",
              corpus_float_array(result.spectral_rolloff, result.spectral_rolloff_count));
  CorpusJsonArray over_time;
  over_time.reserve(result.timbre_over_time_count);
  REQUIRE((result.timbre_over_time != nullptr || result.timbre_over_time_count == 0));
  for (size_t i = 0; i < result.timbre_over_time_count; ++i) {
    CorpusJsonObject frame;
    corpus_put_measured(frame, "brightness", result.timbre_over_time[i].brightness);
    corpus_put_measured(frame, "warmth", result.timbre_over_time[i].warmth);
    corpus_put_measured(frame, "density", result.timbre_over_time[i].density);
    corpus_put_measured(frame, "roughness", result.timbre_over_time[i].roughness);
    corpus_put_measured(frame, "complexity", result.timbre_over_time[i].complexity);
    over_time.emplace_back(std::move(frame));
  }
  out.emplace("timbreOverTime", CorpusJson(std::move(over_time)));
  return corpus_object(std::move(out));
}

CorpusJson lufs_corpus_result(const SonareLufsResult& result) {
  CorpusJsonObject out;
  corpus_put_measured(out, "integratedLufs", result.integrated_lufs);
  corpus_put_measured(out, "momentaryLufs", result.momentary_lufs);
  corpus_put_measured(out, "shortTermLufs", result.short_term_lufs);
  corpus_put_measured(out, "maxMomentaryLufs", result.max_momentary_lufs);
  corpus_put_measured(out, "maxShortTermLufs", result.max_short_term_lufs);
  corpus_put_measured(out, "loudnessRangeLu", result.loudness_range_lu);
  return corpus_object(std::move(out));
}

CorpusJson dynamic_range_corpus_result(const SonareDynamicRangeResult& result) {
  CorpusJsonObject out;
  corpus_put_measured(out, "dynamicRangeDb", result.dynamic_range_db);
  corpus_put_measured(out, "lowPercentileDb", result.low_percentile_db);
  corpus_put_measured(out, "highPercentileDb", result.high_percentile_db);
  out.emplace("windowRmsDb", corpus_float_array(result.window_rms_db, result.window_count));
  return corpus_object(std::move(out));
}

CorpusJson trim_corpus_result(const float* audio, size_t count, int64_t start_sample,
                              int64_t end_sample) {
  CorpusJsonObject out;
  out.emplace("audio", corpus_float_array(audio, count));
  out.emplace("startSample", CorpusJson(static_cast<double>(start_sample)));
  out.emplace("endSample", CorpusJson(static_cast<double>(end_sample)));
  return corpus_object(std::move(out));
}

struct CorpusNullPathPart {
  std::string key;
  bool each = false;
};

std::vector<CorpusNullPathPart> corpus_null_path(const std::string& path) {
  std::vector<CorpusNullPathPart> parts;
  size_t begin = 0;
  while (begin < path.size()) {
    const size_t end = path.find_first_of(".", begin);
    std::string key = path.substr(begin, end == std::string::npos ? end : end - begin);
    const bool each = key.size() >= 2 && key.compare(key.size() - 2, 2, "[]") == 0;
    if (each) key.resize(key.size() - 2);
    parts.push_back({std::move(key), each});
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return parts;
}

void corpus_replace_null_path(CorpusJson& value, const std::vector<CorpusNullPathPart>& path,
                              size_t index, const std::string& replacement) {
  if (index == path.size()) {
    if (value.is_null()) value = CorpusJson(replacement);
    return;
  }
  if (!value.is_object()) return;
  auto found = value.as_object().find(path[index].key);
  if (found == value.as_object().end()) return;
  if (path[index].each) {
    if (!found->second.is_array()) return;
    for (auto& item : found->second.as_array()) {
      corpus_replace_null_path(item, path, index + 1, replacement);
    }
    return;
  }
  corpus_replace_null_path(found->second, path, index + 1, replacement);
}

bool corpus_contains_null(const CorpusJson& value) {
  if (value.is_null()) return true;
  if (value.is_array()) {
    for (const auto& item : value.as_array()) {
      if (corpus_contains_null(item)) return true;
    }
  } else if (value.is_object()) {
    for (const auto& [key, item] : value.as_object()) {
      (void)key;
      if (corpus_contains_null(item)) return true;
    }
  }
  return false;
}

CorpusJson schema_corpus_result(const char* raw, const char* family) {
  REQUIRE(raw != nullptr);
  CorpusJson result = sonare::util::json::parse_strict(raw);

  static const auto schema = read_corpus_json("schemas/result-null-means.json");
  const auto& null_means = schema["families"][family]["nullMeans"];
  for (const auto& [path, meaning] : null_means.as_object()) {
    corpus_replace_null_path(result, corpus_null_path(path), 0, meaning.as_string());
  }
  REQUIRE_FALSE(corpus_contains_null(result));
  return result;
}

CorpusJson profile_corpus_result(const char* raw) {
  return schema_corpus_result(raw, "audio_profile");
}

struct CorpusCallResult {
  SonareError error = SONARE_OK;
  CorpusJson result;
};

CorpusCallResult mix_assistant_corpus_result(const std::vector<float>& samples, int sample_rate) {
  const std::vector<float> silence(samples.size(), 0.0f);
  const float* inputs[] = {samples.data(), silence.data()};
  const char* ids[] = {"tone", "silent"};
  const size_t lengths[] = {samples.size(), silence.size()};
  char* raw = nullptr;
  const SonareError error = sonare_mixing_assistant_suggest(
      inputs, nullptr, ids, nullptr, lengths, nullptr, 2, sample_rate, nullptr, 0, &raw);
  CorpusJson result;
  if (error == SONARE_OK) {
    result = schema_corpus_result(raw, "mix_assistant_result");
    for (auto& track : result.as_object().at("tracks").as_array()) {
      auto& fields = track.as_object();
      // The ABI's empty exclusion reason represents a usable track.
      const auto exclusion = fields.find("exclusionReason");
      if (exclusion != fields.end() && exclusion->second.as_string().empty())
        fields.erase(exclusion);
    }
  }
  sonare_free_string(raw);
  return {error, std::move(result)};
}

CorpusJson midi_corpus_event(const SonareMidiEventPod& event) {
  return corpus_object({{"ppq", CorpusJson(event.ppq)},
                        {"word0", CorpusJson(static_cast<double>(event.word0))},
                        {"word1", CorpusJson(static_cast<double>(event.word1))}});
}

CorpusJson midi_corpus_events(const SonareMidiEventPod* events, size_t count) {
  CorpusJsonArray value;
  for (size_t i = 0; i < count; ++i) value.push_back(midi_corpus_event(events[i]));
  return CorpusJson(std::move(value));
}

std::optional<CorpusCallResult> run_midi_mapping_corpus(const std::string& entry,
                                                        const CorpusJson& values) {
  if (entry == "sonare_midi_bank_program") {
    std::array<SonareMidiEventPod, 3> events{};
    size_t count = 0;
    const auto error = sonare_midi_bank_program(
        values["ppq"].as_number(), values["group"].as_int(), values["channel"].as_int(),
        values["bank_msb"].as_int(), values["bank_lsb"].as_int(), values["program"].as_int(),
        events.data(), events.size(), &count);
    REQUIRE(count <= events.size());
    return CorpusCallResult{error, midi_corpus_events(events.data(), count)};
  }
  if (entry != "sonare_midi_cc_learn" && entry != "sonare_midi_cc_to_breakpoint" &&
      entry != "sonare_midi_param_to_cc" && entry != "sonare_midi_route_events") {
    return std::nullopt;
  }

  std::array<SonareMidiEventPod, 2> events{};
  if (const auto error = sonare_midi_cc(0.5, 1, 2, 74, 17, &events[0]); error != SONARE_OK) {
    return CorpusCallResult{error, {}};
  }
  if (const auto error = sonare_midi_cc(1.0, 1, 2, 74, 101, &events[1]); error != SONARE_OK) {
    return CorpusCallResult{error, {}};
  }
  if (entry == "sonare_midi_route_events") {
    std::array<SonareMidiEventPod, 2> output{};
    size_t count = 0;
    size_t overflow_count = 0;
    int overflowed = 0;
    const auto error =
        sonare_midi_route_events(events.data(), events.size(), nullptr, output.data(),
                                 output.size(), &count, &overflowed, &overflow_count);
    REQUIRE(count <= output.size());
    return CorpusCallResult{
        error, corpus_object({{"events", midi_corpus_events(output.data(), count)},
                              {"overflowed", CorpusJson(overflowed != 0)},
                              {"overflowCount", CorpusJson(static_cast<double>(overflow_count))}})};
  }
  if (entry == "sonare_midi_cc_learn") {
    SonareMidiCcBinding binding{};
    const auto error =
        sonare_midi_cc_learn(events.data(), events.size(), 9, 0.0f, 1.0f, 0, &binding);
    if (error != SONARE_OK) return CorpusCallResult{error, {}};
    REQUIRE(binding.kind == SONARE_MIDI_CC_CONTROL_CHANGE_7);
    CorpusJsonObject out{{"ccNumber", CorpusJson(binding.cc_number)},
                         {"kind", CorpusJson("controlChange7")},
                         {"ccLsbNumber", CorpusJson(binding.cc_lsb_number)},
                         {"selectorMsb", CorpusJson(binding.selector_msb)},
                         {"selectorLsb", CorpusJson(binding.selector_lsb)},
                         {"consume", CorpusJson(binding.consume != 0)},
                         {"paramId", CorpusJson(static_cast<double>(binding.param_id))},
                         {"minValue", CorpusJson(binding.min_value)},
                         {"maxValue", CorpusJson(binding.max_value)}};
    if (binding.channel != SONARE_MIDI_CC_ANY_CHANNEL) {
      out.emplace("channel", CorpusJson(binding.channel));
    }
    return CorpusCallResult{error, corpus_object(std::move(out))};
  }

  SonareMidiCcBinding binding{};
  binding.cc_number = 74;
  binding.channel = 2;
  binding.param_id = 9;
  binding.max_value = 1.0f;
  if (entry == "sonare_midi_cc_to_breakpoint") {
    SonareAutomationPoint point{};
    const auto error = sonare_midi_cc_to_breakpoint(&binding, 1, &events[1], &point);
    if (error != SONARE_OK) return CorpusCallResult{error, {}};
    REQUIRE(point.curve_to_next == SONARE_CURVE_LINEAR);
    return CorpusCallResult{error, corpus_object({{"ppq", CorpusJson(point.ppq)},
                                                  {"value", CorpusJson(point.value)},
                                                  {"curveToNext", CorpusJson("linear")}})};
  }
  SonareMidiEventPod output{};
  const auto error = sonare_midi_param_to_cc(&binding, 1, 9, 0.75f, 1, 0.0, &output);
  return CorpusCallResult{error, midi_corpus_event(output)};
}

CorpusJson corpus_diagnostics() {
  const std::array<const char*, 3> severities{"info", "warning", "error"};
  CorpusJsonArray values;
  const size_t count = sonare_last_diagnostic_count();
  for (size_t i = 0; i < count; ++i) {
    SonareDiagnostic diagnostic{};
    sonare_diagnostic_init(&diagnostic);
    REQUIRE(sonare_last_diagnostic(i, &diagnostic) == SONARE_OK);
    REQUIRE(static_cast<size_t>(diagnostic.severity) < severities.size());
    values.emplace_back(corpus_object({{"code", CorpusJson(diagnostic.code)},
                                       {"message", CorpusJson(diagnostic.message)},
                                       {"severity", CorpusJson(severities[diagnostic.severity])}}));
  }
  return CorpusJson(std::move(values));
}

std::optional<CorpusCallResult> run_timeline_corpus(const std::string& entry,
                                                    const std::vector<float>& samples,
                                                    int sample_rate) {
  if (entry == "sonare_split_silence_common") {
    const float* signals[] = {samples.data(), samples.data()};
    const size_t lengths[] = {samples.size(), samples.size() / 2};
    int* raw = nullptr;
    size_t count = 0;
    SonareSilenceCommonReport report{};
    const auto error =
        sonare_split_silence_common(signals, 2, lengths, 60.0f, 2048, 512, &raw, &count, &report);
    CorpusJsonArray intervals;
    for (size_t i = 0; i < count; ++i) intervals.emplace_back(raw[i]);
    sonare_free_ints(raw);
    CorpusJsonObject summary{{"maxSignalIntervals", CorpusJson(report.max_signal_intervals)},
                             {"minSignalIntervals", CorpusJson(report.min_signal_intervals)}};
    corpus_put_measured(summary, "silenceCeilingDb", report.silence_ceiling_db);
    return CorpusCallResult{error, corpus_object({{"intervals", CorpusJson(std::move(intervals))},
                                                  {"report", corpus_object(std::move(summary))}})};
  }
  if (entry == "sonare_align_take_to_reference") {
    SonareProjectWarpAnchor* raw = nullptr;
    size_t count = 0;
    SonareTakeAlignment alignment{};
    const auto error = sonare_align_take_to_reference(samples.data(), samples.size(), sample_rate,
                                                      samples.data(), samples.size(), sample_rate,
                                                      nullptr, &raw, &count, &alignment);
    CorpusJsonArray anchors;
    for (size_t i = 0; i < count; ++i) {
      anchors.emplace_back(corpus_object({{"warpSample", CorpusJson(raw[i].warp_sample)},
                                          {"sourceSample", CorpusJson(raw[i].source_sample)}}));
    }
    sonare_free_project_warp_anchors(raw);
    auto geometry =
        corpus_object({{"meanResidualFrames", CorpusJson(alignment.mean_residual_frames)},
                       {"referenceFrames", CorpusJson(alignment.reference_frames)},
                       {"takeFrames", CorpusJson(alignment.take_frames)}});
    return CorpusCallResult{error, corpus_object({{"anchors", CorpusJson(std::move(anchors))},
                                                  {"alignment", std::move(geometry)}})};
  }
  return std::nullopt;
}

std::optional<CorpusCallResult> run_room_corpus(const std::string& entry,
                                                const std::vector<float>& samples,
                                                int sample_rate) {
  if (entry == "sonare_room_geometry_from_estimate") {
    float bands[] = {0.25f, 0.5f, 0.75f};
    SonareRoomEstimate estimate{};
    estimate.length_m = 14.0f;
    estimate.width_m = 5.0f;
    estimate.height_m = 6.0f;
    estimate.band_absorption = bands;
    estimate.band_count = std::size(bands);
    SonareRirSynthConfig result{};
    const auto error = sonare_room_geometry_from_estimate(&estimate, &result);
    auto point = [](const SonareRoomPlacement& value) {
      return corpus_object(
          {{"x", CorpusJson(value.x)}, {"y", CorpusJson(value.y)}, {"z", CorpusJson(value.z)}});
    };
    return CorpusCallResult{
        error,
        corpus_object({{"lengthM", CorpusJson(result.length_m)},
                       {"widthM", CorpusJson(result.width_m)},
                       {"heightM", CorpusJson(result.height_m)},
                       {"source", point(result.source)},
                       {"listener", point(result.listener)},
                       {"absorption", CorpusJson(result.absorption)},
                       {"bandAbsorption", corpus_float_array(result.band_absorption,
                                                             result.band_absorption_count)}})};
  }
  if (entry == "sonare_synthesize_rir") {
    SonareRirSynthConfig config{};
    sonare_rir_synth_config_init(&config);
    config.max_sec = 0.12f;
    SonareRirSynthResult result{};
    const auto error = sonare_synthesize_rir(&config, sample_rate, &result);
    auto value = corpus_object({{"rir", corpus_float_array(result.rir, result.length)},
                                {"sampleRate", CorpusJson(result.sample_rate)},
                                {"diagnostics", corpus_diagnostics()}});
    sonare_free_rir_synth_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_room_morph" || entry == "sonare_room_morph_stereo") {
    SonareRoomMorphConfig config{};
    sonare_room_morph_config_init(&config);
    config.max_sec = 0.12f;
    float* left = nullptr;
    float* right = nullptr;
    size_t length = 0;
    CorpusJsonObject out{{"sampleRate", CorpusJson(sample_rate)}};
    SonareError error;
    if (entry == "sonare_room_morph") {
      error =
          sonare_room_morph(samples.data(), samples.size(), sample_rate, &config, &left, &length);
      out.emplace("audio", corpus_float_array(left, length));
    } else {
      const std::vector<float> right_input(samples.rbegin(), samples.rend());
      error = sonare_room_morph_stereo(samples.data(), right_input.data(), samples.size(),
                                       sample_rate, &config, &left, &right, &length);
      out.emplace("left", corpus_float_array(left, length));
      out.emplace("right", corpus_float_array(right, length));
    }
    out.emplace("diagnostics", corpus_diagnostics());
    sonare_free_floats(left);
    sonare_free_floats(right);
    return CorpusCallResult{error, corpus_object(std::move(out))};
  }
  return std::nullopt;
}

std::optional<CorpusCallResult> run_midi_corpus(const std::string& entry,
                                                const CorpusJson& values) {
  SonareMidiEventPod event{};
  SonareError error;
  if (entry == "sonare_midi_note_on") {
    error = sonare_midi_note_on(values["ppq"].as_number(),
                                static_cast<uint8_t>(values["group"].as_number()),
                                static_cast<uint8_t>(values["channel"].as_number()),
                                static_cast<uint8_t>(values["note"].as_number()),
                                static_cast<uint8_t>(values["velocity"].as_number()), &event);
  } else if (entry == "sonare_midi_note_off") {
    error = sonare_midi_note_off(values["ppq"].as_number(),
                                 static_cast<uint8_t>(values["group"].as_number()),
                                 static_cast<uint8_t>(values["channel"].as_number()),
                                 static_cast<uint8_t>(values["note"].as_number()), 0, &event);
  } else if (entry == "sonare_midi_cc") {
    error =
        sonare_midi_cc(values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
                       static_cast<uint8_t>(values["channel"].as_number()),
                       static_cast<uint8_t>(values["controller"].as_number()),
                       static_cast<uint8_t>(values["value"].as_number()), &event);
  } else if (entry == "sonare_midi_poly_pressure") {
    error = sonare_midi_poly_pressure(values["ppq"].as_number(),
                                      static_cast<uint8_t>(values["group"].as_number()),
                                      static_cast<uint8_t>(values["channel"].as_number()),
                                      static_cast<uint8_t>(values["note"].as_number()),
                                      static_cast<uint8_t>(values["pressure"].as_number()), &event);
  } else if (entry == "sonare_midi_program") {
    error = sonare_midi_program(values["ppq"].as_number(),
                                static_cast<uint8_t>(values["group"].as_number()),
                                static_cast<uint8_t>(values["channel"].as_number()),
                                static_cast<uint8_t>(values["program"].as_number()), &event);
  } else if (entry == "sonare_midi2_note_on") {
    error = sonare_midi2_note_on(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()),
        static_cast<uint16_t>(values["velocity16"].as_number()), 0, 0, &event);
  } else if (entry == "sonare_midi2_note_off") {
    error = sonare_midi2_note_off(values["ppq"].as_number(),
                                  static_cast<uint8_t>(values["group"].as_number()),
                                  static_cast<uint8_t>(values["channel"].as_number()),
                                  static_cast<uint8_t>(values["note"].as_number()), 0, &event);
  } else if (entry == "sonare_midi2_cc") {
    error = sonare_midi2_cc(values["ppq"].as_number(),
                            static_cast<uint8_t>(values["group"].as_number()),
                            static_cast<uint8_t>(values["channel"].as_number()),
                            static_cast<uint8_t>(values["controller"].as_number()),
                            static_cast<uint32_t>(values["value32"].as_number()), &event);
  } else if (entry == "sonare_midi2_poly_pressure") {
    error = sonare_midi2_poly_pressure(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()),
        static_cast<uint32_t>(values["pressure32"].as_number()), &event);
  } else if (entry == "sonare_midi2_channel_pressure") {
    error = sonare_midi2_channel_pressure(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint32_t>(values["pressure32"].as_number()), &event);
  } else if (entry == "sonare_midi2_pitch_bend") {
    error = sonare_midi2_pitch_bend(values["ppq"].as_number(),
                                    static_cast<uint8_t>(values["group"].as_number()),
                                    static_cast<uint8_t>(values["channel"].as_number()),
                                    static_cast<uint32_t>(values["bend32"].as_number()), &event);
  } else if (entry == "sonare_midi2_program") {
    error = sonare_midi2_program(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["program"].as_number()), 0, 0, 0, &event);
  } else if (entry == "sonare_midi2_registered_controller") {
    error = sonare_midi2_registered_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["bank"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<uint32_t>(values["value32"].as_number()), &event);
  } else if (entry == "sonare_midi2_assignable_controller") {
    error = sonare_midi2_assignable_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["bank"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<uint32_t>(values["value32"].as_number()), &event);
  } else if (entry == "sonare_midi2_relative_registered_controller") {
    error = sonare_midi2_relative_registered_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["bank"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<int32_t>(values["delta32"].as_number()), &event);
  } else if (entry == "sonare_midi2_relative_assignable_controller") {
    error = sonare_midi2_relative_assignable_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["bank"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<int32_t>(values["delta32"].as_number()), &event);
  } else if (entry == "sonare_midi2_registered_per_note_controller") {
    error = sonare_midi2_registered_per_note_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<uint32_t>(values["value32"].as_number()), &event);
  } else if (entry == "sonare_midi2_assignable_per_note_controller") {
    error = sonare_midi2_assignable_per_note_controller(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()),
        static_cast<uint8_t>(values["index"].as_number()),
        static_cast<uint32_t>(values["value32"].as_number()), &event);
  } else if (entry == "sonare_midi2_per_note_pitch_bend") {
    error = sonare_midi2_per_note_pitch_bend(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()),
        static_cast<uint32_t>(values["bend32"].as_number()), &event);
  } else if (entry == "sonare_midi2_per_note_management") {
    error = sonare_midi2_per_note_management(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["note"].as_number()), 0, 0, &event);
  } else if (entry == "sonare_midi_channel_pressure") {
    error = sonare_midi_channel_pressure(
        values["ppq"].as_number(), static_cast<uint8_t>(values["group"].as_number()),
        static_cast<uint8_t>(values["channel"].as_number()),
        static_cast<uint8_t>(values["pressure"].as_number()), &event);
  } else if (entry == "sonare_midi_pitch_bend") {
    error = sonare_midi_pitch_bend(values["ppq"].as_number(),
                                   static_cast<uint8_t>(values["group"].as_number()),
                                   static_cast<uint8_t>(values["channel"].as_number()),
                                   static_cast<uint16_t>(values["bend"].as_number()), &event);
  } else {
    return std::nullopt;
  }
  if (error != SONARE_OK) return CorpusCallResult{error, CorpusJson()};
  REQUIRE(event.sysex_size == 0);
  return CorpusCallResult{error, corpus_object({
                                     {"ppq", CorpusJson(event.ppq)},
                                     {"word0", CorpusJson(static_cast<double>(event.word0))},
                                     {"word1", CorpusJson(static_cast<double>(event.word1))},
                                 })};
}

CorpusJson repair_result_report(const SonareDeclickReport& report) {
  const auto& detection = report.detected;
  return corpus_object(
      {{"detected",
        corpus_object(
            {{"count", CorpusJson(static_cast<double>(detection.count))},
             {"rejected", CorpusJson(static_cast<double>(detection.rejected))},
             {"longestRunSamples", CorpusJson(static_cast<double>(detection.longest_run_samples))},
             {"perSecond", corpus_float_value(detection.per_second)}})},
       {"repairedRuns", CorpusJson(static_cast<double>(report.repaired_runs))},
       {"repairedSamples", CorpusJson(static_cast<double>(report.repaired_samples))},
       {"linkedRuns", CorpusJson(static_cast<double>(report.linked_runs))},
       {"lpcModelUsed", CorpusJson(report.lpc_model_used != 0)}});
}

CorpusJson repair_result_report(const SonareDeclipReport& report) {
  return corpus_object(
      {{"detected", clipping_corpus_result(report.detected)},
       {"lpcReconstructedRuns", CorpusJson(static_cast<double>(report.lpc_reconstructed_runs))},
       {"unreconstructedRuns", CorpusJson(static_cast<double>(report.unreconstructed_runs))},
       {"repairedSamples", CorpusJson(static_cast<double>(report.repaired_samples))},
       {"linkedRuns", CorpusJson(static_cast<double>(report.linked_runs))}});
}

CorpusJson repair_result_report(const SonareDecrackleReport& report) {
  const auto& detection = report.detected;
  return corpus_object(
      {{"detected",
        corpus_object({{"sampleCount", CorpusJson(static_cast<double>(detection.sample_count))},
                       {"sampleFraction", corpus_float_value(detection.sample_fraction)},
                       {"perSecond", corpus_float_value(detection.per_second)}})},
       {"replacedSamples", CorpusJson(static_cast<double>(report.replaced_samples))},
       {"detailCoefficients", CorpusJson(static_cast<double>(report.detail_coefficients))},
       {"shrunkCoefficients", CorpusJson(static_cast<double>(report.shrunk_coefficients))},
       {"noiseSigma", corpus_float_value(report.noise_sigma)}});
}

CorpusJson repair_result_report(const SonareDehumReport& report) {
  const auto& detection = report.detected;
  CorpusJsonArray harmonics;
  for (float dbfs : detection.harmonic_dbfs) {
    if (std::isnan(dbfs)) break;
    harmonics.push_back(corpus_float_value(dbfs));
  }
  return corpus_object(
      {{"detected", corpus_object({{"fundamentalHz", corpus_float_value(detection.fundamental_hz)},
                                   {"fundamentalProminence",
                                    corpus_float_value(detection.fundamental_prominence)},
                                   {"harmonics", CorpusJson(detection.harmonics)},
                                   {"harmonicDbfs", CorpusJson(std::move(harmonics))}})},
       {"notchedHarmonics", CorpusJson(report.notched_harmonics)},
       {"appliedFundamentalHz", corpus_float_value(report.applied_fundamental_hz)},
       {"fundamentalDriftHz", corpus_float_value(report.fundamental_drift_hz)}});
}

CorpusJson repair_result_report(const SonareDenoiseReport& report) {
  const auto& detection = report.detected;
  CorpusJsonArray levels;
  CorpusJsonArray bands;
  for (size_t i = 0; i < detection.band_count; ++i) {
    levels.push_back(corpus_float_value(detection.band_floor_dbfs[i]));
    bands.emplace_back(detection.band_floor_band[i]);
  }
  CorpusJsonObject detected;
  corpus_put_measured(detected, "floorDbfs", detection.floor_dbfs);
  detected.emplace("bandFloorDbfs", CorpusJson(std::move(levels)));
  detected.emplace("bandFloorBand", CorpusJson(std::move(bands)));
  return corpus_object(
      {{"detected", corpus_object(std::move(detected))},
       {"meanReductionDb", corpus_float_value(report.mean_reduction_db)},
       {"maxReductionDb", corpus_float_value(report.max_reduction_db)},
       {"floorLimitedFraction", corpus_float_value(report.floor_limited_fraction)}});
}

CorpusJson repair_result_report(const SonareDereverbReport& report) {
  const auto& detection = report.detected;
  CorpusJsonObject detected;
  if (detection.has_late_decay_ratio_db != 0) {
    detected.emplace("lateDecayRatioDb", corpus_float_value(detection.late_decay_ratio_db));
  }
  if (detection.has_late_predictability != 0) {
    detected.emplace("latePredictability", corpus_float_value(detection.late_predictability));
  }
  return corpus_object({{"detected", corpus_object(std::move(detected))},
                        {"meanReductionDb", corpus_float_value(report.mean_reduction_db)},
                        {"suppressedFraction", corpus_float_value(report.suppressed_fraction)},
                        {"wpePredictorNorm", corpus_float_value(report.wpe_predictor_norm)}});
}

template <typename Result>
CorpusJson repair_result_mono(const Result& out, SonareError error) {
  if (error != SONARE_OK) return {};
  return corpus_object({{"samples", corpus_float_array(out.samples, out.length)},
                        {"report", repair_result_report(out.report)}});
}

template <typename Result>
CorpusJson repair_result_channel_stereo(const Result& out, SonareError error) {
  if (error != SONARE_OK) return {};
  return corpus_object(
      {{"channels", CorpusJson(CorpusJsonArray{corpus_float_array(out.left, out.length),
                                               corpus_float_array(out.right, out.length)})},
       {"reports", CorpusJson(CorpusJsonArray{repair_result_report(out.left_report),
                                              repair_result_report(out.right_report)})}});
}

template <typename Result>
CorpusJson repair_result_linked(CorpusJsonArray channels, const Result& out) {
  return corpus_object(
      {{"channels", CorpusJson(std::move(channels))},
       {"reports", CorpusJson(CorpusJsonArray{repair_result_report(out.report)})}});
}

std::optional<CorpusCallResult> run_master_corpus(const std::string& entry,
                                                  const std::vector<float>& samples,
                                                  int sample_rate, const std::string& fixture) {
  const float* data = samples.empty() ? nullptr : samples.data();
  const size_t length = samples.size();
  const auto right_fixture = [&]() {
    return fixture == "silence" ? std::vector<float>(length, 0.0f)
                                : result_corpus_fixture("historical_right", length);
  };
  const auto analyze_pair_json = [](const char* raw, const char* family) {
    REQUIRE(raw != nullptr);
    CorpusJson result = sonare::util::json::parse_strict(raw);
    std::ifstream input("schemas/result-null-means.json");
    REQUIRE(input.good());
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const CorpusJson null_means =
        sonare::util::json::parse_strict(text)["families"][family]["nullMeans"];
    for (const auto& [path, meaning] : null_means.as_object()) {
      corpus_replace_null_path(result, corpus_null_path(path), 0, meaning.as_string());
    }
    REQUIRE_FALSE(corpus_contains_null(result));
    return result;
  };

  const auto dynamics = [&](auto process) {
    float* output = nullptr;
    size_t output_length = 0;
    int latency_samples = 0;
    const SonareError error =
        process(data, length, sample_rate, nullptr, &output, &output_length, &latency_samples);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject fields;
      fields.emplace("samples", corpus_float_array(output, output_length));
      fields.emplace("latencySamples", CorpusJson(latency_samples));
      value = corpus_object(std::move(fields));
    }
    sonare_free_floats(output);
    return CorpusCallResult{error, std::move(value)};
  };

  const auto chain_stage_names = [](char** stages, size_t count) {
    CorpusJsonArray values;
    REQUIRE((stages != nullptr || count == 0));
    values.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      REQUIRE(stages[i] != nullptr);
      values.emplace_back(stages[i]);
    }
    return CorpusJson(std::move(values));
  };
  const auto chain_report_json = [](const SonareMasteringReport& report) {
    const auto summary_json = [](const SonareMasteringLoudnessSummary& summary) {
      CorpusJsonObject value;
      corpus_put_measured(value, "integratedLufs", summary.integrated_lufs);
      corpus_put_measured(value, "maxMomentaryLufs", summary.max_momentary_lufs);
      corpus_put_measured(value, "maxShortTermLufs", summary.max_short_term_lufs);
      corpus_put_measured(value, "truePeakDbtp", summary.true_peak_dbtp);
      corpus_put_measured(value, "loudnessRangeLu", summary.loudness_range_lu);
      return corpus_object(std::move(value));
    };
    CorpusJsonObject value;
    value.emplace("before", summary_json(report.before));
    value.emplace("after", summary_json(report.after));
    value.emplace("appliedGainDb", corpus_float_value(report.applied_gain_db));
    value.emplace("maxGainReductionDb", corpus_float_value(report.max_gain_reduction_db));
    value.emplace("loudnessTargetLimited", CorpusJson(report.loudness_target_limited != 0));
    value.emplace("bandEnergyDeltaDb", corpus_float_array(report.band_energy_delta_db,
                                                          SONARE_MASTERING_REPORT_BAND_COUNT));
    corpus_put_measured(value, "loudnessGainDb", report.loudness_gain_db);
    return corpus_object(std::move(value));
  };
  const auto chain_json = [&](const auto& result) {
    CorpusJsonObject value;
    value.emplace("sampleRate", CorpusJson(result.sample_rate));
    value.emplace("inputLufs", corpus_float_value(result.input_lufs));
    value.emplace("outputLufs", corpus_float_value(result.output_lufs));
    value.emplace("appliedGainDb", corpus_float_value(result.applied_gain_db));
    value.emplace("stages", chain_stage_names(result.stages, result.stages_count));
    value.emplace("outputTruePeakDbtp", corpus_float_value(result.output_true_peak_dbtp));
    corpus_put_measured(value, "outputLoudnessRangeLu", result.output_loudness_range_lu);
    corpus_put_measured(value, "loudnessGainDb", result.loudness_gain_db);
    value.emplace("loudnessTargetLimited", CorpusJson(result.loudness_target_limited != 0));
    CorpusJsonArray reductions;
    REQUIRE(
        (result.stage_gain_reduction_stages != nullptr || result.stage_gain_reductions_count == 0));
    REQUIRE(
        (result.stage_gain_reduction_values != nullptr || result.stage_gain_reductions_count == 0));
    reductions.reserve(result.stage_gain_reductions_count);
    for (size_t i = 0; i < result.stage_gain_reductions_count; ++i) {
      REQUIRE(result.stage_gain_reduction_stages[i] != nullptr);
      reductions.emplace_back(corpus_object(
          {{"stage", CorpusJson(result.stage_gain_reduction_stages[i])},
           {"gainReductionDb", corpus_float_value(result.stage_gain_reduction_values[i])}}));
    }
    value.emplace("stageGainReductions", CorpusJson(std::move(reductions)));
    value.emplace("report", chain_report_json(result.report));
    value.emplace("nonFiniteSubstitutionCount",
                  CorpusJson(static_cast<double>(result.non_finite_substitution_count)));
    if (result.has_declip_unreconstructed_runs != 0) {
      value.emplace("declipUnreconstructedRuns",
                    CorpusJson(static_cast<double>(result.declip_unreconstructed_runs)));
    }
    return value;
  };
  if (entry == "sonare_master_audio") {
    SonareMasterAudioOptions options{};
    sonare_master_audio_options_init(&options);
    SonareMasteringChainResult result{};
    const SonareError error =
        sonare_master_audio(nullptr, data, length, sample_rate, "{}", &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      auto fields = chain_json(result);
      fields.emplace("samples", corpus_float_array(result.samples, result.length));
      value = corpus_object(std::move(fields));
    }
    sonare_free_mastering_chain_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_master_audio_stereo") {
    SonareMasterAudioOptions options{};
    sonare_master_audio_options_init(&options);
    SonareMasteringChainStereoResult result{};
    const SonareError error = sonare_master_audio_stereo(nullptr, data, data, length, sample_rate,
                                                         "{}", &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      auto fields = chain_json(result);
      fields.emplace("left", corpus_float_array(result.left, result.length));
      fields.emplace("right", corpus_float_array(result.right, result.length));
      value = corpus_object(std::move(fields));
    }
    sonare_free_mastering_chain_stereo_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_dynamics_compressor") {
    return dynamics(sonare_mastering_dynamics_compressor);
  }
  if (entry == "sonare_mastering_dynamics_gate") {
    return dynamics(sonare_mastering_dynamics_gate);
  }
  if (entry == "sonare_mastering_dynamics_transient_shaper") {
    return dynamics(sonare_mastering_dynamics_transient_shaper);
  }

  if (entry == "sonare_mastering_audio_profile_stereo") {
    std::vector<float> right = right_fixture();
    REQUIRE(right.size() == length);
    char* raw = nullptr;
    const SonareError error = sonare_mastering_audio_profile_stereo(
        data, right.empty() ? nullptr : right.data(), length, sample_rate, nullptr, 0, &raw);
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return CorpusCallResult{error, CorpusJson()};
    }
    CorpusJson result = profile_corpus_result(raw);
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(result)};
  }

  if (entry == "sonare_streaming_loudness_gain") {
    SonareStreamingLoudnessGain gain{};
    const SonareError error =
        sonare_streaming_loudness_gain(data, length, sample_rate, nullptr, &gain);
    CorpusJsonObject result;
    corpus_put_measured(result, "loudnessStaticGainDb", gain.loudness_static_gain_db);
    corpus_put_measured(result, "truePeakDbtp", gain.true_peak_dbtp);
    corpus_put_measured(result, "integratedLufs", gain.integrated_lufs);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_streaming_loudness_gain_stereo") {
    std::vector<float> right = right_fixture();
    REQUIRE(right.size() == length);
    SonareStreamingLoudnessGain gain{};
    const SonareError error = sonare_streaming_loudness_gain_stereo(
        data, right.empty() ? nullptr : right.data(), length, sample_rate, nullptr, &gain);
    CorpusJsonObject result;
    corpus_put_measured(result, "loudnessStaticGainDb", gain.loudness_static_gain_db);
    corpus_put_measured(result, "truePeakDbtp", gain.true_peak_dbtp);
    corpus_put_measured(result, "integratedLufs", gain.integrated_lufs);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_insert_timing") {
    int latency_samples = 0;
    int tail_samples = 0;
    const SonareError error = sonare_mastering_insert_timing("eq.parametric", nullptr, sample_rate,
                                                             &latency_samples, &tail_samples);
    CorpusJsonObject result;
    result.emplace("latencySamples", CorpusJson(latency_samples));
    result.emplace("tailSamples", CorpusJson(tail_samples));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_declick") {
    SonareDeclickResult out{};
    const auto error = sonare_mastering_repair_declick(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_declick_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_declick_stereo") {
    const std::vector<float> right(samples.rbegin(), samples.rend());
    SonareDeclickStereoResult out{};
    const auto error = sonare_mastering_repair_declick_stereo(data, right.data(), length,
                                                              sample_rate, nullptr, &out);
    auto value = repair_result_channel_stereo(out, error);
    sonare_free_declick_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_mastering_repair_declip") {
    SonareDeclipResult out{};
    const auto error = sonare_mastering_repair_declip(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_declip_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_declip_stereo") {
    const std::vector<float> right(samples.rbegin(), samples.rend());
    SonareDeclipStereoResult out{};
    const auto error = sonare_mastering_repair_declip_stereo(data, right.data(), length,
                                                             sample_rate, nullptr, &out);
    auto value = repair_result_channel_stereo(out, error);
    sonare_free_declip_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_mastering_repair_decrackle") {
    SonareDecrackleResult out{};
    const auto error = sonare_mastering_repair_decrackle(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_decrackle_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_decrackle_stereo") {
    const std::vector<float> right(samples.rbegin(), samples.rend());
    SonareDecrackleStereoResult out{};
    const auto error = sonare_mastering_repair_decrackle_stereo(data, right.data(), length,
                                                                sample_rate, nullptr, &out);
    auto value = repair_result_channel_stereo(out, error);
    sonare_free_decrackle_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_mastering_repair_dehum") {
    SonareDehumResult out{};
    const auto error = sonare_mastering_repair_dehum(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_dehum_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_dehum_stereo") {
    SonareDehumStereoResult out{};
    const auto error =
        sonare_mastering_repair_dehum_stereo(data, data, length, sample_rate, nullptr, &out);
    CorpusJson value;
    value = repair_result_channel_stereo(out, error);
    sonare_free_dehum_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_denoise_classical") {
    SonareDenoiseResult out{};
    const auto error =
        sonare_mastering_repair_denoise_classical(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_denoise_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_denoise_classical_stereo") {
    SonareDenoiseStereoResult out{};
    const auto error = sonare_mastering_repair_denoise_classical_stereo(data, data, length,
                                                                        sample_rate, nullptr, &out);
    CorpusJson value;
    if (error == SONARE_OK) {
      value = repair_result_linked(CorpusJsonArray{corpus_float_array(out.left, out.length),
                                                   corpus_float_array(out.right, out.length)},
                                   out);
    }
    sonare_free_denoise_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_denoise_classical_linked") {
    const float* inputs[] = {data, data};
    SonareDenoiseLinkedResult out{};
    const auto error = sonare_mastering_repair_denoise_classical_linked(inputs, 2, length,
                                                                        sample_rate, nullptr, &out);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonArray channels;
      for (size_t i = 0; i < out.num_channels; ++i) {
        channels.push_back(corpus_float_array(out.channels[i], out.length));
      }
      value = repair_result_linked(std::move(channels), out);
    }
    sonare_free_denoise_linked_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_dereverb_classical") {
    SonareDereverbResult out{};
    const auto error =
        sonare_mastering_repair_dereverb_classical(data, length, sample_rate, nullptr, &out);
    auto value = repair_result_mono(out, error);
    sonare_free_dereverb_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_dereverb_classical_stereo") {
    SonareDereverbStereoResult out{};
    const auto error = sonare_mastering_repair_dereverb_classical_stereo(
        data, data, length, sample_rate, nullptr, &out);
    CorpusJson value;
    if (error == SONARE_OK) {
      value = repair_result_linked(CorpusJsonArray{corpus_float_array(out.left, out.length),
                                                   corpus_float_array(out.right, out.length)},
                                   out);
    }
    sonare_free_dereverb_stereo_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_dereverb_classical_linked") {
    const float* inputs[] = {data, data};
    SonareDereverbLinkedResult out{};
    const auto error = sonare_mastering_repair_dereverb_classical_linked(
        inputs, 2, length, sample_rate, nullptr, &out);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonArray channels;
      for (size_t i = 0; i < out.num_channels; ++i) {
        channels.push_back(corpus_float_array(out.channels[i], out.length));
      }
      value = repair_result_linked(std::move(channels), out);
    }
    sonare_free_dereverb_linked_result(&out);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_dereverb_apply_room_estimate") {
    std::array<float, 4> bands{{0.125f, 0.25f, 0.375f, 0.625f}};
    SonareRoomEstimate estimate{};
    estimate.volume = 100.0f;
    estimate.rt60_bands = bands.data();
    estimate.band_count = bands.size();
    SonareDereverbClassicalConfig out{};
    sonare_dereverb_classical_config_init(&out);
    const auto error = sonare_mastering_repair_dereverb_apply_room_estimate(&estimate, &out);
    return CorpusCallResult{
        error, corpus_object({{"threshold", corpus_float_value(out.threshold)},
                              {"attenuation", corpus_float_value(out.attenuation)},
                              {"nFft", CorpusJson(out.n_fft)},
                              {"hopLength", CorpusJson(static_cast<double>(out.hop_length))},
                              {"t60Sec", corpus_float_value(out.t60_sec)},
                              {"lateDelayMs", corpus_float_value(out.late_delay_ms)},
                              {"overSubtraction", corpus_float_value(out.over_subtraction)},
                              {"spectralFloor", corpus_float_value(out.spectral_floor)},
                              {"wpeEnabled", CorpusJson(out.wpe_enabled != 0)},
                              {"wpeIterations", CorpusJson(out.wpe_iterations)},
                              {"wpeTaps", CorpusJson(out.wpe_taps)},
                              {"wpeStrength", corpus_float_value(out.wpe_strength)}})};
  }

  if (entry == "sonare_mastering_assistant_suggest" ||
      entry == "sonare_mastering_assistant_suggest_stereo") {
    char* raw = nullptr;
    const auto error =
        entry == "sonare_mastering_assistant_suggest"
            ? sonare_mastering_assistant_suggest(data, length, sample_rate, nullptr, 0, &raw)
            : sonare_mastering_assistant_suggest_stereo(data, data, length, sample_rate, nullptr, 0,
                                                        &raw);
    CorpusJson value;
    if (error == SONARE_OK) value = analyze_pair_json(raw, "assistant_result");
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_mastering_repair_apply") {
    const float* input[] = {data};
    std::vector<float> repaired(length);
    float* output[] = {repaired.data()};
    char* reports = nullptr;
    const auto error =
        sonare_mastering_repair_apply(input, 1, length, sample_rate, R"([{"stage":"declick"}])",
                                      nullptr, nullptr, output, &reports, nullptr, nullptr);
    CorpusJson value;
    if (error == SONARE_OK) {
      REQUIRE(reports != nullptr);
      value = corpus_object(
          {{"channels", CorpusJson(CorpusJsonArray{corpus_float_array(repaired.data(), length)})},
           {"reports", sonare::util::json::parse_strict(reports)}});
    }
    sonare_free_string(reports);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_mastering_repair_detect_clicks") {
    SonareClickDetection detection{};
    const SonareError error =
        sonare_mastering_repair_detect_clicks(data, length, sample_rate, nullptr, &detection);
    CorpusJsonObject result;
    result.emplace("count", CorpusJson(static_cast<double>(detection.count)));
    result.emplace("rejected", CorpusJson(static_cast<double>(detection.rejected)));
    result.emplace("longestRunSamples",
                   CorpusJson(static_cast<double>(detection.longest_run_samples)));
    result.emplace("perSecond", corpus_float_value(detection.per_second));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_detect_crackle") {
    SonareCrackleDetection detection{};
    const SonareError error =
        sonare_mastering_repair_detect_crackle(data, length, sample_rate, nullptr, &detection);
    CorpusJsonObject result;
    result.emplace("sampleCount", CorpusJson(static_cast<double>(detection.sample_count)));
    result.emplace("sampleFraction", corpus_float_value(detection.sample_fraction));
    result.emplace("perSecond", corpus_float_value(detection.per_second));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_detect_hum") {
    SonareHumDetection detection{};
    const SonareError error =
        sonare_mastering_repair_detect_hum(data, length, sample_rate, nullptr, &detection);
    CorpusJsonArray harmonic_dbfs;
    for (size_t i = 0; i < SONARE_DEHUM_MAX_HARMONICS && !std::isnan(detection.harmonic_dbfs[i]);
         ++i) {
      harmonic_dbfs.push_back(corpus_float_value(detection.harmonic_dbfs[i]));
    }
    CorpusJsonObject result;
    result.emplace("fundamentalHz", corpus_float_value(detection.fundamental_hz));
    result.emplace("fundamentalProminence", corpus_float_value(detection.fundamental_prominence));
    result.emplace("harmonics", CorpusJson(detection.harmonics));
    result.emplace("harmonicDbfs", CorpusJson(std::move(harmonic_dbfs)));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_detect_noise_floor") {
    SonareNoiseDetection detection{};
    const SonareError error =
        sonare_mastering_repair_detect_noise_floor(data, length, sample_rate, nullptr, &detection);
    CorpusJsonArray band_floor_dbfs;
    CorpusJsonArray band_floor_band;
    for (size_t i = 0; i < detection.band_count; ++i) {
      band_floor_dbfs.push_back(corpus_float_value(detection.band_floor_dbfs[i]));
      band_floor_band.emplace_back(detection.band_floor_band[i]);
    }
    CorpusJsonObject result;
    corpus_put_measured(result, "floorDbfs", detection.floor_dbfs);
    result.emplace("bandFloorDbfs", CorpusJson(std::move(band_floor_dbfs)));
    result.emplace("bandFloorBand", CorpusJson(std::move(band_floor_band)));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_detect_reverb") {
    SonareReverbDetection detection{};
    const SonareError error =
        sonare_mastering_repair_detect_reverb(data, length, sample_rate, nullptr, &detection);
    CorpusJsonObject result;
    if (detection.has_late_decay_ratio_db != 0) {
      result.emplace("lateDecayRatioDb", corpus_float_value(detection.late_decay_ratio_db));
    }
    if (detection.has_late_predictability != 0) {
      result.emplace("latePredictability", corpus_float_value(detection.late_predictability));
    }
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_detect_trim_range" ||
      entry == "sonare_mastering_repair_detect_trim_range_stereo") {
    SonareTrimRange range{};
    SonareError error = SONARE_OK;
    if (entry == "sonare_mastering_repair_detect_trim_range") {
      error = sonare_mastering_repair_detect_trim_range(data, length, sample_rate, nullptr, &range);
    } else {
      std::vector<float> right = right_fixture();
      REQUIRE(right.size() == length);
      error = sonare_mastering_repair_detect_trim_range_stereo(
          data, right.empty() ? nullptr : right.data(), length, sample_rate, nullptr, &range);
    }
    CorpusJsonObject result;
    result.emplace("first", CorpusJson(static_cast<double>(range.first)));
    result.emplace("lastExclusive", CorpusJson(static_cast<double>(range.last_exclusive)));
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(result)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_analyze_pair" || entry == "sonare_mastering_analyze_pair_stereo") {
    std::vector<float> reference = result_corpus_fixture("historical_reference", length);
    char* raw = nullptr;
    SonareError error = SONARE_OK;
    if (entry == "sonare_mastering_analyze_pair") {
      error = sonare_mastering_analyze_pair(
          "match.tonalBalance", data, length, reference.empty() ? nullptr : reference.data(),
          reference.size(), sample_rate, sample_rate, nullptr, 0, &raw);
    } else {
      std::vector<float> source_right = right_fixture();
      std::vector<float> reference_right =
          result_corpus_fixture("historical_reference_right", length);
      REQUIRE(source_right.size() == length);
      REQUIRE(reference.size() == length);
      REQUIRE(reference_right.size() == length);
      error = sonare_mastering_analyze_pair_stereo(
          "match.tonalBalance", data, source_right.empty() ? nullptr : source_right.data(), length,
          reference.empty() ? nullptr : reference.data(),
          reference_right.empty() ? nullptr : reference_right.data(), reference.size(), sample_rate,
          sample_rate, nullptr, 0, &raw);
    }
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return CorpusCallResult{error, CorpusJson()};
    }
    CorpusJson result = analyze_pair_json(raw, "match_tonal_balance");
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(result)};
  }

  if (entry == "sonare_mastering_analyze_stereo") {
    std::vector<float> right = right_fixture();
    REQUIRE(right.size() == length);
    char* raw = nullptr;
    const SonareError error = sonare_mastering_analyze_stereo(
        "stereo.monoCompatCheck", data, right.empty() ? nullptr : right.data(), length, sample_rate,
        nullptr, 0, &raw);
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return CorpusCallResult{error, CorpusJson()};
    }
    CorpusJson result = analyze_pair_json(raw, "stereo_mono_compat");
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(result)};
  }

  const auto trim_range_json = [](const SonareTrimRange& range) {
    CorpusJsonObject value;
    value.emplace("first", CorpusJson(static_cast<double>(range.first)));
    value.emplace("lastExclusive", CorpusJson(static_cast<double>(range.last_exclusive)));
    return corpus_object(std::move(value));
  };
  const auto trim_report_json = [&](const SonareTrimReport& report) {
    CorpusJsonObject value;
    value.emplace("range", trim_range_json(report.range));
    value.emplace("removedHeadSamples",
                  CorpusJson(static_cast<double>(report.removed_head_samples)));
    value.emplace("removedTailSamples",
                  CorpusJson(static_cast<double>(report.removed_tail_samples)));
    return corpus_object(std::move(value));
  };

  const auto mastering_result_json = [](const SonareMasteringResult& result) {
    CorpusJsonObject value;
    value.emplace("samples", corpus_float_array(result.samples, result.length));
    value.emplace("sampleRate", CorpusJson(result.sample_rate));
    value.emplace("inputLufs", corpus_float_value(result.input_lufs));
    value.emplace("outputLufs", corpus_float_value(result.output_lufs));
    value.emplace("appliedGainDb", corpus_float_value(result.applied_gain_db));
    value.emplace("latencySamples", CorpusJson(result.latency_samples));
    value.emplace("loudnessTargetLimited", CorpusJson(result.loudness_target_limited != 0));
    value.emplace("nonFiniteSubstitutionCount",
                  CorpusJson(static_cast<double>(result.non_finite_substitution_count)));
    if (result.has_declip_unreconstructed_runs != 0) {
      value.emplace("declipUnreconstructedRuns",
                    CorpusJson(static_cast<double>(result.declip_unreconstructed_runs)));
    }
    if (result.has_quantize_clipped_samples != 0) {
      value.emplace("quantizeClippedSamples",
                    CorpusJson(static_cast<double>(result.quantize_clipped_samples)));
    }
    return corpus_object(std::move(value));
  };
  const auto mastering_stereo_result_json = [](const SonareMasteringStereoResult& result) {
    CorpusJsonObject value;
    value.emplace("left", corpus_float_array(result.left, result.length));
    value.emplace("right", corpus_float_array(result.right, result.length));
    value.emplace("sampleRate", CorpusJson(result.sample_rate));
    value.emplace("inputLufs", corpus_float_value(result.input_lufs));
    value.emplace("outputLufs", corpus_float_value(result.output_lufs));
    value.emplace("appliedGainDb", corpus_float_value(result.applied_gain_db));
    value.emplace("latencySamples", CorpusJson(result.latency_samples));
    value.emplace("loudnessTargetLimited", CorpusJson(result.loudness_target_limited != 0));
    value.emplace("nonFiniteSubstitutionCount",
                  CorpusJson(static_cast<double>(result.non_finite_substitution_count)));
    if (result.has_declip_unreconstructed_runs != 0) {
      value.emplace("declipUnreconstructedRuns",
                    CorpusJson(static_cast<double>(result.declip_unreconstructed_runs)));
    }
    if (result.has_quantize_clipped_samples != 0) {
      value.emplace("quantizeClippedSamples",
                    CorpusJson(static_cast<double>(result.quantize_clipped_samples)));
    }
    return corpus_object(std::move(value));
  };
  if (entry == "sonare_mastering_process") {
    SonareMasteringConfig config{};
    sonare_mastering_config_init(&config);
    SonareMasteringResult result{};
    const SonareError error = sonare_mastering_process(data, length, sample_rate, &config, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = mastering_result_json(result);
    sonare_free_mastering_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_apply_processor" ||
      entry == "sonare_mastering_apply_pair_processor") {
    SonareMasteringResult result{};
    SonareError error = SONARE_OK;
    if (entry == "sonare_mastering_apply_processor") {
      error = sonare_mastering_apply_processor("utility.gain", data, length, sample_rate, nullptr,
                                               0, &result);
    } else {
      const std::vector<float> reference(samples.rbegin(), samples.rend());
      error = sonare_mastering_apply_pair_processor("match.abCrossfade", data, length,
                                                    reference.data(), reference.size(), sample_rate,
                                                    sample_rate, nullptr, 0, &result);
    }
    CorpusJson value;
    if (error == SONARE_OK) value = mastering_result_json(result);
    sonare_free_mastering_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_apply_processor_stereo" ||
      entry == "sonare_mastering_apply_pair_processor_stereo") {
    SonareMasteringStereoResult result{};
    SonareError error = SONARE_OK;
    if (entry == "sonare_mastering_apply_processor_stereo") {
      error = sonare_mastering_apply_processor_stereo("utility.gain", data, data, length,
                                                      sample_rate, nullptr, 0, &result);
    } else {
      const std::vector<float> right(samples.rbegin(), samples.rend());
      std::vector<float> reference_right = right;
      for (float& value : reference_right) value *= 0.5f;
      error = sonare_mastering_apply_pair_processor_stereo(
          "match.abCrossfade", data, right.data(), length, right.data(), reference_right.data(),
          length, sample_rate, sample_rate, nullptr, 0, &result);
    }
    CorpusJson value;
    if (error == SONARE_OK) value = mastering_stereo_result_json(result);
    sonare_free_mastering_stereo_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mastering_ab_match_loudness" ||
      entry == "sonare_mastering_ab_match_loudness_stereo") {
    const std::vector<float> reference = result_corpus_fixture("historical_reference", length);
    SonareLoudnessMatch match{};
    float* output_left = nullptr;
    float* output_right = nullptr;
    size_t output_length = 0;
    const bool stereo = entry == "sonare_mastering_ab_match_loudness_stereo";
    SonareError error = SONARE_OK;
    if (stereo) {
      const std::vector<float> reference_right =
          result_corpus_fixture("historical_reference_right", length);
      const std::vector<float> right = right_fixture();
      REQUIRE(right.size() == length);
      REQUIRE(reference.size() == reference_right.size());
      error = sonare_mastering_ab_match_loudness_stereo(
          data, right.data(), length, reference.data(), reference_right.data(), reference.size(),
          sample_rate, sample_rate, &output_left, &output_right, &output_length, &match);
    } else {
      error = sonare_mastering_ab_match_loudness(data, length, reference.data(), reference.size(),
                                                 sample_rate, sample_rate, &output_left,
                                                 &output_length, &match);
    }
    CorpusJsonObject value;
    if (error == SONARE_OK) {
      if (stereo) {
        value.emplace("left", corpus_float_array(output_left, output_length));
        value.emplace("right", corpus_float_array(output_right, output_length));
      } else {
        value.emplace("samples", corpus_float_array(output_left, output_length));
      }
      value.emplace("sampleRate", CorpusJson(sample_rate));
      value.emplace("referenceLufs", corpus_float_value(match.reference_lufs));
      value.emplace("sourceLufs", corpus_float_value(match.source_lufs));
      value.emplace("appliedGainDb", corpus_float_value(match.applied_gain_db));
      value.emplace("matchedTruePeakDbtp", corpus_float_value(match.matched_true_peak_dbtp));
    }
    sonare_free_floats(output_left);
    sonare_free_floats(output_right);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(value)) : CorpusJson()};
  }

  if (entry == "sonare_mastering_repair_trim_silence") {
    SonareTrimSilenceResult trimmed{};
    const SonareError error =
        sonare_mastering_repair_trim_silence(data, length, sample_rate, nullptr, &trimmed);
    CorpusJsonObject value;
    if (error == SONARE_OK) {
      value.emplace("samples", corpus_float_array(trimmed.samples, trimmed.length));
      value.emplace("report", trim_report_json(trimmed.report));
    }
    sonare_free_trim_silence_result(&trimmed);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(value)) : CorpusJson()};
  }
  if (entry == "sonare_mastering_repair_trim_silence_stereo") {
    SonareTrimSilenceStereoResult trimmed{};
    const SonareError error = sonare_mastering_repair_trim_silence_stereo(
        data, data, length, sample_rate, nullptr, &trimmed);
    CorpusJsonObject value;
    if (error == SONARE_OK) {
      CorpusJsonArray channels;
      channels.push_back(corpus_float_array(trimmed.left, trimmed.length));
      channels.push_back(corpus_float_array(trimmed.right, trimmed.length));
      CorpusJsonArray reports;
      reports.push_back(trim_report_json(trimmed.report));
      CorpusJsonArray ranges;
      ranges.push_back(trim_range_json(trimmed.left_range));
      ranges.push_back(trim_range_json(trimmed.right_range));
      value.emplace("channels", CorpusJson(std::move(channels)));
      value.emplace("reports", CorpusJson(std::move(reports)));
      value.emplace("ranges", CorpusJson(std::move(ranges)));
    }
    sonare_free_trim_silence_stereo_result(&trimmed);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(value)) : CorpusJson()};
  }
  if (entry == "sonare_normalize_stereo" || entry == "sonare_normalize_rms_stereo") {
    float* left_out = nullptr;
    float* right_out = nullptr;
    size_t output_length = 0;
    const SonareError error =
        entry == "sonare_normalize_stereo"
            ? sonare_normalize_stereo(data, data, length, sample_rate, 0.0f, &left_out, &right_out,
                                      &output_length)
            : sonare_normalize_rms_stereo(data, data, length, sample_rate, -20.0f, &left_out,
                                          &right_out, &output_length);
    CorpusJsonObject value;
    if (error == SONARE_OK) {
      float input_peak = 0.0f;
      float output_peak = 0.0f;
      for (size_t i = 0; i < length; ++i) input_peak = std::max(input_peak, std::abs(data[i]));
      for (size_t i = 0; i < output_length; ++i) {
        output_peak =
            std::max(output_peak, std::max(std::abs(left_out[i]), std::abs(right_out[i])));
      }
      value.emplace("left", corpus_float_array(left_out, output_length));
      value.emplace("right", corpus_float_array(right_out, output_length));
      value.emplace("appliedGainDb",
                    corpus_float_value(20.0 * std::log10(output_peak / input_peak)));
    }
    sonare_free_floats(left_out);
    sonare_free_floats(right_out);
    return CorpusCallResult{error,
                            error == SONARE_OK ? corpus_object(std::move(value)) : CorpusJson()};
  }

  if (entry == "sonare_capability_catalog_json" || entry == "sonare_mastering_processor_catalog") {
    char* raw = nullptr;
    const auto error = entry == "sonare_capability_catalog_json"
                           ? sonare_capability_catalog_json(&raw)
                           : sonare_mastering_processor_catalog(&raw);
    const std::unique_ptr<char, decltype(&sonare_free_string)> owned(raw, sonare_free_string);
    return CorpusCallResult{
        error, error == SONARE_OK ? sonare::util::json::parse_strict(raw) : CorpusJson()};
  }
  if (entry == "sonare_mastering_repair_analyze") {
    std::vector<float> right = right_fixture();
    REQUIRE(right.size() == length);
    const float* channels[] = {data, right.empty() ? nullptr : right.data()};
    char* raw = nullptr;
    const SonareError error = sonare_mastering_repair_analyze(
        channels, 2, length, sample_rate, "{\"preferStreamingSafe\":true}", &raw);
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return CorpusCallResult{error, CorpusJson()};
    }
    CorpusJson result = analyze_pair_json(raw, "repair_analysis");
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(result)};
  }
  if (entry == "sonare_mastering_streaming_preview" ||
      entry == "sonare_mastering_streaming_preview_stereo") {
    char* raw = nullptr;
    SonareError error = SONARE_OK;
    if (entry == "sonare_mastering_streaming_preview") {
      error = sonare_mastering_streaming_preview(data, length, sample_rate, nullptr, 0, &raw);
    } else {
      std::vector<float> right = right_fixture();
      REQUIRE(right.size() == length);
      error = sonare_mastering_streaming_preview_stereo(
          data, right.empty() ? nullptr : right.data(), length, sample_rate, nullptr, 0, &raw);
    }
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return CorpusCallResult{error, CorpusJson()};
    }
    CorpusJson result = analyze_pair_json(raw, "streaming_preview");
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(result)};
  }

  return std::nullopt;
}

std::optional<CorpusCallResult> run_analysis_corpus(const std::string& entry,
                                                    const std::vector<float>& samples,
                                                    int sample_rate, const std::string& fixture) {
  const float* data = samples.empty() ? nullptr : samples.data();
  const size_t length = samples.size();

  const auto acoustic_json = [](const SonareAcousticResult& result) {
    CorpusJsonObject out;
    corpus_put_measured(out, "rt60", result.rt60);
    corpus_put_measured(out, "edt", result.edt);
    corpus_put_measured(out, "c50", result.c50);
    corpus_put_measured(out, "c80", result.c80);
    corpus_put_measured(out, "d50", result.d50);
    out.emplace("confidence", corpus_float_value(result.confidence));
    out.emplace("isBlind", CorpusJson(result.is_blind != 0));
    const auto put_array = [&out, &result](const char* key, const float* values) {
      if (result.band_count == 0 || values != nullptr) {
        out.emplace(key, corpus_float_array(values, result.band_count));
      }
    };
    put_array("rt60Bands", result.rt60_bands);
    if (result.is_blind == 0) put_array("edtBands", result.edt_bands);
    put_array("c50Bands", result.c50_bands);
    put_array("c80Bands", result.c80_bands);
    return corpus_object(std::move(out));
  };

  if (entry == "sonare_estimate_meter_json") {
    SonareMeterBeats beats{};
    sonare_meter_beats_init(&beats);
    beats.accent = data;
    beats.beat_count = length;
    SonareMeterOptions options{};
    sonare_meter_options_init(&options);
    char* raw = nullptr;
    const SonareError error = sonare_estimate_meter_json(&beats, &options, &raw);
    CorpusJson value;
    if (error == SONARE_OK) {
      REQUIRE(raw != nullptr);
      value = sonare::util::json::parse_strict(raw);
    }
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_analyze_json") {
    char* raw = nullptr;
    const SonareError error =
        sonare_analyze_json(samples.data(), samples.size(), sample_rate, nullptr, &raw);
    CorpusJson value;
    if (error == SONARE_OK) {
      REQUIRE(raw != nullptr);
      value = sonare::util::json::parse_strict(raw);
    }
    sonare_free_string(raw);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_analyze_bpm") {
    const sonare::BpmConfig config;
    SonareBpmAnalysisResult result{};
    const SonareError error =
        sonare_analyze_bpm(data, length, sample_rate, config.bpm_min, config.bpm_max, config.n_fft,
                           config.hop_length, 5, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      corpus_put_measured(out, "bpm", result.bpm);
      out.emplace("confidence", corpus_float_value(result.confidence));
      CorpusJsonArray candidates;
      candidates.reserve(result.candidate_count);
      REQUIRE((result.candidates != nullptr || result.candidate_count == 0));
      for (size_t i = 0; i < result.candidate_count; ++i) {
        CorpusJsonObject candidate;
        candidate.emplace("bpm", corpus_float_value(result.candidates[i].bpm));
        candidate.emplace("confidence", corpus_float_value(result.candidates[i].confidence));
        candidates.emplace_back(std::move(candidate));
      }
      out.emplace("candidates", CorpusJson(std::move(candidates)));
      out.emplace("autocorrelation",
                  corpus_float_array(result.autocorrelation, result.autocorrelation_count));
      out.emplace("tempogram", corpus_float_array(result.tempogram, result.tempogram_count));
      value = corpus_object(std::move(out));
    }
    sonare_free_bpm_analysis_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_analyze_rhythm") {
    const sonare::RhythmConfig config;
    SonareRhythmResult result{};
    const SonareError error =
        sonare_analyze_rhythm(data, length, sample_rate, config.bpm_min, config.bpm_max,
                              config.start_bpm, config.n_fft, config.hop_length, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      const char* groove = "straight";
      if (result.groove_type == SONARE_GROOVE_SHUFFLE) groove = "shuffle";
      if (result.groove_type == SONARE_GROOVE_SWING) groove = "swing";
      CorpusJsonObject time_signature;
      time_signature.emplace("numerator", CorpusJson(result.time_signature.numerator));
      time_signature.emplace("denominator", CorpusJson(result.time_signature.denominator));
      time_signature.emplace("confidence", corpus_float_value(result.time_signature.confidence));
      CorpusJsonObject out;
      corpus_put_measured(out, "bpm", result.bpm);
      out.emplace("timeSignature", corpus_object(std::move(time_signature)));
      out.emplace("grooveType", CorpusJson(groove));
      corpus_put_measured(out, "syncopation", result.syncopation);
      corpus_put_measured(out, "patternRegularity", result.pattern_regularity);
      corpus_put_measured(out, "tempoStability", result.tempo_stability);
      out.emplace("beatIntervals",
                  corpus_float_array(result.beat_intervals, result.beat_interval_count));
      value = corpus_object(std::move(out));
    }
    sonare_free_rhythm_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_analyze_melody") {
    SonareAnalyzeMelodyOptions options{};
    sonare_analyze_melody_options_init(&options);
    SonareMelodyResult result{};
    const sonare::MelodyConfig config;
    const SonareError error = sonare_analyze_melody(
        data, length, sample_rate, config.fmin, config.fmax, config.frame_length, config.hop_length,
        config.threshold, &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonArray points;
      points.reserve(result.point_count);
      REQUIRE((result.points != nullptr || result.point_count == 0));
      for (size_t i = 0; i < result.point_count; ++i) {
        const auto& point = result.points[i];
        CorpusJsonObject item;
        item.emplace("timeSec", corpus_float_value(point.time_sec));
        if (point.frequency_hz > 0.0f) {
          item.emplace("frequencyHz", corpus_float_value(point.frequency_hz));
        }
        item.emplace("confidence", corpus_float_value(point.confidence));
        points.emplace_back(std::move(item));
      }
      CorpusJsonObject out;
      out.emplace("points", CorpusJson(std::move(points)));
      corpus_put_measured(out, "pitchRangeOctaves", result.pitch_range_octaves);
      corpus_put_measured(out, "pitchStability", result.pitch_stability);
      corpus_put_measured(out, "meanFrequencyHz", result.mean_frequency_hz);
      corpus_put_measured(out, "vibratoRateHz", result.vibrato_rate_hz);
      value = corpus_object(std::move(out));
    }
    sonare_free_melody_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_analyze_sections") {
    SonareSectionOptions options{};
    sonare_section_options_init(&options);
    SonareSectionResult result{};
    const SonareError error = sonare_analyze_sections(data, length, sample_rate, &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      static constexpr std::array<const char*, 8> section_types = {
          "intro", "verse", "preChorus", "chorus", "bridge", "instrumental", "outro", "unknown"};
      static constexpr std::array<const char*, 8> section_names = {
          "Intro", "Verse", "Pre-Chorus", "Chorus", "Bridge", "Instrumental", "Outro", "Unknown"};
      CorpusJsonArray sections;
      sections.reserve(result.section_count);
      REQUIRE((result.sections != nullptr || result.section_count == 0));
      for (size_t i = 0; i < result.section_count; ++i) {
        const auto& section = result.sections[i];
        const int type = static_cast<int>(section.type);
        REQUIRE((type >= 0 && static_cast<size_t>(type) < section_types.size()));
        CorpusJsonObject item;
        item.emplace("type", CorpusJson(section_types[static_cast<size_t>(type)]));
        item.emplace("name", CorpusJson(section_names[static_cast<size_t>(type)]));
        item.emplace("startSec", corpus_float_value(section.start_sec));
        item.emplace("endSec", corpus_float_value(section.end_sec));
        item.emplace("energyLevel", corpus_float_value(section.energy_level));
        item.emplace("confidence", corpus_float_value(section.confidence));
        item.emplace("group", CorpusJson(section.group));
        sections.emplace_back(std::move(item));
      }
      CorpusJsonObject out;
      out.emplace("sections", CorpusJson(std::move(sections)));
      out.emplace("form", CorpusJson(result.form != nullptr ? result.form : ""));
      value = corpus_object(std::move(out));
    }
    sonare_free_section_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_frame_signal") {
    if (fixture != "historical_frame") return std::nullopt;
    constexpr size_t frame_length = 4;
    constexpr size_t hop_length = 2;
    float* frames = nullptr;
    size_t frame_count = 0;
    int n_frames = 0;
    const SonareError error = sonare_frame_signal(data, length, frame_length, hop_length, &frames,
                                                  &frame_count, &n_frames);
    CorpusJson value;
    if (error == SONARE_OK) {
      const size_t rows = frame_length;
      const size_t cols = static_cast<size_t>(n_frames);
      CorpusJsonArray matrix;
      matrix.resize(frame_count);
      REQUIRE((frames != nullptr || frame_count == 0));
      for (size_t frame = 0; frame < cols; ++frame) {
        for (size_t sample = 0; sample < rows; ++sample) {
          matrix[sample * cols + frame] = corpus_float_value(frames[frame * rows + sample]);
        }
      }
      CorpusJsonObject out;
      out.emplace("data", CorpusJson(std::move(matrix)));
      out.emplace("rows", CorpusJson(static_cast<double>(rows)));
      out.emplace("cols", CorpusJson(static_cast<double>(cols)));
      value = corpus_object(std::move(out));
    }
    sonare_free_floats(frames);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_analyze_impulse_response" || entry == "sonare_detect_acoustic") {
    if (fixture != "historical_rir" &&
        (entry != "sonare_detect_acoustic" ||
         (fixture != "decay_envelope" && fixture != "lone_impulse"))) {
      return std::nullopt;
    }
    const sonare::AcousticConfig config;
    SonareAcousticResult result{};
    SonareError error = SONARE_OK;
    if (entry == "sonare_analyze_impulse_response") {
      SonareAnalyzeImpulseResponseOptions options{};
      sonare_analyze_impulse_response_options_init(&options);
      error = sonare_analyze_impulse_response(data, length, sample_rate, config.n_octave_bands,
                                              &options, &result);
    } else {
      error = sonare_detect_acoustic(data, length, sample_rate, config.n_octave_bands,
                                     config.n_third_octave_subbands, config.min_decay_db,
                                     config.noise_floor_margin_db, &result);
    }
    CorpusJson value;
    if (error == SONARE_OK) value = acoustic_json(result);
    sonare_free_acoustic_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  if (entry == "sonare_estimate_room") {
    if (fixture != "historical_rir") return std::nullopt;
    SonareRoomEstimateConfig config{};
    sonare_room_estimate_config_init(&config);
    SonareRoomEstimate result{};
    const SonareError error = sonare_estimate_room(data, length, sample_rate, &config, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      corpus_put_measured(out, "volume", result.volume);
      corpus_put_measured(out, "lengthM", result.length_m);
      corpus_put_measured(out, "widthM", result.width_m);
      corpus_put_measured(out, "heightM", result.height_m);
      corpus_put_measured(out, "drrDb", result.drr_db);
      out.emplace("confidence", corpus_float_value(result.confidence));
      if (result.band_count == 0 || result.band_absorption != nullptr) {
        out.emplace("bandAbsorption",
                    corpus_float_array(result.band_absorption, result.band_count));
      }
      if (result.band_count == 0 || result.rt60_bands != nullptr) {
        out.emplace("rt60Bands", corpus_float_array(result.rt60_bands, result.band_count));
      }
      value = corpus_object(std::move(out));
    }
    sonare_free_room_estimate(&result);
    return CorpusCallResult{error, std::move(value)};
  }

  return std::nullopt;
}

CorpusJson corpus_matrix(const float* values, size_t rows, size_t cols) {
  CorpusJsonObject matrix;
  matrix.emplace("rows", CorpusJson(static_cast<double>(rows)));
  matrix.emplace("cols", CorpusJson(static_cast<double>(cols)));
  matrix.emplace("data", corpus_float_array(values, rows * cols));
  return corpus_object(std::move(matrix));
}

std::vector<float> corpus_interleave_stereo(const std::vector<float>& left,
                                            const std::string& fixture) {
  const std::vector<float> right = fixture == "silence"
                                       ? result_corpus_fixture("silence", left.size())
                                       : result_corpus_fixture("historical_right", left.size());
  REQUIRE(right.size() == left.size());
  std::vector<float> interleaved;
  interleaved.reserve(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    interleaved.push_back(left[i]);
    interleaved.push_back(right[i]);
  }
  return interleaved;
}

CorpusJson vectorscope_corpus_result(const SonareVectorscopeResult& result) {
  CorpusJsonArray mid;
  CorpusJsonArray side;
  mid.reserve(result.point_count);
  side.reserve(result.point_count);
  REQUIRE((result.points != nullptr || result.point_count == 0));
  for (size_t i = 0; i < result.point_count; ++i) {
    mid.push_back(corpus_float_value(result.points[i].mid));
    side.push_back(corpus_float_value(result.points[i].side));
  }
  CorpusJsonObject out;
  out.emplace("mid", CorpusJson(std::move(mid)));
  out.emplace("side", CorpusJson(std::move(side)));
  return corpus_object(std::move(out));
}

CorpusJson phase_scope_corpus_result(const SonarePhaseScopeResult& result) {
  CorpusJsonArray mid;
  CorpusJsonArray side;
  CorpusJsonArray radius;
  CorpusJsonArray angle;
  mid.reserve(result.point_count);
  side.reserve(result.point_count);
  radius.reserve(result.point_count);
  angle.reserve(result.point_count);
  REQUIRE((result.points != nullptr || result.point_count == 0));
  for (size_t i = 0; i < result.point_count; ++i) {
    mid.push_back(corpus_float_value(result.points[i].mid));
    side.push_back(corpus_float_value(result.points[i].side));
    radius.push_back(corpus_float_value(result.points[i].radius));
    angle.push_back(corpus_float_value(result.points[i].angle_rad));
  }
  CorpusJsonObject out;
  out.emplace("mid", CorpusJson(std::move(mid)));
  out.emplace("side", CorpusJson(std::move(side)));
  out.emplace("radius", CorpusJson(std::move(radius)));
  out.emplace("angleRad", CorpusJson(std::move(angle)));
  corpus_put_measured(out, "correlation", result.correlation);
  corpus_put_measured(out, "averageAbsAngleRad", result.average_abs_angle_rad);
  out.emplace("maxRadius", corpus_float_value(result.max_radius));
  return corpus_object(std::move(out));
}

CorpusJson spectrum_corpus_result(const SonareSpectrumResult& result) {
  CorpusJsonObject out;
  out.emplace("frequencies", corpus_float_array(result.frequencies, result.bin_count));
  out.emplace("magnitude", corpus_float_array(result.magnitude, result.bin_count));
  out.emplace("power", corpus_float_array(result.power, result.bin_count));
  out.emplace("db", corpus_float_array(result.db, result.bin_count));
  out.emplace("nFft", CorpusJson(result.n_fft));
  out.emplace("sampleRate", CorpusJson(result.sample_rate));
  return corpus_object(std::move(out));
}

CorpusJson waveform_peaks_corpus_result(const SonareWaveformPeaksResult& result) {
  const size_t channels = static_cast<size_t>(result.num_channels);
  CorpusJsonArray min;
  CorpusJsonArray max;
  min.reserve(channels);
  max.reserve(channels);
  REQUIRE((result.min != nullptr || channels * result.bucket_count == 0));
  REQUIRE((result.max != nullptr || channels * result.bucket_count == 0));
  for (size_t channel = 0; channel < channels; ++channel) {
    CorpusJsonArray channel_min;
    CorpusJsonArray channel_max;
    channel_min.reserve(result.bucket_count);
    channel_max.reserve(result.bucket_count);
    for (size_t bucket = 0; bucket < result.bucket_count; ++bucket) {
      const size_t index = channel * result.bucket_count + bucket;
      channel_min.push_back(corpus_float_value(result.min[index]));
      channel_max.push_back(corpus_float_value(result.max[index]));
    }
    min.emplace_back(std::move(channel_min));
    max.emplace_back(std::move(channel_max));
  }
  CorpusJsonObject out;
  out.emplace("min", CorpusJson(std::move(min)));
  out.emplace("max", CorpusJson(std::move(max)));
  out.emplace("bucketCount", CorpusJson(static_cast<double>(result.bucket_count)));
  out.emplace("samplesPerBucket", CorpusJson(static_cast<double>(result.samples_per_bucket)));
  return corpus_object(std::move(out));
}

CorpusJson clipping_regions_corpus_result(const SonareClippingResult& result) {
  REQUIRE((result.regions != nullptr || result.region_count == 0));
  CorpusJsonArray regions;
  for (size_t i = 0; i < result.region_count; ++i) {
    const auto& region = result.regions[i];
    regions.push_back(corpus_object({
        {"startSample", CorpusJson(static_cast<double>(region.start_sample))},
        {"endSample", CorpusJson(static_cast<double>(region.end_sample))},
        {"length", CorpusJson(static_cast<double>(region.length))},
        {"peak", corpus_float_value(region.peak)},
    }));
  }
  return corpus_object({
      {"clippedRatio", corpus_float_value(result.clipping_ratio)},
      {"clippedSamples", CorpusJson(static_cast<double>(result.clipped_samples))},
      {"maxClippedPeak", corpus_float_value(result.max_clipped_peak)},
      {"regions", CorpusJson(std::move(regions))},
  });
}

std::optional<CorpusCallResult> run_meter_corpus(const std::string& entry,
                                                 const std::vector<float>& samples, int sample_rate,
                                                 const std::string& fixture) {
  const float* data = samples.empty() ? nullptr : samples.data();
  const size_t length = samples.size();
  if (entry == "sonare_metering_detect_clipping") {
    SonareClippingResult result{};
    const SonareError error = sonare_metering_detect_clipping(
        data, length, sample_rate, std::numeric_limits<float>::quiet_NaN(), 1, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = clipping_regions_corpus_result(result);
    sonare_free_clipping_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_metering_vectorscope" || entry == "sonare_metering_phase_scope") {
    const auto right = fixture == "silence" ? result_corpus_fixture("silence", length)
                                            : result_corpus_fixture("historical_right", length);
    REQUIRE(right.size() == length);
    CorpusJson value;
    if (entry == "sonare_metering_vectorscope") {
      SonareVectorscopeResult result{};
      const SonareError error = sonare_metering_vectorscope(
          data, right.empty() ? nullptr : right.data(), length, sample_rate, 0, &result);
      if (error == SONARE_OK) value = vectorscope_corpus_result(result);
      sonare_free_vectorscope_result(&result);
      return CorpusCallResult{error, std::move(value)};
    }
    SonarePhaseScopeResult result{};
    const SonareError error = sonare_metering_phase_scope(
        data, right.empty() ? nullptr : right.data(), length, sample_rate, 0, &result);
    if (error == SONARE_OK) value = phase_scope_corpus_result(result);
    sonare_free_phase_scope_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_metering_spectrum" || entry == "sonare_metering_spectrum_frame") {
    SonareSpectrumResult result{};
    SonareError error = SONARE_OK;
    if (entry == "sonare_metering_spectrum") {
      error = sonare_metering_spectrum(data, length, sample_rate, 0, 0, 0, 0.0f, 0.0f, &result);
    } else {
      error = sonare_metering_spectrum_frame(data, length, sample_rate, 0, 0, 0, 0, 0.0f, 0.0f,
                                             &result);
    }
    CorpusJson value;
    if (error == SONARE_OK) value = spectrum_corpus_result(result);
    sonare_free_spectrum_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_lufs_interleaved") {
    const auto interleaved = corpus_interleave_stereo(samples, fixture);
    const float* interleaved_data = interleaved.empty() ? nullptr : interleaved.data();
    SonareLufsResult result{};
    const SonareError error =
        sonare_lufs_interleaved(interleaved_data, length, 2, sample_rate, &result);
    return CorpusCallResult{error, error == SONARE_OK ? lufs_corpus_result(result) : CorpusJson()};
  }
  if (entry == "sonare_waveform_peaks") {
    const auto interleaved = corpus_interleave_stereo(samples, fixture);
    const float* interleaved_data = interleaved.empty() ? nullptr : interleaved.data();
    SonareWaveformPeaksResult result{};
    const SonareError error = sonare_waveform_peaks(interleaved_data, length, 2, 512, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = waveform_peaks_corpus_result(result);
    sonare_free_waveform_peaks_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_chroma") {
    SonareChromaResult result{};
    const SonareError error = sonare_chroma(data, length, sample_rate, 2048, 512, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      out.emplace("sampleRate", CorpusJson(result.sample_rate));
      out.emplace("hopLength", CorpusJson(static_cast<double>(result.hop_length)));
      out.emplace("features", corpus_matrix(result.features, static_cast<size_t>(result.n_chroma),
                                            static_cast<size_t>(result.n_frames)));
      out.emplace("meanEnergy",
                  corpus_float_array(result.mean_energy, static_cast<size_t>(result.n_chroma)));
      value = corpus_object(std::move(out));
    }
    sonare_free_chroma_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mel_spectrogram") {
    SonareMelSpectrogramOptions options{};
    sonare_mel_spectrogram_options_init(&options);
    SonareMelResult result{};
    const SonareError error =
        sonare_mel_spectrogram(data, length, sample_rate, 2048, 512, 128, &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      out.emplace("sampleRate", CorpusJson(result.sample_rate));
      out.emplace("hopLength", CorpusJson(static_cast<double>(result.hop_length)));
      out.emplace("nFft", CorpusJson(result.n_fft));
      out.emplace("fminHz", corpus_float_value(result.fmin_hz));
      out.emplace("fmaxHz", corpus_float_value(result.fmax_hz));
      out.emplace("htk", CorpusJson(result.htk != 0));
      out.emplace("power", corpus_matrix(result.power, static_cast<size_t>(result.n_mels),
                                         static_cast<size_t>(result.n_frames)));
      out.emplace("db", corpus_matrix(result.db, static_cast<size_t>(result.n_mels),
                                      static_cast<size_t>(result.n_frames)));
      value = corpus_object(std::move(out));
    }
    sonare_free_mel_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_mfcc") {
    SonareMfccOptions options{};
    sonare_mfcc_options_init(&options);
    SonareMfccResult result{};
    const SonareError error =
        sonare_mfcc(data, length, sample_rate, 2048, 512, 128, 20, &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      out.emplace("sampleRate", CorpusJson(result.sample_rate));
      out.emplace("hopLength", CorpusJson(static_cast<double>(result.hop_length)));
      out.emplace("nFft", CorpusJson(result.n_fft));
      out.emplace("nMels", CorpusJson(result.n_mels));
      out.emplace("fminHz", corpus_float_value(result.fmin_hz));
      out.emplace("fmaxHz", corpus_float_value(result.fmax_hz));
      out.emplace("htk", CorpusJson(result.htk != 0));
      out.emplace("lifter", corpus_float_value(result.lifter));
      out.emplace("coefficients",
                  corpus_matrix(result.coefficients, static_cast<size_t>(result.n_mfcc),
                                static_cast<size_t>(result.n_frames)));
      value = corpus_object(std::move(out));
    }
    sonare_free_mfcc_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_stft") {
    SonareStftResult result{};
    const SonareError error = sonare_stft(data, length, sample_rate, 2048, 512, nullptr, &result);
    CorpusJson value;
    if (error == SONARE_OK) {
      CorpusJsonObject out;
      out.emplace("nFft", CorpusJson(result.n_fft));
      out.emplace("hopLength", CorpusJson(static_cast<double>(result.hop_length)));
      out.emplace("sampleRate", CorpusJson(result.sample_rate));
      out.emplace("magnitude", corpus_matrix(result.magnitude, static_cast<size_t>(result.n_bins),
                                             static_cast<size_t>(result.n_frames)));
      out.emplace("power", corpus_matrix(result.power, static_cast<size_t>(result.n_bins),
                                         static_cast<size_t>(result.n_frames)));
      if (result.db != nullptr) {
        out.emplace("db", corpus_matrix(result.db, static_cast<size_t>(result.n_bins),
                                        static_cast<size_t>(result.n_frames)));
      }
      if (result.phase != nullptr) {
        out.emplace("phase", corpus_matrix(result.phase, static_cast<size_t>(result.n_bins),
                                           static_cast<size_t>(result.n_frames)));
      }
      value = corpus_object(std::move(out));
    }
    sonare_free_stft_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  return std::nullopt;
}

std::optional<CorpusCallResult> run_small_input_corpus(const std::string& entry,
                                                       const std::vector<float>& samples,
                                                       int sample_rate,
                                                       const std::string& fixture) {
  if (entry == "sonare_mixer_process_stereo") {
    SonareMixer* mixer = nullptr;
    const auto error = sonare_mixer_create(sample_rate, static_cast<int>(samples.size()), &mixer);
    const std::unique_ptr<SonareMixer, decltype(&sonare_mixer_destroy)> owner(mixer,
                                                                              sonare_mixer_destroy);
    if (error != SONARE_OK) return CorpusCallResult{error, {}};
    const SonareStrip* strip = sonare_mixer_add_strip(mixer, "strip0", nullptr);
    REQUIRE(strip != nullptr);
    std::vector<float> right(samples.rbegin(), samples.rend());
    for (float& value : right) value *= 0.5f;
    const float* in_left[] = {samples.data()};
    const float* in_right[] = {right.data()};
    std::vector<float> out_left(samples.size()), out_right(samples.size());
    REQUIRE(sonare_mixer_process_stereo(mixer, in_left, in_right, 1, out_left.data(),
                                        out_right.data(), samples.size()) == SONARE_OK);
    REQUIRE(sonare_mixer_flush_meters(mixer) == SONARE_OK);
    SonareMixMeterSnapshot snapshot{};
    REQUIRE(sonare_strip_meter(strip, SONARE_METER_TAP_POST_FADER, &snapshot) == SONARE_OK);
    REQUIRE(snapshot.num_channels == 2);
    CorpusJsonObject meter;
    corpus_put_measured(meter, "correlation", snapshot.correlation);
    corpus_put_measured(meter, "monoCompatWidth", snapshot.mono_compat_width);
    corpus_put_measured(meter, "momentaryLufs", snapshot.momentary_lufs);
    corpus_put_measured(meter, "shortTermLufs", snapshot.short_term_lufs);
    corpus_put_measured(meter, "integratedLufs", snapshot.integrated_lufs);
    corpus_put_measured(meter, "maxTruePeakDbtp", snapshot.max_true_peak_dbtp);
    meter.emplace("monoCompatPeak", corpus_float_value(snapshot.mono_compat_peak));
    meter.emplace("monoCompatSideRms", corpus_float_value(snapshot.mono_compat_side_rms));
    meter.emplace("likelyMonoCompatible", CorpusJson(snapshot.likely_mono_compatible != 0));
    meter.emplace("gainReductionDb", corpus_float_value(snapshot.gain_reduction_db));
    meter.emplace("seq", CorpusJson(static_cast<double>(snapshot.seq)));
    meter.emplace("numChannels", CorpusJson(snapshot.num_channels));
    meter.emplace("peakDb", corpus_float_array(snapshot.peak_db, 2));
    meter.emplace("rmsDb", corpus_float_array(snapshot.rms_db, 2));
    meter.emplace("truePeakDbtp", corpus_float_array(snapshot.true_peak_dbtp, 2));
    return CorpusCallResult{
        SONARE_OK,
        corpus_object({{"left", corpus_float_array(out_left.data(), out_left.size())},
                       {"right", corpus_float_array(out_right.data(), out_right.size())},
                       {"sampleRate", CorpusJson(sample_rate)},
                       {"meters", CorpusJson(CorpusJsonArray{corpus_object(std::move(meter))})}})};
  }
  if (entry == "sonare_playback_render_interleaved") {
    const char* document = R"({"target":{"kind":"speakers","layout":"stereo"}})";
    float* out = nullptr;
    size_t frames = 0;
    int channels = 0;
    const auto error =
        sonare_playback_render_interleaved(samples.data(), samples.size() / 2, 2, sample_rate,
                                           document, nullptr, &out, &frames, &channels);
    const std::unique_ptr<float, decltype(&sonare_free_floats)> owned(out, sonare_free_floats);
    CorpusJson result;
    if (error == SONARE_OK) {
      REQUIRE(frames == samples.size() / 2);
      REQUIRE(channels == 2);
      result = corpus_object({{"samples", corpus_float_array(owned.get(), frames * 2)},
                              {"numChannels", CorpusJson(channels)}});
    }
    return CorpusCallResult{error, std::move(result)};
  }
  if (entry == "sonare_lufs_series_interleaved") {
    const auto stereo = corpus_interleave_stereo(samples, fixture);
    float* momentary = nullptr;
    float* short_term = nullptr;
    size_t momentary_count = 0;
    size_t short_term_count = 0;
    const auto error =
        sonare_lufs_series_interleaved(stereo.data(), samples.size(), 2, sample_rate, &momentary,
                                       &momentary_count, &short_term, &short_term_count);
    auto result =
        corpus_object({{"momentaryLufs", corpus_float_array(momentary, momentary_count)},
                       {"shortTermLufs", corpus_float_array(short_term, short_term_count)}});
    sonare_free_floats(momentary);
    sonare_free_floats(short_term);
    return CorpusCallResult{error, std::move(result)};
  }
  if (entry == "sonare_decompose_note_pitch") {
    const float pitch[] = {440.0f, 441.0f, 442.0f, 0.0f, 438.0f, 437.0f, 440.0f};
    const float unvoiced[] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const bool measured = fixture != "empty";
    SonarePitchDecompositionResult result{};
    const auto error =
        sonare_decompose_note_pitch(measured ? pitch : unvoiced, nullptr, std::size(pitch), 100.0f,
                                    measured ? 440.0f : 0.0f, 0.0f, &result);
    CorpusJsonObject fields{
        {"driftCents", corpus_float_array(result.drift_cents, result.count)},
        {"vibratoCents", corpus_float_array(result.vibrato_cents, result.count)}};
    if (result.center_hz > 0) fields.emplace("centerHz", CorpusJson(result.center_hz));
    sonare_free_pitch_decomposition_result(&result);
    return CorpusCallResult{error, corpus_object(std::move(fields))};
  }
  if (entry == "sonare_note_targets_from_smf") {
    // Format 0, 96 ticks per quarter note, one middle-C note with a duration of one quarter note.
    static constexpr uint8_t smf[] = {'M', 'T', 'h', 'd',  0,   0,   0, 6,    0,    0,  0, 1,
                                      0,   96,  'M', 'T',  'r', 'k', 0, 0,    0,    12, 0, 0x90,
                                      60,  80,  96,  0x80, 60,  0,   0, 0xff, 0x2f, 0};
    SonareNoteTarget* raw = nullptr;
    size_t count = 0;
    const auto error = sonare_note_targets_from_smf(smf, std::size(smf), 0, &raw, &count);
    CorpusJsonArray notes;
    for (size_t i = 0; i < count; ++i) {
      notes.emplace_back(corpus_object({{"startSec", CorpusJson(raw[i].start_sec)},
                                        {"endSec", CorpusJson(raw[i].end_sec)},
                                        {"targetMidi", CorpusJson(raw[i].target_midi)}}));
    }
    sonare_free_note_targets(raw);
    return CorpusCallResult{error, CorpusJson(std::move(notes))};
  }
  return std::nullopt;
}

CorpusJson corpus_key(const SonareKey& key) {
  constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  REQUIRE(static_cast<size_t>(key.root) < std::size(names));
  REQUIRE((key.mode == SONARE_MODE_MAJOR || key.mode == SONARE_MODE_MINOR));
  const bool minor = key.mode == SONARE_MODE_MINOR;
  const std::string root = names[key.root];
  return corpus_object({{"root", CorpusJson(static_cast<int>(key.root))},
                        {"mode", CorpusJson(minor ? "minor" : "major")},
                        {"confidence", CorpusJson(key.confidence)},
                        {"name", CorpusJson(root + (minor ? " minor" : " major"))},
                        {"shortName", CorpusJson(root + (minor ? "m" : ""))}});
}

std::optional<CorpusCallResult> run_auto_tune_corpus(const std::string& entry,
                                                     const std::vector<float>& samples,
                                                     int sample_rate) {
  if (entry == "sonare_detect_key") {
    SonareKey key{};
    float tuning = 0.0f;
    const auto error =
        sonare_detect_key(samples.data(), samples.size(), sample_rate, nullptr, &key, &tuning);
    if (error != SONARE_OK) return CorpusCallResult{error, {}};
    auto value = corpus_key(key);
    value.as_object().emplace("tuningCents", CorpusJson(tuning));
    return CorpusCallResult{error, std::move(value)};
  }
  if (entry == "sonare_detect_chords") {
    SonareChordAnalysisResult result{};
    const auto error =
        sonare_detect_chords(samples.data(), samples.size(), sample_rate, nullptr, &result);
    CorpusJsonArray chords;
    for (size_t i = 0; i < result.chord_count; ++i) {
      const auto& chord = result.chords[i];
      sonare::Chord named{};
      named.root = static_cast<sonare::PitchClass>(chord.root);
      named.bass = static_cast<sonare::PitchClass>(chord.bass);
      named.quality = static_cast<sonare::ChordQuality>(chord.quality);
      chords.emplace_back(
          corpus_object({{"root", CorpusJson(static_cast<int>(chord.root))},
                         {"bass", CorpusJson(static_cast<int>(chord.bass))},
                         {"quality", CorpusJson(sonare::chord_quality_name(named.quality))},
                         {"name", CorpusJson(named.to_string())},
                         {"startSec", CorpusJson(chord.start_sec)},
                         {"endSec", CorpusJson(chord.end_sec)},
                         {"confidence", CorpusJson(chord.confidence)}}));
    }
    const float tuning = result.tuning_cents;
    sonare_free_chord_analysis_result(&result);
    return CorpusCallResult{error, corpus_object({{"chords", CorpusJson(std::move(chords))},
                                                  {"tuningCents", CorpusJson(tuning)}})};
  }
  if (entry == "sonare_detect_key_candidates") {
    SonareDetectKeyOptions options{};
    sonare_detect_key_options_init(&options);
    SonareKeyCandidate* raw = nullptr;
    size_t count = 0;
    const auto error = sonare_detect_key_candidates(samples.data(), samples.size(), sample_rate,
                                                    &options, &raw, &count, nullptr);
    CorpusJsonArray values;
    for (size_t i = 0; i < count; ++i) {
      values.emplace_back(corpus_object(
          {{"key", corpus_key(raw[i].key)}, {"correlation", CorpusJson(raw[i].correlation)}}));
    }
    sonare_free_key_candidates(raw);
    return CorpusCallResult{error, CorpusJson(std::move(values))};
  }
  if (entry != "sonare_auto_tune") return std::nullopt;
  float* raw = nullptr;
  size_t count = 0;
  SonareKey key{};
  const auto error = sonare_auto_tune(samples.data(), samples.size(), sample_rate, nullptr, nullptr,
                                      &raw, &count, &key);
  if (error != SONARE_OK) return CorpusCallResult{error, {}};
  auto output = corpus_float_array(raw, count);
  sonare_free_floats(raw);
  auto key_value = corpus_key(key);
  return CorpusCallResult{
      error, corpus_object({{"samples", std::move(output)}, {"key", std::move(key_value)}})};
}

std::optional<CorpusCallResult> run_separation_corpus(const std::string& entry,
                                                      const std::vector<float>& samples,
                                                      int sample_rate) {
  if (entry == "sonare_hpss") {
    SonareHpssOptions config{};
    sonare_hpss_options_init(&config);
    SonareHpssResult result{};
    const auto error =
        sonare_hpss(samples.data(), samples.size(), sample_rate, 31, 31, &config, &result, nullptr);
    CorpusJson out =
        corpus_object({{"harmonic", corpus_float_array(result.harmonic, result.length)},
                       {"percussive", corpus_float_array(result.percussive, result.length)},
                       {"sampleRate", CorpusJson(sample_rate)}});
    sonare_free_hpss_result(&result);
    return CorpusCallResult{error, std::move(out)};
  }
  if (entry == "sonare_decompose") {
    constexpr float spectrum[] = {3, 2, 1, 2, 3, 2, 1, 2, 4, 3, 2, 1,
                                  2, 4, 3, 1, 2, 4, 4, 1, 3, 2, 1, 3};
    float* w = nullptr;
    float* h = nullptr;
    size_t w_length = 0;
    size_t h_length = 0;
    const auto error =
        sonare_decompose(spectrum, 4, 6, 2, 50, 2, nullptr, &w, &w_length, &h, &h_length);
    CorpusJson out;
    if (error == SONARE_OK) {
      REQUIRE(w_length == 8);
      REQUIRE(h_length == 12);
      out = corpus_object({{"w", corpus_matrix(w, 4, 2)}, {"h", corpus_matrix(h, 2, 6)}});
    }
    sonare_free_floats(w);
    sonare_free_floats(h);
    return CorpusCallResult{error, std::move(out)};
  }
  if (entry == "sonare_decompose_stems" || entry == "sonare_decompose_stems_linked") {
    const bool linked = entry == "sonare_decompose_stems_linked";
    const auto right = result_corpus_fixture("historical_right", samples.size());
    REQUIRE(right.size() == samples.size());
    const float* channels[] = {samples.data(), right.data()};
    float* raw = nullptr;
    float* w = nullptr;
    float* h = nullptr;
    size_t frames = 0;
    size_t w_length = 0;
    size_t h_length = 0;
    int component_count = 0;
    const auto error =
        linked ? sonare_decompose_stems_linked(channels, std::size(channels), samples.size(),
                                               sample_rate, nullptr, &raw, &frames,
                                               &component_count, &w, &w_length, &h, &h_length)
               : sonare_decompose_stems(samples.data(), samples.size(), sample_rate, nullptr, &raw,
                                        &frames, &component_count, &w, &w_length, &h, &h_length);
    CorpusJson out;
    if (error == SONARE_OK) {
      CorpusJsonArray components;
      for (int k = 0; k < component_count; ++k) {
        if (linked) {
          const float* base = raw + static_cast<size_t>(k) * 2 * frames;
          components.emplace_back(CorpusJsonArray{corpus_float_array(base, frames),
                                                  corpus_float_array(base + frames, frames)});
        } else {
          components.emplace_back(
              corpus_float_array(raw + static_cast<size_t>(k) * frames, frames));
        }
      }
      out = corpus_object({{"components", CorpusJson(std::move(components))},
                           {"sampleRate", CorpusJson(sample_rate)},
                           {"w", corpus_float_array(w, w_length)},
                           {"h", corpus_float_array(h, h_length)}});
    }
    sonare_free_floats(raw);
    sonare_free_floats(w);
    sonare_free_floats(h);
    return CorpusCallResult{error, std::move(out)};
  }
  return std::nullopt;
}

std::optional<CorpusCallResult> run_constant_q_corpus(const std::string& entry,
                                                      const std::vector<float>& samples,
                                                      int sample_rate) {
  const auto* data = samples.data();
  const auto length = samples.size();
  if (entry == "sonare_bass_chroma" || entry == "sonare_chroma_cens" ||
      entry == "sonare_chroma_cqt") {
    SonareChromaResult result{};
    const auto error =
        entry == "sonare_bass_chroma"
            ? sonare_bass_chroma(data, length, sample_rate, 512, 12, &result)
        : entry == "sonare_chroma_cqt"
            ? sonare_chroma_cqt(data, length, sample_rate, 512, 12, nullptr, &result)
            : sonare_chroma_cens(data, length, sample_rate, 512, 12, nullptr, &result);
    auto value = corpus_object(
        {{"sampleRate", CorpusJson(result.sample_rate)},
         {"hopLength", CorpusJson(static_cast<double>(result.hop_length))},
         {"features", corpus_matrix(result.features, result.n_chroma, result.n_frames)},
         {"meanEnergy", corpus_float_array(result.mean_energy, result.n_chroma)}});
    sonare_free_chroma_result(&result);
    return CorpusCallResult{error, std::move(value)};
  }
  using Transform = decltype(&sonare_cqt);
  Transform transform = nullptr;
  if (entry == "sonare_cqt") transform = sonare_cqt;
  if (entry == "sonare_hybrid_cqt") transform = sonare_hybrid_cqt;
  if (entry == "sonare_pseudo_cqt") transform = sonare_pseudo_cqt;
  if (transform == nullptr && entry != "sonare_vqt") return std::nullopt;

  const sonare::CqtConfig config;
  SonareCqtResult result{};
  const auto error =
      transform != nullptr
          ? transform(data, length, sample_rate, config.hop_length, config.fmin, config.n_bins,
                      config.bins_per_octave, config.filter_scale, &result)
          : sonare_vqt(data, length, sample_rate, config.hop_length, config.fmin, config.n_bins,
                       config.bins_per_octave, config.filter_scale,
                       std::numeric_limits<float>::quiet_NaN(), &result);
  auto value =
      corpus_object({{"sampleRate", CorpusJson(result.sample_rate)},
                     {"hopLength", CorpusJson(static_cast<double>(result.hop_length))},
                     {"magnitude", corpus_matrix(result.magnitude, result.n_bins, result.n_frames)},
                     {"frequencies", corpus_float_array(result.frequencies, result.n_bins)},
                     {"gamma", CorpusJson(result.gamma)}});
  sonare_free_cqt_result(&result);
  return CorpusCallResult{error, std::move(value)};
}

std::optional<CorpusCallResult> run_pitch_track_corpus(const std::string& entry,
                                                       const std::vector<float>& samples,
                                                       int sample_rate) {
  if (entry != "sonare_pitch_yin" && entry != "sonare_pitch_pyin") return std::nullopt;
  SonarePitchResult result{};
  const auto error = entry == "sonare_pitch_yin"
                         ? sonare_pitch_yin(samples.data(), samples.size(), sample_rate, 2048, 512,
                                            65.0f, 2093.0f, 0.1f, &result)
                         : sonare_pitch_pyin(samples.data(), samples.size(), sample_rate, 2048, 512,
                                             65.0f, 2093.0f, 0.1f, 0, &result);
  CorpusJson value;
  if (error == SONARE_OK) {
    REQUIRE(result.n_frames >= 0);
    const auto count = static_cast<size_t>(result.n_frames);
    CorpusJsonArray flags;
    for (size_t i = 0; i < count; ++i) flags.emplace_back(result.voiced_flag[i] != 0);
    CorpusJsonObject fields{{"f0", corpus_float_array(result.f0, count)},
                            {"voicedProb", corpus_float_array(result.voiced_prob, count)},
                            {"voicedFlag", CorpusJson(std::move(flags))}};
    corpus_put_measured(fields, "meanF0", result.mean_f0);
    corpus_put_measured(fields, "medianF0", result.median_f0);
    value = corpus_object(std::move(fields));
  }
  sonare_free_pitch_result(&result);
  return CorpusCallResult{error, std::move(value)};
}

std::optional<CorpusCallResult> run_matrix_corpus(const std::string& entry,
                                                  const std::vector<float>& samples,
                                                  int sample_rate) {
  constexpr float values[] = {3, 2, 1, 2, 3, 2, 1, 2, 4, 3, 2, 1,
                              2, 4, 3, 1, 2, 4, 4, 1, 3, 2, 1, 3};
  const float* data = samples.data();
  const size_t length = samples.size();
  float* result = nullptr;
  size_t count = 0;
  int rows = 0;
  int cols = 0;
  SonareError error = SONARE_OK;
  if (entry == "sonare_mel_filterbank") {
    error = sonare_mel_filterbank(sample_rate, 128, 6, nullptr, &result, &count, &rows, &cols);
  } else if (entry == "sonare_pcen") {
    rows = 2;
    cols = 12;
    error = sonare_pcen(values, rows, cols, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, &result,
                        &count);
  } else if (entry == "sonare_mel_delta") {
    rows = 2;
    cols = 12;
    error = sonare_mel_delta(values, rows, cols, 9, &result);
    count = std::size(values);
  } else if (entry == "sonare_tonnetz") {
    rows = 6;
    cols = 2;
    error = sonare_tonnetz(values, 12, cols, &result, &count);
  } else if (entry == "sonare_nn_filter") {
    rows = 2;
    cols = 12;
    error = sonare_nn_filter(values, rows, cols, "mean", 7, 1, &result, &count);
  } else if (entry == "sonare_tempogram") {
    rows = 384;
    error = sonare_tempogram(data, length, 22050, 512, rows, 1, 1, SONARE_TEMPOGRAM_AUTOCORRELATION,
                             &result, &count, &cols);
  } else if (entry == "sonare_fourier_tempogram") {
    rows = 193;
    error = sonare_fourier_tempogram(data, length, 22050, 512, 384, 1, 1, &result, &count, &cols);
  } else if (entry == "sonare_cyclic_tempogram") {
    rows = 60;
    error = sonare_cyclic_tempogram(data, length, sample_rate, 512, 384, 60.0f, rows, &result,
                                    &count, &cols);
  } else if (entry == "sonare_poly_features") {
    error = sonare_poly_features(data, length, sample_rate, 2048, 512, 1, &result, &count, &rows,
                                 &cols);
  } else if (entry == "sonare_spectral_contrast") {
    error = sonare_spectral_contrast(data, length, sample_rate, 2048, 512, 6, 200.0f, 0.02f,
                                     &result, &count, &rows, &cols);
  } else if (entry == "sonare_onset_strength_multi") {
    rows = 3;
    error = sonare_onset_strength_multi(data, length, sample_rate, 2048, 512, 128, rows, &result,
                                        &count, &cols);
  } else if (entry == "sonare_nnls_chroma") {
    rows = 12;
    SonareNnlsChromaOptions options{};
    sonare_nnls_chroma_options_init(&options);
    error = sonare_nnls_chroma(data, length, sample_rate, &options, &result, &count, &cols);
  } else {
    return std::nullopt;
  }
  CorpusJson value;
  if (error == SONARE_OK) {
    REQUIRE(rows >= 0);
    REQUIRE(cols >= 0);
    REQUIRE(count == static_cast<size_t>(rows) * static_cast<size_t>(cols));
    value = corpus_matrix(result, rows, cols);
  }
  sonare_free_floats(result);
  return CorpusCallResult{error, std::move(value)};
}

struct SegmentCorpusMatrix {
  std::vector<float> values;
  int rows = 0;
  int cols = 0;
};
SegmentCorpusMatrix segment_corpus_matrix(const CorpusJson& input) {
  SegmentCorpusMatrix r;
  r.rows = input["rows"].as_int();
  r.cols = input["cols"].as_int();
  for (const auto& v : input["data"].as_array())
    r.values.push_back(static_cast<float>(v.as_number()));
  REQUIRE(r.rows > 0);
  REQUIRE(r.cols > 0);
  REQUIRE(r.values.size() == static_cast<size_t>(r.rows * r.cols));
  return r;
}
CorpusJson segment_indices_json(const SonareSegmentIndices& x) {
  CorpusJsonArray a;
  a.reserve(x.count);
  REQUIRE((x.values != nullptr || x.count == 0));
  for (size_t i = 0; i < x.count; ++i) a.emplace_back(static_cast<double>(x.values[i]));
  return CorpusJson(std::move(a));
}
std::optional<CorpusCallResult> run_segment_corpus(const std::string& entry, const CorpusJson& tc) {
  if (!tc.contains("input")) return std::nullopt;
  const auto& i = tc["input"];
  if (entry == "sonare_segment_cross_similarity") {
    auto x = segment_corpus_matrix(i["x"]);
    auto y = segment_corpus_matrix(i["y"]);
    SonareSegmentMatrix o{};
    auto e = sonare_segment_cross_similarity(x.values.data(), x.rows, x.cols, y.values.data(),
                                             y.rows, y.cols, 0, "cosine", "connectivity", &o);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(o.values, o.rows, o.cols);
    sonare_free_segment_matrix(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_recurrence_matrix") {
    auto d = segment_corpus_matrix(i["data"]);
    SonareSegmentMatrix o{};
    auto e = sonare_segment_recurrence_matrix(d.values.data(), d.rows, d.cols, 0, 1, 0, "euclidean",
                                              "connectivity", &o);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(o.values, o.rows, o.cols);
    sonare_free_segment_matrix(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_recurrence_to_lag") {
    auto d = segment_corpus_matrix(i["recurrence"]);
    SonareSegmentMatrix o{};
    auto e = sonare_segment_recurrence_to_lag(d.values.data(), d.rows, 1, &o);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(o.values, o.rows, o.cols);
    sonare_free_segment_matrix(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_lag_to_recurrence") {
    auto d = segment_corpus_matrix(i["lag"]);
    SonareSegmentMatrix o{};
    auto e = sonare_segment_lag_to_recurrence(d.values.data(), d.cols, d.rows, &o);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(o.values, o.rows, o.cols);
    sonare_free_segment_matrix(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_subsegment") {
    auto d = segment_corpus_matrix(i["data"]);
    std::vector<int> b;
    for (const auto& x : i["boundaries"].as_array()) b.push_back(x.as_int());
    SonareSegmentIndices o{};
    auto e = sonare_segment_subsegment(d.values.data(), d.rows, d.cols, b.data(), b.size(), 4, &o);
    CorpusJson v;
    if (e == SONARE_OK) v = segment_indices_json(o);
    sonare_free_segment_indices(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_agglomerative") {
    auto d = segment_corpus_matrix(i["data"]);
    SonareSegmentIndices o{};
    auto e = sonare_segment_agglomerative(d.values.data(), d.rows, d.cols, i["k"].as_int(),
                                          "average", &o);
    CorpusJson v;
    if (e == SONARE_OK) v = segment_indices_json(o);
    sonare_free_segment_indices(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_segment_path_enhance") {
    auto d = segment_corpus_matrix(i["recurrence"]);
    SonareSegmentMatrix o{};
    auto e = sonare_segment_path_enhance(d.values.data(), d.rows, i["win"].as_int(), 2, 0, 7, &o);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(o.values, o.rows, o.cols);
    sonare_free_segment_matrix(&o);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_mel_to_stft") {
    const auto mel = segment_corpus_matrix(i["mel"]);
    SonareInverseResult out{};
    const SonareError e = sonare_mel_to_stft(mel.values.data(), mel.values.size(), mel.rows,
                                             mel.cols, 22050, 64, 0.0f, 0.0f, nullptr, &out);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(out.data, out.rows, out.n_frames);
    sonare_free_inverse_result(&out);
    return CorpusCallResult{e, std::move(v)};
  }
  if (entry == "sonare_mfcc_to_mel") {
    const auto coefficients = segment_corpus_matrix(i["coefficients"]);
    SonareInverseResult out{};
    const SonareError e =
        sonare_mfcc_to_mel(coefficients.values.data(), coefficients.values.size(),
                           coefficients.rows, coefficients.cols, 128, nullptr, &out);
    CorpusJson v;
    if (e == SONARE_OK) v = corpus_matrix(out.data, out.rows, out.n_frames);
    sonare_free_inverse_result(&out);
    return CorpusCallResult{e, std::move(v)};
  }
  return std::nullopt;
}

CorpusCallResult metadata_json_result(SonareError error, char* raw) {
  CorpusJson value;
  if (error == SONARE_OK) {
    REQUIRE(raw != nullptr);
    value = sonare::util::json::parse_strict(raw);
  }
  sonare_free_string(raw);
  return {error, std::move(value)};
}
std::vector<uint8_t> tagged_wav_result_fixture() {
  char title_key[] = "title";
  char title_value[] = "Morning";
  char artist_key[] = "artist";
  char artist_value[] = "Ada";
  char* keys[] = {title_key, artist_key};
  char* values[] = {title_value, artist_value};
  SonareAudioTags tags{keys, values, 2};
  SonareEncodeOptions options{};
  sonare_encode_options_init(&options);
  options.tags = &tags;
  const float samples[] = {0.25f, -0.25f, 0.75f, -0.5f, -0.125f, 0.375f, 0.5f, -0.75f};
  uint8_t* raw = nullptr;
  size_t size = 0;
  REQUIRE(sonare_encode_audio(samples, 4, 2, 22050, &options, &raw, &size) == SONARE_OK);
  REQUIRE(raw != nullptr);
  std::vector<uint8_t> bytes(raw, raw + size);
  sonare_free_bytes(raw);
  return bytes;
}
std::optional<CorpusCallResult> run_metadata_result(const std::string& entry,
                                                    const std::vector<float>& samples,
                                                    int sample_rate, const std::string& fixture) {
  if (entry == "sonare_mastering_amp_preset_catalog") {
    char* raw = nullptr;
    auto e = sonare_mastering_amp_preset_catalog(&raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_mastering_preset_params_json") {
    char* raw = nullptr;
    auto e = sonare_mastering_preset_params_json("pop", &raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_mastering_chain_config_from_json") {
    char* raw = nullptr;
    const auto error = sonare_mastering_chain_config_from_json(R"({"params":{}})", &raw);
    return metadata_json_result(error, raw);
  }
  if (entry == "sonare_mastering_insert_param_info") {
    char* raw = nullptr;
    auto e = sonare_mastering_insert_param_info("eq.parametric", 48000, &raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_mixing_scene_preset_json") {
    char* raw = nullptr;
    auto e = sonare_mixing_scene_preset_json("vocalReverbSend", &raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_synth_engine_param_info") {
    char* raw = nullptr;
    auto e = sonare_synth_engine_param_info(SONARE_SYNTH_ENGINE_SUBTRACTIVE, &raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_synth_patch_param_info") {
    char* raw = nullptr;
    auto e = sonare_synth_patch_param_info(&raw);
    return metadata_json_result(e, raw);
  }
  if (entry == "sonare_waveform_peak_pyramid") {
    const size_t widths[] = {1, 2, 4};
    SonareWaveformPeakPyramidResult out{};
    auto e = sonare_waveform_peak_pyramid(samples.data(), samples.size(), 1, widths,
                                          std::size(widths), &out);
    CorpusJsonArray levels;
    if (e == SONARE_OK) {
      REQUIRE((out.levels != nullptr || out.level_count == 0));
      for (size_t i = 0; i < out.level_count; ++i)
        levels.push_back(waveform_peaks_corpus_result(out.levels[i]));
    }
    sonare_free_waveform_peak_pyramid_result(&out);
    return CorpusCallResult{e, CorpusJson(std::move(levels))};
  }
  if (entry == "sonare_decode_channels" || entry == "sonare_read_audio_tags") {
    REQUIRE(fixture == "tagged_wav");
    auto bytes = tagged_wav_result_fixture();
    if (entry == "sonare_decode_channels") {
      float* data = nullptr;
      size_t frames = 0;
      int channels = 0, rate = 0;
      auto e = sonare_decode_channels(bytes.data(), bytes.size(), &data, &frames, &channels, &rate);
      CorpusJsonObject out;
      if (e == SONARE_OK) {
        CorpusJsonArray planes;
        for (int c = 0; c < channels; ++c) {
          CorpusJsonArray plane;
          for (size_t i = 0; i < frames; ++i)
            plane.push_back(corpus_float_value(data[static_cast<size_t>(c) * frames + i]));
          planes.emplace_back(std::move(plane));
        }
        out.emplace("sampleRate", CorpusJson(rate));
        out.emplace("channels", CorpusJson(std::move(planes)));
      }
      sonare_free_floats(data);
      return CorpusCallResult{e, corpus_object(std::move(out))};
    }
    SonareAudioTags tags{};
    auto e = sonare_read_audio_tags(bytes.data(), bytes.size(), &tags);
    CorpusJsonObject out;
    if (e == SONARE_OK)
      for (size_t i = 0; i < tags.count; ++i) {
        REQUIRE((tags.keys[i] && tags.values[i]));
        out.emplace(tags.keys[i], CorpusJson(tags.values[i]));
      }
    sonare_free_audio_tags(&tags);
    return CorpusCallResult{e, corpus_object(std::move(out))};
  }
  if (entry == "sonare_chord_symbol_from_analysis") {
    uint32_t root = 0, quality = 0, slash = 255;
    uint8_t ext[32]{};
    size_t count = 0;
    auto e =
        sonare_chord_symbol_from_analysis(SONARE_PITCH_C, SONARE_CHORD_MAJOR, SONARE_PITCH_C, &root,
                                          &quality, ext, std::size(ext), &count, &slash);
    static constexpr const char* q[] = {"unknown",   "major",    "minor",          "diminished",
                                        "augmented", "dominant", "halfDiminished", "suspended"};
    CorpusJsonObject out;
    CorpusJsonArray extensions;
    if (e == SONARE_OK) {
      REQUIRE(quality < std::size(q));
      for (size_t i = 0; i < count; ++i) extensions.emplace_back(static_cast<int>(ext[i]));
      out.emplace("rootPc", CorpusJson(static_cast<int>(root)));
      out.emplace("quality", CorpusJson(q[quality]));
      out.emplace("extensions", CorpusJson(std::move(extensions)));
      if (slash != 255) out.emplace("slashBassPc", CorpusJson(static_cast<int>(slash)));
    }
    return CorpusCallResult{e, corpus_object(std::move(out))};
  }
  (void)sample_rate;
  return std::nullopt;
}

[[maybe_unused]] CorpusJson note_objects_corpus_result(const SonareNoteObjectsResult& result) {
  CorpusJsonArray notes;
  notes.reserve(result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    REQUIRE(note.frame_end >= note.frame_start);
    REQUIRE(note.amplitude_offset >= 0);
    REQUIRE(note.edit.amplitude_envelope_offset >= 0);
    const size_t amplitude_offset = static_cast<size_t>(note.amplitude_offset);
    const size_t amplitude_count = static_cast<size_t>(note.frame_end - note.frame_start);
    REQUIRE(amplitude_offset <= result.amplitude_count);
    REQUIRE(amplitude_count <= result.amplitude_count - amplitude_offset);
    const size_t envelope_offset = static_cast<size_t>(note.edit.amplitude_envelope_offset);
    const size_t envelope_count = note.edit.amplitude_envelope_count;
    REQUIRE(envelope_offset <= result.envelope_count);
    REQUIRE(envelope_count <= result.envelope_count - envelope_offset);
    const auto amplitude = corpus_float_array(
        result.amplitude == nullptr ? nullptr : result.amplitude + amplitude_offset,
        amplitude_count);
    const auto envelope = corpus_float_array(
        result.envelopes == nullptr ? nullptr : result.envelopes + envelope_offset, envelope_count);
    CorpusJsonObject edit{
        {"amplitudeEnvelope", envelope},
        {"driftChange", corpus_float_value(note.edit.drift_change)},
        {"formantSemitones", corpus_float_value(note.edit.formant_semitones)},
        {"gainDb", corpus_float_value(note.edit.gain_db)},
        {"muted", CorpusJson(note.edit.muted != 0)},
        {"pitchSemitones", corpus_float_value(note.edit.pitch_semitones)},
        {"timeOffsetSamples", CorpusJson(static_cast<double>(note.edit.time_offset_samples))},
        {"timeStretchRatio", corpus_float_value(note.edit.time_stretch_ratio)},
        {"vibratoDepthChange", corpus_float_value(note.edit.vibrato_depth_change)},
    };
    notes.push_back(corpus_object({
        {"amplitude", amplitude},
        {"edit", corpus_object(std::move(edit))},
        {"f0Stability", corpus_float_value(note.f0_stability)},
        {"frameEnd", CorpusJson(note.frame_end)},
        {"frameStart", CorpusJson(note.frame_start)},
        {"medianCents", corpus_float_value(note.median_cents)},
        {"medianHz", corpus_float_value(note.median_hz)},
        {"offsetSample", CorpusJson(static_cast<double>(note.offset_sample))},
        {"onsetSample", CorpusJson(static_cast<double>(note.onset_sample))},
    }));
  }
  return CorpusJson(std::move(notes));
}

CorpusCallResult run_note_ops_corpus(const std::string& entry, const std::vector<float>& samples,
                                     int sample_rate) {
#ifndef SONARE_WITH_PITCH_EDITOR
  (void)entry;
  (void)samples;
  (void)sample_rate;
  return {SONARE_ERROR_NOT_SUPPORTED, {}};
#else
  constexpr size_t kFrames = 32;
  constexpr size_t kHopLength = 256;
  const float frame_rate = static_cast<float>(sample_rate) / static_cast<float>(kHopLength);
  std::array<float, kFrames> f0{};
  f0.fill(440.0f);
  if (entry == "sonare_merge_notes") {
    std::fill(f0.begin() + 16, f0.end(), 660.0f);
  }
  std::array<int32_t, kFrames> voiced{};
  voiced.fill(1);

  SonareNoteObjectsResult source{};
  const std::unique_ptr<SonareNoteObjectsResult, decltype(&sonare_free_note_objects_result)>
      source_owner(&source, sonare_free_note_objects_result);
  SonareError error =
      sonare_extract_notes(samples.data(), samples.size(), sample_rate, f0.data(), nullptr,
                           voiced.data(), f0.size(), frame_rate, nullptr, &source);
  if (error != SONARE_OK) {
    return {error, {}};
  }

  if (entry == "sonare_assign_note_targets") {
    const SonareNoteTarget target{0.0, 1.0, 60.0f};
    size_t assigned = 0;
    error = sonare_assign_note_targets(source.notes, source.count, sample_rate, &target, 1, nullptr,
                                       &assigned);
    CorpusJson result;
    if (error == SONARE_OK) {
      result = corpus_object({
          {"assignedCount", CorpusJson(static_cast<double>(assigned))},
          {"notes", note_objects_corpus_result(source)},
      });
    }
    return {error, std::move(result)};
  }

  const size_t expected_count = entry == "sonare_split_note" ? 1 : 2;
  REQUIRE(source.count == expected_count);
  const std::array<float, 3> envelopes{{0.25f, 0.5f, 1.0f}};
  source.notes[0].edit.amplitude_envelope_offset = 0;
  source.notes[0].edit.amplitude_envelope_count = envelopes.size();

  SonareNoteObjectsResult result{};
  const std::unique_ptr<SonareNoteObjectsResult, decltype(&sonare_free_note_objects_result)>
      result_owner(&result, sonare_free_note_objects_result);
  if (entry == "sonare_split_note") {
    error = sonare_split_note(samples.data(), samples.size(), sample_rate, f0.data(), nullptr,
                              voiced.data(), f0.size(), frame_rate, nullptr, source.notes,
                              source.count, envelopes.data(), envelopes.size(), 0, 16, &result);
  } else {
    REQUIRE(entry == "sonare_merge_notes");
    error = sonare_merge_notes(samples.data(), samples.size(), sample_rate, f0.data(), nullptr,
                               voiced.data(), f0.size(), frame_rate, nullptr, source.notes,
                               source.count, envelopes.data(), envelopes.size(), 0, 1, &result);
  }
  CorpusJson projected;
  if (error == SONARE_OK) projected = note_objects_corpus_result(result);
  return {error, std::move(projected)};
#endif
}

CorpusCallResult voice_config_corpus_result(const std::string& entry, int sample_rate) {
  constexpr char kPresetName[] = "neutralMonitor";
  constexpr float kReverbDamping = 0.375f;
  SonareRealtimeVoiceChangerConfig config{};
  sonare_realtime_voice_changer_config_init(&config);
  SonareError error = SONARE_OK;
  if (entry == "sonare_realtime_voice_changer_config_from_json") {
    char* raw = nullptr;
    error = sonare_realtime_voice_changer_preset_json(kPresetName, &raw);
    const std::unique_ptr<char, decltype(&sonare_free_string)> owned(raw, sonare_free_string);
    if (error != SONARE_OK) return {error, {}};
    REQUIRE(raw != nullptr);
    auto document = sonare::util::json::parse_strict(raw);
    document.as_object().at("dsp").as_object().at("reverb").as_object()["damping"] =
        CorpusJson(kReverbDamping);
    const auto serialized = sonare::util::json::dump(document);
    error = sonare_realtime_voice_changer_config_from_json(serialized.c_str(), &config);
    if (error == SONARE_OK) CHECK(config.reverb_damping == kReverbDamping);
  } else {
    error = sonare_realtime_voice_changer_preset_config(SONARE_VC_PRESET_NEUTRAL_MONITOR, &config);
  }
  if (error != SONARE_OK) return {error, {}};
  SonareRealtimeVoiceChanger* changer = nullptr;
  error = sonare_realtime_voice_changer_create(&config, sample_rate, 128, 1, &changer);
  const std::unique_ptr<SonareRealtimeVoiceChanger,
                        decltype(&sonare_realtime_voice_changer_destroy)>
      owner(changer, sonare_realtime_voice_changer_destroy);
  if (error != SONARE_OK) return {error, {}};
  char* raw = nullptr;
  error = sonare_realtime_voice_changer_config_json(changer, &raw);
  const std::unique_ptr<char, decltype(&sonare_free_string)> owned(raw, sonare_free_string);
  if (error != SONARE_OK) return {error, {}};
  REQUIRE(raw != nullptr);
  return {error, sonare::util::json::parse_strict(raw)};
}

CorpusCallResult run_one_shot_result(const std::string& entry, const std::vector<float>& samples,
                                     int sample_rate, const std::string& fixture,
                                     const CorpusJson& test_case) {
  if (entry == "sonare_synth_enum_names") {
    return {SONARE_OK, synth_enum_tables_corpus_result()};
  }
  if (entry == "sonare_synth_preset_patch") {
    SonareSynthPatch patch{};
    const SonareError error = sonare_synth_preset_patch("sine", &patch);
    return {error, error == SONARE_OK ? synth_preset_patch_corpus_result(patch) : CorpusJson()};
  }
  if (entry == "sonare_assign_note_targets" || entry == "sonare_merge_notes" ||
      entry == "sonare_split_note") {
    return run_note_ops_corpus(entry, samples, sample_rate);
  }
  if (fixture == "midi") {
    if (auto mapped = run_midi_mapping_corpus(entry, test_case["midi"])) return std::move(*mapped);
    const auto result = run_midi_corpus(entry, test_case["midi"]);
    REQUIRE(result.has_value());
    return *result;
  }
  if (entry == "sonare_realtime_voice_changer_preset_config" ||
      entry == "sonare_realtime_voice_changer_config_from_json") {
    return voice_config_corpus_result(entry, sample_rate);
  }
  if (entry == "sonare_transcribe") {
    SonareTranscribeResult result{};
    const auto error =
        sonare_transcribe(samples.data(), samples.size(), sample_rate, 0, nullptr, &result);
    auto value = corpus_object({{"events", midi_corpus_events(result.events, result.count)},
                                {"noteCount", CorpusJson(static_cast<double>(result.note_count))},
                                {"tempoBpm", CorpusJson(result.tempo_bpm)},
                                {"tuningCents", CorpusJson(result.tuning_cents)}});
    sonare_free_transcribe_result(&result);
    return {error, std::move(value)};
  }
  if (entry == "sonare_realtime_voice_changer_preset_json" ||
      entry == "sonare_realtime_voice_changer_validate_preset_json") {
    char* document = nullptr;
    auto error = sonare_realtime_voice_changer_preset_json("neutralMonitor", &document);
    if (error != SONARE_OK) {
      sonare_free_string(document);
      return {error, {}};
    }
    REQUIRE(document != nullptr);
    if (entry == "sonare_realtime_voice_changer_preset_json") {
      const std::string text(document);
      sonare_free_string(document);
      return {error, sonare::util::json::parse_strict(text)};
    }
    char* normalized = nullptr;
    char* diagnostic = nullptr;
    error = sonare_realtime_voice_changer_validate_preset_json(document, &normalized, &diagnostic);
    sonare_free_string(document);
    sonare_free_string(diagnostic);
    if (error != SONARE_OK) {
      sonare_free_string(normalized);
      return {error, {}};
    }
    REQUIRE(normalized != nullptr);
    const std::string text(normalized);
    sonare_free_string(normalized);
    return {error, corpus_object({{"ok", CorpusJson(true)},
                                  {"normalizedJson", sonare::util::json::parse_strict(text)}})};
  }
  if (entry == "sonare_piptrack") {
    int bins = 0;
    int frames = 0;
    size_t count = 0;
    float* pitches = nullptr;
    float* magnitudes = nullptr;
    const auto error =
        sonare_piptrack(samples.data(), samples.size(), sample_rate, 512, 128, 150.0f, 4000.0f,
                        0.1f, &pitches, &magnitudes, &count, &bins, &frames);
    CorpusJson value;
    if (error == SONARE_OK) {
      REQUIRE(count == static_cast<size_t>(bins) * static_cast<size_t>(frames));
      value = corpus_object({{"pitches", corpus_matrix(pitches, bins, frames)},
                             {"magnitudes", corpus_matrix(magnitudes, bins, frames)}});
    }
    sonare_free_floats(pitches);
    sonare_free_floats(magnitudes);
    return {error, std::move(value)};
  }
  if (entry == "sonare_reassigned_spectrogram") {
    SonareReassignedSpectrogramResult out{};
    const auto error = sonare_reassigned_spectrogram(samples.data(), samples.size(), sample_rate,
                                                     512, 128, 1e-6f, 0, &out);
    CorpusJson value;
    if (error == SONARE_OK) {
      value = corpus_object(
          {{"magnitude", corpus_matrix(out.magnitude, out.n_bins, out.n_frames)},
           {"times", corpus_matrix(out.times, out.n_bins, out.n_frames)},
           {"frequencies", corpus_matrix(out.frequencies, out.n_bins, out.n_frames)}});
    }
    sonare_free_reassigned_spectrogram_result(&out);
    return {error, std::move(value)};
  }
  if (entry == "sonare_extract_notes") {
    const std::array<float, 16> f0{};
    const std::array<int32_t, 16> voiced{};
    SonareNoteObjectsResult out{};
    const auto error =
        sonare_extract_notes(samples.data(), samples.size(), sample_rate, f0.data(), nullptr,
                             voiced.data(), f0.size(), 100.0f, nullptr, &out);
    if (error == SONARE_OK) {
      REQUIRE(out.count == 0);
      REQUIRE(out.notes == nullptr);
      REQUIRE(out.amplitude_count == 0);
      REQUIRE(out.amplitude == nullptr);
      REQUIRE(out.envelope_count == 0);
      REQUIRE(out.envelopes == nullptr);
    }
    sonare_free_note_objects_result(&out);
    return {error, CorpusJson(CorpusJsonArray{})};
  }
  if (entry == "sonare_extract_percussive_events") {
    SonarePercussiveEventsResult out{};
    const auto error = sonare_extract_percussive_events(samples.data(), samples.size(), sample_rate,
                                                        nullptr, &out);
    if (error == SONARE_OK) {
      REQUIRE(out.count == 0);
      REQUIRE(out.events == nullptr);
    }
    sonare_free_percussive_events_result(&out);
    return {error, CorpusJson(CorpusJsonArray{})};
  }
  if (entry == "sonare_note_segments") {
    const std::array<float, 10> f0{{440, 440, 440, 440, 0, 880, 880, 880, 880, 0}};
    const std::array<int32_t, 10> voiced{{1, 1, 1, 1, 0, 1, 1, 1, 1, 0}};
    SonareNoteSegmentsResult out{};
    const auto error =
        sonare_note_segments(f0.data(), nullptr, voiced.data(), f0.size(), 100.0f, nullptr, &out);
    CorpusJsonArray segments;
    for (size_t i = 0; i < out.count; ++i) {
      const auto& note = out.segments[i];
      segments.push_back(corpus_object({{"frameStart", CorpusJson(note.frame_start)},
                                        {"frameEnd", CorpusJson(note.frame_end)},
                                        {"startSec", CorpusJson(note.start_sec)},
                                        {"endSec", CorpusJson(note.end_sec)},
                                        {"medianCents", CorpusJson(note.median_cents)}}));
    }
    sonare_free_note_segments_result(&out);
    return {error, CorpusJson(std::move(segments))};
  }
  if (auto matrix = run_matrix_corpus(entry, samples, sample_rate)) return std::move(*matrix);
  if (auto pitch = run_pitch_track_corpus(entry, samples, sample_rate)) return std::move(*pitch);
  if (auto result = run_metadata_result(entry, samples, sample_rate, fixture))
    return std::move(*result);
  if (auto cq = run_constant_q_corpus(entry, samples, sample_rate)) return std::move(*cq);
  if (auto separation = run_separation_corpus(entry, samples, sample_rate)) {
    return std::move(*separation);
  }
  if (auto small = run_small_input_corpus(entry, samples, sample_rate, fixture))
    return std::move(*small);
  if (auto tuned = run_auto_tune_corpus(entry, samples, sample_rate)) return std::move(*tuned);
  if (auto timeline = run_timeline_corpus(entry, samples, sample_rate)) return std::move(*timeline);
  if (auto room = run_room_corpus(entry, samples, sample_rate)) return std::move(*room);
  if (entry == "sonare_mixing_assistant_suggest") {
    return mix_assistant_corpus_result(samples, sample_rate);
  }
  if (auto result = run_segment_corpus(entry, test_case)) return std::move(*result);
  for (const auto run : {run_master_corpus, run_analysis_corpus, run_meter_corpus}) {
    if (auto result = run(entry, samples, sample_rate, fixture)) return std::move(*result);
  }
  const sonare::DynamicsConfig dynamics_config;
  const sonare::TimbreConfig timbre_config;
  const float* data = samples.empty() ? nullptr : samples.data();
  const size_t length = samples.size();
  if (entry == "sonare_detect_boundaries") {
    SonareBoundaryOptions options{};
    sonare_boundary_options_init(&options);
    SonareBoundaryResult result{};
    const SonareError error =
        sonare_detect_boundaries(data, length, sample_rate, &options, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = boundaries_corpus_result(result);
    sonare_free_boundary_result(&result);
    return {error, std::move(value)};
  }
  if (entry == "sonare_mastering_repair_detect_clipping") {
    SonareDeclipConfig config{};
    sonare_declip_config_init(&config);
    SonareClipDetection result{};
    const SonareError error =
        sonare_mastering_repair_detect_clipping(data, length, sample_rate, &config, &result);
    return {error, error == SONARE_OK ? clipping_corpus_result(result) : CorpusJson()};
  }
  if (entry == "sonare_analyze_dynamics") {
    SonareDynamicsResult result{};
    const SonareError error = sonare_analyze_dynamics(
        data, length, sample_rate, dynamics_config.window_sec, dynamics_config.hop_length,
        dynamics_config.compression_threshold, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = dynamics_corpus_result(result);
    sonare_free_dynamics_result(&result);
    return {error, std::move(value)};
  }
  if (entry == "sonare_analyze_timbre") {
    SonareTimbreResult result{};
    const SonareError error = sonare_analyze_timbre(
        data, length, sample_rate, timbre_config.n_fft, timbre_config.hop_length,
        timbre_config.n_mels, timbre_config.n_mfcc, timbre_config.window_sec, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = timbre_corpus_result(result);
    sonare_free_timbre_result(&result);
    return {error, std::move(value)};
  }
  if (entry == "sonare_lufs") {
    SonareLufsResult result{};
    const SonareError error = sonare_lufs(data, length, sample_rate, &result);
    return {error, error == SONARE_OK ? lufs_corpus_result(result) : CorpusJson()};
  }
  if (entry == "sonare_metering_dynamic_range") {
    SonareDynamicRangeResult result{};
    const SonareError error =
        sonare_metering_dynamic_range(data, length, sample_rate, 0.0f, 0.0f, -1.0f, -1.0f, &result);
    CorpusJson value;
    if (error == SONARE_OK) value = dynamic_range_corpus_result(result);
    sonare_free_dynamic_range_result(&result);
    return {error, std::move(value)};
  }
  if (entry == "sonare_trim_silence") {
    float* audio = nullptr;
    size_t count = 0;
    int64_t start_sample = 0;
    int64_t end_sample = 0;
    const SonareError error = sonare_trim_silence(
        data, length, sonare::kDefaultSilenceTopDb, sonare::kDefaultSilenceFrameLength,
        sonare::kDefaultSilenceHopLength, &audio, &count, &start_sample, &end_sample);
    CorpusJson value;
    if (error == SONARE_OK) value = trim_corpus_result(audio, count, start_sample, end_sample);
    sonare_free_floats(audio);
    return {error, std::move(value)};
  }
  if (entry == "sonare_mastering_audio_profile") {
    char* raw = nullptr;
    const SonareMasteringParam detect_defects{"detectDefects", 1.0};
    const bool measure_defects = fixture == "profile_silence" || fixture == "clipped" ||
                                 fixture == "historical_reference_right";
    const SonareError error = sonare_mastering_audio_profile(
        data, length, sample_rate, measure_defects ? &detect_defects : nullptr,
        measure_defects ? 1 : 0, &raw);
    if (error != SONARE_OK) {
      sonare_free_string(raw);
      return {error, CorpusJson()};
    }
    CorpusJson value = profile_corpus_result(raw);
    sonare_free_string(raw);
    if (measure_defects) {
      const auto& clip = value["defects"]["clip"];
      REQUIRE(clip.is_object());
      CHECK(clip.contains("flatLevel") == (fixture == "clipped"));
    }
    return {error, std::move(value)};
  }
  FAIL("unknown one-shot result corpus entry");
  return {};
}

std::string corpus_path_leaf(const std::string& path) {
  const size_t separator = path.rfind('.');
  const size_t start = separator == std::string::npos ? 0 : separator + 1;
  const size_t index = path.find('[', start);
  return path.substr(start, index == std::string::npos ? index : index - start);
}

bool corpus_integer_field(const std::string& path) {
  const std::string field = corpus_path_leaf(path);
  const auto ends_with = [&field](const char* suffix) {
    const std::string value(suffix);
    return field.size() >= value.size() &&
           field.compare(field.size() - value.size(), value.size(), value) == 0;
  };
  return field == "count" || field == "index" || field == "frame" || field == "sampleRate" ||
         field == "hopLength" || field == "rows" || field == "cols" || field == "nFft" ||
         field == "unison" || field == "polyphony" || field == "sampleSet" ||
         field == "startSample" || field == "endSample" || field == "onsetSample" ||
         field == "offsetSample" || field == "frameStart" || field == "frameEnd" ||
         field == "first" || field == "lastExclusive" || field == "numerator" ||
         field == "denominator" || field == "harmonics" || field == "bandFloorBand" ||
         field == "group" || field == "nMels" || field == "length" || field == "numChannels" ||
         field == "frameStride" || field == "samplesPerBucket" || ends_with("Count") ||
         ends_with("Index") || ends_with("Samples");
}

double corpus_numeric_array_scale(const CorpusJsonArray& actual, const CorpusJsonArray& expected) {
  double scale = 0.0;
  bool has_numbers = false;
  const size_t shared = std::min(actual.size(), expected.size());
  for (size_t i = 0; i < shared; ++i) {
    if (actual[i].is_number() && expected[i].is_number()) {
      has_numbers = true;
      scale = std::max(scale, std::abs(expected[i].as_number()));
    }
  }
  return has_numbers ? scale : -1.0;
}

double corpus_tolerance(double fallback, const CorpusJsonObject* fields, const std::string& path,
                        double scale) {
  if (fields == nullptr) return fallback * scale;
  std::string normalized;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '[') {
      normalized += "[]";
      i = path.find(']', i);
      REQUIRE(i != std::string::npos);
    } else {
      normalized += path[i];
    }
  }
  while (true) {
    const auto found = fields->find(normalized);
    if (found != fields->end()) return found->second.as_number();
    if (normalized.size() < 2 || normalized.compare(normalized.size() - 2, 2, "[]") != 0) break;
    normalized.resize(normalized.size() - 2);
  }
  const auto any = fields->find("*");
  return any == fields->end() ? 0.0 : any->second.as_number() * scale;
}

void compare_corpus_json(const CorpusJson& actual, const CorpusJson& expected, double tolerance,
                         const std::string& path, double array_scale = -1.0,
                         const CorpusJsonObject* fields = nullptr) {
  INFO(path);
  if (actual.is_object() && expected.is_object()) {
    const auto& actual_object = actual.as_object();
    const auto& expected_object = expected.as_object();
    CHECK(actual_object.size() == expected_object.size());
    for (const auto& [key, expected_value] : expected_object) {
      const auto found = actual_object.find(key);
      CHECK(found != actual_object.end());
      if (found != actual_object.end()) {
        compare_corpus_json(found->second, expected_value, tolerance,
                            path.empty() ? key : path + "." + key, -1.0, fields);
      }
    }
    return;
  }
  if (actual.is_array() && expected.is_array()) {
    const auto& actual_array = actual.as_array();
    const auto& expected_array = expected.as_array();
    CHECK(actual_array.size() == expected_array.size());
    const size_t shared = std::min(actual_array.size(), expected_array.size());
    const double scale = corpus_numeric_array_scale(actual_array, expected_array);
    for (size_t i = 0; i < shared; ++i) {
      compare_corpus_json(actual_array[i], expected_array[i], tolerance,
                          path + "[" + std::to_string(i) + "]", scale, fields);
    }
    return;
  }
  if (actual.is_number() && expected.is_number()) {
    const double actual_number = actual.as_number();
    const double expected_number = expected.as_number();
    if (corpus_integer_field(path)) {
      CHECK(actual_number == expected_number);
    } else {
      const double scale = array_scale >= 0.0 ? array_scale : std::abs(expected_number);
      CHECK(std::abs(actual_number - expected_number) <=
            corpus_tolerance(tolerance, fields, path, scale));
    }
    return;
  }
  if (actual.is_string() && expected.is_string()) {
    CHECK(actual.as_string() == expected.as_string());
    return;
  }
  if (actual.is_bool() && expected.is_bool()) {
    CHECK(actual.as_bool() == expected.as_bool());
    return;
  }
  CHECK((actual.is_null() && expected.is_null()));
}

CorpusJson load_public_result_corpus() {
  std::ifstream input("tests/conformance/public_result_corpus.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

}  // namespace

TEST_CASE("public input corpus preserves atomic marker state across the C ABI",
          "[conformance][c_api][engine]") {
  const auto corpus = load_public_input_corpus();
  const auto& transaction = corpus["marker_transaction"];
  const auto initial = corpus_markers(transaction["initial"]);

  SonareEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, 48000, 128, 16, 16, 0) == SONARE_OK);

  for (const auto& test_case : transaction["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      if (!test_case["c_representable"].as_bool()) continue;
      REQUIRE(sonare_engine_set_markers(engine, initial.data(), initial.size()) == SONARE_OK);
      const auto before = marker_snapshot(engine);
      const auto candidate = corpus_markers(test_case["markers"]);
      const SonareError result =
          sonare_engine_set_markers(engine, candidate.data(), candidate.size());
      if (test_case["accepted"].as_bool()) {
        REQUIRE(result == SONARE_OK);
        REQUIRE(marker_snapshot(engine) == [&candidate] {
          std::vector<MarkerBytes> bytes(candidate.size());
          for (size_t i = 0; i < candidate.size(); ++i) {
            std::memcpy(bytes[i].data(), &candidate[i], sizeof(candidate[i]));
          }
          return bytes;
        }());
      } else {
        REQUIRE(result == SONARE_ERROR_INVALID_PARAMETER);
        REQUIRE(marker_snapshot(engine) == before);
      }
    }
  }

  sonare_engine_destroy(engine);
}

#ifdef SONARE_WITH_MIXING
TEST_CASE("public input corpus keeps mixer option rejection conformant across the C ABI",
          "[conformance][c_api][mixing]") {
  const auto corpus = load_public_input_corpus();
  for (const auto& test_case : corpus["mix_options"]["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      if (!test_case["c_representable"].as_bool()) continue;

      SonareMixer* mixer = nullptr;
      REQUIRE(sonare_mixer_create(48000, 128, &mixer) == SONARE_OK);
      REQUIRE(mixer != nullptr);
      SonareStrip* strip = sonare_mixer_add_strip(mixer, "corpus", nullptr);
      REQUIRE(strip != nullptr);

      const std::string& field = test_case["field"].as_string();
      const float value = corpus_float(test_case["value"]);
      SonareError result = SONARE_OK;
      if (field == "input_trim_db") {
        result = sonare_strip_set_input_trim_db(strip, value);
      } else if (field == "fader_db") {
        result = sonare_strip_set_fader_db(strip, value);
      } else if (field == "pan") {
        result = sonare_strip_set_pan(strip, value, SONARE_PAN_MODE_BALANCE);
      } else if (field == "pan_mode") {
        result = sonare_strip_set_pan(strip, 0.0f, static_cast<int>(value));
      } else if (field == "width") {
        result = sonare_strip_set_width(strip, value);
      } else {
        FAIL("unknown mix option corpus field");
      }

      REQUIRE(result == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_strip_set_fader_db(strip, 0.0f) == SONARE_OK);
      sonare_mixer_destroy(mixer);
    }
  }
}
#endif

#ifdef SONARE_WITH_MIXING
TEST_CASE("public input corpus keeps the bus role set closed across the C ABI",
          "[conformance][c_api][mixing]") {
  const auto corpus = load_public_input_corpus();
  const auto& section = corpus["mixer_bus_role"];
  for (const auto& test_case : section["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      SonareMixer* mixer = nullptr;
      REQUIRE(sonare_mixer_create(48000, 128, &mixer) == SONARE_OK);
      const bool absent = test_case["role"].is_null();
      const std::string role = absent ? std::string() : test_case["role"].as_string();
      const SonareError result =
          sonare_mixer_add_bus(mixer, "corpus", absent ? nullptr : role.c_str());
      if (test_case.contains("stored")) {
        REQUIRE(result == SONARE_OK);
        char* json = nullptr;
        REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
        const auto scene = sonare::util::json::parse_strict(json);
        sonare_free_string(json);
        bool found = false;
        for (const auto& bus : scene["buses"].as_array()) {
          if (bus["id"].as_string() != "corpus") continue;
          CHECK(bus["role"].as_string() == test_case["stored"].as_string());
          found = true;
        }
        CHECK(found);
      } else {
        CHECK(result == SONARE_ERROR_INVALID_PARAMETER);
        CHECK(std::string(sonare_last_error_message()) == section["message"].as_string());
      }
      sonare_mixer_destroy(mixer);
    }
  }
}
#endif

#ifdef SONARE_WITH_ARRANGEMENT
TEST_CASE("public input corpus keeps project document enums as tokens across the C ABI",
          "[conformance][c_api][project]") {
  const auto corpus = load_public_input_corpus();
  const auto& section = corpus["project_document_enum_tokens"];
  for (const auto& test_case : section["cases"].as_array()) {
    DYNAMIC_SECTION(test_case["id"].as_string()) {
      const std::string json = sonare::util::json::dump(test_case["document"]);
      SonareProject* project = nullptr;
      char* diagnostics = nullptr;
      const SonareError result =
          sonare_project_deserialize(json.c_str(), json.size(), &project, &diagnostics);
      const std::string diagnostic_text = diagnostics != nullptr ? diagnostics : "";
      sonare_free_string(diagnostics);
      if (test_case.contains("accepted")) {
        REQUIRE(result == SONARE_OK);
        sonare_project_destroy(project);
      } else {
        CHECK(result == SONARE_ERROR_INVALID_FORMAT);
        CHECK(project == nullptr);
        CHECK(diagnostic_text == "invalid_format: " + test_case["message"].as_string());
      }
    }
  }
}
#endif

namespace {

void check_result_corpus(bool slow) {
  CorpusJson corpus = load_public_result_corpus();
  auto& section = corpus.as_object().at("one_shot_results");
  const int sample_rate = section["sample_rate"].as_int();
  const size_t frames = static_cast<size_t>(section["frames"].as_int());
  const double tolerance = section["relative_tolerance"].as_number();
  const char* update_env = std::getenv("SONARE_UPDATE_RESULT_CORPUS");
  const bool update = update_env != nullptr && std::string(update_env) == "1";
#ifndef SONARE_WITH_MASTERING
  REQUIRE_FALSE(update);
#endif

  for (auto& test_case : section.as_object().at("cases").as_array()) {
    if ((test_case.contains("slow") && test_case["slow"].as_bool()) != slow) continue;
    INFO(test_case["id"].as_string());
    const std::string entry = test_case["entry"].as_string();
    const auto& fixture = test_case["fixture"].as_string();
    const auto samples = result_corpus_fixture(fixture, frames);
    const auto actual = run_one_shot_result(entry, samples, sample_rate, fixture, test_case);
#ifndef SONARE_WITH_ARRANGEMENT
    if (entry == "sonare_synth_enum_names") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_OK);
      for (const auto& [name, names] : actual.result.as_object()) {
        INFO(name);
        CHECK(names.as_array().empty());
      }
      continue;
    }
#endif
#ifndef SONARE_WITH_PLAYBACK
    if (entry == "sonare_playback_render_interleaved") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_ACOUSTIC_SIM
    if (entry == "sonare_estimate_room" || entry == "sonare_synthesize_rir" ||
        entry == "sonare_room_morph" || entry == "sonare_room_morph_stereo" ||
        entry == "sonare_room_geometry_from_estimate") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_PITCH_EDITOR
    if (entry == "sonare_auto_tune" || entry == "sonare_extract_notes" ||
        entry == "sonare_assign_note_targets" || entry == "sonare_merge_notes" ||
        entry == "sonare_split_note" || entry == "sonare_note_segments" ||
        entry == "sonare_decompose_note_pitch") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_ARRANGEMENT
    if (fixture == "midi" || entry == "sonare_align_take_to_reference" ||
        entry == "sonare_chord_symbol_from_analysis" || entry == "sonare_note_targets_from_smf" ||
        entry == "sonare_synth_engine_param_info" || entry == "sonare_synth_patch_param_info" ||
        entry == "sonare_synth_preset_patch") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_MIXING
    if (entry == "sonare_mixer_process_stereo" || entry == "sonare_mixing_scene_preset_json") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_VOICE_CHANGER
    if (entry.rfind("sonare_realtime_voice_changer_", 0) == 0) {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#if !defined(SONARE_WITH_PITCH_EDITOR) || !defined(SONARE_WITH_ARRANGEMENT)
    if (entry == "sonare_transcribe") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_MIXING_ASSISTANT
    if (entry == "sonare_mixing_assistant_suggest") {
      REQUIRE_FALSE(update);
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_MASTERING
    if (entry == "sonare_capability_catalog_json" || entry == "sonare_master_audio" ||
        entry == "sonare_master_audio_stereo" || entry.rfind("sonare_mastering_", 0) == 0 ||
        entry.rfind("sonare_streaming_loudness_gain", 0) == 0) {
      CHECK(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
    REQUIRE(actual.error == SONARE_OK);
    if (test_case.contains("catalogReference")) {
      auto catalog = read_corpus_json("tools/capability-catalog.json");
      const auto& reference = test_case["catalogReference"].as_string();
      if (reference == "all") {
        char* raw = nullptr;
        REQUIRE(sonare_capabilities_json(&raw) == SONARE_OK);
        const std::unique_ptr<char, decltype(&sonare_free_string)> owned(raw, sonare_free_string);
        REQUIRE(raw != nullptr);
        const auto capabilities = sonare::util::json::parse_strict(raw);
        catalog.as_object().at("encode").as_object().at("ffmpeg") =
            capabilities["encode"]["ffmpeg"];
        auto& presets = catalog.as_object().at("presets").as_object();
        const std::pair<const char*, const char*> feature_presets[] = {
            {"arrangement", "synth"},
            {"mixing", "mixingScene"},
            {"voiceChanger", "voiceChanger"},
            {"playback", "playbackRoom"}};
        for (const auto& [feature, preset] : feature_presets) {
          if (!sonare_has_feature(feature)) presets.at(preset) = CorpusJson(CorpusJsonArray{});
        }
        compare_corpus_json(actual.result, catalog, tolerance, "result");
      } else {
        REQUIRE(reference == "processors");
        compare_corpus_json(actual.result, catalog["processors"], tolerance, "result");
      }
    } else if (update) {
      test_case.as_object()["result"] = actual.result;
    } else {
      const auto* fields =
          test_case.contains("tolerances") ? &test_case["tolerances"].as_object() : nullptr;
      compare_corpus_json(actual.result, test_case["result"], tolerance, "result", -1.0, fields);
    }
  }

  for (const auto& refusal : section.as_object().at("refusals").as_array()) {
    if (slow) break;
    INFO(refusal["id"].as_string());
    const std::string fixture = refusal["fixture"].as_string();
    const std::string entry = refusal["entry"].as_string();
    REQUIRE((fixture == "empty" || fixture == "midi"));
    const auto samples = result_corpus_fixture(fixture, frames);
    const auto actual = run_one_shot_result(entry, samples, sample_rate, fixture, refusal);
#ifndef SONARE_WITH_ARRANGEMENT
    if (fixture == "midi") {
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
#ifndef SONARE_WITH_MASTERING
    if (entry == "sonare_master_audio" || entry == "sonare_master_audio_stereo" ||
        entry.rfind("sonare_mastering_", 0) == 0 ||
        entry.rfind("sonare_streaming_loudness_gain", 0) == 0) {
      REQUIRE(actual.error == SONARE_ERROR_NOT_SUPPORTED);
      continue;
    }
#endif
    REQUIRE(static_cast<int>(actual.error) == refusal["code"].as_int());
    REQUIRE_FALSE(refusal["js_class"].as_string().empty());
    REQUIRE_FALSE(refusal["python_class"].as_string().empty());
  }

  if (update) {
    std::ofstream output("tests/conformance/public_result_corpus.json", std::ios::trunc);
    REQUIRE(output.good());
    output << sonare::util::json::dump_pretty(corpus, 2) << std::endl;
    REQUIRE(output.good());
  }
}

}  // namespace

TEST_CASE("one-shot result corpus matches the C ABI reference",
          "[conformance][result_corpus][c_api]") {
  check_result_corpus(false);
}

TEST_CASE("slow one-shot results match the C ABI reference",
          "[.][slow][conformance][result_corpus][c_api]") {
  check_result_corpus(true);
}
