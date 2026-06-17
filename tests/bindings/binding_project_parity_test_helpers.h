/// @file binding_project_parity_test_helpers.h
/// @brief Shared helpers for project binding parity tests.

#pragma once

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "midi/smf.h"
#include "midi/smf2.h"
#include "util/constants.h"

namespace {

// Builds a small but non-trivial project through the C ABI: 48 kHz, an audio
// track + audio clip carrying decoded stereo samples, and a MIDI track + clip
// with a couple of events. Returns the handle (caller destroys) and the ids.
struct BuiltProject {
  SonareProject* project = nullptr;
  uint32_t audio_track = 0;
  uint32_t audio_clip = 0;
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
};

[[maybe_unused]] std::vector<float> make_stereo_sine(int frames) {
  std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    out[static_cast<size_t>(i) * 2] =
        0.25f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 220.0 * t));
    out[static_cast<size_t>(i) * 2 + 1] =
        0.18f * static_cast<float>(std::sin(sonare::constants::kTwoPiD * 330.0 * t));
  }
  return out;
}

[[maybe_unused]] void push_u32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
  v->push_back(static_cast<uint8_t>(x & 0xFFu));
}

[[maybe_unused]] void push_u16(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
  v->push_back(static_cast<uint8_t>(x & 0xFFu));
}

[[maybe_unused]] void push_tag(std::vector<uint8_t>* v, const char* tag) {
  for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>(tag[i]));
}

[[maybe_unused]] std::vector<uint8_t> make_project_sysex_smf() {
  const std::vector<uint8_t> payload = {0x7E, 0x7F, 0x09, 0x01, 0xF7};
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xF0);
  body.push_back(static_cast<uint8_t>(payload.size()));
  body.insert(body.end(), payload.begin(), payload.end());
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(0x3C);
  body.push_back(0x40);
  body.push_back(0x83);
  body.push_back(0x60);
  body.push_back(0x80);
  body.push_back(0x3C);
  body.push_back(0x00);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 0);
  push_u16(&smf, 1);
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(body.size()));
  smf.insert(smf.end(), body.begin(), body.end());
  return smf;
}

[[maybe_unused]] BuiltProject build_project(const std::vector<float>& audio) {
  BuiltProject built;
  REQUIRE(sonare_project_create(&built.project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(built.project, 48000.0) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  REQUIRE(sonare_project_add_track(built.project, &track_desc, &built.audio_track) == SONARE_OK);
  REQUIRE(built.audio_track != 0);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = built.audio_track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 2.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = static_cast<int64_t>(audio.size() / 2);
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  REQUIRE(sonare_project_add_clip(built.project, &clip_desc, &built.audio_clip) == SONARE_OK);
  REQUIRE(built.audio_clip != 0);

  REQUIRE(sonare_project_add_midi_clip(built.project, 0.0, 4.0, &built.midi_track,
                                       &built.midi_clip) == SONARE_OK);
  REQUIRE(built.midi_clip != 0);

  SonareMidiEventPod events[2];
  events[0].ppq = 0.0;
  events[0].data0 = 0x20903C40u;  // UMP word
  events[0].data1 = 0u;
  events[1].ppq = 1.0;
  events[1].data0 = 0x20803C00u;
  events[1].data1 = 0u;
  REQUIRE(sonare_project_set_midi_events(built.project, built.midi_clip, events, 2) == SONARE_OK);

  return built;
}

[[maybe_unused]] std::string serialize(const SonareProject* project) {
  char* json = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_serialize(project, &json, &len) == SONARE_OK);
  REQUIRE(json != nullptr);
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

}  // namespace

namespace {
// Host-side callback-instrument state: counts events and emits DC while a note
// sounds, so the bounced audio reflects whether the instrument was driven.
struct CallbackInstrumentState {
  int prepared = 0;
  int note_on = 0;
  int note_off = 0;
};

[[maybe_unused]] void cb_prepare(void* user, double, int) {
  static_cast<CallbackInstrumentState*>(user)->prepared += 1;
}
[[maybe_unused]] void cb_on_event(void* user, uint32_t /*destination_id*/, const uint32_t* words,
                                  int word_count, int64_t /*render_frame*/) {
  if (word_count < 1) return;
  auto* state = static_cast<CallbackInstrumentState*>(user);
  const uint8_t status = static_cast<uint8_t>((words[0] >> 16) & 0xF0u);
  if (status == 0x90u) {
    state->note_on += 1;
  } else if (status == 0x80u) {
    state->note_off += 1;
  }
}
[[maybe_unused]] void cb_render(void* user, float* const* channels, int num_channels,
                                int num_frames) {
  const auto* state = static_cast<const CallbackInstrumentState*>(user);
  const float value = state->note_on > state->note_off ? 0.5f : 0.0f;
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_frames; ++i) channels[ch][i] += value;
  }
}

// Host-side latency-bearing instrument: reports `latency` samples and emits a
// single unit impulse `latency` samples after the note-on's render frame, so the
// bounced output reveals whether plugin-delay compensation realigned it.
struct LatencyCallbackState {
  int latency = 0;
  int64_t frame = 0;           // advanced once per render() call
  int64_t impulse_frame = -1;  // note-on render frame + latency
};
[[maybe_unused]] void lcb_on_event(void* user, uint32_t /*destination_id*/, const uint32_t* words,
                                   int word_count, int64_t render_frame) {
  if (word_count < 1) return;
  auto* state = static_cast<LatencyCallbackState*>(user);
  if (((words[0] >> 16) & 0xF0u) == 0x90u) {
    state->impulse_frame = render_frame + state->latency;
  }
}
[[maybe_unused]] void lcb_render(void* user, float* const* channels, int num_channels,
                                 int num_frames) {
  auto* state = static_cast<LatencyCallbackState*>(user);
  for (int i = 0; i < num_frames; ++i) {
    if (state->frame + i == state->impulse_frame) {
      for (int ch = 0; ch < num_channels; ++ch) channels[ch][i] += 1.0f;
    }
  }
  state->frame += num_frames;
}
}  // namespace
