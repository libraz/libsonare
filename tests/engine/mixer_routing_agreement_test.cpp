/// @file mixer_routing_agreement_test.cpp
/// @brief Cross-implementation agreement for bus routing, bus sends, bus/master
///        sidechain keys, surround buses and MIDI clip gain/fades: the live
///        engine (block processing), the engine's offline render, and project
///        bounce (the standalone mixer graph) must produce the same audio.
///
/// Every case wires two buses A and B, one MIDI instrument track (also the
/// track-sourced sidechain key) feeding A and one audio track feeding B. An
/// engine bus maps onto the scene as the bus itself plus a return strip after
/// it carrying the bus's gain_db as its fader and the bus's sends; a return
/// strip that would be at its defaults is left out and the bus connects to its
/// destination directly.
///
/// The cases cover every pair of values across: routing {parallel, A_out_B, A_send_pre_B,
/// A_send_post_B} x sidechain {none, B_from_track, B_from_A, master_from_A, master_from_track} x
/// layout {stereo, bus51_to_stereo_master, bus51_to_51_master} x latency {none, A_insert_latency} x
/// midiClip {unity, gain_fades} x sourceFader {0dB, m12dB}, constrained by IF sidechain = none THEN
/// sourceFader = 0dB.

#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project_edit.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_ARRANGEMENT)

#include "c_api/sonare_c_internal.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr int64_t kTotalFrames = kBlockSize * 80;
// Discarded before comparing, so the synth attack and the smoothers have settled.
constexpr int64_t kSettleFrames = kBlockSize * 20;
// max|a-b| <= kMetricScale * max(1, max|a|), the bound bus_strip_agreement uses.
constexpr double kMetricScale = 1.0e-6;
// Reference RMS floor: two silent renders agreeing proves nothing.
constexpr double kMinRmsDb = -40.0;
// A sidechain key must move the keyed render by at least this much.
constexpr double kMinKeyEffectDb = 1.0;

constexpr uint32_t kMidiTrack = 10;
constexpr uint32_t kAudioTrack = 20;
constexpr uint32_t kBusA = 1;
constexpr uint32_t kBusB = 2;
constexpr uint32_t kDestination = 77;

// 120 BPM at 48 kHz: one quarter note is 24000 samples, so every PPQ below lands
// on a whole sample.
constexpr double kSamplesPerPpq = 24000.0;
constexpr double kMidiClipStartPpq = 0.0625;
constexpr double kMidiClipLengthPpq = 0.3125;
constexpr double kMidiFadePpq = 0.125;
constexpr float kMidiClipGain = 0.7f;
constexpr double kAudioClipLengthPpq = 0.5;
constexpr float kSendDb = -6.0f;
constexpr float kSourceFaderDb = -12.0f;

enum class Routing { kParallel, kAOutB, kASendPreB, kASendPostB };
enum class Sidechain { kNone, kBFromTrack, kBFromA, kMasterFromA, kMasterFromTrack };
enum class Layout { kStereo, kBus51ToStereoMaster, kBus51To51Master };

struct Row {
  int number;
  Routing routing;
  Sidechain sidechain;
  Layout layout;
  bool a_latency;
  bool clip_gain_fades;
  bool source_fader_m12;
};

// clang-format off
const Row kRows[] = {
    { 1, Routing::kAOutB,      Sidechain::kMasterFromA,     Layout::kBus51ToStereoMaster, true,  true,  false},
    { 2, Routing::kASendPostB, Sidechain::kBFromTrack,      Layout::kBus51To51Master,     false, true,  true },
    { 3, Routing::kParallel,   Sidechain::kNone,            Layout::kBus51To51Master,     true,  false, false},
    { 4, Routing::kParallel,   Sidechain::kMasterFromA,     Layout::kStereo,              false, false, true },
    { 5, Routing::kASendPreB,  Sidechain::kMasterFromTrack, Layout::kBus51ToStereoMaster, false, false, false},
    { 6, Routing::kASendPreB,  Sidechain::kNone,            Layout::kStereo,              false, true,  false},
    { 7, Routing::kASendPreB,  Sidechain::kBFromA,          Layout::kBus51To51Master,     true,  true,  true },
    { 8, Routing::kASendPostB, Sidechain::kBFromA,          Layout::kStereo,              true,  false, false},
    { 9, Routing::kAOutB,      Sidechain::kMasterFromTrack, Layout::kBus51ToStereoMaster, true,  true,  true },
    {10, Routing::kAOutB,      Sidechain::kBFromTrack,      Layout::kStereo,              false, false, false},
    {11, Routing::kAOutB,      Sidechain::kBFromA,          Layout::kBus51To51Master,     false, false, false},
    {12, Routing::kASendPostB, Sidechain::kMasterFromA,     Layout::kBus51To51Master,     false, true,  true },
    {13, Routing::kASendPreB,  Sidechain::kBFromTrack,      Layout::kBus51ToStereoMaster, true,  false, false},
    {14, Routing::kASendPostB, Sidechain::kMasterFromTrack, Layout::kBus51To51Master,     true,  false, true },
    {15, Routing::kParallel,   Sidechain::kBFromA,          Layout::kBus51ToStereoMaster, true,  true,  false},
    {16, Routing::kASendPostB, Sidechain::kMasterFromTrack, Layout::kStereo,              false, false, true },
    {17, Routing::kParallel,   Sidechain::kBFromTrack,      Layout::kStereo,              false, false, false},
    {18, Routing::kParallel,   Sidechain::kMasterFromTrack, Layout::kBus51ToStereoMaster, false, false, false},
    {19, Routing::kASendPostB, Sidechain::kNone,            Layout::kStereo,              true,  true,  false},
    {20, Routing::kASendPostB, Sidechain::kMasterFromA,     Layout::kBus51ToStereoMaster, true,  true,  false},
    {21, Routing::kASendPreB,  Sidechain::kMasterFromA,     Layout::kStereo,              true,  true,  false},
    {22, Routing::kASendPostB, Sidechain::kNone,            Layout::kBus51ToStereoMaster, false, false, false},
    {23, Routing::kAOutB,      Sidechain::kNone,            Layout::kBus51To51Master,     true,  true,  false},
};
// clang-format on

bool keys_from_track(const Row& row) {
  return row.sidechain == Sidechain::kBFromTrack || row.sidechain == Sidechain::kMasterFromTrack;
}
bool keys_from_a(const Row& row) {
  return row.sidechain == Sidechain::kBFromA || row.sidechain == Sidechain::kMasterFromA;
}
bool keys_b(const Row& row) {
  return row.sidechain == Sidechain::kBFromTrack || row.sidechain == Sidechain::kBFromA;
}
bool keys_master(const Row& row) {
  return row.sidechain == Sidechain::kMasterFromTrack || row.sidechain == Sidechain::kMasterFromA;
}
bool has_sends(const Row& row) {
  return row.routing == Routing::kASendPreB || row.routing == Routing::kASendPostB;
}
bool a_is_surround(const Row& row) { return row.layout != Layout::kStereo; }
int output_channels(const Row& row) { return row.layout == Layout::kBus51To51Master ? 6 : 2; }
// A bus-sourced key's engine-only fader is bus A's gain_db (the return strip fader in the scene).
float a_gain_db(const Row& row) {
  return row.source_fader_m12 && keys_from_a(row) ? kSourceFaderDb : 0.0f;
}
// A track-sourced key's engine-only fader is the lane fader, which the scene does not have.
bool compares_bounce(const Row& row) { return !(row.source_fader_m12 && keys_from_track(row)); }

std::string describe(const Row& row) {
  static const char* const kRouting[] = {"parallel", "A_out_B", "A_send_pre_B", "A_send_post_B"};
  static const char* const kSidechain[] = {"none", "B_from_track", "B_from_A", "master_from_A",
                                           "master_from_track"};
  static const char* const kLayout[] = {"stereo", "bus51_to_stereo_master", "bus51_to_51_master"};
  return std::string(kRouting[static_cast<int>(row.routing)]) + "/" +
         kSidechain[static_cast<int>(row.sidechain)] + "/" + kLayout[static_cast<int>(row.layout)] +
         "/" + (row.a_latency ? "A_insert_latency" : "no_latency") + "/" +
         (row.clip_gain_fades ? "gain_fades" : "unity") + "/" +
         (row.source_fader_m12 ? "m12dB" : "0dB");
}

// ---------------------------------------------------------------------------
// Shared JSON fragments: the same strings configure the engine strips and the
// project scene.
// ---------------------------------------------------------------------------

constexpr const char* kSurroundPan =
    R"("surroundPan":{"azimuth":60.0,"divergence":0.25,"lfe":0.3})";
// 1 ms of lookahead; the threshold sits far above the signal, so the limiter is
// a pure delay.
constexpr const char* kLatentInsert =
    R"({"slot":"pre","processor":"dynamics.limiter","params":"{\"thresholdDb\":24,\"releaseMs\":50,\"lookaheadMs\":1}"})";
std::string ducker_insert(const std::string& key) {
  std::string insert =
      R"({"slot":"pre","processor":"dynamics.duckingProcessor","params":"{\"thresholdDb\":-30,\"ratio\":8,\"attackMs\":1,\"releaseMs\":20,\"rangeDb\":18}")";
  if (!key.empty()) insert += R"(,"sidechainKey":")" + key + "\"";
  return insert + "}";
}

std::string midi_strip_body() { return std::string(R"("id":"tmidi",)") + kSurroundPan; }

std::string a_inserts(const Row& row) {
  return row.a_latency ? std::string(R"("inserts":[)") + kLatentInsert + "]" : std::string();
}

std::string layout_field(bool surround) {
  return surround ? std::string(R"("layout":"5.1")") : std::string(R"("layout":"stereo")");
}

std::string wrap_strip_scene(const std::string& body) {
  return std::string(R"({"version":1,"strips":[{)") + body + R"(}],"buses":[],"connections":[]})";
}

std::string wrap_bus_scene(const std::string& body) {
  return std::string(R"({"version":1,"strips":[],"buses":[{)") + body + R"(}],"connections":[]})";
}

// ---------------------------------------------------------------------------
// Project scene
// ---------------------------------------------------------------------------

bool needs_return_strip(const Row& row) { return has_sends(row) || a_gain_db(row) != 0.0f; }

std::string project_scene_json(const Row& row) {
  const std::string key_source = keys_from_track(row) ? "tmidi" : keys_from_a(row) ? "A" : "";
  std::string strips = "{" + midi_strip_body() + R"(},{"id":"taudio"})";
  if (needs_return_strip(row)) {
    strips += R"(,{"id":"retA","faderDb":)" + std::to_string(a_gain_db(row));
    if (has_sends(row)) {
      strips += R"(,"sends":[{"id":"toB","destinationBusId":"B","sendDb":)" +
                std::to_string(kSendDb) + R"(,"timing":")" +
                (row.routing == Routing::kASendPreB ? "pre" : "post") + "\"}]";
    }
    strips += "}";
  }

  std::string bus_a = R"({"id":"A",)" + layout_field(a_is_surround(row));
  if (row.a_latency) bus_a += "," + a_inserts(row);
  bus_a += "}";
  std::string bus_b = R"({"id":"B","layout":"stereo")";
  if (keys_b(row)) bus_b += R"(,"inserts":[)" + ducker_insert(key_source) + "]";
  bus_b += "}";
  std::string master =
      R"({"id":"master","role":"master",)" + layout_field(row.layout == Layout::kBus51To51Master);
  if (keys_master(row)) master += R"(,"inserts":[)" + ducker_insert(key_source) + "]";
  master += "}";

  const std::string a_destination = row.routing == Routing::kAOutB ? "B" : "master";
  std::string connections =
      R"({"source":"tmidi","destination":"A"},{"source":"taudio","destination":"B"},)";
  if (needs_return_strip(row)) {
    connections += R"({"source":"A","destination":"retA"},{"source":"retA","destination":")" +
                   a_destination + "\"},";
  } else {
    connections += R"({"source":"A","destination":")" + a_destination + "\"},";
  }
  connections += R"({"source":"B","destination":"master"})";

  return R"({"version":1,"strips":[)" + strips + R"(],"buses":[)" + bus_a + "," + bus_b + "," +
         master + R"(],"connections":[)" + connections + "]}";
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

int64_t ppq_samples(double ppq) { return static_cast<int64_t>(std::llround(ppq * kSamplesPerPpq)); }

// A 220 Hz tone on L and a 330 Hz tone on R.
const std::vector<float>& audio_plane(int channel) {
  static const std::vector<std::vector<float>> planes = [] {
    const size_t n = static_cast<size_t>(ppq_samples(kAudioClipLengthPpq));
    std::vector<std::vector<float>> p(2, std::vector<float>(n));
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      p[0][i] = static_cast<float>(0.3 * std::sin(kTwoPiD * 220.0 * t));
      p[1][i] = static_cast<float>(0.3 * std::sin(kTwoPiD * 330.0 * t));
    }
    return p;
  }();
  return planes[static_cast<size_t>(channel)];
}

SonareEngineBuiltinSynthConfig engine_synth() {
  SonareEngineBuiltinSynthConfig synth{};
  synth.waveform = 0;
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

SonareBuiltinSynthConfig project_synth() {
  SonareBuiltinSynthConfig synth{};
  synth.waveform = 0;
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

struct EngineOptions {
  bool bind_key = true;
};

SonareRealtimeEngine* make_engine(const Row& row, const EngineOptions& options = {}) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlockSize, 64, 16) == SONARE_OK);

  const SonareEngineTrackSend sends[] = {{kBusB, kSendDb, 1,
                                          row.routing == Routing::kASendPreB
                                              ? SONARE_SEND_TIMING_PRE_FADER
                                              : SONARE_SEND_TIMING_POST_FADER}};
  const uint8_t a_layout = static_cast<uint8_t>(a_is_surround(row) ? SONARE_CHANNEL_LAYOUT_5_1
                                                                   : SONARE_CHANNEL_LAYOUT_STEREO);
  const SonareEngineBus buses[] = {
      {kBusA, a_gain_db(row), a_layout, row.routing == Routing::kAOutB ? kBusB : 0u,
       has_sends(row) ? sends : nullptr, has_sends(row) ? 1u : 0u},
      {kBusB, 0.0f, static_cast<uint8_t>(SONARE_CHANNEL_LAYOUT_STEREO), 0, nullptr, 0}};
  REQUIRE(sonare_engine_set_track_buses(engine, buses, 2) == SONARE_OK);
  const SonareEngineTrackLane lanes[] = {
      {kMidiTrack, nullptr, 0, kBusA, static_cast<uint8_t>(SONARE_CHANNEL_LAYOUT_STEREO)},
      {kAudioTrack, nullptr, 0, kBusB, static_cast<uint8_t>(SONARE_CHANNEL_LAYOUT_STEREO)}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lanes, 2) == SONARE_OK);
  REQUIRE(sonare_engine_set_track_strip_json(
              engine, kMidiTrack, wrap_strip_scene(midi_strip_body()).c_str()) == SONARE_OK);

  if (row.a_latency) {
    const std::string json =
        wrap_bus_scene(R"("id":"A",)" + layout_field(a_is_surround(row)) + "," + a_inserts(row));
    REQUIRE(sonare_engine_set_bus_strip_json(engine, kBusA, json.c_str()) == SONARE_OK);
  }
  if (keys_b(row)) {
    const std::string json =
        wrap_bus_scene(R"("id":"B","inserts":[)" + ducker_insert(std::string()) + "]");
    REQUIRE(sonare_engine_set_bus_strip_json(engine, kBusB, json.c_str()) == SONARE_OK);
  }
  if (keys_master(row)) {
    const std::string json =
        wrap_strip_scene(R"("id":"master","inserts":[)" + ducker_insert(std::string()) + "]");
    REQUIRE(sonare_engine_set_master_strip_json(engine, json.c_str()) == SONARE_OK);
  }
  if (options.bind_key && row.sidechain != Sidechain::kNone) {
    const int kind =
        keys_from_track(row) ? SONARE_SIDECHAIN_SOURCE_TRACK : SONARE_SIDECHAIN_SOURCE_BUS;
    const uint32_t source = keys_from_track(row) ? kMidiTrack : kBusA;
    if (keys_b(row)) {
      REQUIRE(sonare_engine_set_bus_sidechain(engine, kBusB, 0, kind, source) == SONARE_OK);
    } else {
      REQUIRE(sonare_engine_set_master_sidechain(engine, 0, kind, source) == SONARE_OK);
    }
  }
  if (row.source_fader_m12 && keys_from_track(row)) {
    uint32_t lane_fader = 0;
    REQUIRE(sonare_engine_resolve_track_lane_automation_id(engine, kMidiTrack, "faderDb",
                                                           &lane_fader) == SONARE_OK);
    REQUIRE(sonare_engine_set_parameter(engine, lane_fader, kSourceFaderDb, 0) == SONARE_OK);
    REQUIRE(sonare_engine_flush_control_commands(engine) == SONARE_OK);
  }

  SonareEngineBuiltinSynthConfig synth = engine_synth();
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);
  const int64_t clip_start = ppq_samples(kMidiClipStartPpq);
  const SonareEngineMidiEvent events[] = {
      {clip_start, 0x20903C7Fu, 0u, 0u, 0u, 1u, 0u, 0u, 0u},  // note-on 60, vel 127
  };
  SonareEngineMidiClipSchedule midi_clip{};
  midi_clip.id = 1;
  midi_clip.track_id = kMidiTrack;
  midi_clip.start_sample = clip_start;
  midi_clip.start_ppq = kMidiClipStartPpq;
  midi_clip.length_samples = ppq_samples(kMidiClipLengthPpq);
  midi_clip.destination_id = kDestination;
  midi_clip.events = events;
  midi_clip.event_count = std::size(events);
  midi_clip.gain = row.clip_gain_fades ? kMidiClipGain : 1.0f;
  midi_clip.fade_in_samples = row.clip_gain_fades ? ppq_samples(kMidiFadePpq) : 0;
  midi_clip.fade_out_samples = row.clip_gain_fades ? ppq_samples(kMidiFadePpq) : 0;
  REQUIRE(sonare_engine_set_midi_clips(engine, &midi_clip, 1) == SONARE_OK);

  static const float* audio_channels[] = {audio_plane(0).data(), audio_plane(1).data()};
  SonareEngineClip audio_clip{};
  audio_clip.id = 2;
  audio_clip.track_id = kAudioTrack;
  audio_clip.channels = audio_channels;
  audio_clip.num_channels = 2;
  audio_clip.num_samples = static_cast<int64_t>(audio_plane(0).size());
  audio_clip.length_samples = audio_clip.num_samples;
  audio_clip.gain = 1.0f;
  REQUIRE(sonare_engine_set_clips(engine, &audio_clip, 1) == SONARE_OK);

  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);
  return engine;
}

int engine_latency_samples(SonareRealtimeEngine* engine) {
  return engine->engine.graph_latency_samples_q8() >> 8;
}

std::vector<float> interleave(const std::vector<std::vector<float>>& planes) {
  const size_t channels = planes.size();
  const size_t frames = planes.empty() ? 0 : planes[0].size();
  std::vector<float> out(frames * channels);
  for (size_t f = 0; f < frames; ++f) {
    for (size_t c = 0; c < channels; ++c) out[f * channels + c] = planes[c][f];
  }
  return out;
}

// Live block-processing loop.
std::vector<float> render_live(const Row& row) {
  SonareRealtimeEngine* engine = make_engine(row);
  REQUIRE(sonare_engine_play(engine, -1) == SONARE_OK);
  const int channels = output_channels(row);
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                         std::vector<float>(static_cast<size_t>(kTotalFrames)));
  std::vector<std::vector<float>> block(static_cast<size_t>(channels),
                                        std::vector<float>(kBlockSize));
  std::vector<float*> io(static_cast<size_t>(channels));
  for (int c = 0; c < channels; ++c)
    io[static_cast<size_t>(c)] = block[static_cast<size_t>(c)].data();
  for (int64_t b = 0; b < kTotalFrames / kBlockSize; ++b) {
    for (auto& plane : block) std::fill(plane.begin(), plane.end(), 0.0f);
    REQUIRE(sonare_engine_process(engine, io.data(), channels, kBlockSize) == SONARE_OK);
    for (int c = 0; c < channels; ++c) {
      std::copy(block[static_cast<size_t>(c)].begin(), block[static_cast<size_t>(c)].end(),
                planes[static_cast<size_t>(c)].begin() + b * kBlockSize);
    }
  }
  sonare_engine_destroy(engine);
  return interleave(planes);
}

struct OfflineRender {
  std::vector<float> samples;
  int latency = 0;
};

// One sonare_engine_render_offline call.
OfflineRender render_offline(const Row& row, const EngineOptions& options = {}) {
  SonareRealtimeEngine* engine = make_engine(row, options);
  OfflineRender result;
  result.latency = engine_latency_samples(engine);
  const int channels = output_channels(row);
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                         std::vector<float>(static_cast<size_t>(kTotalFrames)));
  std::vector<float*> ptrs;
  for (auto& plane : planes) ptrs.push_back(plane.data());
  REQUIRE(sonare_engine_render_offline(engine, ptrs.data(), channels, kTotalFrames, kBlockSize) ==
          SONARE_OK);
  sonare_engine_destroy(engine);
  result.samples = interleave(planes);
  return result;
}

// ---------------------------------------------------------------------------
// Project bounce
// ---------------------------------------------------------------------------

std::vector<float> render_project_bounce(const Row& row) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  const std::string scene = project_scene_json(row);
  CAPTURE(scene);
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene.c_str()) == SONARE_OK);

  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, kMidiClipStartPpq, kMidiClipLengthPpq, &midi_track,
                                       &midi_clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
  REQUIRE(sonare_project_set_midi_events(project, midi_clip, events, std::size(events)) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, midi_track, kDestination) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, midi_track, "tmidi", "") == SONARE_OK);
  if (row.clip_gain_fades) {
    REQUIRE(sonare_project_set_clip_gain(project, midi_clip, kMidiClipGain) == SONARE_OK);
    SonareProjectClipFade fade{};
    fade.length_ppq = kMidiFadePpq;
    fade.curve = 0;  // linear
    REQUIRE(sonare_project_set_clip_fade(project, midi_clip, &fade, &fade) == SONARE_OK);
  }

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t audio_track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &audio_track) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, audio_track, "taudio", "") == SONARE_OK);
  const std::vector<float> audio = interleave({audio_plane(0), audio_plane(1)});
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = audio_track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = kAudioClipLengthPpq;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = static_cast<int64_t>(audio_plane(0).size());
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = static_cast<int>(kSampleRate);
  uint32_t audio_clip = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &audio_clip) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = kTotalFrames;
  options.block_size = kBlockSize;
  options.num_channels = output_channels(row);
  options.sample_rate = static_cast<int>(kSampleRate);
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config = project_synth();

  float* out = nullptr;
  size_t out_len = 0;
  REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                         &out_len) == SONARE_OK);
  REQUIRE(out_len == static_cast<size_t>(kTotalFrames * output_channels(row)));
  std::vector<float> result(out, out + out_len);
  sonare_free_floats(out);
  sonare_project_destroy(project);
  return result;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

double rms_db(const std::vector<float>& interleaved, int channels, int64_t begin_frame,
              int64_t end_frame) {
  double sum_sq = 0.0;
  size_t count = 0;
  for (size_t i = static_cast<size_t>(begin_frame * channels);
       i < static_cast<size_t>(end_frame * channels) && i < interleaved.size(); ++i) {
    sum_sq += static_cast<double>(interleaved[i]) * interleaved[i];
    ++count;
  }
  return 10.0 *
         std::log10(std::max(sum_sq / static_cast<double>(std::max<size_t>(count, 1)), 1.0e-30));
}

// Largest per-block level difference between two renders, in dB, over the
// compared window; blocks silent in both are skipped.
double max_block_level_difference_db(const std::vector<float>& a, const std::vector<float>& b,
                                     int channels) {
  double worst = 0.0;
  for (int64_t start = kSettleFrames; start + kBlockSize <= kTotalFrames; start += kBlockSize) {
    const double level_a = rms_db(a, channels, start, start + kBlockSize);
    const double level_b = rms_db(b, channels, start, start + kBlockSize);
    if (std::max(level_a, level_b) < -80.0) continue;
    worst = std::max(worst, std::abs(level_a - level_b));
  }
  return worst;
}

struct Agreement {
  double max_diff = 0.0;
  double bound = 0.0;
  int64_t worst_frame = -1;
  bool ok = false;
};

// max|reference[t + offset] - other[t]| <= kMetricScale * max(1, max|reference|)
// over t in [kSettleFrames, kTotalFrames - offset). @p offset is the engine's
// reported latency when @p other is a latency-compensated bounce.
Agreement check_agreement(const std::vector<float>& reference, const std::vector<float>& other,
                          int channels, int64_t offset = 0) {
  Agreement result;
  double max_ref = 0.0;
  for (int64_t t = kSettleFrames; t + offset < kTotalFrames; ++t) {
    for (int c = 0; c < channels; ++c) {
      const float r = reference[static_cast<size_t>((t + offset) * channels + c)];
      const float o = other[static_cast<size_t>(t * channels + c)];
      const double diff = std::abs(static_cast<double>(r) - o);
      if (diff > result.max_diff) {
        result.max_diff = diff;
        result.worst_frame = t;
      }
      max_ref = std::max(max_ref, static_cast<double>(std::abs(r)));
    }
  }
  result.bound = kMetricScale * std::max(1.0, max_ref);
  result.ok = reference.size() == other.size() && result.max_diff <= result.bound;
  return result;
}

}  // namespace

TEST_CASE(
    "bus routing, sidechain keys, surround buses and MIDI clip gain agree across the "
    "live engine, offline render and project bounce",
    "[mixer_routing_agreement]") {
  for (const Row& row : kRows) {
    DYNAMIC_SECTION("row " << row.number << ": " << describe(row)) {
      const int channels = output_channels(row);
      const OfflineRender offline = render_offline(row);
      const std::vector<float> live = render_live(row);

      const double reference_rms_db =
          rms_db(offline.samples, channels, kSettleFrames, kTotalFrames);
      INFO("row " << row.number << " reference rms_db=" << reference_rms_db
                  << " engine latency=" << offline.latency);
      CHECK(reference_rms_db >= kMinRmsDb);

      const Agreement live_vs_offline = check_agreement(offline.samples, live, channels);
      INFO("row " << row.number << " live vs offline: max_diff=" << live_vs_offline.max_diff
                  << " bound=" << live_vs_offline.bound
                  << " worst_frame=" << live_vs_offline.worst_frame);
      CHECK(live_vs_offline.ok);

      if (compares_bounce(row)) {
        const std::vector<float> bounce = render_project_bounce(row);
        const double bounce_rms_db = rms_db(bounce, channels, kSettleFrames, kTotalFrames);
        const Agreement offline_vs_bounce =
            check_agreement(offline.samples, bounce, channels, offline.latency);
        INFO("row " << row.number << " offline vs project bounce: max_diff="
                    << offline_vs_bounce.max_diff << " bound=" << offline_vs_bounce.bound
                    << " worst_frame=" << offline_vs_bounce.worst_frame
                    << " bounce rms_db=" << bounce_rms_db);
        CHECK(offline_vs_bounce.ok);
      }
      // Otherwise the key source is a track whose lane fader is engine-only: the
      // scene has no equivalent stage, so only the engine's two paths compare.

      if (row.sidechain != Sidechain::kNone) {
        // Non-vacuity: the key has to move the render (an unbound ducker detects
        // on its own input), or agreement proves nothing about the key.
        const OfflineRender unkeyed = render_offline(row, EngineOptions{false});
        const double effect_db =
            max_block_level_difference_db(offline.samples, unkeyed.samples, channels);
        INFO("row " << row.number
                    << " keyed vs unkeyed, largest block level difference: " << effect_db << " dB");
        CHECK(effect_db >= kMinKeyEffectDb);
      }
    }
  }
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_ARRANGEMENT
