/// @file note_transcriber_eval_test.cpp
/// @brief Note-level polyphonic transcription accuracy against rendered scores.
///
/// Every item is a score played through the GM fallback bank's piano, so the
/// ground truth is the note list that was rendered rather than an annotation.
/// A transcribed note matches a true one when its onset is within 50 ms and its
/// median F0 within 50 cents, one-to-one, the pairs with the smallest onset
/// difference taken first. Each item prints two `TRANSCRIBE_EVAL {json}` lines,
/// one under the transcription defaults and one under the editing chain's limits
/// (four voices, 140 ms, no re-strike split), so the two can be read side by side.
///
/// Hidden by default: rendering and transcribing every item twice runs for many
/// seconds. The sweep case prints one `TRANSCRIBE_SWEEP {json}` line per item for
/// each candidate default, varied one at a time, and asserts nothing.
///
/// The floors were taken from a Release run, minus 0.05; a Debug run may fall
/// below them, so read a Debug failure against a Release one before acting on it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_transcriber.h"
#include "editing/polyphony/multi_f0.h"
#include "midi/ump.h"
#include "support/midi_sequence_render.h"
#include "util/constants.h"
#include "util/db.h"

namespace {

using sonare::editing::note_model::resolve_transcribe_f0_range;
using sonare::editing::note_model::resolve_transcribe_polyphony_limits;
using sonare::editing::note_model::transcribe_notes;
using sonare::editing::note_model::TranscribeConfig;
using sonare::editing::note_model::TranscribedNote;
using sonare::editing::note_model::TranscribeF0Range;
using sonare::editing::note_model::TranscribePolyphonyLimits;
using sonare::editing::note_model::TranscribeSource;
using sonare::editing::polyphony::extract_multi_f0;
using sonare::editing::polyphony::MultiF0ExtractorConfig;
using sonare::editing::polyphony::MultiF0Track;
using sonare::test::render_sequence_mono;
using sonare::test::TimedUmp;

using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

/// The polyphony framing is calibrated at this rate.
constexpr int kSampleRate = 44100;
constexpr uint8_t kPianoProgram = 0;
constexpr uint8_t kVelocity = 90;
constexpr double kOnsetToleranceSec = 0.050;
constexpr double kPitchToleranceCents = 50.0;
/// Silence before the first note, so no onset sits on the centre-padded frame.
constexpr double kLeadSec = 0.1;
/// Ring-out after a line's last note, so it is not cut off at its own onset.
constexpr double kTailSec = 0.2;
constexpr double kBpm = 128.0;
constexpr double kSecondsPerMinute = 60.0;
constexpr float kNoiseRmsDbfs = -20.0f;
constexpr double kNoiseSec = 5.0;

#ifdef NDEBUG
constexpr const char* kBuild = "Release";
#else
constexpr const char* kBuild = "Debug";
#endif

struct ScoreNote {
  double onset_sec;
  double offset_sec;
  uint8_t pitch;
  uint8_t velocity;
};

enum class Source { kPiano, kWhiteNoise, kPinkNoise };

struct Item {
  std::string name;
  Source source = Source::kPiano;
  std::vector<ScoreNote> notes;
  double seconds = 0.0;
  /// default.recall >= legacy.recall is asserted.
  bool recall_gate = false;
  /// The default config must report no false alarm.
  bool no_false_alarm = false;
  /// Release-measured default F-measure minus 0.05, floored at 0.
  double min_f_measure = 0.0;
};

double note_hz(int midi_note) {
  return kA4Hz * std::exp2((static_cast<double>(midi_note) - kMidiA4) / kSemitonesPerOctave);
}

int64_t to_sample(double sec) { return static_cast<int64_t>(std::llround(sec * kSampleRate)); }

Item chord(const std::string& name, const std::vector<uint8_t>& pitches, bool gate, double floor) {
  Item item;
  item.name = name;
  item.seconds = kLeadSec + 1.5;
  for (uint8_t p : pitches) item.notes.push_back({kLeadSec, item.seconds, p, kVelocity});
  item.recall_gate = gate;
  item.min_f_measure = floor;
  return item;
}

Item sixteenth_line(double floor) {
  const double step = kSecondsPerMinute / kBpm / 4.0;
  const std::vector<uint8_t> pitches = {60, 62, 64, 65, 67, 69, 71, 72, 74,
                                        72, 71, 69, 67, 65, 64, 62, 60};
  Item item;
  item.name = "sixteenths 128bpm";
  for (size_t i = 0; i < pitches.size(); ++i) {
    const double on = kLeadSec + static_cast<double>(i) * step;
    item.notes.push_back({on, on + 0.9 * step, pitches[i], kVelocity});
  }
  item.seconds = kLeadSec + static_cast<double>(pitches.size()) * step + kTailSec;
  item.recall_gate = true;
  item.min_f_measure = floor;
  return item;
}

Item melody_over_chord(double floor) {
  const double step = kSecondsPerMinute / kBpm / 2.0;
  // No pitch class of the held C major triad, so no melody note doubles one.
  const std::vector<uint8_t> melody = {69, 71, 74, 77, 74, 71, 69, 65};
  Item item;
  item.name = "melody over held chord";
  item.seconds = kLeadSec + static_cast<double>(melody.size()) * step + kTailSec;
  for (const int p : {48, 52, 55}) {
    item.notes.push_back({kLeadSec, item.seconds, static_cast<uint8_t>(p), 70});
  }
  for (size_t i = 0; i < melody.size(); ++i) {
    const double on = kLeadSec + static_cast<double>(i) * step;
    item.notes.push_back({on, on + 0.9 * step, melody[i], kVelocity});
  }
  item.min_f_measure = floor;
  return item;
}

/// Four strikes of one key, each struck again while the previous still sounds.
Item restrike(int interval_ms, double velocity_ratio, double floor) {
  const double step = interval_ms / 1000.0;
  constexpr int kStrikes = 4;
  constexpr uint8_t kPitch = 60;
  Item item;
  char name[64];
  std::snprintf(name, sizeof(name), "restrike %dms vel %.1f", interval_ms, velocity_ratio);
  item.name = name;
  item.seconds = kLeadSec + (kStrikes - 1) * step + 0.5;
  for (int i = 0; i < kStrikes; ++i) {
    const double on = kLeadSec + i * step;
    const double off = i + 1 < kStrikes ? on + step : item.seconds;
    const auto velocity =
        static_cast<uint8_t>(i == 0 ? kVelocity : std::lround(kVelocity * velocity_ratio));
    item.notes.push_back({on, off, kPitch, velocity});
  }
  item.recall_gate = velocity_ratio == 1.0;
  item.min_f_measure = floor;
  return item;
}

Item held_note(double floor) {
  Item item;
  item.name = "held note";
  item.seconds = kLeadSec + 2.0;
  item.notes.push_back({kLeadSec, item.seconds, 60, kVelocity});
  item.no_false_alarm = true;
  item.min_f_measure = floor;
  return item;
}

Item noise(const char* name, Source source) {
  Item item;
  item.name = name;
  item.source = source;
  item.seconds = kNoiseSec;
  return item;
}

/// The floors sit 0.05 under the Release figures of the estimator that takes a
/// sounding note before the peak on its partial. Two groups read lower than
/// under the greedy order: an octave doubling's upper voice is now lost whenever
/// its lower voice sounds (full-partial subtraction had decided that whichever
/// note came first), and a re-strike 150 ms after an equal strike climbs to
/// about twice the level held before it, which at the default ratio is read as
/// a re-strike three times in four. The eight-voice octave chord's C4 ridge runs
/// 46 ms, under the 60 ms minimum, once its onset sits at the strike.
const std::vector<Item>& corpus() {
  static const std::vector<Item> items = {
      chord("chord 5 voices", {48, 52, 55, 59, 62}, true, 0.78),
      chord("chord 5 voices octave", {48, 55, 60, 64, 72}, false, 0.70),
      chord("chord 6 voices", {48, 52, 55, 59, 62, 66}, true, 0.55),
      chord("chord 6 voices octave", {48, 55, 60, 64, 67, 72}, false, 0.61),
      chord("chord 8 voices", {48, 52, 55, 59, 62, 66, 69, 73}, true, 0.51),
      chord("chord 8 voices octave", {48, 52, 55, 60, 64, 67, 72, 76}, false, 0.38),
      sixteenth_line(0.95),
      melody_over_chord(0.55),
      restrike(250, 1.0, 0.95),
      restrike(250, 0.7, 0.80),
      restrike(250, 0.5, 0.80),
      restrike(150, 1.0, 0.80),
      restrike(150, 0.7, 0.52),
      restrike(150, 0.5, 0.52),
      held_note(0.95),
      noise("white noise", Source::kWhiteNoise),
      noise("pink noise", Source::kPinkNoise),
  };
  return items;
}

/// Fixed-seed noise at kNoiseRmsDbfs RMS, so a figure that moves is the model.
std::vector<float> render_noise(Source source, int samples) {
  std::vector<float> out(static_cast<size_t>(samples), 0.0f);
  uint32_t state = 0x9e3779b9u;
  // Pink by the Kellet weighting of white noise (within 0.05 dB of -3 dB/oct).
  double b[7] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  for (float& v : out) {
    state = state * 1664525u + 1013904223u;
    const double white = static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    if (source == Source::kWhiteNoise) {
      v = static_cast<float>(white);
      continue;
    }
    b[0] = 0.99886 * b[0] + white * 0.0555179;
    b[1] = 0.99332 * b[1] + white * 0.0750759;
    b[2] = 0.96900 * b[2] + white * 0.1538520;
    b[3] = 0.86650 * b[3] + white * 0.3104856;
    b[4] = 0.55000 * b[4] + white * 0.5329522;
    b[5] = -0.7616 * b[5] - white * 0.0168980;
    v = static_cast<float>(b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362);
    b[6] = white * 0.115926;
  }
  double energy = 0.0;
  for (const float v : out) energy += static_cast<double>(v) * v;
  const double rms = std::sqrt(energy / samples);
  const double gain = sonare::db_to_linear(static_cast<double>(kNoiseRmsDbfs)) / rms;
  for (float& v : out) v = static_cast<float>(v * gain);
  return out;
}

sonare::Audio render(const Item& item) {
  const int samples = static_cast<int>(to_sample(item.seconds));
  if (item.source != Source::kPiano) {
    return sonare::Audio::from_vector(render_noise(item.source, samples), kSampleRate);
  }
  std::vector<TimedUmp> events;
  events.push_back({0, sonare::midi::make_midi1_program_change(0, 0, kPianoProgram)});
  // A note-off is listed before a note-on at the same sample, so a re-strike
  // releases the sounding voice and strikes again on one sample.
  for (const ScoreNote& n : item.notes) {
    events.push_back(
        {to_sample(n.offset_sec), sonare::midi::make_midi1_note_off(0, 0, n.pitch, 0)});
  }
  for (const ScoreNote& n : item.notes) {
    events.push_back(
        {to_sample(n.onset_sec), sonare::midi::make_midi1_note_on(0, 0, n.pitch, n.velocity)});
  }
  return sonare::Audio::from_vector(render_sequence_mono(events, kSampleRate, samples),
                                    kSampleRate);
}

struct Score {
  int tp = 0;
  int fp = 0;
  int fn = 0;

  double precision() const { return tp + fp > 0 ? static_cast<double>(tp) / (tp + fp) : 0.0; }
  double recall() const { return tp + fn > 0 ? static_cast<double>(tp) / (tp + fn) : 0.0; }
  double f_measure() const {
    const double p = precision();
    const double r = recall();
    return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
  }
};

/// One-to-one matching, greedy by smallest onset difference among the pairs
/// inside both tolerances.
Score score(const std::vector<ScoreNote>& truth, const std::vector<TranscribedNote>& est) {
  struct Pair {
    double dt;
    size_t t;
    size_t e;
  };
  std::vector<Pair> pairs;
  for (size_t t = 0; t < truth.size(); ++t) {
    for (size_t e = 0; e < est.size(); ++e) {
      const double dt =
          std::abs(static_cast<double>(est[e].onset_sample) / kSampleRate - truth[t].onset_sec);
      const double cents =
          std::abs(kCentsPerOctave * std::log2(est[e].median_hz / note_hz(truth[t].pitch)));
      if (dt <= kOnsetToleranceSec && cents <= kPitchToleranceCents) pairs.push_back({dt, t, e});
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const Pair& a, const Pair& b) { return a.dt < b.dt; });
  std::vector<bool> truth_taken(truth.size(), false);
  std::vector<bool> est_taken(est.size(), false);
  Score s;
  for (const Pair& p : pairs) {
    if (truth_taken[p.t] || est_taken[p.e]) continue;
    truth_taken[p.t] = true;
    est_taken[p.e] = true;
    ++s.tp;
  }
  s.fn = static_cast<int>(truth.size()) - s.tp;
  s.fp = static_cast<int>(est.size()) - s.tp;
  return s;
}

struct Run {
  Score score;
  int notes = 0;
  double wall_ms = 0.0;
  double cap_hit_share = 0.0;
  double mean_polyphony = 0.0;
};

Run run(const Item& item, const sonare::Audio& audio, const TranscribeConfig& config) {
  Run r;
  const auto start = std::chrono::steady_clock::now();
  const std::vector<TranscribedNote> notes = transcribe_notes(audio, config);
  const auto stop = std::chrono::steady_clock::now();
  r.wall_ms = std::chrono::duration<double, std::milli>(stop - start).count();
  r.notes = static_cast<int>(notes.size());
  r.score = score(item.notes, notes);

  // The same extraction limits the transcription resolves, read for the
  // per-frame voice count the tracker never sees.
  const TranscribeF0Range range = resolve_transcribe_f0_range(config);
  const TranscribePolyphonyLimits limits = resolve_transcribe_polyphony_limits(config);
  MultiF0ExtractorConfig extraction;
  extraction.estimation.salience.f0_min_hz = range.fmin;
  extraction.estimation.salience.f0_max_hz = range.fmax;
  extraction.estimation.max_polyphony = limits.max_polyphony;
  extraction.estimation.min_frame_peak_ratio = limits.min_frame_peak_ratio;
  extraction.ridges.min_duration_ms = limits.min_note_ms;
  extraction.ridges.min_ridge_peak_ratio = limits.min_ridge_peak_ratio;
  extraction.ridges.reattack_ratio = limits.reattack_ratio;
  const MultiF0Track track = extract_multi_f0(audio, extraction);
  int at_cap = 0;
  double voices = 0.0;
  for (const int p : track.polyphony) {
    if (p >= limits.max_polyphony) ++at_cap;
    voices += p;
  }
  if (!track.polyphony.empty()) {
    const auto frames = static_cast<double>(track.polyphony.size());
    r.cap_hit_share = at_cap / frames;
    r.mean_polyphony = voices / frames;
  }
  return r;
}

void print(const char* prefix, const std::string& head, const Item& item, const Run& r) {
  std::printf(
      "%s {%s\"item\":\"%s\",\"build\":\"%s\",\"tp\":%d,\"fp\":%d,\"fn\":%d,"
      "\"precision\":%.4f,\"recall\":%.4f,\"f_measure\":%.4f,\"cap_hit_share\":%.4f,"
      "\"mean_polyphony\":%.4f,\"notes_per_second\":%.4f,\"wall_ms\":%.1f}\n",
      prefix, head.c_str(), item.name.c_str(), kBuild, r.score.tp, r.score.fp, r.score.fn,
      r.score.precision(), r.score.recall(), r.score.f_measure(), r.cap_hit_share, r.mean_polyphony,
      r.notes / item.seconds, r.wall_ms);
}

/// Read at the scores' tempo, as the C ABI reads the defaults at the transcription tempo.
TranscribeConfig default_config() {
  TranscribeConfig config;
  config.source = TranscribeSource::kPolyphonic;
  config.tempo_bpm = static_cast<float>(kBpm);
  return config;
}

/// The editing chain's limits, spelled out rather than left to its defaults.
TranscribeConfig legacy_config() {
  TranscribeConfig config = default_config();
  config.max_polyphony = 4;
  config.min_note_ms = 140.0f;
  config.reattack_ratio = -1.0f;
  return config;
}

}  // namespace

TEST_CASE("note-level polyphonic transcription on rendered scores", "[.][transcribe_eval]") {
  for (const Item& item : corpus()) {
    const sonare::Audio audio = render(item);
    const Run def = run(item, audio, default_config());
    const Run legacy = run(item, audio, legacy_config());
    print("TRANSCRIBE_EVAL", "\"config\":\"default\",", item, def);
    print("TRANSCRIBE_EVAL", "\"config\":\"legacy\",", item, legacy);

    INFO("item " << item.name);
    if (item.recall_gate) CHECK(def.score.recall() >= legacy.score.recall());
    if (item.no_false_alarm) CHECK(def.score.fp == 0);
    CHECK(def.score.f_measure() >= item.min_f_measure);
  }
}

TEST_CASE("note-level transcription across candidate defaults", "[.][transcribe_eval_sweep]") {
  struct Candidate {
    const char* param;
    float value;
  };
  const std::vector<Candidate> candidates = {
      {"max_polyphony", 6.0f},  {"max_polyphony", 8.0f},  {"max_polyphony", 10.0f},
      {"min_note_ms", 60.0f},   {"min_note_ms", 80.0f},   {"min_note_ms", 100.0f},
      {"reattack_ratio", 1.5f}, {"reattack_ratio", 2.0f}, {"reattack_ratio", 2.5f},
      {"reattack_ratio", 3.0f},
  };
  for (const Item& item : corpus()) {
    const sonare::Audio audio = render(item);
    for (const Candidate& c : candidates) {
      TranscribeConfig config = default_config();
      const std::string param = c.param;
      if (param == "max_polyphony") config.max_polyphony = static_cast<int>(c.value);
      if (param == "min_note_ms") config.min_note_ms = c.value;
      if (param == "reattack_ratio") config.reattack_ratio = c.value;
      char head[96];
      std::snprintf(head, sizeof(head), "\"param\":\"%s\",\"value\":%g,", c.param,
                    static_cast<double>(c.value));
      print("TRANSCRIBE_SWEEP", head, item, run(item, audio, config));
    }
  }
}
