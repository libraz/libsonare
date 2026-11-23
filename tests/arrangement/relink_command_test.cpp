#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
#include "arrangement/relink_source.h"
#include "util/constants.h"

namespace {

namespace arr = sonare::arrangement;

struct Fixture {
  arr::Project project;
  arr::AudioContentStore audio;
  arr::SourceId source_id = 0;
  arr::ClipId clip_id = 0;

  Fixture() {
    arr::AudioSourceRef source;
    source.uri = "old.wav";
    source.channel_count = 2;
    source.sample_rate_hint = 44100.0;
    source.storage_handle_id = 17;
    source.content_hash = "old-content";
    source.external_stem_role = "lead";
    source_id = project.add_audio_source(std::move(source));

    arr::Track track;
    track.name = "audio";
    const arr::TrackId track_id = project.add_track(std::move(track));

    arr::EditClip clip;
    clip.track_id = track_id;
    clip.source_id = source_id;
    clip.length_ppq = 96.0;
    clip_id = project.add_clip(std::move(clip));

    arr::AudioSourceSamples samples;
    samples.channels = {{0.25f, -0.5f}, {0.75f, -1.0f}};
    samples.sample_rate = 44100.0;
    audio.sources.emplace(source_id, std::move(samples));
  }
};

class RejectEdit final : public arr::EditCommand {
 public:
  bool apply(arr::Project& /*project*/, arr::MidiContentStore& /*store*/) override { return false; }

  arr::EditCommandPtr invert(const arr::Project& /*before*/,
                             const arr::MidiContentStore& /*store_before*/) const override {
    return nullptr;
  }

  const char* type_name() const noexcept override { return "RejectEdit"; }
  std::size_t retained_bytes() const noexcept override { return sizeof(*this); }
  bool mutates_midi_store() const noexcept override { return false; }
  arr::ProjectMemberMask affected_members() const noexcept override { return 0; }
};

const arr::AudioSourceRef* audio_source(const arr::Project& project, arr::SourceId id) {
  const arr::ClipSource* source = project.find_source(id);
  return source == nullptr ? nullptr : std::get_if<arr::AudioSourceRef>(source);
}

arr::AudioSourceSamples samples_with_value(float value) {
  arr::AudioSourceSamples samples;
  samples.sample_rate = sonare::constants::kDefaultDawSampleRate;
  samples.channels.assign(2, std::vector<float>(48000, value));
  return samples;
}

}  // namespace

TEST_CASE("relink changes the reference and restores decoded audio", "[arrangement][relink]") {
  Fixture fixture;
  arr::EditHistory history(std::move(fixture.project));
  const arr::AudioSourceSamples original = fixture.audio.sources.at(fixture.source_id);

  auto commands = arr::make_relink_source_commands(history.project(), &fixture.audio,
                                                   fixture.source_id, "new.wav");
  REQUIRE(commands.error == arr::RelinkSourceError::kNone);
  REQUIRE(history.apply_transaction(std::move(commands.commands)));

  const arr::AudioSourceRef* relinked = audio_source(history.project(), fixture.source_id);
  REQUIRE(relinked != nullptr);
  CHECK(relinked->id == fixture.source_id);
  CHECK(relinked->uri == "new.wav");
  CHECK(relinked->content_hash.empty());
  CHECK(relinked->channel_count == 0);
  CHECK(relinked->sample_rate_hint == 0.0);
  CHECK(relinked->storage_handle_id == 17);
  CHECK(relinked->external_stem_role == "lead");
  REQUIRE(history.project().find_clip(fixture.clip_id) != nullptr);
  CHECK(history.project().find_clip(fixture.clip_id)->source_id == fixture.source_id);
  CHECK(fixture.audio.sources.find(fixture.source_id) == fixture.audio.sources.end());

  REQUIRE(history.undo());
  const arr::AudioSourceRef* restored = audio_source(history.project(), fixture.source_id);
  REQUIRE(restored != nullptr);
  CHECK(restored->uri == "old.wav");
  CHECK(restored->content_hash == "old-content");
  CHECK(restored->channel_count == 2);
  CHECK(restored->sample_rate_hint == 44100.0);
  REQUIRE(fixture.audio.sources.find(fixture.source_id) != fixture.audio.sources.end());
  CHECK(fixture.audio.sources.at(fixture.source_id).channels == original.channels);
  CHECK(fixture.audio.sources.at(fixture.source_id).sample_rate == original.sample_rate);

  REQUIRE(history.redo());
  CHECK(audio_source(history.project(), fixture.source_id)->uri == "new.wav");
  CHECK(fixture.audio.sources.find(fixture.source_id) == fixture.audio.sources.end());
}

TEST_CASE("relink invalidates source warps and undo restores the old decoded and warped audio",
          "[arrangement][relink]") {
  constexpr arr::WarpRefId kWarp = 55;
  Fixture fixture;
  arr::EditClip* clip = fixture.project.find_clip_mutable(fixture.clip_id);
  REQUIRE(clip != nullptr);
  clip->warp_ref_id = kWarp;
  clip->warp_mode = arr::WarpMode::kRepitch;
  REQUIRE(fixture.project.set_warp_map({kWarp, "repitch", {{0.0, 0.0}, {48000.0, 24000.0}}}));
  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{fixture.source_id, kWarp},
                                       samples_with_value(0.2f));
  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{fixture.source_id, kWarp + 1},
                                       samples_with_value(0.3f));

  const arr::AudioSourceRef other_ref{};
  const arr::SourceId other_source = fixture.project.add_audio_source(other_ref);
  fixture.audio.sources.emplace(other_source, samples_with_value(0.4f));
  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{other_source, kWarp},
                                       samples_with_value(0.4f));

  arr::EditHistory history(std::move(fixture.project));
  const auto compile = [&]() { return arr::compile(history.project(), {}, fixture.audio); };
  const arr::CompileResult old_audio = compile();
  REQUIRE_FALSE(old_audio.has_errors());
  REQUIRE(old_audio.timeline);
  REQUIRE(old_audio.timeline->audio_clips.size() == 1);
  REQUIRE(old_audio.timeline->audio_clips.front().storage != nullptr);
  CHECK(old_audio.timeline->audio_clips.front().storage->channels[0][0] == Catch::Approx(0.2f));

  auto commands = arr::make_relink_source_commands(history.project(), &fixture.audio,
                                                   fixture.source_id, "new.wav");
  REQUIRE(commands.error == arr::RelinkSourceError::kNone);
  REQUIRE(history.apply_transaction(std::move(commands.commands)));
  CHECK(fixture.audio.sources.find(fixture.source_id) == fixture.audio.sources.end());
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp) == nullptr);
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp + 1) == nullptr);
  REQUIRE(fixture.audio.find_warped(other_source, kWarp) != nullptr);
  CHECK(fixture.audio.find_warped(other_source, kWarp)->channels[0][0] == Catch::Approx(0.4f));

  const arr::CompileResult unresolved = compile();
  CHECK(unresolved.has_errors());

  fixture.audio.sources.emplace(fixture.source_id, samples_with_value(0.6f));
  const arr::CompileResult before_rebake = compile();
  REQUIRE_FALSE(before_rebake.has_errors());
  REQUIRE(before_rebake.timeline);
  REQUIRE(before_rebake.timeline->audio_clips.size() == 1);
  REQUIRE(before_rebake.timeline->audio_clips.front().storage != nullptr);
  CHECK(before_rebake.timeline->audio_clips.front().warp_mode ==
        sonare::engine::WarpMode::kRepitch);
  CHECK(before_rebake.timeline->audio_clips.front().storage->channels[0][0] == Catch::Approx(0.6f));

  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{fixture.source_id, kWarp},
                                       samples_with_value(0.8f));
  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{fixture.source_id, kWarp + 2},
                                       samples_with_value(0.9f));
  const arr::CompileResult new_warp = compile();
  REQUIRE_FALSE(new_warp.has_errors());
  REQUIRE(new_warp.timeline);
  REQUIRE(new_warp.timeline->audio_clips.size() == 1);
  REQUIRE(new_warp.timeline->audio_clips.front().storage != nullptr);
  CHECK(new_warp.timeline->audio_clips.front().storage->channels[0][0] == Catch::Approx(0.8f));
  REQUIRE(fixture.audio.find_warped(other_source, kWarp) != nullptr);
  CHECK(fixture.audio.find_warped(other_source, kWarp)->channels[0][0] == Catch::Approx(0.4f));

  REQUIRE(history.undo());
  REQUIRE(fixture.audio.sources.find(fixture.source_id) != fixture.audio.sources.end());
  CHECK(fixture.audio.sources.at(fixture.source_id).channels[0][0] == Catch::Approx(0.25f));
  REQUIRE(fixture.audio.find_warped(fixture.source_id, kWarp) != nullptr);
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp)->channels[0][0] == Catch::Approx(0.2f));
  REQUIRE(fixture.audio.find_warped(fixture.source_id, kWarp + 1) != nullptr);
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp + 1)->channels[0][0] ==
        Catch::Approx(0.3f));
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp + 2) == nullptr);
  CHECK(fixture.audio.find_warped(other_source, kWarp)->channels[0][0] == Catch::Approx(0.4f));
  const arr::CompileResult restored = compile();
  REQUIRE_FALSE(restored.has_errors());
  REQUIRE(restored.timeline);
  REQUIRE(restored.timeline->audio_clips.size() == 1);
  REQUIRE(restored.timeline->audio_clips.front().storage != nullptr);
  CHECK(restored.timeline->audio_clips.front().storage->channels[0][0] == Catch::Approx(0.2f));

  REQUIRE(history.redo());
  CHECK(fixture.audio.sources.at(fixture.source_id).channels[0][0] == 0.6f);
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp)->channels[0][0] == Catch::Approx(0.8f));
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp + 1) == nullptr);
  REQUIRE(fixture.audio.find_warped(fixture.source_id, kWarp + 2) != nullptr);
  CHECK(fixture.audio.find_warped(fixture.source_id, kWarp + 2)->channels[0][0] ==
        Catch::Approx(0.9f));
}

TEST_CASE("relink to the current reference clears decoded audio", "[arrangement][relink]") {
  Fixture fixture;
  arr::EditHistory history(std::move(fixture.project));

  auto commands = arr::make_relink_source_commands(history.project(), &fixture.audio,
                                                   fixture.source_id, "old.wav");
  REQUIRE(commands.error == arr::RelinkSourceError::kNone);
  REQUIRE(history.apply_transaction(std::move(commands.commands)));

  const arr::AudioSourceRef* source = audio_source(history.project(), fixture.source_id);
  REQUIRE(source != nullptr);
  CHECK(source->uri == "old.wav");
  CHECK(source->content_hash.empty());
  CHECK(source->channel_count == 0);
  CHECK(source->sample_rate_hint == 0.0);
  CHECK(fixture.audio.sources.find(fixture.source_id) == fixture.audio.sources.end());
}

TEST_CASE("relink without decoded audio remains undoable", "[arrangement][relink]") {
  Fixture fixture;
  fixture.audio.sources.clear();
  arr::EditHistory history(std::move(fixture.project));

  auto commands = arr::make_relink_source_commands(history.project(), &fixture.audio,
                                                   fixture.source_id, "new.wav");
  REQUIRE(commands.error == arr::RelinkSourceError::kNone);
  REQUIRE(history.apply_transaction(std::move(commands.commands)));
  REQUIRE(history.undo());
  const arr::AudioSourceRef* restored = audio_source(history.project(), fixture.source_id);
  REQUIRE(restored != nullptr);
  CHECK(restored->uri == "old.wav");
  REQUIRE(history.redo());
  const arr::AudioSourceRef* relinked = audio_source(history.project(), fixture.source_id);
  REQUIRE(relinked != nullptr);
  CHECK(relinked->uri == "new.wav");
  CHECK(fixture.audio.sources.empty());
}

TEST_CASE("relink refuses unknown, MIDI, null, and empty inputs", "[arrangement][relink]") {
  Fixture fixture;
  arr::MidiSourceRef midi;
  midi.name = "notes";
  const arr::SourceId midi_id = fixture.project.add_midi_source(std::move(midi));
  const arr::AudioSourceRef original = *audio_source(fixture.project, fixture.source_id);
  const arr::AudioSourceSamples samples = fixture.audio.sources.at(fixture.source_id);

  const auto unknown = arr::make_relink_source_commands(
      fixture.project, &fixture.audio, static_cast<arr::SourceId>(0xffffffffu), "new.wav");
  CHECK(unknown.error == arr::RelinkSourceError::kUnknownSource);

  const auto midi_result =
      arr::make_relink_source_commands(fixture.project, &fixture.audio, midi_id, "new.wav");
  CHECK(midi_result.error == arr::RelinkSourceError::kNotAudioSource);

  const auto null_uri =
      arr::make_relink_source_commands(fixture.project, &fixture.audio, fixture.source_id, nullptr);
  CHECK(null_uri.error == arr::RelinkSourceError::kNullUri);

  const auto empty_uri =
      arr::make_relink_source_commands(fixture.project, &fixture.audio, fixture.source_id, "");
  CHECK(empty_uri.error == arr::RelinkSourceError::kEmptyUri);

  const arr::AudioSourceRef* unchanged = audio_source(fixture.project, fixture.source_id);
  REQUIRE(unchanged != nullptr);
  CHECK(unchanged->uri == original.uri);
  CHECK(unchanged->content_hash == original.content_hash);
  CHECK(fixture.audio.sources.at(fixture.source_id).channels == samples.channels);
}

TEST_CASE("relink rolls back decoded audio when a later edit fails", "[arrangement][relink]") {
  Fixture fixture;
  const arr::AudioSourceSamples original = fixture.audio.sources.at(fixture.source_id);
  fixture.audio.warped_sources.emplace(arr::WarpedAudioKey{fixture.source_id, 42},
                                       samples_with_value(0.35f));
  arr::EditHistory history(std::move(fixture.project));

  auto commands = arr::make_relink_source_commands(history.project(), &fixture.audio,
                                                   fixture.source_id, "new.wav");
  REQUIRE(commands.error == arr::RelinkSourceError::kNone);
  commands.commands.push_back(std::make_unique<RejectEdit>());
  CHECK_FALSE(history.apply_transaction(std::move(commands.commands)));

  const arr::AudioSourceRef* source = audio_source(history.project(), fixture.source_id);
  REQUIRE(source != nullptr);
  CHECK(source->uri == "old.wav");
  CHECK(source->content_hash == "old-content");
  REQUIRE(fixture.audio.sources.find(fixture.source_id) != fixture.audio.sources.end());
  CHECK(fixture.audio.sources.at(fixture.source_id).channels == original.channels);
  REQUIRE(fixture.audio.find_warped(fixture.source_id, 42) != nullptr);
  CHECK(fixture.audio.find_warped(fixture.source_id, 42)->channels[0][0] == Catch::Approx(0.35f));
  CHECK_FALSE(history.can_undo());
}
