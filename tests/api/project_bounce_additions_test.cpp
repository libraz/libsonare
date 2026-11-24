#include <sonare/sonare_c.h>
#include <sonare/sonare_c_engine.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gm_fallback_tail_bounds.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int64_t kMusicFrames = 24000;
constexpr int64_t kOneBarFrames = 96000;

struct Project {
  SonareProject* handle = nullptr;

  ~Project() { sonare_project_destroy(handle); }
  Project() = default;
  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;
  Project(Project&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
  Project& operator=(Project&& other) noexcept {
    if (this != &other) {
      sonare_project_destroy(handle);
      handle = std::exchange(other.handle, nullptr);
    }
    return *this;
  }
};

std::vector<float> source_audio() {
  std::vector<float> samples(static_cast<size_t>(kMusicFrames) * 2);
  for (int64_t frame = 0; frame < kMusicFrames; ++frame) {
    const float value = static_cast<float>(
        0.25 * std::sin(sonare::constants::kTwoPiD * 440.0 * frame / kSampleRate));
    samples[static_cast<size_t>(frame) * 2] = value;
    samples[static_cast<size_t>(frame) * 2 + 1] = value;
  }
  return samples;
}

const char* kSendScene =
    R"({"version":2,"strips":[{"id":"lead","sends":[)"
    R"({"id":"discard-send","destinationBusId":"discard","sendDb":-120,"timing":"postFader"},)"
    R"({"id":"wet-send","destinationBusId":"audible","sendDb":-120,"timing":"postFader"}]}],)"
    R"("buses":[{"id":"master","role":"master"},{"id":"discard","role":"aux"},)"
    R"({"id":"audible","role":"aux"}],"connections":[)"
    R"({"source":"lead","destination":"master"},{"source":"audible","destination":"master"}]})";

Project project_with_audio(bool use_scene = false) {
  Project authored;
  REQUIRE(sonare_project_create(&authored.handle) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(authored.handle, kSampleRate) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "send automation";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(authored.handle, &track_desc, &track_id) == SONARE_OK);
  if (use_scene) {
    REQUIRE(sonare_project_set_track_route(authored.handle, track_id, "lead", nullptr) ==
            SONARE_OK);
    REQUIRE(sonare_project_set_mixer_scene_json(authored.handle, kSendScene) == SONARE_OK);
  }

  const std::vector<float> audio = source_audio();
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.start_ppq = 0.0;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = kMusicFrames;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = kSampleRate;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(authored.handle, &clip_desc, &clip_id) == SONARE_OK);
  return authored;
}

#if defined(SONARE_WITH_MIXING)
Project project_with_send_lane(const std::string& send_id) {
  Project authored = project_with_audio(true);
  SonareProjectClip clip{};
  REQUIRE(sonare_project_clip_by_index(authored.handle, 0, &clip) == SONARE_OK);

  char* serialized = nullptr;
  size_t serialized_size = 0;
  REQUIRE(sonare_project_serialize(authored.handle, &serialized, &serialized_size) == SONARE_OK);
  std::string document(serialized, serialized_size);
  sonare_free_string(serialized);
  if (!send_id.empty()) {
    const std::string empty_lanes = R"("automationLanes":[])";
    const size_t lane_pos = document.find(empty_lanes);
    REQUIRE(lane_pos != std::string::npos);
    const std::string lanes =
        R"("automationLanes":[{"targetParamId":1,"targetKind":"trackSendDb","sendId":")" + send_id +
        R"(","points":[{"ppq":0,"value":-120,"curveToNext":"linear"},)"
        R"({"ppq":1,"value":0,"curveToNext":"linear"}]}])";
    document.replace(lane_pos, empty_lanes.size(), lanes);
  }

  Project loaded;
  char* diagnostics = nullptr;
  REQUIRE(sonare_project_deserialize(document.data(), document.size(), &loaded.handle,
                                     &diagnostics) == SONARE_OK);
  sonare_free_string(diagnostics);
  const auto audio = source_audio();
  REQUIRE(sonare_project_set_source_audio(loaded.handle, clip.source_id, audio.data(), kMusicFrames,
                                          2, kSampleRate) == SONARE_OK);
  return loaded;
}
#endif

std::vector<float> bounce(Project& project, int64_t total_frames,
                          const SonareProjectBounceOptions* supplied = nullptr) {
  SonareProjectBounceOptions defaults{};
  sonare_project_bounce_options_init(&defaults);
  defaults.total_frames = total_frames;
  defaults.block_size = 128;
  defaults.num_channels = 2;
  const SonareProjectBounceOptions* options = supplied != nullptr ? supplied : &defaults;
  float* interleaved = nullptr;
  size_t frames = 0;
  int channels = 0;
  REQUIRE(sonare_project_bounce(project.handle, options, &interleaved, &frames, &channels) ==
          SONARE_OK);
  REQUIRE(channels == 2);
  std::vector<float> result(interleaved, interleaved + frames * static_cast<size_t>(channels));
  sonare_free_floats(interleaved);
  return result;
}

std::string last_error() {
  const char* message = sonare_last_error_message();
  return message == nullptr ? std::string() : std::string(message);
}

}  // namespace

#if defined(SONARE_WITH_MIXING)
TEST_CASE("project bounce schedules scene send automation by send identity",
          "[c_api][bounce][track-send]") {
  Project baseline = project_with_audio(true);
  Project automated = project_with_send_lane("wet-send");

  const auto dry = bounce(baseline, kMusicFrames);
  const auto wet = bounce(automated, kMusicFrames);
  REQUIRE(dry.size() == wet.size());

  float max_difference = 0.0f;
  for (size_t i = 0; i < dry.size(); ++i) {
    max_difference = std::max(max_difference, std::abs(dry[i] - wet[i]));
  }
  CHECK(max_difference > 0.01f);
}

TEST_CASE("project bounce refuses send automation with a missing send identity",
          "[c_api][bounce][track-send]") {
  Project project = project_with_send_lane("missing-send");
  SonareProjectBounceOptions options{};
  sonare_project_bounce_options_init(&options);
  options.total_frames = kMusicFrames;

  float* interleaved = nullptr;
  size_t frames = 0;
  int channels = 0;

  CHECK(sonare_project_bounce(project.handle, &options, &interleaved, &frames, &channels) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(interleaved == nullptr);
  CHECK(frames == 0);
  CHECK(channels == 0);

  CHECK(last_error().find("missing-send") != std::string::npos);

  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_last_bounce_compile_result(project.handle, &result) == SONARE_OK);
  CHECK(result.has_timeline == 0);
  CHECK(result.messages == nullptr);
  CHECK(result.diagnostic_count == 0);
  CHECK(result.diagnostics == nullptr);
  sonare_free_project_compile_result(&result);
}
#endif

TEST_CASE("project bounce validates input arguments before output pointers",
          "[c_api][bounce][count-in]") {
  Project project;
  REQUIRE(sonare_project_create(&project.handle) == SONARE_OK);

  CHECK(sonare_project_count_in_frames(project.handle, -1, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "count_in_bars must be an integer in [0, 16]");
  CHECK(sonare_project_count_in_frames(project.handle, 0, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out_frames must not be NULL");

  SonareProjectBounceOptions options{};
  sonare_project_bounce_options_init(&options);
  options.struct_size = sizeof(options.struct_size);
  size_t frames = 42;
  int channels = 2;
  CHECK(sonare_project_bounce(project.handle, &options, nullptr, &frames, &channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "options.struct_size must be set by sonare_project_bounce_options_init");
  CHECK(frames == 0);
  CHECK(channels == 0);
  sonare_project_bounce_options_init(&options);
  CHECK(sonare_project_bounce(project.handle, &options, nullptr, nullptr, &channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error() == "out must not be NULL");
}

TEST_CASE("project bounce count-in query and prefix use the initial meter",
          "[c_api][bounce][count-in]") {
  Project project = project_with_audio();
  const SonareProjectTempoSegment tempo[] = {{0.0, 120.0, 60.0}, {4.0, 60.0, 0.0}};
  const SonareProjectTimeSignatureSegment meter[] = {{0.0, 3, 4}, {1.5, 2, 4}};
  REQUIRE(sonare_project_set_tempo_segments(project.handle, tempo, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(project.handle, meter, 2) == SONARE_OK);
  int64_t count_in_frames = 0;
  REQUIRE(sonare_project_count_in_frames(project.handle, 1, &count_in_frames) == SONARE_OK);
  const int64_t initial_beat_frames = kSampleRate / 2;
  REQUIRE(count_in_frames == 3 * initial_beat_frames);

  Project alternate_rate;
  REQUIRE(sonare_project_create(&alternate_rate.handle) == SONARE_OK);
  constexpr int kAlternateSampleRate = 44100;
  REQUIRE(sonare_project_set_sample_rate(alternate_rate.handle, kAlternateSampleRate) == SONARE_OK);
  REQUIRE(sonare_project_set_tempo_segments(alternate_rate.handle, tempo, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_time_signatures(alternate_rate.handle, meter, 2) == SONARE_OK);
  int64_t alternate_frames = 0;
  REQUIRE(sonare_project_count_in_frames(alternate_rate.handle, 1, &alternate_frames) == SONARE_OK);
  CHECK(alternate_frames == 3 * kAlternateSampleRate / 2);

  SonareEngineMetronomeConfig metronome{};
  sonare_engine_metronome_config_init(&metronome);
  metronome.enabled = 1;
  SonareProjectBounceOptions options{};
  sonare_project_bounce_options_init(&options);
  options.total_frames = count_in_frames + kMusicFrames;

  options.num_channels = 2;
  options.count_in_bars = 1;
  const auto without_click = bounce(project, options.total_frames, &options);
  const size_t music_start = static_cast<size_t>(count_in_frames) * 2;
  CHECK(std::all_of(without_click.begin(), without_click.begin() + music_start,
                    [](float sample) { return sample == 0.0f; }));

  options.metronome = &metronome;
  const auto rendered = bounce(project, options.total_frames, &options);
  REQUIRE(rendered.size() == static_cast<size_t>(options.total_frames) * 2);
  const auto click_at = [&](int64_t frame) {
    const auto index = static_cast<size_t>(frame) * 2;
    return rendered[index] - without_click[index];
  };
  CHECK(click_at(0) > click_at(initial_beat_frames));
  CHECK(click_at(initial_beat_frames) > 0.0f);
  CHECK(std::abs(click_at(count_in_frames - 2)) < 1e-6f);
  CHECK(click_at(count_in_frames) > 0.0f);

  options.total_frames = initial_beat_frames;
  options.metronome = nullptr;
  const auto truncated = bounce(project, options.total_frames, &options);
  REQUIRE(truncated.size() == static_cast<size_t>(initial_beat_frames) * 2);
  CHECK(
      std::all_of(truncated.begin(), truncated.end(), [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("disabled project bounce click preserves the original render bytes",
          "[c_api][bounce][count-in]") {
  Project project = project_with_audio();
  SonareProjectBounceOptions without_click{};
  sonare_project_bounce_options_init(&without_click);
  without_click.total_frames = kMusicFrames;
  without_click.num_channels = 2;
  SonareEngineMetronomeConfig disabled{};
  sonare_engine_metronome_config_init(&disabled);
  disabled.enabled = 0;
  for (const int bars : {0, 1}) {
    CAPTURE(bars);
    without_click.count_in_bars = bars;
    without_click.total_frames = bars * kOneBarFrames + kMusicFrames;
    const auto baseline = bounce(project, without_click.total_frames, &without_click);
    SonareProjectBounceOptions with_disabled_click = without_click;
    with_disabled_click.metronome = &disabled;
    const auto explicit_disabled =
        bounce(project, without_click.total_frames, &with_disabled_click);
    REQUIRE(explicit_disabled.size() == baseline.size());
    CHECK(std::memcmp(explicit_disabled.data(), baseline.data(), baseline.size() * sizeof(float)) ==
          0);
  }
}

TEST_CASE("project bounce names unknown controller destinations and bad articulation channels",
          "[c_api][bounce][instrument-controls]") {
  Project project;
  REQUIRE(sonare_project_create(&project.handle) == SONARE_OK);
  SonareProjectControllerProfileBinding unknown{};
  unknown.destination_id = 999;
  unknown.preset_name = "gm";
  SonareProjectBounceOptions unknown_options{};
  sonare_project_bounce_options_init(&unknown_options);
  unknown_options.controller_profiles = &unknown;
  unknown_options.controller_profile_count = 1;
  float* audio = nullptr;
  size_t frames = 0;
  int channels = 0;
  CHECK(sonare_project_bounce(project.handle, &unknown_options, &audio, &frames, &channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("options.controller_profiles[0].destination_id") != std::string::npos);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) ==
        "options.controller_profiles[0].destination_id");

  SonareProjectArticulationBinding bad_channel{};
  bad_channel.destination_id = 1;
  bad_channel.channel = 16;
  bad_channel.articulation = SONARE_ARTICULATION_POLY;
  SonareProjectBounceOptions articulation_options{};
  sonare_project_bounce_options_init(&articulation_options);
  articulation_options.articulations = &bad_channel;
  articulation_options.articulation_count = 1;
  CHECK(sonare_project_bounce(project.handle, &articulation_options, &audio, &frames, &channels) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_error().find("options.articulations[0].channel") != std::string::npos);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "options.articulations[0].channel");
}

TEST_CASE("project bounce applies controller profiles and articulation to the hosted synth",
          "[c_api][bounce][instrument-controls]") {
  Project project;
  REQUIRE(sonare_project_create(&project.handle) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project.handle, kSampleRate) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.handle, 0.0, 2.0, &track, &clip) == SONARE_OK);
  SonareMidiEventPod events[6]{};
  REQUIRE(sonare_midi_note_on(0.0, 0, 0, 60, 100, &events[0]) == SONARE_OK);
  REQUIRE(sonare_midi_cc(0.125, 0, 0, 2, 127, &events[1]) == SONARE_OK);
  REQUIRE(sonare_midi_note_on(0.25, 0, 0, 69, 100, &events[2]) == SONARE_OK);
  REQUIRE(sonare_midi_cc(0.375, 0, 0, 2, 32, &events[3]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(0.75, 0, 0, 60, 0, &events[4]) == SONARE_OK);
  REQUIRE(sonare_midi_note_off(0.875, 0, 0, 69, 0, &events[5]) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project.handle, clip, events, 6) == SONARE_OK);

  SonareSynthInstrumentBinding synth{};
  sonare_synth_patch_init(&synth.patch);
  synth.patch.engine_mode = SONARE_SYNTH_ENGINE_REED;
  SonareProjectBounceOptions options{};
  sonare_project_bounce_options_init(&options);
  options.total_frames = kMusicFrames;
  options.synth_instruments = &synth;
  options.synth_instrument_count = 1;
  const auto plain = bounce(project, options.total_frames, &options);

  const SonareProjectControllerProfileBinding breath{0, 0, "breath"};
  options.controller_profiles = &breath;
  options.controller_profile_count = 1;
  const auto with_profile = bounce(project, options.total_frames, &options);

  const SonareProjectArticulationBinding legato{0, 0, SONARE_ARTICULATION_MONO_LEGATO, 0};
  options.articulations = &legato;
  options.articulation_count = 1;
  const auto with_legato = bounce(project, options.total_frames, &options);
  const auto max_difference = [](const auto& left, const auto& right) {
    REQUIRE(left.size() == right.size());
    float distance = 0.0f;
    for (size_t i = 0; i < left.size(); ++i) {
      distance = std::max(distance, std::abs(left[i] - right[i]));
    }
    return distance;
  };
  CHECK(max_difference(plain, with_profile) > 1e-3f);
  CHECK(max_difference(with_profile, with_legato) > 1e-3f);

  SonareProjectCompileResult compiled{};
  SonareProjectTimeline* compiled_timeline = nullptr;
  REQUIRE(sonare_project_compile(project.handle, &compiled, &compiled_timeline) == SONARE_OK);
  const bool has_timeline = compiled.has_timeline != 0;
  sonare_free_project_compile_result(&compiled);
  const std::unique_ptr<SonareProjectTimeline, decltype(&sonare_project_timeline_destroy)> timeline(
      compiled_timeline, sonare_project_timeline_destroy);
  REQUIRE(has_timeline);
  REQUIRE(timeline != nullptr);

  const auto compare_engine = [&](const std::vector<float>& bounced,
                                  const SonareProjectControllerProfileBinding* profile,
                                  const SonareProjectArticulationBinding* articulation) {
    CAPTURE(profile != nullptr, articulation != nullptr);
    SonareEngine* created = nullptr;
    REQUIRE(sonare_engine_create(&created) == SONARE_OK);
    const std::unique_ptr<SonareEngine, decltype(&sonare_engine_destroy)> engine(
        created, sonare_engine_destroy);
    constexpr int kStereoChannels = 2;
    REQUIRE(sonare_engine_prepare(engine.get(), kSampleRate, options.block_size, 64, 16,
                                  kStereoChannels) == SONARE_OK);
    REQUIRE(sonare_engine_apply_project_timeline(engine.get(), timeline.get()) == SONARE_OK);
    REQUIRE(sonare_engine_set_synth_instrument(engine.get(), &synth) == SONARE_OK);
    if (profile != nullptr) {
      REQUIRE(sonare_engine_set_controller_profile(engine.get(), profile->destination_id,
                                                   profile->preset_name) == SONARE_OK);
    }
    if (articulation != nullptr) {
      REQUIRE(sonare_engine_set_articulation(engine.get(), articulation->destination_id,
                                             static_cast<uint8_t>(articulation->channel),
                                             articulation->articulation) == SONARE_OK);
    }
    REQUIRE(sonare_engine_prime_offline_parameters(engine.get(), kStereoChannels,
                                                   options.block_size) == SONARE_OK);
    REQUIRE(sonare_engine_play(engine.get(), -1) == SONARE_OK);
    const auto frames = static_cast<size_t>(options.total_frames);
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    float* planes[] = {left.data(), right.data()};
    REQUIRE(sonare_engine_render_offline(engine.get(), planes, kStereoChannels,
                                         options.total_frames, options.block_size,
                                         nullptr) == SONARE_OK);

    REQUIRE(bounced.size() == frames * kStereoChannels);
    for (size_t channel = 0; channel < kStereoChannels; ++channel) {
      CAPTURE(channel);
      float peak = 0.0f;
      float max_error = 0.0f;
      bool finite = true;
      for (size_t frame = 0; frame < frames; ++frame) {
        const float expected = planes[channel][frame];
        const float actual = bounced[frame * kStereoChannels + channel];
        finite &= std::isfinite(expected) && std::isfinite(actual);
        peak = std::max({peak, std::abs(expected), std::abs(actual)});
        max_error = std::max(max_error, std::abs(expected - actual));
      }
      REQUIRE(finite);
      REQUIRE(peak > 1e-3f);
      const float tolerance = std::min(1e-6f, 32.0f * std::numeric_limits<float>::epsilon() * peak);
      CHECK(max_error <= tolerance);
    }
  };
  compare_engine(plain, nullptr, nullptr);
  compare_engine(with_profile, &breath, nullptr);
  compare_engine(with_legato, &breath, &legato);
}

TEST_CASE("project bounce automatic tail follows the sounded SF2 fallback program",
          "[c_api][bounce][offline-tail]") {
  Project project;
  REQUIRE(sonare_project_create(&project.handle) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project.handle, kSampleRate) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project.handle, 0.0, 4.0, &track, &clip) == SONARE_OK);
  constexpr uint8_t kFingeredBass = 33;
  constexpr uint8_t kBassNote = 45;
  constexpr double kNoteOffPpq = 3.99;
  constexpr size_t kStereoChannels = 2;
  constexpr int64_t kAutomatic = SONARE_PROJECT_BOUNCE_LENGTH_FROM_ARRANGEMENT;
  SonareSf2InstrumentBinding sf2{};
  sonare_sf2_instrument_config_init(&sf2.config);
  SonareProjectBounceOptions options{};
  sonare_project_bounce_options_init(&options);
  options.sf2_instruments = &sf2;
  options.sf2_instrument_count = 1;
  const auto render_program = [&](uint8_t program, uint8_t note, int64_t frames,
                                  bool sound_note = true) {
    SonareMidiEventPod events[3]{};
    REQUIRE(sonare_midi_program(0.0, 0, 0, program, &events[0]) == SONARE_OK);
    REQUIRE(sonare_midi_note_on(2.99, 0, 0, note, 100, &events[1]) == SONARE_OK);
    REQUIRE(sonare_midi_note_off(kNoteOffPpq, 0, 0, note, 0, &events[2]) == SONARE_OK);
    REQUIRE(sonare_project_set_midi_events(project.handle, clip, events, sound_note ? 3 : 1) ==
            SONARE_OK);
    options.total_frames = frames;
    return bounce(project, options.total_frames, &options);
  };

  // The idle render includes the arrangement and the default GS system effect budget.
  const auto idle = render_program(kFingeredBass, kBassNote, kAutomatic, false);
  const auto bass = render_program(kFingeredBass, kBassNote, kAutomatic);
  const auto bass_frames = static_cast<int64_t>(bass.size() / kStereoChannels);
  const auto idle_frames = static_cast<int64_t>(idle.size() / kStereoChannels);
  const auto& bass_patch = sonare::midi::synth::gm_fallback_patch(0, kFingeredBass);
  const int64_t bass_tail =
      sonare::midi::synth::gm_fallback_program_tail_samples(bass_patch, kSampleRate, {});
  REQUIRE(bass_tail > 0);
  CHECK(bass_frames - idle_frames == bass_tail);

  // An explicit render beyond the automatic end exposes any truncated release.
  const float tail_threshold = std::pow(10.0f, -90.0f / 20.0f);
  const auto reference = render_program(kFingeredBass, kBassNote, bass_frames + kSampleRate);
  const auto last_audible = std::find_if(reference.rbegin(), reference.rend(), [=](float sample) {
    return std::abs(sample) > tail_threshold;
  });
  REQUIRE(last_audible != reference.rend());
  const auto last_audible_frame =
      (std::distance(last_audible, reference.rend()) - 1) / static_cast<ptrdiff_t>(kStereoChannels);
  CHECK(last_audible_frame > static_cast<int64_t>(kNoteOffPpq * kMusicFrames));
  CHECK(last_audible_frame < bass_frames);

  const auto piano = render_program(0, 60, kAutomatic);
  CHECK(piano.size() > bass.size());
  const size_t last_block = static_cast<size_t>(options.block_size) * kStereoChannels;
  REQUIRE(piano.size() >= last_block);
  CHECK(std::all_of(piano.end() - static_cast<ptrdiff_t>(last_block), piano.end(),
                    [=](float sample) { return std::abs(sample) < tail_threshold; }));
}
