/// @file polyphonic_f0_eval_test.cpp
/// @brief Frame-level multiple-F0 accuracy against material this repo renders.
///
/// The ground truth is the note list rather than an annotation: the chords are
/// played through the GM fallback bank, so what sounded in every frame is known
/// exactly and no corpus has to be obtained or trusted. The figures are the
/// frame-level precision, recall and F-measure at a 50-cent tolerance, plus the
/// polyphony error, which is the mean absolute difference between the number of
/// F0s reported and the number sounding.
///
/// Hidden by default because rendering and analysing six items runs well past
/// the second a default case is allowed. It prints one JSON observation per item
/// for tools/eval/summarize_polyphony.py and asserts a floor on the same run, so
/// the measurement and the gate are the same execution rather than two that can
/// drift apart.
///
/// The floors are the measured figures with margin, not targets. A floor here is
/// a statement that the model has not got worse, and it is the only thing in
/// this file that is a choice rather than an observation.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/audio.h"
#include "editing/polyphony/multi_f0.h"
#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::editing::polyphony::extract_multi_f0;
using sonare::editing::polyphony::F0Ridge;
using sonare::editing::polyphony::MultiF0Track;
using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;

using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

/// The framing defaults are calibrated at this rate; measuring at another one
/// would report the resampling as accuracy.
constexpr int kSampleRate = 44100;
constexpr int kBlock = 256;
constexpr float kToleranceCents = 50.0f;

/// Centre padding puts half a window of silence under the first and last frames,
/// so their content is not the chord and their score is not the model's.
constexpr int kEdgeFrames = 4;

using sonare::test::event;

float note_hz(int midi_note) {
  return kA4Hz * std::exp2((static_cast<float>(midi_note) - kMidiA4) / kSemitonesPerOctave);
}

float cents_between(float a, float b) { return kCentsPerOctave * std::log2(b / a); }

struct Item {
  const char* name;
  uint8_t program;
  std::vector<uint8_t> notes;
  /// White noise at this share of the render's peak, mixed in after rendering.
  float noise = 0.0f;
  /// The measured F-measure with margin. It says the model has not got worse and
  /// makes no claim that the value is good.
  double min_f_measure = 0.0;
};

sonare::Audio render(const Item& item, float seconds) {
  NativeSynthConfig cfg;
  NativeSynth synth(cfg);
  synth.prepare(kSampleRate, kBlock);
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, item.program)));
  for (uint8_t note : item.notes) {
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, 90)));
  }

  const int total = static_cast<int>(seconds * kSampleRate);
  std::vector<float> mono(static_cast<size_t>(total), 0.0f);
  std::vector<float> left(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> right(static_cast<size_t>(kBlock), 0.0f);
  for (int at = 0; at < total; at += kBlock) {
    const int n = std::min(kBlock, total - at);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    float* channels[2] = {left.data(), right.data()};
    synth.process(channels, 2, n);
    for (int i = 0; i < n; ++i) {
      mono[static_cast<size_t>(at + i)] =
          0.5f * (left[static_cast<size_t>(i)] + right[static_cast<size_t>(i)]);
    }
  }
  if (item.noise > 0.0f) {
    float peak = 0.0f;
    for (const float v : mono) peak = std::max(peak, std::abs(v));
    // A fixed sequence rather than a generator, so a figure that moves is the
    // model moving and never the corpus.
    uint32_t state = 0x9e3779b9u;
    for (float& v : mono) {
      state = state * 1664525u + 1013904223u;
      const float uniform = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
      v += item.noise * peak * uniform;
    }
  }
  return sonare::Audio::from_vector(std::move(mono), kSampleRate);
}

/// The F0s standing in one frame, read out of the ridges that span it.
std::vector<float> frame_f0s(const MultiF0Track& track, int frame) {
  std::vector<float> out;
  for (const F0Ridge& ridge : track.ridges) {
    if (ridge.frame_start > frame || frame >= ridge.frame_end()) continue;
    out.push_back(ridge.f0_hz[static_cast<size_t>(frame - ridge.frame_start)]);
  }
  return out;
}

struct Score {
  int tp = 0;
  int fp = 0;
  int fn = 0;
  double polyphony_error = 0.0;
  int frames = 0;

  double precision() const { return tp + fp > 0 ? static_cast<double>(tp) / (tp + fp) : 0.0; }
  double recall() const { return tp + fn > 0 ? static_cast<double>(tp) / (tp + fn) : 0.0; }
  double f_measure() const {
    const double p = precision();
    const double r = recall();
    return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
  }
};

/// Greedy nearest matching within the tolerance, one estimate per true F0. A
/// true F0 with two estimates on it scores one hit and one false alarm, which is
/// what makes a duplicated voice cost something rather than being free.
Score score(const MultiF0Track& track, const std::vector<float>& truth) {
  Score s;
  for (int frame = kEdgeFrames; frame < track.n_frames - kEdgeFrames; ++frame) {
    const std::vector<float> est = frame_f0s(track, frame);
    std::vector<bool> taken(est.size(), false);
    int hits = 0;
    for (const float want : truth) {
      int best = -1;
      float best_cents = kToleranceCents;
      for (size_t i = 0; i < est.size(); ++i) {
        if (taken[i]) continue;
        const float d = std::abs(cents_between(want, est[i]));
        if (d <= best_cents) {
          best_cents = d;
          best = static_cast<int>(i);
        }
      }
      if (best >= 0) {
        taken[static_cast<size_t>(best)] = true;
        ++hits;
      }
    }
    s.tp += hits;
    s.fn += static_cast<int>(truth.size()) - hits;
    s.fp += static_cast<int>(est.size()) - hits;
    s.polyphony_error +=
        std::abs(static_cast<double>(est.size()) - static_cast<double>(truth.size()));
    ++s.frames;
  }
  return s;
}

const std::vector<Item>& corpus() {
  // Sustained mid-register chords score a flat 1.0 whatever the program is, so
  // they are kept as the floor of the corpus rather than as its subject. The
  // items that separate one model from another are the ones under them: a count
  // the cap cannot satisfy, intervals inside the separation rule, a register the
  // framing does not reach, and noise.
  static const std::vector<Item> items = {
      {"strings 2 voices C4", 48, {60, 67}, 0.0f, 0.95},
      {"strings 4 voices C4", 48, {60, 64, 67, 71}, 0.0f, 0.95},
      {"organ 4 voices C4", 16, {60, 64, 67, 71}, 0.0f, 0.95},
      {"piano 4 voices C4", 0, {60, 64, 67, 71}, 0.0f, 0.95},
      // One voice against a cap of four: the only item where a false alarm is
      // free to appear, since everywhere else the cap is what bounds it.
      {"strings 1 voice C4", 48, {60}, 0.0f, 0.95},
      // Five voices against a cap of four, so the model cannot be right and the
      // figure says how it is wrong. It misses about one and a half voices and
      // invents none, which is the failure a hard cap should produce.
      {"strings 5 voices C4", 48, {60, 64, 67, 71, 74}, 0.0f, 0.75},
      // Semitones are inside the 50-cent separation rule at every interval the
      // cent axis can name them apart by. Two of the three are found and the
      // count is exactly right, so the third is not missing -- it is a peak
      // standing somewhere the notes are not.
      {"strings cluster C4", 48, {60, 61, 62}, 0.0f, 0.60},
      {"strings 4 voices C3", 48, {48, 52, 55, 59}, 0.0f, 0.65},
      // The default framing does not reach C2 and scores zero there, so this
      // item documents the register limit rather than gating anything. A figure
      // above zero here is a change worth reading, not a failure.
      {"strings 4 voices C2", 48, {36, 40, 43, 47}, 0.0f, 0.0},
      {"strings 4 voices C4 + noise", 48, {60, 64, 67, 71}, 0.25f, 0.95},
      // Noise *raises* this one over its quiet counterpart, which is a property
      // of the ridge floor rather than of the estimate: see the eval README.
      {"strings 4 voices C3 + noise", 48, {48, 52, 55, 59}, 0.25f, 0.75},
  };
  return items;
}

}  // namespace

TEST_CASE("frame-level multiple-F0 accuracy on rendered chords", "[.][polyphony_eval]") {
  for (const Item& item : corpus()) {
    std::vector<float> truth;
    truth.reserve(item.notes.size());
    for (uint8_t note : item.notes) truth.push_back(note_hz(note));

    const sonare::Audio audio = render(item, 1.5f);
    const MultiF0Track track = extract_multi_f0(audio);
    const Score s = score(track, truth);

    INFO("item " << item.name);
    REQUIRE(s.frames > 0);

    // One line per item, parsed by tools/eval/summarize_polyphony.py.
    std::printf(
        "POLYF0_EVAL {\"item\":\"%s\",\"program\":%u,\"voices\":%zu,\"frames\":%d,"
        "\"tp\":%d,\"fp\":%d,\"fn\":%d,\"precision\":%.4f,\"recall\":%.4f,"
        "\"f_measure\":%.4f,\"polyphony_error\":%.4f}\n",
        item.name, static_cast<unsigned>(item.program), item.notes.size(), s.frames, s.tp, s.fp,
        s.fn, s.precision(), s.recall(), s.f_measure(),
        s.polyphony_error / static_cast<double>(s.frames));

    REQUIRE(s.precision() <= 1.0);
    REQUIRE(s.recall() <= 1.0);
    REQUIRE(s.f_measure() >= item.min_f_measure);
  }
}
