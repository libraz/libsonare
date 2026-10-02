/// @file new_engines_voice_test.cpp
/// @brief Buzzing-bridge plucked string (midi/synth/plucked_string_voice),
///        source-filter vocal (midi/synth/vocal_voice) and free-reed
///        (midi/synth/free_reed_voice) cores through the NativeSynth voice:
///        each engine sounds across the keyboard, stays bounded (no NaN / no
///        runaway), rings down on note-off, renders deterministically, and its
///        named presets resolve.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::PluckedStringPatchParams;
using sonare::midi::synth::PluckedStringVoiceCore;
using sonare::midi::synth::SynthEngineMode;

constexpr double kRate = 48000.0;

using sonare::test::event;
using sonare::test::render_left;

std::vector<float> render_patch(const NativeSynthPatch& patch, uint8_t note, uint8_t velocity,
                                int num_samples, int note_off_at = -1) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  if (note_off_at < 0) return render_left(synth, num_samples);
  std::vector<float> head = render_left(synth, note_off_at);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, note, 0)));
  std::vector<float> tail = render_left(synth, num_samples - note_off_at);
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

bool all_finite(const std::vector<float>& x) {
  for (float v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

float peak(const std::vector<float>& x) {
  float p = 0.0f;
  for (float v : x) p = std::max(p, std::fabs(v));
  return p;
}

float rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return static_cast<float>(std::sqrt(s / std::max<size_t>(1, x.size())));
}

NativeSynthPatch engine_patch(SynthEngineMode mode) {
  NativeSynthPatch patch{};
  patch.mode = mode;
  patch.amp_env.attack_ms = 5.0f;
  patch.amp_env.sustain = 1.0f;
  patch.amp_env.release_ms = 120.0f;
  patch.cutoff_hz = 20000.0f;
  patch.gain = 0.8f;
  return patch;
}

double midi_note_hz(uint8_t note) {
  return 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0);
}

std::vector<float> render_plucked_core(const PluckedStringPatchParams& params, double sample_rate,
                                       uint8_t note, size_t frames, size_t release_at = 0) {
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::plucked_string_slab_capacity(sample_rate)));
  PluckedStringVoiceCore core;
  core.attach(slab.data(), static_cast<int>(slab.size()));
  core.start(params, sample_rate, note, sonare::midi::Velocity16::from7(110), 0x51A7u);

  std::vector<float> output(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    if (release_at != 0 && i == release_at) core.release();
    output[i] = core.render(1.0f);
  }
  return output;
}

double fundamental_projection(const std::vector<float>& samples, size_t begin, size_t length,
                              double sample_rate, double frequency) {
  if (length == 0 || begin >= samples.size() || length > samples.size() - begin) return 0.0;
  const double omega = sonare::constants::kTwoPiD * frequency / sample_rate;
  double real = 0.0;
  double imag = 0.0;
  for (size_t i = 0; i < length; ++i) {
    const double phase = omega * static_cast<double>(begin + i);
    const double sample = static_cast<double>(samples[begin + i]);
    real += sample * std::cos(phase);
    imag -= sample * std::sin(phase);
  }
  return 2.0 * std::sqrt(real * real + imag * imag) / static_cast<double>(length);
}

double projected_t60(const std::vector<float>& samples, double sample_rate, uint8_t note,
                     size_t first_begin, size_t separation) {
  const double frequency = midi_note_hz(note);
  const size_t period =
      std::max<size_t>(1, static_cast<size_t>(std::llround(sample_rate / frequency)));
  const size_t window = 8 * period;
  const double first = fundamental_projection(samples, first_begin, window, sample_rate, frequency);
  const double second =
      fundamental_projection(samples, first_begin + separation, window, sample_rate, frequency);
  if (!(first > 0.0) || !(second > 0.0) || !(separation > 0)) return 0.0;
  const double seconds = static_cast<double>(separation) / sample_rate;
  const double db_per_second = 20.0 * std::log10(first / second) / seconds;
  return db_per_second > 0.0 ? 60.0 / db_per_second : 0.0;
}

}  // namespace

TEST_CASE("new engines sound and stay bounded across the keyboard", "[midi][synth][new_engines]") {
  const SynthEngineMode modes[] = {SynthEngineMode::kPluckedString, SynthEngineMode::kVocal,
                                   SynthEngineMode::kFreeReed};
  for (SynthEngineMode mode : modes) {
    for (uint8_t note : {36, 48, 60, 72, 84}) {
      const std::vector<float> out = render_patch(engine_patch(mode), note, 100, 12000);
      REQUIRE(all_finite(out));
      // Bounded: the wrapped voice output never runs away.
      REQUIRE(peak(out) < 4.0f);
      // Audible: the engine actually produces a tone.
      REQUIRE(rms(out) > 1.0e-4f);
    }
  }
}

TEST_CASE("new engines ring down after note-off", "[midi][synth][new_engines]") {
  const SynthEngineMode modes[] = {SynthEngineMode::kPluckedString, SynthEngineMode::kVocal,
                                   SynthEngineMode::kFreeReed};
  for (SynthEngineMode mode : modes) {
    const std::vector<float> out = render_patch(engine_patch(mode), 60, 100, 40000, 8000);
    REQUIRE(all_finite(out));
    const std::vector<float> tail(out.end() - 4000, out.end());
    const std::vector<float> sustain(out.begin() + 4000, out.begin() + 8000);
    // The far tail is quieter than the sustained note (the voice released).
    REQUIRE(rms(tail) < rms(sustain) + 1.0e-6f);
    REQUIRE(peak(tail) < 4.0f);
  }
}

TEST_CASE("new engines render deterministically", "[midi][synth][new_engines]") {
  const SynthEngineMode modes[] = {SynthEngineMode::kPluckedString, SynthEngineMode::kVocal,
                                   SynthEngineMode::kFreeReed};
  for (SynthEngineMode mode : modes) {
    const std::vector<float> a = render_patch(engine_patch(mode), 57, 96, 8000);
    const std::vector<float> b = render_patch(engine_patch(mode), 57, 96, 8000);
    REQUIRE(a == b);
  }
}

TEST_CASE("plucked-string fundamental decay follows t60 across brightness, notes, and rates",
          "[midi][synth][new_engines][plucked]") {
  // The projection ignores the upper partials, so this witnesses the string
  // loop's fundamental rather than the excitation envelope or its timbre.
  constexpr float kTargetT60 = 0.4f;
  constexpr size_t kFirstBeginAt48k = 2880;  // 60 ms, safely past the burst.
  constexpr size_t kSeparationAt48k = 7680;  // 160 ms between projections.

  for (const double sample_rate : {44100.0, 96000.0}) {
    for (const uint8_t note : {static_cast<uint8_t>(48), static_cast<uint8_t>(84)}) {
      for (const float brightness : {0.0f, 1.0f}) {
        PluckedStringPatchParams params;
        params.brightness = brightness;
        params.decay_s = kTargetT60;
        params.decay_stretch = 0.0f;
        params.buzz = 0.0f;

        const size_t first_begin = static_cast<size_t>(
            std::llround(static_cast<double>(kFirstBeginAt48k) * sample_rate / kRate));
        const size_t separation = static_cast<size_t>(
            std::llround(static_cast<double>(kSeparationAt48k) * sample_rate / kRate));
        const size_t period = std::max<size_t>(
            1, static_cast<size_t>(std::llround(sample_rate / midi_note_hz(note))));
        const size_t frames = first_begin + separation + 8 * period + 1;
        const std::vector<float> output = render_plucked_core(params, sample_rate, note, frames);
        const double measured = projected_t60(output, sample_rate, note, first_begin, separation);

        INFO("sample rate " << sample_rate << " note " << static_cast<int>(note) << " brightness "
                            << brightness << " measured t60 " << measured);
        // A brightness change is allowed to change the upper partials, but it
        // must not make the fundamental lose its requested decay time.
        REQUIRE(measured == Catch::Approx(static_cast<double>(kTargetT60)).margin(0.05));
      }
    }
  }
}

TEST_CASE("plucked-string release decay follows release_damp_s through its loss filter",
          "[midi][synth][new_engines][plucked]") {
  constexpr float kNaturalT60 = 0.4f;
  constexpr float kReleaseT60 = 0.12f;
  constexpr size_t kReleaseAt48k = 3840;           // 80 ms.
  constexpr size_t kFirstAfterReleaseAt48k = 960;  // 20 ms after note-off.
  constexpr size_t kSeparationAt48k = 1920;        // 40 ms between projections.

  // The high note and dark pole are the case where an uncompensated
  // release gain loses the fundamental fastest. Check both rate mappings.
  for (const double sample_rate : {44100.0, 96000.0}) {
    constexpr uint8_t kNote = 84;
    PluckedStringPatchParams params;
    params.brightness = 0.0f;
    params.decay_s = kNaturalT60;
    params.decay_stretch = 0.0f;
    params.release_damp_s = kReleaseT60;
    params.buzz = 0.0f;

    const size_t release_at =
        static_cast<size_t>(std::llround(static_cast<double>(kReleaseAt48k) * sample_rate / kRate));
    const size_t first_begin =
        release_at + static_cast<size_t>(std::llround(static_cast<double>(kFirstAfterReleaseAt48k) *
                                                      sample_rate / kRate));
    const size_t separation = static_cast<size_t>(
        std::llround(static_cast<double>(kSeparationAt48k) * sample_rate / kRate));
    const size_t period =
        std::max<size_t>(1, static_cast<size_t>(std::llround(sample_rate / midi_note_hz(kNote))));
    const size_t frames = first_begin + separation + 8 * period + 1;
    const std::vector<float> output =
        render_plucked_core(params, sample_rate, kNote, frames, release_at);
    const double measured = projected_t60(output, sample_rate, kNote, first_begin, separation);

    INFO("sample rate " << sample_rate << " measured release t60 " << measured);
    REQUIRE(measured == Catch::Approx(static_cast<double>(kReleaseT60)).margin(0.02));
  }
}

TEST_CASE("plucked-string default A4 decay keeps its four-second target",
          "[midi][synth][new_engines][plucked]") {
  // This is the shipped patch default at its voiced rate.
  const PluckedStringPatchParams params;
  constexpr double kSampleRate = 48000.0;
  constexpr uint8_t kNote = 69;
  constexpr size_t kFirstBegin = 9600;   // 200 ms, past the excitation burst.
  constexpr size_t kSeparation = 19200;  // 400 ms between projections.
  const size_t period =
      std::max<size_t>(1, static_cast<size_t>(std::llround(kSampleRate / midi_note_hz(kNote))));
  const size_t frames = kFirstBegin + kSeparation + 8 * period + 1;
  const std::vector<float> output = render_plucked_core(params, kSampleRate, kNote, frames);
  const double measured = projected_t60(output, kSampleRate, kNote, kFirstBegin, kSeparation);

  INFO("default A4 measured t60 " << measured);
  REQUIRE(measured == Catch::Approx(4.0).margin(0.25));
}

TEST_CASE("new-engine presets resolve and select their engine", "[midi][synth][new_engines]") {
  using sonare::midi::synth::find_synth_preset;
  for (const char* name :
       {"harp", "harp-plucked", "koto", "sitar", "tanpura", "choir-aah", "choir-ooh", "voice-eeh",
        "accordion", "harmonica", "bandoneon", "reed-organ"}) {
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
  }
}

TEST_CASE("named synth presets are unique and preserve both harp voicings", "[midi][synth]") {
  using sonare::midi::synth::find_synth_preset;
  using sonare::midi::synth::synth_preset_at;
  using sonare::midi::synth::synth_preset_count;

  std::set<std::string> names;
  for (size_t index = 0; index < synth_preset_count(); ++index) {
    const auto* preset = synth_preset_at(index);
    REQUIRE(preset != nullptr);
    REQUIRE(names.insert(preset->name).second);
    REQUIRE(find_synth_preset(preset->name) == preset);
  }

  const auto* gm_harp = find_synth_preset("harp");
  const auto* plucked_harp = find_synth_preset("harp-plucked");
  REQUIRE(gm_harp != nullptr);
  REQUIRE(plucked_harp != nullptr);
  REQUIRE(gm_harp->config.patch.mode != plucked_harp->config.patch.mode);
}

TEST_CASE("plucked-string kill silences subsequent rendering and permits restart",
          "[midi][synth][new_engines][plucked]") {
  PluckedStringPatchParams params;
  const double sample_rate = 48000.0;
  const int capacity = sonare::midi::synth::plucked_string_buffer_capacity(sample_rate);
  std::vector<float> slab(
      static_cast<size_t>(sonare::midi::synth::plucked_string_slab_capacity(sample_rate)));
  PluckedStringVoiceCore core;
  core.attach(slab.data(), capacity);
  core.start(params, sample_rate, 60, sonare::midi::Velocity16::from7(110), 0x4b494c4cULL);

  float sounding = 0.0f;
  for (int i = 0; i < 12000; ++i) sounding = std::max(sounding, std::fabs(core.render(1.0f)));
  REQUIRE(sounding > 0.01f);

  core.kill();
  bool silent = true;
  for (int i = 0; i < 4 * capacity; ++i) silent = (core.render(1.0f) == 0.0f) && silent;
  REQUIRE(silent);

  core.start(params, sample_rate, 60, sonare::midi::Velocity16::from7(110), 0x4b494c4dULL);
  std::vector<float> restarted_samples(12000, 0.0f);
  float restarted = 0.0f;
  for (float& sample : restarted_samples) {
    sample = core.render(1.0f);
    restarted = std::max(restarted, std::fabs(sample));
  }
  REQUIRE(restarted > 0.01f);

  std::vector<float> fresh_slab(
      static_cast<size_t>(sonare::midi::synth::plucked_string_slab_capacity(sample_rate)));
  PluckedStringVoiceCore fresh;
  fresh.attach(fresh_slab.data(), capacity);
  fresh.start(params, sample_rate, 60, sonare::midi::Velocity16::from7(110), 0x4b494c4dULL);
  std::vector<float> fresh_samples(12000, 0.0f);
  for (float& sample : fresh_samples) sample = fresh.render(1.0f);
  const bool deterministic = restarted_samples == fresh_samples;
  REQUIRE(deterministic);
}
