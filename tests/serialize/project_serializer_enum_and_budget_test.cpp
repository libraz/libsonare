/// @file project_serializer_enum_and_budget_test.cpp
/// @brief Persistence invariants that keep a saved project loadable.
///
/// Two ways a save could previously produce a file nothing can read back:
///   1. A decode bound written as a literal lagging its enum, so a clip carrying
///      the newest enumerator (warp mode "time stretch") made the loader reject
///      the document and discard the WHOLE project.
///   2. An edit API that is not bounded by the import budget, so a large project
///      serialized successfully into a document the loader refuses.
/// Both are checked here against the public surfaces that can produce them.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"
#include "arrangement/edit_source.h"
#include "automation/automation_lane.h"
#include "bindings/binding_project_parity_test_helpers.h"
#include "serialize/project_serializer.h"
#include "serialize/serialized_enum_bounds.h"
#include "util/exception.h"
#include "util/resource_limits.h"

namespace arr = sonare::arrangement;
namespace aut = sonare::automation;
namespace sz = sonare::serialize;

namespace {

struct Fixture {
  arr::Project project;
  arr::MidiContentStore midi;
  arr::TrackId track_id = 0;
  arr::ClipId clip_id = 0;
};

// A project touching every group of encoded enums: one track carrying an
// automation lane, one clip with fades and loop/warp state, a marker, and a
// harmonic annotation.
Fixture make_fixture(arr::SourceKind source_kind = arr::SourceKind::kAudio) {
  Fixture f;
  arr::Project& p = f.project;

  arr::SourceId source_id = 0;
  if (source_kind == arr::SourceKind::kMidi) {
    arr::MidiSourceRef midi_source;
    midi_source.name = "lead";
    midi_source.channel_hint = 1;
    source_id = p.add_midi_source(midi_source);
  } else {
    arr::AudioSourceRef audio_source;
    audio_source.uri = "file:///fixture.wav";
    audio_source.channel_count = 2;
    audio_source.sample_rate_hint = 48000.0;
    source_id = p.add_audio_source(audio_source);
  }
  REQUIRE(source_id != 0);

  arr::Track track;
  track.name = "Track";
  aut::AutomationLane lane(7);
  lane.set_points({{0.0, 0.25f, sonare::AutomationCurve::Linear}});
  track.automation_lanes.push_back(lane);
  f.track_id = p.add_track(track);
  REQUIRE(f.track_id != 0);

  arr::EditClip clip;
  clip.track_id = f.track_id;
  clip.source_id = source_id;
  clip.start_ppq = 0.0;
  clip.length_ppq = 960.0;
  clip.fade_in = {32.0, arr::FadeCurve::kLinear};
  clip.fade_out = {32.0, arr::FadeCurve::kLinear};
  f.clip_id = p.add_clip(clip);
  REQUIRE(f.clip_id != 0);

  REQUIRE(p.add_marker(480.0, "Marker") != 0);

  arr::KeySegment key;
  key.start_ppq = 0.0;
  key.end_ppq = 960.0;
  key.tonic_pc = 0;
  p.annotation().keys.push_back(key);

  arr::ChordSymbol chord;
  chord.start_ppq = 0.0;
  chord.end_ppq = 480.0;
  chord.root_pc = 0;
  p.annotation().chords.push_back(chord);

  return f;
}

// Saves and loads the fixture, requiring that the document the encoder just
// produced is readable and re-encodes to the same bytes.
Fixture round_trip(const Fixture& f) {
  const std::string json = sz::project_to_json(f.project, f.midi);
  sz::DeserializeResult result = sz::project_from_json(json);
  REQUIRE(result.ok());
  CHECK(sz::project_to_json(*result.project, result.midi) == json);

  Fixture loaded;
  loaded.project = std::move(*result.project);
  loaded.midi = std::move(result.midi);
  loaded.track_id = f.track_id;
  loaded.clip_id = f.clip_id;
  return loaded;
}

// Drives every ordinal in [0, last enumerator] of `Enum` through a save/load.
// The bound is serialize::kMaxSerializedOrdinal, which is pinned to the enum's
// own last enumerator, so a newly added enumerator extends this sweep by itself
// (and failing to teach serialized_enum_bounds.h about it is a build error).
template <typename Enum, typename Apply, typename Read>
void check_every_enumerator(Apply apply, Read read) {
  for (uint32_t ordinal = 0; ordinal <= sz::kMaxSerializedOrdinal<Enum>; ++ordinal) {
    INFO("ordinal " << ordinal);
    const Enum value = static_cast<Enum>(ordinal);
    Fixture f = make_fixture();
    apply(&f, value);
    const Fixture loaded = round_trip(f);
    CHECK(read(loaded) == value);
  }
}

}  // namespace

TEST_CASE("every enumerator of every serialized enum survives a project round-trip",
          "[serialize][arrangement]") {
  check_every_enumerator<arr::WarpMode>(
      [](Fixture* f, arr::WarpMode value) {
        f->project.find_clip_mutable(f->clip_id)->warp_mode = value;
      },
      [](const Fixture& f) { return f.project.clips().front().warp_mode; });

  check_every_enumerator<arr::FadeCurve>(
      [](Fixture* f, arr::FadeCurve value) {
        arr::EditClip* clip = f->project.find_clip_mutable(f->clip_id);
        clip->fade_in.curve = value;
        clip->fade_out.curve = value;
      },
      [](const Fixture& f) { return f.project.clips().front().fade_in.curve; });

  check_every_enumerator<arr::LoopMode>(
      [](Fixture* f, arr::LoopMode value) {
        f->project.find_clip_mutable(f->clip_id)->loop_mode = value;
      },
      [](const Fixture& f) { return f.project.clips().front().loop_mode; });

  check_every_enumerator<arr::Track::Kind>(
      [](Fixture* f, arr::Track::Kind value) {
        f->project.find_track_mutable(f->track_id)->kind = value;
      },
      [](const Fixture& f) { return f.project.tracks().front().kind; });

  check_every_enumerator<arr::OverlapPolicy>(
      [](Fixture* f, arr::OverlapPolicy value) { f->project.set_overlap_policy(value); },
      [](const Fixture& f) { return f.project.overlap_policy(); });

  check_every_enumerator<arr::MarkerKind>(
      [](Fixture* f, arr::MarkerKind value) {
        f->project.markers_mutable().front().kind = static_cast<uint8_t>(value);
      },
      [](const Fixture& f) {
        return static_cast<arr::MarkerKind>(f.project.markers().front().kind);
      });

  check_every_enumerator<arr::ChordQuality>(
      [](Fixture* f, arr::ChordQuality value) {
        f->project.annotation().chords.front().quality = value;
      },
      [](const Fixture& f) { return f.project.annotation().chords.front().quality; });

  check_every_enumerator<arr::KeyMode>(
      [](Fixture* f, arr::KeyMode value) { f->project.annotation().keys.front().mode = value; },
      [](const Fixture& f) { return f.project.annotation().keys.front().mode; });

  check_every_enumerator<aut::AutomationTargetKind>(
      [](Fixture* f, aut::AutomationTargetKind value) {
        f->project.find_track_mutable(f->track_id)->automation_lanes.front().set_target_kind(value);
      },
      [](const Fixture& f) {
        return f.project.tracks().front().automation_lanes.front().target_kind();
      });

  check_every_enumerator<sonare::AutomationCurve>(
      [](Fixture* f, sonare::AutomationCurve value) {
        f->project.find_track_mutable(f->track_id)
            ->automation_lanes.front()
            .set_points({{0.0, 0.25f, value}});
      },
      [](const Fixture& f) {
        return f.project.tracks().front().automation_lanes.front().points().front().curve_to_next;
      });

  // The source kind selects the encoded variant rather than a field, so it is
  // swept by rebuilding the fixture with each kind.
  for (uint32_t ordinal = 0; ordinal <= sz::kMaxSerializedOrdinal<arr::SourceKind>; ++ordinal) {
    INFO("source kind ordinal " << ordinal);
    const auto kind = static_cast<arr::SourceKind>(ordinal);
    const Fixture loaded = round_trip(make_fixture(kind));
    REQUIRE(!loaded.project.sources().empty());
    CHECK(arr::source_kind(loaded.project.sources().front()) == kind);
  }
}

TEST_CASE("every warp mode set through the C ABI survives save and load", "[project][serialize]") {
  const SonareProjectWarpMode modes[] = {
      SONARE_PROJECT_WARP_MODE_OFF,
      SONARE_PROJECT_WARP_MODE_REPITCH,
      SONARE_PROJECT_WARP_MODE_TEMPO_SYNC,
      SONARE_PROJECT_WARP_MODE_TIME_STRETCH,
  };
  static_assert(sizeof(modes) / sizeof(modes[0]) == sz::kMaxSerializedOrdinal<arr::WarpMode> + 1u,
                "the C warp-mode enum and the model warp-mode enum cover different ordinals");

  for (const SonareProjectWarpMode mode : modes) {
    INFO("warp mode " << static_cast<int>(mode));
    BuiltProject built = build_project(make_stereo_sine(240));
    REQUIRE(sonare_project_set_clip_warp_mode(built.project, built.audio_clip, mode) == SONARE_OK);
    const std::string json = serialize(built.project);

    // The public load path must accept the document the public save path wrote.
    SonareProject* reloaded = nullptr;
    char* diagnostics = nullptr;
    REQUIRE(sonare_project_deserialize(json.data(), json.size(), &reloaded, &diagnostics) ==
            SONARE_OK);
    REQUIRE(reloaded != nullptr);
    if (diagnostics != nullptr) sonare_free_string(diagnostics);
    CHECK(serialize(reloaded) == json);
    sonare_project_destroy(reloaded);

    // ... and the reloaded clip must carry the mode that was set, not a default.
    const sz::DeserializeResult result = sz::project_from_json(json);
    REQUIRE(result.ok());
    const arr::EditClip* clip = result.project->find_clip(built.audio_clip);
    REQUIRE(clip != nullptr);
    CHECK(static_cast<uint32_t>(clip->warp_mode) == static_cast<uint32_t>(mode));

    sonare_project_destroy(built.project);
  }
}

TEST_CASE("serialization is refused when the encoded document exceeds the persistence budget",
          "[serialize][arrangement]") {
  Fixture f = make_fixture();
  arr::AssistSidecar sidecar;
  sidecar.module_id = "test.budget";
  sidecar.schema_version = 1;
  sidecar.payload = std::vector<uint8_t>(64u, 0x2Au);
  f.project.add_assist_sidecar(sidecar);

  const std::string reference = sz::project_to_json(f.project, f.midi);
  const sz::ProjectDocumentShape shape = sz::measure_project_document(f.project, f.midi);
  CHECK(shape.json_bytes == reference.size());
  REQUIRE(shape.json_nodes > 0u);
  REQUIRE(shape.entities > 0u);
  REQUIRE(shape.string_bytes > 0u);
  REQUIRE(shape.decoded_payload_bytes == 64u);

  // A budget the document exactly fills: still emitted, byte for byte, and still
  // loadable -- which is what the budget stands in for.
  const sonare::resource::ProjectImportResourceLimits exact{shape.json_bytes, shape.json_nodes,
                                                            shape.entities, shape.string_bytes,
                                                            shape.decoded_payload_bytes};
  CHECK(sz::project_to_json(f.project, f.midi, exact) == reference);
  CHECK(sz::project_from_json(reference).ok());

  const auto rejects = [&](const sonare::resource::ProjectImportResourceLimits& limits) {
    bool threw = false;
    try {
      (void)sz::project_to_json(f.project, f.midi, limits);
    } catch (const sonare::SonareException& error) {
      threw = true;
      CHECK(error.code() == sonare::ErrorCode::InvalidState);
    }
    CHECK(threw);
  };
  {
    auto limits = exact;
    limits.max_json_bytes -= 1u;
    rejects(limits);
  }
  {
    auto limits = exact;
    limits.max_json_nodes -= 1u;
    rejects(limits);
  }
  {
    auto limits = exact;
    limits.max_entities -= 1u;
    rejects(limits);
  }
  {
    auto limits = exact;
    limits.max_string_bytes -= 1u;
    rejects(limits);
  }
  {
    auto limits = exact;
    limits.max_decoded_payload_bytes -= 1u;
    rejects(limits);
  }
}

TEST_CASE("the C ABI refuses to serialize a project the loader could not read back",
          "[project][serialize]") {
  BuiltProject built = build_project(make_stereo_sine(240));

  // Below budget: unchanged behaviour, and the bytes load back.
  const std::string json = serialize(built.project);
  SonareProject* reloaded = nullptr;
  REQUIRE(sonare_project_deserialize(json.data(), json.size(), &reloaded, nullptr) == SONARE_OK);
  sonare_project_destroy(reloaded);

  // One byte past the import budget for opaque payloads. The edit call accepts
  // it (its own ceiling is far higher), so the save must be the boundary.
  const std::vector<uint8_t> payload(
      sonare::resource::kDefaultProjectImportResourceLimits.max_decoded_payload_bytes + 1u, 0x5Au);
  REQUIRE(sonare_project_set_assist_sidecar(built.project, "test.oversized", 1, 0, 0.0, 0.0,
                                            payload.data(), payload.size()) == SONARE_OK);

  char* oversized_json = nullptr;
  size_t oversized_len = 0;
  CHECK(sonare_project_serialize(built.project, &oversized_json, &oversized_len) ==
        SONARE_ERROR_INVALID_STATE);
  CHECK(oversized_json == nullptr);
  CHECK(oversized_len == 0u);

  sonare_project_destroy(built.project);
}
