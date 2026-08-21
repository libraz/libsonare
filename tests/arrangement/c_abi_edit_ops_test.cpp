/// @file c_abi_edit_ops_test.cpp
/// @brief Exercises the headless-DAW edit C-ABI wrappers (clip/track edits,
/// automation lanes, and the MoveClip wrong-kind guard) including undo/redo
/// round-trips and deep-equality (serialized bytes) after undo.

#include <sonare/sonare_c.h>
#include <sonare/sonare_c_project.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "c_api/project_internal.h"

namespace {

// Serializes the project to deterministic JSON for deep-equality comparisons.
std::string serialize(const SonareProject* project) {
  char* json = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_serialize(project, &json, &len) == SONARE_OK);
  REQUIRE(json != nullptr);
  std::string out(json, len);
  sonare_free_string(json);
  return out;
}

// Small interleaved stereo buffer so audio clips carry renderable samples.
std::vector<float> make_stereo(int frames) {
  std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
  for (int i = 0; i < frames; ++i) {
    const float v = 0.1f * static_cast<float>(std::sin(0.05 * i));
    out[static_cast<size_t>(i) * 2] = v;
    out[static_cast<size_t>(i) * 2 + 1] = v;
  }
  return out;
}

// Adds an audio track carrying one audio clip with decoded samples. Returns the
// track id via out params.
struct AudioFixture {
  uint32_t track = 0;
  uint32_t clip = 0;
  std::vector<float> audio;
};

AudioFixture add_audio_track_clip(SonareProject* project, double start_ppq, double length_ppq) {
  AudioFixture fx;
  fx.audio = make_stereo(480);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  REQUIRE(sonare_project_add_track(project, &track_desc, &fx.track) == SONARE_OK);
  REQUIRE(fx.track != 0);

  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = fx.track;
  clip_desc.is_midi = 0;
  clip_desc.start_ppq = start_ppq;
  clip_desc.length_ppq = length_ppq;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = fx.audio.data();
  clip_desc.audio_frames = static_cast<int64_t>(fx.audio.size() / 2);
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &fx.clip) == SONARE_OK);
  REQUIRE(fx.clip != 0);
  return fx;
}

}  // namespace

TEST_CASE("C-ABI add_clip owns decoded audio in one undo transaction", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);
  const std::string before = serialize(project);

  const std::vector<float> audio = {0.25f, -0.25f, 0.5f, -0.5f};
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = 2;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
  const std::string added = serialize(project);

  size_t count = 0;
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  REQUIRE(count == 1);
  REQUIRE(project->audio.sources.size() == 1);
  const arr::SourceId source_id = project->audio.sources.begin()->first;
  REQUIRE(project->audio.sources.at(source_id).channels[0] == std::vector<float>{0.25f, 0.5f});
  REQUIRE(project->audio.sources.at(source_id).channels[1] == std::vector<float>{-0.25f, -0.5f});

  // One undo removes the clip, its source metadata, and decoded sample owner.
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  REQUIRE(count == 0);
  REQUIRE(project->audio.sources.empty());

  // Redo restores the same ids, bytes, and sample content in one step.
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == added);
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(project->audio.sources.at(source_id).channels[0] == std::vector<float>{0.25f, 0.5f});

  // A later command failure (overlap rejection) rolls the source metadata back
  // before the audio transfer command runs, leaving the existing store exact.
  uint32_t failed_clip_id = 123;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &failed_clip_id) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(failed_clip_id == 0);
  REQUIRE(serialize(project) == added);
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(project->audio.sources.at(source_id).channels[0] == std::vector<float>{0.25f, 0.5f});

  SonareProjectCompileResult compile{};
  REQUIRE(sonare_project_compile(project, &compile) == SONARE_OK);
  REQUIRE(compile.has_timeline == 1);
  sonare_project_free_compile_result(&compile);

  // Discarding the redo branch releases history-owned detached audio rather
  // than returning it to AudioContentStore as an orphan.
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(project->audio.sources.empty());
  REQUIRE(sonare_project_rename_track(project, track_id, "renamed") == SONARE_OK);
  REQUIRE(sonare_project_redo(project) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(project->audio.sources.empty());
  REQUIRE(sonare_project_source_count(project, &count) == SONARE_OK);
  REQUIRE(count == 0);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI remove_clip collects orphaned source metadata and PCM", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "audio";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);
  const std::string empty = serialize(project);

  const std::vector<float> audio = {0.25f, -0.25f, 0.5f, -0.5f};
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = audio.data();
  clip_desc.audio_frames = 2;
  clip_desc.audio_channels = 2;
  clip_desc.audio_sample_rate = 48000;

  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
  const std::string added = serialize(project);
  const arr::SourceId source_id = project->audio.sources.begin()->first;

  REQUIRE(sonare_project_remove_clip(project, clip_id) == SONARE_OK);
  size_t source_count = 0;
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(source_count == 0);
  REQUIRE(project->audio.sources.empty());
  REQUIRE(serialize(project) == empty);

  // Undo restores both the source registry and the exact PCM map node; redo
  // removes both again as a single user-visible edit.
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == added);
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(project->audio.sources.at(source_id).channels[0] == std::vector<float>{0.25f, 0.5f});
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == empty);
  REQUIRE(project->audio.sources.empty());

  // Bound history to one entry, then repeat the public add/remove path. This
  // exercises release of discarded history-owned PCM and verifies that the
  // serialized project and live AudioContentStore remain steady.
  REQUIRE(sonare_project_set_max_undo_depth(project, 1) == SONARE_OK);
  for (int i = 0; i < 100; ++i) {
    clip_id = 0;
    REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
    REQUIRE(sonare_project_remove_clip(project, clip_id) == SONARE_OK);
    REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
    REQUIRE(source_count == 0);
    REQUIRE(project->audio.sources.empty());
    REQUIRE(serialize(project) == empty);
  }

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI remove_clip retains sources shared by another clip", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  const AudioFixture first = add_audio_track_clip(project, 0.0, 1.0);
  REQUIRE(project->audio.sources.size() == 1);
  const arr::SourceId shared_source = project->audio.sources.begin()->first;

  // A second placement can reference the same source. It is deliberately
  // created at the model layer here because the public add API imports a new
  // source by design; public removal must nevertheless preserve shared data.
  arr::EditClip second;
  second.track_id = first.track;
  second.source_id = shared_source;
  second.start_ppq = 2.0;
  second.length_ppq = 1.0;
  second.gain = 1.0f;
  auto add_second = std::make_unique<arr::AddClip>(second);
  arr::AddClip* raw_second = add_second.get();
  REQUIRE(project->history.apply(std::move(add_second)));
  const uint32_t second_clip = raw_second->allocated_id();
  REQUIRE(second_clip != 0);

  REQUIRE(sonare_project_remove_clip(project, first.clip) == SONARE_OK);
  size_t source_count = 0;
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(source_count == 1);
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(project->audio.sources.find(shared_source) != project->audio.sources.end());

  REQUIRE(sonare_project_remove_clip(project, second_clip) == SONARE_OK);
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(source_count == 0);
  REQUIRE(project->audio.sources.empty());
  sonare_project_destroy(project);
}

TEST_CASE("C-ABI add_midi_clip creates track source and clip as one transaction",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  const std::string before = serialize(project);

  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 2.0, 4.0, &track_id, &clip_id) == SONARE_OK);
  const std::string added = serialize(project);

  size_t track_count = 0;
  size_t clip_count = 0;
  size_t source_count = 0;
  REQUIRE(sonare_project_track_count(project, &track_count) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clip_count) == SONARE_OK);
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(track_count == 1);
  REQUIRE(clip_count == 1);
  REQUIRE(source_count == 1);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(sonare_project_track_count(project, &track_count) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clip_count) == SONARE_OK);
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(track_count == 0);
  REQUIRE(clip_count == 0);
  REQUIRE(source_count == 0);

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == added);
  REQUIRE(sonare_project_track_count(project, &track_count) == SONARE_OK);
  REQUIRE(sonare_project_clip_count(project, &clip_count) == SONARE_OK);
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(track_count == 1);
  REQUIRE(clip_count == 1);
  REQUIRE(source_count == 1);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI clear_history and set_max_undo_depth manage the undo stack",
          "[project][c-abi-edit]") {
  REQUIRE(sonare_project_clear_history(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_max_undo_depth(nullptr, 4) == SONARE_ERROR_INVALID_PARAMETER);

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  // Cap the retained undo depth, then apply more edits than the cap.
  REQUIRE(sonare_project_set_max_undo_depth(project, 3) == SONARE_OK);
  for (int i = 0; i < 6; ++i) {
    uint32_t track_id = 0;
    uint32_t clip_id = 0;
    REQUIRE(sonare_project_add_midi_clip(project, i * 4.0, 4.0, &track_id, &clip_id) == SONARE_OK);
  }
  // Only the last three edits remain undoable; the rest were evicted.
  int undone = 0;
  while (sonare_project_undo(project) == SONARE_OK) ++undone;
  REQUIRE(undone == 3);

  // clear_history drops both stacks, so neither undo nor redo has anything left.
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_clear_history(project) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(sonare_project_redo(project) == SONARE_ERROR_INVALID_STATE);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI max history bytes accepts zero and tiny caps safely", "[project][c-abi-edit]") {
  REQUIRE(sonare_project_set_max_history_bytes(nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_max_history_bytes(nullptr, std::numeric_limits<size_t>::max()) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(project->history.max_history_bytes() == 256u * 1024u * 1024u);
  REQUIRE(sonare_project_set_max_history_bytes(project, std::numeric_limits<size_t>::max()) ==
          SONARE_OK);
  REQUIRE(project->history.max_history_bytes() == std::numeric_limits<size_t>::max());
  REQUIRE(sonare_project_set_max_history_bytes(project, 0) == SONARE_OK);
  REQUIRE(project->history.max_history_bytes() == 0);

  // A successful edit still mutates the project, but its command pair is too
  // large for a zero-byte budget and is therefore immediately non-undoable.
  SonareProjectTrackDesc desc{};
  desc.kind = SONARE_TRACK_AUDIO;
  desc.name = "tiny-cap";
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &desc, &track_id) == SONARE_OK);
  REQUIRE(track_id != 0);
  size_t track_count = 0;
  REQUIRE(sonare_project_track_count(project, &track_count) == SONARE_OK);
  REQUIRE(track_count == 1);
  REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);

  // A tiny positive cap is accepted and remains safe for compound edits that
  // move detached PCM/map nodes through the history command state.
  REQUIRE(sonare_project_set_max_history_bytes(project, 1) == SONARE_OK);
  std::vector<float> pcm = {0.25f, -0.5f};
  SonareProjectClipDesc clip{};
  clip.track_id = track_id;
  clip.start_ppq = 0.0;
  clip.length_ppq = 1.0;
  clip.audio_interleaved = pcm.data();
  clip.audio_frames = 2;
  clip.audio_channels = 1;
  clip.audio_sample_rate = 48000;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip, &clip_id) == SONARE_OK);
  REQUIRE(clip_id != 0);
  REQUIRE(project->audio.sources.size() == 1);
  REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);
  REQUIRE(project->audio.sources.size() == 1);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI detached PCM edits become non-undoable at a byte boundary",
          "[project][c-abi-edit][history_bytes]") {
  SECTION("remove_clip") {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_max_history_bytes(project, 1) == SONARE_OK);

    SonareProjectTrackDesc track_desc{};
    track_desc.kind = SONARE_TRACK_AUDIO;
    uint32_t track_id = 0;
    REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);

    const std::vector<float> samples = {0.25f, -0.5f};
    SonareProjectClipDesc clip_desc{};
    clip_desc.track_id = track_id;
    clip_desc.length_ppq = 1.0;
    clip_desc.gain = 1.0f;
    clip_desc.audio_interleaved = samples.data();
    clip_desc.audio_frames = static_cast<int64_t>(samples.size());
    clip_desc.audio_channels = 1;
    clip_desc.audio_sample_rate = 48000;
    uint32_t clip_id = 0;
    REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
    REQUIRE(project->audio.sources.size() == 1);

    // remove_clip detaches the PCM map node into its history-owned command
    // state; the one-byte cap must discard that chain while preserving success.
    REQUIRE(sonare_project_remove_clip(project, clip_id) == SONARE_OK);
    REQUIRE(project->audio.sources.empty());
    REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);
    sonare_project_destroy(project);
  }

  SECTION("set_source_audio") {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_max_history_bytes(project, 1) == SONARE_OK);

    SonareProjectTrackDesc track_desc{};
    track_desc.kind = SONARE_TRACK_AUDIO;
    uint32_t track_id = 0;
    REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);

    const std::vector<float> initial = {0.25f, -0.5f};
    SonareProjectClipDesc clip_desc{};
    clip_desc.track_id = track_id;
    clip_desc.length_ppq = 1.0;
    clip_desc.gain = 1.0f;
    clip_desc.audio_interleaved = initial.data();
    clip_desc.audio_frames = static_cast<int64_t>(initial.size());
    clip_desc.audio_channels = 1;
    clip_desc.audio_sample_rate = 48000;
    uint32_t clip_id = 0;
    REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
    const arr::SourceId source_id = project->audio.sources.begin()->first;

    const std::vector<float> replacement = {-1.0f, 0.75f};
    REQUIRE(sonare_project_set_source_audio(project, source_id, replacement.data(),
                                            static_cast<int64_t>(replacement.size()), 1,
                                            48000) == SONARE_OK);
    REQUIRE(project->audio.sources.at(source_id).channels.front() == replacement);
    REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);
    sonare_project_destroy(project);
  }

  SECTION("external_stems") {
    SonareProject* project = nullptr;
    REQUIRE(sonare_project_create(&project) == SONARE_OK);
    REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
    REQUIRE(sonare_project_set_max_history_bytes(project, 1) == SONARE_OK);

    const std::vector<float> left = {1.0f, 0.5f};
    const std::vector<float> right = {-0.5f, -0.25f};
    const float* planes[] = {left.data(), right.data()};
    SonareExternalStemDesc stem{};
    stem.name = "boundary";
    stem.layout = SONARE_EXTERNAL_STEM_STEREO;
    stem.planar_samples = planes;
    stem.frame_count = static_cast<int64_t>(left.size());
    stem.start_frame = 0;
    SonareExternalStemImportRequest request{};
    request.sample_rate = 48000;
    request.stems = &stem;
    request.stem_count = 1;
    SonareExternalStemImportResult result{};
    REQUIRE(sonare_project_import_external_stems(project, &request, &result) == SONARE_OK);
    REQUIRE(result.count == 1);
    sonare_free_external_stem_import_result(&result);
    REQUIRE(project->audio.sources.size() == 1);
    REQUIRE(project->history.project().tracks().size() == 1);
    REQUIRE(project->history.project().clips().size() == 1);
    REQUIRE(sonare_project_undo(project) == SONARE_ERROR_INVALID_STATE);
    sonare_project_destroy(project);
  }
}

TEST_CASE("C-ABI set_sample_rate names its accepted range when it refuses one",
          "[project][c-abi-edit]") {
  // Documenting the range only helps a caller who reads the header. The one who
  // hits it at runtime saw a bare "invalid parameter" for a rate nothing told
  // them was out of bounds, on every facade, since the error code is all any of
  // them has to report.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  REQUIRE(sonare_project_set_sample_rate(project, 4000.0) == SONARE_ERROR_INVALID_PARAMETER);
  const std::string too_low = sonare_last_error_message();
  CAPTURE(too_low);
  REQUIRE(too_low.find("8000") != std::string::npos);
  REQUIRE(too_low.find("384000") != std::string::npos);

  REQUIRE(sonare_project_set_sample_rate(project, 1'000'000.0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("384000") != std::string::npos);

  // A rate inside the range still succeeds and leaves no error behind it.
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  REQUIRE(std::string(sonare_last_error_message()).empty());

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_sample_rate is undoable like every other setter", "[project][c-abi-edit]") {
  // Every facade documents that all mutation routes through the native
  // EditHistory, so undo() must return the whole serialized project to what it
  // was -- a setter that writes straight to the Project leaves its value behind
  // after an undo that reported success, and that wrong rate is what gets
  // saved and what later bounces and resampling read.
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);

  const std::string before = serialize(project);
  double rate = 0.0;
  REQUIRE(sonare_project_get_sample_rate(project, &rate) == SONARE_OK);
  REQUIRE(rate == 48000.0);

  REQUIRE(sonare_project_set_sample_rate(project, 96000.0) == SONARE_OK);
  REQUIRE(sonare_project_get_sample_rate(project, &rate) == SONARE_OK);
  REQUIRE(rate == 96000.0);
  REQUIRE(serialize(project) != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(sonare_project_get_sample_rate(project, &rate) == SONARE_OK);
  CHECK(rate == 48000.0);
  CHECK(serialize(project) == before);

  // And redo puts it back, so the command is a full member of the stack rather
  // than a one-way trip.
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_get_sample_rate(project, &rate) == SONARE_OK);
  CHECK(rate == 96000.0);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI remove_clip removes and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, 48000.0) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);

  const std::string before = serialize(project);

  REQUIRE(sonare_project_remove_clip(project, fx.clip) == SONARE_OK);
  REQUIRE(serialize(project) != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) != before);

  // Invalid params.
  REQUIRE(sonare_project_remove_clip(nullptr, fx.clip) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_clip(project, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_clip(project, 999999) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_gain incl. gain=0 mute and undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);

  const std::string before = serialize(project);

  // Explicit gain = 0 (muted) is accepted (add_clip coerces 0 -> 1.0; this is
  // the path that lets a caller actually mute a clip).
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, 0.0f) == SONARE_OK);
  const std::string muted = serialize(project);
  REQUIRE(muted != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // A non-zero gain round-trips through undo too.
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, 0.5f) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: negative / non-finite / unknown clip.
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, -0.1f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, fx.clip, std::nanf("")) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, 0, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_gain(project, 999999, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_fade applies and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string before = serialize(project);

  SonareProjectClipFade fade_in{};
  fade_in.length_ppq = 0.5;
  fade_in.curve = SONARE_FADE_CURVE_EQUAL_POWER;
  SonareProjectClipFade fade_out{};
  fade_out.length_ppq = 1.0;
  fade_out.curve = SONARE_FADE_CURVE_EXPONENTIAL;

  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &fade_in, &fade_out) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Zero-length fade is valid (no fade).
  SonareProjectClipFade none{};
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &none, &none) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // Invalid: null descs, negative length, out-of-range curve, bad clip.
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, nullptr, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &fade_in, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectClipFade bad_len{};
  bad_len.length_ppq = -1.0;
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &bad_len, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectClipFade bad_curve{};
  bad_curve.curve = 99;
  REQUIRE(sonare_project_set_clip_fade(project, fx.clip, &bad_curve, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_fade(project, 999999, &fade_in, &fade_out) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI resolves canonical clip fade curve names", "[project][c-abi-edit]") {
  uint32_t curve = 99;
  REQUIRE(sonare_project_fade_curve_from_name("Equal_Power", &curve) == SONARE_OK);
  REQUIRE(curve == SONARE_FADE_CURVE_EQUAL_POWER);
  REQUIRE(sonare_project_fade_curve_from_name("EXP", &curve) == SONARE_OK);
  REQUIRE(curve == SONARE_FADE_CURVE_EXPONENTIAL);
  REQUIRE(sonare_project_fade_curve_from_name("log", &curve) == SONARE_OK);
  REQUIRE(curve == SONARE_FADE_CURVE_LOGARITHMIC);
  REQUIRE(sonare_project_fade_curve_from_name("unknown", &curve) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("C-ABI set_clip_loop applies and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 2.0, 0.0) ==
          SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Turning loop off with length 0 is valid.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_OFF, 0.0, 0.0) ==
          SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // A loop crossfade round-trips through serialization and undo restores it.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 4.0, 0.5) ==
          SONARE_OK);
  const std::string with_crossfade = serialize(project);
  REQUIRE(with_crossfade.find("loop_crossfade_ppq") != std::string::npos);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Looping with length 0 means "loop the entire clip"; it is accepted and
  // round-trips through undo.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 0.0, 0.0) ==
          SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: negative loop length, bad mode, bad clip, negative crossfade.
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, -1.0, 0.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, 7, 2.0, 0.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_loop(project, 999999, SONARE_LOOP_MODE_LOOP, 2.0, 0.0) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 2.0, -1.0) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_takes and comp segments apply and undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string before = serialize(project);

  SonareProjectClipTake takes[2]{};
  takes[0].id = 1;
  takes[0].source_id = 0;
  takes[0].source_offset_ppq = 0.0;
  takes[0].name = "take A";
  takes[1].id = 2;
  takes[1].source_id = 0;
  takes[1].source_offset_ppq = 1.0;
  takes[1].name = "take B";
  REQUIRE(sonare_project_set_clip_takes(project, fx.clip, takes, 2, 1) == SONARE_OK);
  const std::string with_takes = serialize(project);
  REQUIRE(with_takes != before);
  REQUIRE(with_takes.find("\"takes\"") != std::string::npos);
  REQUIRE(with_takes.find("\"active_take_id\":1") != std::string::npos);

  SonareProjectClipCompSegment segments[2]{};
  segments[0].start_ppq = 0.0;
  segments[0].end_ppq = 2.0;
  segments[0].take_id = 1;
  segments[1].start_ppq = 2.0;
  segments[1].end_ppq = 4.0;
  segments[1].take_id = 2;
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, segments, 2) == SONARE_OK);
  const std::string with_comp = serialize(project);
  REQUIRE(with_comp != with_takes);
  REQUIRE(with_comp.find("\"comp_segments\"") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_takes);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_takes);

  SonareProjectClipTake dup[2] = {takes[0], takes[0]};
  REQUIRE(sonare_project_set_clip_takes(project, fx.clip, dup, 2, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_takes(project, fx.clip, takes, 2, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);
  SonareProjectClipCompSegment bad_segment{};
  bad_segment.start_ppq = 4.0;
  bad_segment.end_ppq = 2.0;
  bad_segment.take_id = 1;
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, &bad_segment, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  segments[1].take_id = 99;
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, segments, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  segments[1].take_id = 2;
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, segments, 2) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 2.0, 0.0) ==
          SONARE_ERROR_INVALID_STATE);

  SonareProjectClipCompSegment whole_segment{};
  whole_segment.start_ppq = 0.0;
  whole_segment.end_ppq = 8.0;
  whole_segment.take_id = 1;
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, &whole_segment, 1) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_loop(project, fx.clip, SONARE_LOOP_MODE_LOOP, 2.0, 0.0) ==
          SONARE_OK);
  REQUIRE(sonare_project_set_clip_comp_segments(project, fx.clip, segments, 2) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI rejects takes and comp segments on MIDI clips", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &midi_track, &midi_clip) == SONARE_OK);
  const std::string before = serialize(project);

  SonareProjectClipTake take{};
  take.id = 1;
  take.source_id = 0;
  take.source_offset_ppq = 0.0;
  take.name = "midi take";
  REQUIRE(sonare_project_set_clip_takes(project, midi_clip, &take, 1, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == before);

  SonareProjectClipCompSegment segment{};
  segment.start_ppq = 0.0;
  segment.end_ppq = 1.0;
  segment.take_id = 0;
  REQUIRE(sonare_project_set_clip_comp_segments(project, midi_clip, &segment, 1) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_set_clip_takes(project, midi_clip, nullptr, 0, 0) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_comp_segments(project, midi_clip, nullptr, 0) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI loop recording audio is split into clip takes", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track{};
  track.kind = SONARE_TRACK_AUDIO;
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track, &track_id) == SONARE_OK);
  const std::string before = serialize(project);

  std::vector<float> audio(48000, 0.0f);
  std::fill(audio.begin(), audio.begin() + 24000, 0.25f);
  std::fill(audio.begin() + 24000, audio.end(), 0.75f);

  SonareProjectLoopRecordingDesc desc{};
  desc.track_id = track_id;
  desc.start_ppq = 0.0;
  desc.loop_length_ppq = 1.0;
  desc.audio_interleaved = audio.data();
  desc.audio_frames = 48000;
  desc.audio_channels = 1;
  desc.audio_sample_rate = 48000;
  uint32_t clip_id = 0;
  size_t take_count = 0;
  REQUIRE(sonare_project_add_loop_recording_takes(project, &desc, &clip_id, &take_count) ==
          SONARE_OK);
  REQUIRE(clip_id != 0);
  REQUIRE(take_count == 2);
  REQUIRE(project->audio.sources.size() == 2);

  const std::string json = serialize(project);
  REQUIRE(json.find("\"takes\"") != std::string::npos);
  REQUIRE(json.find("\"active_take_id\":2") != std::string::npos);
  REQUIRE(json.find("\"length_ppq\":1") != std::string::npos);

  SonareProjectCompileResult compile{};
  REQUIRE(sonare_project_compile(project, &compile) == SONARE_OK);
  REQUIRE(compile.has_timeline == 1);
  sonare_project_free_compile_result(&compile);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  const std::string undone = serialize(project);
  REQUIRE(undone == before);
  REQUIRE(project->audio.sources.empty());

  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == json);
  REQUIRE(project->audio.sources.size() == 2);
  REQUIRE(sonare_project_compile(project, &compile) == SONARE_OK);
  REQUIRE(compile.has_timeline == 1);
  sonare_project_free_compile_result(&compile);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(project->audio.sources.empty());

  desc.track_id = 9999;
  REQUIRE(sonare_project_add_loop_recording_takes(project, &desc, &clip_id, &take_count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  sonare_project_destroy(project);

  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture existing = add_audio_track_clip(project, 0.0, 1.0);
  const std::string before_failed_add = serialize(project);
  desc.track_id = existing.track;
  desc.start_ppq = 0.0;
  desc.loop_length_ppq = 1.0;
  clip_id = 0;
  take_count = 0;
  REQUIRE(sonare_project_add_loop_recording_takes(project, &desc, &clip_id, &take_count) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(clip_id == 0);
  REQUIRE(take_count == 0);
  REQUIRE(serialize(project) == before_failed_add);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"clips\":[]") != std::string::npos);
  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_clip_source rebinds and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  // Two audio clips on two tracks => two distinct audio sources.
  AudioFixture a = add_audio_track_clip(project, 0.0, 4.0);
  AudioFixture b = add_audio_track_clip(project, 0.0, 4.0);

  // Discover b's source id by rebinding a's clip to it: we can't read ids
  // directly through the C surface, but source ids are allocated monotonically;
  // clip b's source is the second registered audio source (id 2).
  const std::string before = serialize(project);
  REQUIRE(sonare_project_set_clip_source(project, a.clip, /*source_id=*/2) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: unknown source / clip, zero ids.
  REQUIRE(sonare_project_set_clip_source(project, a.clip, 999999) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, 999999, 2) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, a.clip, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_clip_source(project, 0, 2) == SONARE_ERROR_INVALID_PARAMETER);
  (void)b;

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI duplicate_clip allocates a new id and undo restores", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 2.0);
  const std::string before = serialize(project);

  uint32_t dup = 0;
  REQUIRE(sonare_project_duplicate_clip(project, fx.clip, 4.0, &dup) == SONARE_OK);
  REQUIRE(dup != 0);
  REQUIRE(dup != fx.clip);
  REQUIRE(serialize(project) != before);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid: bad clip / start, zero id.
  uint32_t dud = 123;
  REQUIRE(sonare_project_duplicate_clip(project, 999999, 4.0, &dud) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(dud == 0);
  REQUIRE(sonare_project_duplicate_clip(project, fx.clip, -1.0, &dud) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_duplicate_clip(project, 0, 4.0, &dud) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI rename_track / remove_track with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_rename_track(project, fx.track, "lead-vox") == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // NULL name clears to empty (still a valid command).
  REQUIRE(sonare_project_rename_track(project, fx.track, nullptr) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // remove_track removes track (and its clips); undo restores everything.
  REQUIRE(sonare_project_remove_track(project, fx.track) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  size_t source_count = 0;
  REQUIRE(sonare_project_source_count(project, &source_count) == SONARE_OK);
  REQUIRE(source_count == 0);
  REQUIRE(project->audio.sources.empty());
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(project->audio.sources.size() == 1);

  // Invalid params.
  REQUIRE(sonare_project_rename_track(project, 0, "x") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_rename_track(project, 999999, "x") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_track(project, 0) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_track(project, 999999) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_track_route with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_track_route(project, fx.track, "strip:vox", "bus:main") == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Empty / NULL args clear the route (still a valid command, no-op here).
  REQUIRE(sonare_project_set_track_route(project, fx.track, "", nullptr) == SONARE_OK);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid track.
  REQUIRE(sonare_project_set_track_route(project, 0, "a", "b") == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_route(project, 999999, "a", "b") ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_track_gain/mute/solo/pan with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_track_gain(project, fx.track, 0.5f) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_set_track_mute(project, fx.track, 1) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_set_track_solo(project, fx.track, 1) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  REQUIRE(sonare_project_set_track_pan(project, fx.track, -0.5f) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // Invalid track ids are rejected for every setter.
  REQUIRE(sonare_project_set_track_gain(project, 0, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_gain(project, 999999, 1.0f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_mute(project, 999999, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_solo(project, 999999, 1) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_pan(project, 999999, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);

  // Non-finite / negative numeric inputs are rejected at the ABI boundary, matching
  // the set_clip_gain contract, and leave the project unmutated.
  REQUIRE(sonare_project_set_track_gain(project, fx.track, -0.1f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_gain(project, fx.track, std::nanf("")) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_pan(project, fx.track, std::nanf("")) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == before);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_track_kind with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  track_desc.name = "utility";
  uint32_t track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track) == SONARE_OK);
  const std::string before = serialize(project);

  REQUIRE(sonare_project_set_track_kind(project, track, SONARE_TRACK_AUX) == SONARE_OK);
  const std::string aux = serialize(project);
  REQUIRE(aux != before);
  REQUIRE(aux.find("\"kind\":2") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == aux);

  REQUIRE(sonare_project_set_track_kind(project, 0, SONARE_TRACK_AUDIO) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_kind(project, 999999, SONARE_TRACK_AUDIO) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_set_track_kind(project, track, 99) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_warp_map/remove_warp_map with clip reference and undo",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 4.0);
  const std::string before = serialize(project);

  SonareProjectWarpAnchor anchors[] = {{0.0, 0.0}, {48000.0, 44100.0}};
  SonareProjectWarpMapDesc map{};
  map.id = 77;
  map.name = "manual warp";
  map.anchors = anchors;
  map.anchor_count = 2;
  REQUIRE(sonare_project_set_warp_map(project, &map) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_warp_ref(project, fx.clip, 77) == SONARE_OK);
  REQUIRE(sonare_project_set_clip_warp_mode(project, fx.clip, SONARE_PROJECT_WARP_MODE_REPITCH) ==
          SONARE_OK);
  const std::string mapped = serialize(project);
  REQUIRE(mapped.find("\"warp_maps\"") != std::string::npos);
  REQUIRE(mapped.find("\"id\":77") != std::string::npos);
  REQUIRE(mapped.find("\"name\":\"manual warp\"") != std::string::npos);
  REQUIRE(mapped.find("\"warp_ref_id\":77") != std::string::npos);
  REQUIRE(mapped.find("\"warp_mode\":1") != std::string::npos);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_mode\":0") != std::string::npos);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_ref_id\":0") != std::string::npos);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(serialize(project) == mapped);

  uint32_t split_id = 0;
  CHECK(sonare_project_split_clip(project, fx.clip, 2.0, &split_id) == SONARE_ERROR_INVALID_STATE);
  CHECK(sonare_project_trim_clip(project, fx.clip, 0.5, 3.0) == SONARE_ERROR_INVALID_STATE);

  REQUIRE(sonare_project_remove_warp_map(project, 77) == SONARE_OK);
  const std::string removed = serialize(project);
  REQUIRE(removed.find("\"name\":\"manual warp\"") == std::string::npos);
  REQUIRE(removed.find("\"warp_ref_id\":0") != std::string::npos);
  SonareProjectBounceOptions options{};
  options.total_frames = 480;
  options.block_size = 64;
  options.num_channels = 2;
  options.sample_rate = 48000;
  float* bounced = nullptr;
  size_t bounced_len = 0;
  REQUIRE(sonare_project_bounce(project, &options, &bounced, &bounced_len) == SONARE_OK);
  REQUIRE(bounced_len == 960);
  sonare_free_floats(bounced);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == mapped);
  REQUIRE(sonare_project_set_clip_warp_mode(project, fx.clip,
                                            SONARE_PROJECT_WARP_MODE_TEMPO_SYNC) == SONARE_OK);
  REQUIRE(serialize(project).find("\"warp_mode\":2") != std::string::npos);
  CHECK(
      sonare_project_set_clip_warp_mode(project, fx.clip, static_cast<SonareProjectWarpMode>(99)) ==
      SONARE_ERROR_INVALID_PARAMETER);

  SonareProjectWarpAnchor non_monotonic[] = {{0.0, 0.0}, {10.0, 10.0}, {5.0, 12.0}};
  SonareProjectWarpMapDesc bad = map;
  bad.id = 78;
  bad.anchors = non_monotonic;
  bad.anchor_count = 3;
  REQUIRE(sonare_project_set_warp_map(project, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  bad.anchors = anchors;
  bad.anchor_count = 1;
  REQUIRE(sonare_project_set_warp_map(project, &bad) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_warp_map(project, 999999) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI automation lane add / edit / remove with undo", "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);
  const std::string empty = serialize(project);

  SonareAutomationPoint points[3];
  points[0].ppq = 0.0;
  points[0].value = 0.0f;
  points[0].curve_to_next = SONARE_CURVE_LINEAR;
  points[1].ppq = 4.0;
  points[1].value = 1.0f;
  points[1].curve_to_next = SONARE_CURVE_SCURVE;
  points[2].ppq = 8.0;
  points[2].value = 0.5f;
  points[2].curve_to_next = SONARE_CURVE_HOLD;

  SonareAutomationLaneDesc desc{};
  desc.target_param_id = 42;
  desc.points = points;
  desc.point_count = 3;

  uint32_t target_param_id = 999;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &desc, &target_param_id) ==
          SONARE_OK);
  REQUIRE(target_param_id == 42);
  const std::string with_lane = serialize(project);
  REQUIRE(with_lane != empty);

  // Target id zero is reserved as the invalid/unset value. Descriptor
  // validation rejects it before either add or edit can mutate the project.
  SonareAutomationLaneDesc zero_target = desc;
  zero_target.target_param_id = 0;
  uint32_t zero_target_out = 999;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &zero_target, &zero_target_out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(zero_target_out == 0);
  REQUIRE(serialize(project) == with_lane);

  // Edit the lane in place without changing its stable target id.
  SonareAutomationPoint edited[2];
  edited[0].ppq = 0.0;
  edited[0].value = 0.25f;
  edited[0].curve_to_next = SONARE_CURVE_EXPONENTIAL;
  edited[1].ppq = 8.0;
  edited[1].value = 0.75f;
  edited[1].curve_to_next = SONARE_CURVE_LINEAR;
  SonareAutomationLaneDesc edit_desc{};
  edit_desc.target_param_id = 42;
  edit_desc.points = edited;
  edit_desc.point_count = 2;
  SonareAutomationLaneDesc zero_target_edit = edit_desc;
  zero_target_edit.target_param_id = 0;
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 42, &zero_target_edit) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == with_lane);
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 42, &edit_desc) == SONARE_OK);
  REQUIRE(serialize(project) != with_lane);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_lane);

  // Remove the lane; undo restores it.
  REQUIRE(sonare_project_remove_automation_lane(project, fx.track, 42) == SONARE_OK);
  REQUIRE(serialize(project) == empty);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == with_lane);

  // Target ids remain valid after a preceding lane is removed; array position
  // must not redirect this edit to a different parameter.
  SonareAutomationLaneDesc second_lane{};
  second_lane.target_param_id = 7;
  uint32_t second_target = 0;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &second_lane, &second_target) ==
          SONARE_OK);
  REQUIRE(second_target == 7);
  REQUIRE(sonare_project_remove_automation_lane(project, fx.track, 42) == SONARE_OK);
  SonareAutomationPoint second_points[1] = {{0.0, 0.75f, SONARE_CURVE_HOLD}};
  SonareAutomationLaneDesc second_edit{};
  second_edit.target_param_id = 7;
  second_edit.points = second_points;
  second_edit.point_count = 1;
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 7, &second_edit) == SONARE_OK);

  // A lane with zero breakpoints is valid (empty desc.points / count 0).
  SonareAutomationLaneDesc empty_lane{};
  empty_lane.target_param_id = 1;
  uint32_t empty_target = 999;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &empty_lane, &empty_target) ==
          SONARE_OK);
  REQUIRE(empty_target == 1);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);

  // Invalid: bad track, missing target id, null desc, bad point fields.
  REQUIRE(sonare_project_add_automation_lane(project, 999999, &desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 99, &desc) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_project_remove_automation_lane(project, fx.track, 99) ==
          SONARE_ERROR_INVALID_PARAMETER);

  SonareAutomationPoint bad[1];
  bad[0].ppq = -1.0;
  bad[0].value = 0.0f;
  bad[0].curve_to_next = SONARE_CURVE_LINEAR;
  SonareAutomationLaneDesc bad_desc{};
  bad_desc.target_param_id = 123;
  bad_desc.points = bad;
  bad_desc.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &bad_desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  bad[0].ppq = 0.0;
  bad[0].curve_to_next = 99;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &bad_desc, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI typed automation lane add/edit preserves legacy compatibility",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  AudioFixture fx = add_audio_track_clip(project, 0.0, 8.0);

  SonareAutomationPoint points[1] = {{0.0, -3.0f, SONARE_CURVE_LINEAR}};
  SonareAutomationLaneDescEx fader{};
  fader.target_param_id = 100;
  fader.target_kind = SONARE_AUTOMATION_TARGET_TRACK_FADER_DB;
  fader.points = points;
  fader.point_count = 1;
  uint32_t out_target = 0;
  REQUIRE(sonare_project_add_automation_lane_ex(project, fx.track, &fader, &out_target) ==
          SONARE_OK);
  REQUIRE(out_target == 100);
  const std::string with_fader = serialize(project);
  REQUIRE(with_fader.find("\"version\":2") != std::string::npos);
  REQUIRE(with_fader.find("\"target_kind\":1") != std::string::npos);

  // The legacy edit descriptor has no kind and must retain the typed target.
  SonareAutomationPoint edited[1] = {{0.0, -6.0f, SONARE_CURVE_HOLD}};
  SonareAutomationLaneDesc legacy_edit{};
  legacy_edit.target_param_id = 100;
  legacy_edit.points = edited;
  legacy_edit.point_count = 1;
  REQUIRE(sonare_project_edit_automation_lane(project, fx.track, 100, &legacy_edit) == SONARE_OK);
  const std::string after_legacy_edit = serialize(project);
  REQUIRE(after_legacy_edit.find("\"target_kind\":1") != std::string::npos);

  // Opaque lanes continue to coexist with typed mixer lanes.
  SonareAutomationLaneDesc opaque{};
  opaque.target_param_id = 200;
  opaque.points = edited;
  opaque.point_count = 1;
  REQUIRE(sonare_project_add_automation_lane(project, fx.track, &opaque, nullptr) == SONARE_OK);

  SonareAutomationLaneDescEx pan = fader;
  pan.target_param_id = 101;
  pan.target_kind = SONARE_AUTOMATION_TARGET_TRACK_PAN;
  REQUIRE(sonare_project_add_automation_lane_ex(project, fx.track, &pan, nullptr) == SONARE_OK);
  const std::string with_fader_and_pan = serialize(project);

  // A second lane of the same typed kind is a command conflict and must not
  // partially mutate the project or history.
  SonareAutomationLaneDescEx duplicate_fader = fader;
  duplicate_fader.target_param_id = 102;
  REQUIRE(sonare_project_add_automation_lane_ex(project, fx.track, &duplicate_fader, nullptr) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == with_fader_and_pan);

  SonareAutomationLaneDescEx unknown = fader;
  unknown.target_param_id = 103;
  unknown.target_kind = static_cast<SonareAutomationTargetKind>(3);
  REQUIRE(sonare_project_add_automation_lane_ex(project, fx.track, &unknown, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(serialize(project) == with_fader_and_pan);

  // Editing the fader into the already occupied pan slot is also atomic.
  SonareAutomationLaneDescEx conflicting_edit = fader;
  conflicting_edit.target_kind = SONARE_AUTOMATION_TARGET_TRACK_PAN;
  REQUIRE(sonare_project_edit_automation_lane_ex(project, fx.track, 100, &conflicting_edit) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == with_fader_and_pan);

  // IDs are track-local: the same typed id is valid on another track.
  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  uint32_t second_track = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &second_track) == SONARE_OK);
  REQUIRE(sonare_project_add_automation_lane_ex(project, second_track, &fader, nullptr) ==
          SONARE_OK);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo second-track lane
  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo second track
  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo pan lane
  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo opaque lane
  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo legacy edit
  REQUIRE(sonare_project_undo(project) == SONARE_OK);  // undo fader add
  REQUIRE(serialize(project).find("\"automation_lanes\":[]") != std::string::npos);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI move_clip rejects a cross-kind move without mutating state",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  // Audio track + audio clip.
  AudioFixture fx = add_audio_track_clip(project, 0.0, 2.0);

  // A MIDI track (kind mismatch target for the audio clip).
  uint32_t midi_track = 0;
  uint32_t midi_clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 4.0, &midi_track, &midi_clip) == SONARE_OK);
  REQUIRE(midi_track != 0);

  const std::string before = serialize(project);

  // Moving the audio clip onto the MIDI track must fail cleanly and leave the
  // project unchanged (so it still compiles).
  REQUIRE(sonare_project_move_clip(project, fx.clip, 0.0, midi_track) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == before);

  // Likewise the MIDI clip cannot move onto the audio track.
  REQUIRE(sonare_project_move_clip(project, midi_clip, 0.0, fx.track) ==
          SONARE_ERROR_INVALID_STATE);
  REQUIRE(serialize(project) == before);

  // A same-track move (new_track_id == 0) still works and round-trips.
  REQUIRE(sonare_project_move_clip(project, fx.clip, 1.0, 0) == SONARE_OK);
  REQUIRE(serialize(project) != before);
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(serialize(project) == before);

  // The project still compiles after the rejected cross-kind moves.
  SonareProjectCompileResult result{};
  REQUIRE(sonare_project_compile(project, &result) == SONARE_OK);
  REQUIRE(result.has_timeline == 1);
  sonare_project_free_compile_result(&result);

  sonare_project_destroy(project);
}

TEST_CASE("C-ABI set_source_audio preserves PCM ownership through undo and redo",
          "[project][c-abi-edit]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);

  SonareProjectTrackDesc track_desc{};
  track_desc.kind = SONARE_TRACK_AUDIO;
  uint32_t track_id = 0;
  REQUIRE(sonare_project_add_track(project, &track_desc, &track_id) == SONARE_OK);

  const std::vector<float> original = {0.25f, -0.5f, 0.75f};
  SonareProjectClipDesc clip_desc{};
  clip_desc.track_id = track_id;
  clip_desc.length_ppq = 1.0;
  clip_desc.gain = 1.0f;
  clip_desc.audio_interleaved = original.data();
  clip_desc.audio_frames = static_cast<int64_t>(original.size());
  clip_desc.audio_channels = 1;
  clip_desc.audio_sample_rate = 48000;
  uint32_t clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &clip_desc, &clip_id) == SONARE_OK);
  const arr::SourceId existing_source = project->audio.sources.begin()->first;
  const float* original_pcm = project->audio.sources.at(existing_source).channels.front().data();

  const std::vector<float> replacement = {-1.0f, 0.5f, 1.0f};
  REQUIRE(sonare_project_set_source_audio(project, existing_source, replacement.data(),
                                          static_cast<int64_t>(replacement.size()), 1,
                                          48000) == SONARE_OK);
  REQUIRE(project->audio.sources.at(existing_source).channels.front() == replacement);
  REQUIRE(project->audio.sources.at(existing_source).channels.front().data() != original_pcm);

  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(project->audio.sources.at(existing_source).channels.front() == original);
  REQUIRE(project->audio.sources.at(existing_source).channels.front().data() == original_pcm);
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(project->audio.sources.at(existing_source).channels.front() == replacement);

  // A source may exist in Project before decoded PCM is supplied.  This path
  // transfers a complete map node on set/restore rather than allocating a new
  // PCM owner during rollback.
  SonareProjectClipDesc unresolved_desc{};
  unresolved_desc.track_id = track_id;
  unresolved_desc.start_ppq = 2.0;
  unresolved_desc.length_ppq = 1.0;
  unresolved_desc.gain = 1.0f;
  uint32_t unresolved_clip_id = 0;
  REQUIRE(sonare_project_add_clip(project, &unresolved_desc, &unresolved_clip_id) == SONARE_OK);
  const auto* unresolved_clip = project->history.project().find_clip(unresolved_clip_id);
  REQUIRE(unresolved_clip != nullptr);
  const arr::SourceId unresolved_source = unresolved_clip->source_id;
  REQUIRE(project->audio.sources.find(unresolved_source) == project->audio.sources.end());

  const std::vector<float> unresolved_pcm = {0.125f, 0.25f};
  REQUIRE(sonare_project_set_source_audio(project, unresolved_source, unresolved_pcm.data(),
                                          static_cast<int64_t>(unresolved_pcm.size()), 1,
                                          48000) == SONARE_OK);
  const float* unresolved_data =
      project->audio.sources.at(unresolved_source).channels.front().data();
  REQUIRE(sonare_project_undo(project) == SONARE_OK);
  REQUIRE(project->audio.sources.find(unresolved_source) == project->audio.sources.end());
  REQUIRE(sonare_project_redo(project) == SONARE_OK);
  REQUIRE(project->audio.sources.at(unresolved_source).channels.front() == unresolved_pcm);
  REQUIRE(project->audio.sources.at(unresolved_source).channels.front().data() == unresolved_data);

  sonare_project_destroy(project);
}
