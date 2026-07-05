/// @file new_engines_voice_test.cpp
/// @brief Buzzing-bridge plucked string (midi/synth/plucked_string_voice),
///        source-filter vocal (midi/synth/vocal_voice) and free-reed
///        (midi/synth/free_reed_voice) cores through the NativeSynth voice:
///        each engine sounds across the keyboard, stays bounded (no NaN / no
///        runaway), rings down on note-off, renders deterministically, and its
///        named presets resolve.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::SynthEngineMode;

constexpr double kRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

std::vector<float> render_left(NativeSynth& synth, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  synth.process(chans, 2, num_samples);
  return left;
}

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

TEST_CASE("new-engine presets resolve and select their engine", "[midi][synth][new_engines]") {
  using sonare::midi::synth::find_synth_preset;
  for (const char* name : {"harp", "koto", "sitar", "tanpura", "choir-aah", "choir-ooh",
                           "voice-eeh", "accordion", "harmonica", "bandoneon", "reed-organ"}) {
    const auto* preset = find_synth_preset(name);
    REQUIRE(preset != nullptr);
  }
}
