/// @file assist_seam_test.cpp
/// @brief Composition-assist seam tests + a mock module + a static layering
///        guard. Tag: [assist].
///
/// Proves the seam end to end WITHOUT any real theory algorithm:
///   - no module registered -> empty run, project byte-identical after apply;
///   - register/clear -> only registered slots drive;
///   - a deterministic MockNoteGenerator (seed + scope -> EditCommands) round-
///     trips through EditHistory; same seed + same request -> identical project;
///   - a throwing / invalid module -> run discarded, project unchanged, registry
///     still registered;
///   - a tiny iteration budget -> truncated, returns promptly;
///   - partial regen (scope = one track) -> out-of-scope tracks unchanged;
///   - candidate payload -> re-applied as a deterministic history branch;
///   - a multi-track batch run emits a coherent command set;
///   - STATIC GUARD: rt/ and engine/ source files do NOT include "midi/assist/"
///     and compiler/rt/engine do not reference AssistSidecar.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/project_view.h"
#include "automation/automation_lane.h"
#include "c_api/project_internal.h"
#include "midi/assist/assist_budget_detail.h"
#include "midi/assist/assist_registry.h"
#include "midi/assist/composition_assist.h"
#include "midi/part_rig.h"
#include "midi/ump.h"
#include "mixing/api/scene.h"
#include "serialize/project_serializer.h"
#include "support/alloc_guard.h"
#include "transport/tempo_map.h"

namespace {

using sonare::arrangement::AddAutomationLane;
using sonare::arrangement::AddClip;
using sonare::arrangement::AddTrack;
using sonare::arrangement::AssistSidecar;
using sonare::arrangement::AttachAudioSource;
using sonare::arrangement::AttachMidiSource;
using sonare::arrangement::AudioContentStore;
using sonare::arrangement::AudioSourceRef;
using sonare::arrangement::EditClip;
using sonare::arrangement::EditCommandExternalEffects;
using sonare::arrangement::EditCommandPtr;
using sonare::arrangement::EditCommandRollback;
using sonare::arrangement::EditHistory;
using sonare::arrangement::MidiClipEvent;
using sonare::arrangement::MidiClipEventList;
using sonare::arrangement::MidiContentStore;
using sonare::arrangement::MidiSourceRef;
using sonare::arrangement::OverlapPolicy;
using sonare::arrangement::Project;
using sonare::arrangement::ProjectView;
using sonare::arrangement::RemoveClip;
using sonare::arrangement::ReplaceMidiClipEvents;
using sonare::arrangement::ReplaceSource;
using sonare::arrangement::SetAnnotation;
using sonare::arrangement::SetAssistSidecar;
using sonare::arrangement::SetClipCompSegments;
using sonare::arrangement::SetClipLoop;
using sonare::arrangement::SetClipTakes;
using sonare::arrangement::SetClipWarpMode;
using sonare::arrangement::SetMarker;
using sonare::arrangement::SetOverlapPolicy;
using sonare::arrangement::SetPartRig;
using sonare::arrangement::SetSampleRate;
using sonare::arrangement::SetScene;
using sonare::arrangement::SetTempoSegment;
using sonare::arrangement::SetTimeSignatureSegment;
using sonare::arrangement::SetTrackGain;
using sonare::arrangement::SetTrackMidiDestination;
using sonare::arrangement::SetTrackMute;
using sonare::arrangement::SetTrackPan;
using sonare::arrangement::SetTrackRoute;
using sonare::arrangement::SetTrackSolo;
using sonare::arrangement::SetWarpMap;
using sonare::arrangement::Track;
using sonare::arrangement::TrackId;
using sonare::arrangement::WarpMapRef;
using sonare::midi::assist::AssistQueryContext;
using sonare::midi::assist::AssistRegistry;
using sonare::midi::assist::AssistRequest;
using sonare::midi::assist::AssistResult;
using sonare::midi::assist::AssistStatus;
using sonare::midi::assist::CompositionAssist;
using sonare::midi::assist::ICounterpointEngine;
using sonare::midi::assist::IDissonanceAnalyzer;
using sonare::midi::assist::IHarmonyContext;
using sonare::midi::assist::INoteGenerator;
using sonare::midi::assist::INotePlacementJudge;
using sonare::midi::assist::IRhythmGenerator;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr char kMockModuleId[] = "mock-sketch";

sonare::midi::assist::detail::AssistTimePoint g_fake_assist_now{};
uint32_t g_scripted_assist_now_calls = 0;

sonare::midi::assist::detail::AssistTimePoint fake_assist_now() noexcept {
  return g_fake_assist_now;
}

sonare::midi::assist::detail::AssistTimePoint scripted_assist_now() noexcept {
  ++g_scripted_assist_now_calls;
  if (g_scripted_assist_now_calls == 1) return {};
  using Clock = sonare::midi::assist::detail::AssistClock;
  if (g_scripted_assist_now_calls == 2) {
    return sonare::midi::assist::detail::AssistTimePoint(
        std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(9)));
  }
  return sonare::midi::assist::detail::AssistTimePoint(
      std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(10)));
}

// A tiny, deterministic, header-inline PRNG (SplitMix32). No Date/now/std::rand.
struct SeededRng {
  uint32_t state;
  explicit SeededRng(uint32_t seed) : state(seed) {}
  uint32_t next() {
    state += 0x9E3779B9u;
    uint32_t z = state;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
  }
  // Returns a value in [lo, hi].
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + next() % (hi - lo + 1); }
};

// Builds a one-MIDI-track project with a MIDI source + an empty MIDI clip the
// generator can fill. Returns the EditHistory plus the relevant ids.
struct Fixture {
  EditHistory history;
  TrackId track_id = 0;
  sonare::arrangement::ClipId clip_id = 0;
};

Fixture make_fixture() {
  Fixture f;
  // Track.
  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "lead";
    t.kind = Track::Kind::kMidi;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f.history.apply(std::move(add_track)));
  f.track_id = add_track_ptr->allocated_id();

  // MIDI source.
  MidiSourceRef ref;
  ref.name = "lead-src";
  auto attach = std::make_unique<AttachMidiSource>(ref);
  auto* attach_ptr = attach.get();
  REQUIRE(f.history.apply(std::move(attach)));
  const auto source_id = attach_ptr->allocated_id();

  // Clip on the track referencing the source.
  EditClip clip;
  clip.track_id = f.track_id;
  clip.source_id = source_id;
  clip.start_ppq = 0.0;
  clip.length_ppq = 1920.0;  // 4 quarter notes at 480 PPQ.
  auto add_clip = std::make_unique<AddClip>(clip);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f.history.apply(std::move(add_clip)));
  f.clip_id = add_clip_ptr->allocated_id();
  return f;
}

struct ExtraMidiFixture {
  TrackId track_id = 0;
  sonare::arrangement::ClipId clip_id = 0;
};

ExtraMidiFixture add_extra_midi_fixture(Fixture* f) {
  ExtraMidiFixture extra;
  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "outside";
    t.kind = Track::Kind::kMidi;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f->history.apply(std::move(add_track)));
  extra.track_id = add_track_ptr->allocated_id();

  MidiSourceRef ref;
  ref.name = "outside-src";
  auto attach = std::make_unique<AttachMidiSource>(ref);
  auto* attach_ptr = attach.get();
  REQUIRE(f->history.apply(std::move(attach)));

  EditClip clip;
  clip.track_id = extra.track_id;
  clip.source_id = attach_ptr->allocated_id();
  clip.length_ppq = 1920.0;
  auto add_clip = std::make_unique<AddClip>(clip);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f->history.apply(std::move(add_clip)));
  extra.clip_id = add_clip_ptr->allocated_id();
  return extra;
}

struct ExtraAudioFixture {
  TrackId track_id = 0;
  sonare::arrangement::ClipId clip_id = 0;
  sonare::arrangement::SourceId source_id = 0;
  sonare::arrangement::SourceId take_source_id = 0;
};

ExtraAudioFixture add_extra_audio_fixture(Fixture* f) {
  ExtraAudioFixture extra;
  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "audio-outside";
    t.kind = Track::Kind::kAudio;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f->history.apply(std::move(add_track)));
  extra.track_id = add_track_ptr->allocated_id();

  AudioSourceRef first;
  first.uri = "file:///outside-audio.wav";
  first.channel_count = 2;
  first.sample_rate_hint = 48000.0;
  first.storage_handle_id = 11;
  first.content_hash = "outside-hash";
  first.external_stem_role = "dialogue";
  auto attach_first = std::make_unique<AttachAudioSource>(first);
  auto* attach_first_ptr = attach_first.get();
  REQUIRE(f->history.apply(std::move(attach_first)));
  extra.source_id = attach_first_ptr->allocated_id();

  AudioSourceRef second = first;
  second.uri = "file:///outside-take.wav";
  second.storage_handle_id = 12;
  second.content_hash = "outside-take-hash";
  auto attach_second = std::make_unique<AttachAudioSource>(second);
  auto* attach_second_ptr = attach_second.get();
  REQUIRE(f->history.apply(std::move(attach_second)));
  extra.take_source_id = attach_second_ptr->allocated_id();

  EditClip clip;
  clip.track_id = extra.track_id;
  clip.source_id = extra.source_id;
  clip.length_ppq = 1920.0;
  clip.takes = {{1, extra.take_source_id, 240.0, "take-1", 0.5, false}};
  clip.active_take_id = 1;
  auto add_clip = std::make_unique<AddClip>(clip);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f->history.apply(std::move(add_clip)));
  extra.clip_id = add_clip_ptr->allocated_id();
  return extra;
}

std::string serialize(const EditHistory& h) {
  return sonare::serialize::project_to_json(h.project(), h.midi_content());
}

// Applies a result's commands via a single undoable transaction. An empty
// command set is a trivial no-op success: the project is left untouched (the
// empty-run guarantee), mirroring how a caller would skip an empty apply.
bool apply_result(EditHistory* h, AssistResult&& result) {
  if (result.commands.empty()) return true;
  return h->apply_transaction(std::move(result.commands));
}

// ---------------------------------------------------------------------------
// Mock module: deterministic note generator.
// ---------------------------------------------------------------------------

// Given (seed, scope) it emits, for each in-scope MIDI track, a deterministic
// set of EditCommands: a ReplaceMidiClipEvents that fills the first clip on the
// track with seeded notes, plus a SetAssistSidecar persisting its seed/state so
// the run is resumable and round-trips through the serializer. It NEVER mutates
// the project: it reads the ProjectView and RETURNS commands.
class MockNoteGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return kMockModuleId; }

  AssistResult generate(const ProjectView& view, const AssistRequest& request) override {
    AssistResult result;
    SeededRng rng(request.seed);
    uint32_t notes_emitted = 0;

    for (const auto& clip : view.clips()) {
      if (!request.scope.covers_track(clip.track_id)) continue;
      const Track* track = view.find_track(clip.track_id);
      if (track == nullptr || track->kind != Track::Kind::kMidi) continue;

      MidiClipEventList events;
      // Four deterministic notes per clip; positions on a fixed grid, pitches
      // from the seeded RNG so a different seed gives a different (but
      // reproducible) line.
      for (int i = 0; i < 4; ++i) {
        // Cooperative budget: stop early if the iteration cap is reached.
        if (request.budget.has_iteration_cap() && notes_emitted >= request.budget.max_iterations) {
          break;
        }
        const double on_ppq = i * 480.0;
        const double off_ppq = on_ppq + 240.0;
        const uint8_t note = static_cast<uint8_t>(rng.range(60, 72));
        const uint8_t vel = static_cast<uint8_t>(rng.range(80, 110));
        const auto on = sonare::midi::make_midi1_note_on(0, 0, note, vel);
        const auto off = sonare::midi::make_midi1_note_off(0, 0, note, 0);
        events.push_back(MidiClipEvent{on_ppq, on.words[0], on.words[1]});
        events.push_back(MidiClipEvent{off_ppq, off.words[0], off.words[1]});
        ++notes_emitted;
      }

      result.commands.push_back(std::make_unique<ReplaceMidiClipEvents>(clip.id, events));
    }

    // Persist module state as a sidecar (opaque payload = seed bytes).
    AssistSidecar sidecar;
    sidecar.module_id = kMockModuleId;
    sidecar.schema_version = 1;
    sidecar.payload = {static_cast<uint8_t>(request.seed & 0xFF),
                       static_cast<uint8_t>((request.seed >> 8) & 0xFF),
                       static_cast<uint8_t>((request.seed >> 16) & 0xFF),
                       static_cast<uint8_t>((request.seed >> 24) & 0xFF)};
    result.commands.push_back(std::make_unique<SetAssistSidecar>(sidecar));

    // Candidate payload: seed + note count, opaque to the core.
    result.candidate_payload =
        "seed=" + std::to_string(request.seed) + ";notes=" + std::to_string(notes_emitted);
    result.diagnostics.iterations_consumed = notes_emitted;
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};

// A module that always throws (exercises the discard-on-exception contract).
class ThrowingGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return "throwing"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    throw std::runtime_error("boom");
  }
};

// A module that returns an INVALID command (targets a non-existent clip), so the
// dry-apply validation rejects it and the call is discarded.
class InvalidPatchGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return "invalid"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    AssistResult result;
    // Clip id 999999 does not exist -> ReplaceMidiClipEvents::apply fails.
    MidiClipEventList ev{MidiClipEvent{0.0, 0x20903C64u, 0u}};
    result.commands.push_back(std::make_unique<ReplaceMidiClipEvents>(999999u, ev));
    return result;
  }
};

class ScratchClipGenerator final : public INoteGenerator {
 public:
  static constexpr TrackId kTrackId = 9001;
  static constexpr sonare::arrangement::SourceId kSourceId = 9002;
  static constexpr sonare::arrangement::ClipId kClipId = 9003;

  const char* module_id() const noexcept override { return "scratch-gen"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    AssistResult result;

    Track track;
    track.name = "scratch";
    track.kind = Track::Kind::kMidi;
    auto add_track = std::make_unique<AddTrack>(track);
    add_track->reseed_id(kTrackId);
    result.commands.push_back(std::move(add_track));

    MidiSourceRef ref;
    ref.name = "scratch-src";
    auto attach = std::make_unique<AttachMidiSource>(ref);
    attach->reseed_id(kSourceId);
    result.commands.push_back(std::move(attach));

    EditClip clip;
    clip.track_id = kTrackId;
    clip.source_id = kSourceId;
    clip.length_ppq = 480.0;
    auto add_clip = std::make_unique<AddClip>(clip);
    add_clip->reseed_id(kClipId);
    result.commands.push_back(std::move(add_clip));

    result.candidate_payload = "scratch-generator";
    result.diagnostics.iterations_consumed = 1;
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};

class QueryAwareCounterpoint final : public ICounterpointEngine {
 public:
  bool saw_queries = false;

  const char* module_id() const noexcept override { return "query-cp"; }
  AssistResult derive(const ProjectView&, const AssistRequest&,
                      const std::vector<sonare::midi::assist::VoiceModel>&,
                      const AssistQueryContext& queries) override {
    saw_queries =
        queries.harmony != nullptr && queries.dissonance != nullptr && queries.judge != nullptr;

    MidiClipEventList events;
    const auto on = sonare::midi::make_midi1_note_on(0, 0, 64, 96);
    const auto off = sonare::midi::make_midi1_note_off(0, 0, 64, 0);
    events.push_back(MidiClipEvent{0.0, on.words[0], on.words[1]});
    events.push_back(MidiClipEvent{240.0, off.words[0], off.words[1]});

    AssistResult result;
    result.commands.push_back(
        std::make_unique<ReplaceMidiClipEvents>(ScratchClipGenerator::kClipId, events));
    result.candidate_payload = "counterpoint";
    result.diagnostics.iterations_consumed = 1;
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};

// Reports a fixed iteration count larger than any test's cap, ignoring the
// cooperative budget, so a single dispatched slot can exhaust the driver's
// iteration budget on its own.
class HeavyGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return "heavy"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    AssistResult result;
    result.candidate_payload = "heavy";
    result.diagnostics.iterations_consumed = 5;
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};

struct BudgetObservation {
  std::vector<uint32_t> max_iterations;
  std::vector<uint32_t> max_time_ms;

  void record(const AssistRequest& request) {
    max_iterations.push_back(request.budget.max_iterations);
    max_time_ms.push_back(request.budget.max_time_ms);
  }
};

using BudgetResultFactory = std::function<AssistResult(const AssistRequest&)>;

class BudgetGenerator final : public INoteGenerator {
 public:
  BudgetGenerator(const char* id, BudgetObservation* observation, BudgetResultFactory factory)
      : id_(id), observation_(observation), factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return id_; }

  AssistResult generate(const ProjectView&, const AssistRequest& request) override {
    if (observation_ != nullptr) observation_->record(request);
    return factory_(request);
  }

 private:
  const char* id_ = nullptr;
  BudgetObservation* observation_ = nullptr;
  BudgetResultFactory factory_;
};

class BudgetCounterpoint final : public ICounterpointEngine {
 public:
  BudgetCounterpoint(const char* id, BudgetObservation* observation, BudgetResultFactory factory)
      : id_(id), observation_(observation), factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return id_; }

  AssistResult derive(const ProjectView&, const AssistRequest& request,
                      const std::vector<sonare::midi::assist::VoiceModel>&,
                      const AssistQueryContext&) override {
    if (observation_ != nullptr) observation_->record(request);
    return factory_(request);
  }

 private:
  const char* id_ = nullptr;
  BudgetObservation* observation_ = nullptr;
  BudgetResultFactory factory_;
};

class BudgetRhythm final : public IRhythmGenerator {
 public:
  BudgetRhythm(const char* id, BudgetObservation* observation, BudgetResultFactory factory)
      : id_(id), observation_(observation), factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return id_; }

  AssistResult generate(const ProjectView&, const AssistRequest& request) override {
    if (observation_ != nullptr) observation_->record(request);
    return factory_(request);
  }

 private:
  const char* id_ = nullptr;
  BudgetObservation* observation_ = nullptr;
  BudgetResultFactory factory_;
};

AssistResult budget_result(uint32_t iterations, const char* payload,
                           AssistStatus status = AssistStatus::kOk) {
  AssistResult result;
  result.diagnostics.iterations_consumed = iterations;
  result.diagnostics.status = status;
  result.candidate_payload = payload;
  return result;
}

class NestedRunGenerator final : public INoteGenerator {
 public:
  NestedRunGenerator(AssistRegistry* nested_registry, bool* restored, bool* parent_active)
      : nested_registry_(nested_registry), restored_(restored), parent_active_(parent_active) {}

  const char* module_id() const noexcept override { return "nested-run"; }

  AssistResult generate(const ProjectView& view, const AssistRequest&) override {
    const auto* before = sonare::midi::assist::detail::current_deadline();
    AssistRequest nested_request;
    (void)CompositionAssist(*nested_registry_).run(view, nested_request);
    const auto* after = sonare::midi::assist::detail::current_deadline();
    *restored_ = before == after;
    *parent_active_ = after != nullptr && after->active();
    return budget_result(0, "outer");
  }

 private:
  AssistRegistry* nested_registry_ = nullptr;
  bool* restored_ = nullptr;
  bool* parent_active_ = nullptr;
};

class DummyHarmonyContext final : public IHarmonyContext {
 public:
  sonare::arrangement::ChordSymbol chord_at(const ProjectView&, double) const override {
    return {};
  }
  sonare::arrangement::KeySegment key_at(const ProjectView&, double) const override { return {}; }
  std::vector<uint8_t> scale_pitch_classes(const ProjectView&, double) const override {
    return {0, 2, 4, 5, 7, 9, 11};
  }
};

class DummyDissonanceAnalyzer final : public IDissonanceAnalyzer {
 public:
  float score_note(const ProjectView&, const sonare::midi::assist::CandidateNote&) const override {
    return 0.0f;
  }
  float score_vertical(const ProjectView&, double,
                       const std::vector<sonare::midi::assist::CandidateNote>&) const override {
    return 0.0f;
  }
};

class DummyPlacementJudge final : public INotePlacementJudge {
 public:
  sonare::midi::assist::PlacementVerdict judge(
      const ProjectView&, const sonare::midi::assist::VoiceModel&,
      const sonare::midi::assist::CandidateNote& candidate) const override {
    sonare::midi::assist::PlacementVerdict verdict;
    verdict.accepted = true;
    verdict.adjusted = candidate;
    return verdict;
  }
};

class SlowGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return "slow"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    AssistResult result;
    result.candidate_payload = "slow";
    result.diagnostics.iterations_consumed = 1;
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};

class ScopeBreakingGenerator final : public INoteGenerator {
 public:
  explicit ScopeBreakingGenerator(sonare::arrangement::ClipId clip_id) : clip_id_(clip_id) {}
  const char* module_id() const noexcept override { return "scope-breaking"; }
  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    MidiClipEventList events;
    const auto on = sonare::midi::make_midi1_note_on(0, 0, 67, 96);
    events.push_back(MidiClipEvent{0.0, on.words[0], on.words[1]});
    AssistResult result;
    result.commands.push_back(std::make_unique<ReplaceMidiClipEvents>(clip_id_, events));
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }

 private:
  sonare::arrangement::ClipId clip_id_ = 0;
};

class OneCommandGenerator final : public INoteGenerator {
 public:
  explicit OneCommandGenerator(std::function<EditCommandPtr()> factory)
      : factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return "one-command"; }

  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    AssistResult result;
    result.commands.push_back(factory_());
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }

 private:
  std::function<EditCommandPtr()> factory_;
};

class SequenceGenerator final : public INoteGenerator {
 public:
  explicit SequenceGenerator(std::function<std::vector<EditCommandPtr>()> factory)
      : factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return "sequence"; }

  AssistResult generate(const ProjectView&, const AssistRequest&) override {
    AssistResult result;
    result.commands = factory_();
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }

 private:
  std::function<std::vector<EditCommandPtr>()> factory_;
};

class OneCommandCounterpoint final : public ICounterpointEngine {
 public:
  explicit OneCommandCounterpoint(std::function<EditCommandPtr()> factory)
      : factory_(std::move(factory)) {}

  const char* module_id() const noexcept override { return "one-command-counterpoint"; }

  AssistResult derive(const ProjectView&, const AssistRequest&,
                      const std::vector<sonare::midi::assist::VoiceModel>&,
                      const AssistQueryContext&) override {
    AssistResult result;
    result.commands.push_back(factory_());
    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }

 private:
  std::function<EditCommandPtr()> factory_;
};

enum class ProbeBehavior { kSuccess, kFalse, kThrow };

struct ProbeLog {
  std::array<int, 32> apply_ids{};
  std::array<int, 32> rollback_ids{};
  std::size_t apply_count = 0;
  std::size_t rollback_count = 0;
  int external = 0;
  bool observed_prior_state = false;

  void applied(int id) noexcept {
    if (apply_count < apply_ids.size()) apply_ids[apply_count++] = id;
  }
  void rolled_back(int id) noexcept {
    if (rollback_count < rollback_ids.size()) rollback_ids[rollback_count++] = id;
  }
};

class SnapshotRestoreCommand final : public sonare::arrangement::EditCommand {
 public:
  SnapshotRestoreCommand(Project project, MidiContentStore store, std::shared_ptr<ProbeLog> log,
                         int external_delta)
      : project_(std::move(project)),
        store_(std::move(store)),
        log_(std::move(log)),
        external_delta_(external_delta) {}

  bool apply(Project& project, MidiContentStore& store) override {
    std::swap(project_, project);
    std::swap(store_, store);
    if (log_ != nullptr) log_->external += external_delta_;
    return true;
  }

  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& store_before) const override {
    return std::make_unique<SnapshotRestoreCommand>(before, store_before, log_, -external_delta_);
  }

  const char* type_name() const noexcept override { return "AssistSnapshotRestore"; }
  std::size_t retained_bytes() const noexcept override { return sizeof(*this); }

 private:
  Project project_;
  MidiContentStore store_;
  std::shared_ptr<ProbeLog> log_;
  int external_delta_ = 0;
};

class ProbeCore {
 public:
  using Mutator = std::function<void(Project&, MidiContentStore&)>;

  ProbeCore(int id, std::shared_ptr<ProbeLog> log, Mutator mutator, ProbeBehavior behavior,
            sonare::arrangement::SourceId audio_source = 0,
            sonare::arrangement::WarpRefId warp_source = 0, bool dynamic_audio = false,
            std::size_t fail_allocation_nth = 0)
      : id_(id),
        log_(std::move(log)),
        mutator_(std::move(mutator)),
        behavior_(behavior),
        audio_source_(audio_source),
        warp_source_(warp_source),
        dynamic_audio_(dynamic_audio),
        fail_allocation_nth_(fail_allocation_nth) {}

  bool apply(Project& project, MidiContentStore& store) {
    before_project_ = project;
    before_store_ = store;
    has_snapshot_ = true;
    log_->applied(id_);
    ++log_->external;
    if (mutator_) mutator_(project, store);
    if (behavior_ == ProbeBehavior::kFalse) {
      rollback(project, store);
      return false;
    }
    if (behavior_ == ProbeBehavior::kThrow) {
      throw std::runtime_error("assist probe apply failure");
    }
    if (fail_allocation_nth_ != 0) {
      sonare::test::g_allocation_attempts.store(0, std::memory_order_relaxed);
      sonare::test::g_fail_allocation_number.store(fail_allocation_nth_, std::memory_order_relaxed);
    }
    return true;
  }

  void rollback(Project& project, MidiContentStore& store) noexcept {
    if (!has_snapshot_) return;
    std::swap(before_project_, project);
    std::swap(before_store_, store);
    has_snapshot_ = false;
    log_->rolled_back(id_);
    --log_->external;
  }

  bool affects_audio_source(sonare::arrangement::SourceId source_id) const noexcept {
    return dynamic_audio_ || (audio_source_ != 0 && audio_source_ == source_id);
  }

  bool affects_warp_source(sonare::arrangement::WarpRefId warp_source) const noexcept {
    return warp_source_ != 0 && warp_source_ == warp_source;
  }

  EditCommandPtr inverse(const Project& before, const MidiContentStore& store_before) const {
    // The probe's external counter is part of the side effect being tested.
    // Undo must restore it together with the value-owned snapshots; a bare
    // snapshot swap would make replay/undo appear correct while leaking the
    // external state forward.
    return std::make_unique<SnapshotRestoreCommand>(before, store_before, log_, -1);
  }

  std::size_t retained_bytes() const noexcept { return sizeof(*this); }

 private:
  int id_ = 0;
  std::shared_ptr<ProbeLog> log_;
  Mutator mutator_;
  ProbeBehavior behavior_ = ProbeBehavior::kSuccess;
  sonare::arrangement::SourceId audio_source_ = 0;
  sonare::arrangement::WarpRefId warp_source_ = 0;
  bool dynamic_audio_ = false;
  std::size_t fail_allocation_nth_ = 0;
  Project before_project_;
  MidiContentStore before_store_;
  bool has_snapshot_ = false;
};

class ProbeCommand final : public sonare::arrangement::EditCommand,
                           public EditCommandExternalEffects {
 public:
  ProbeCommand(int id, std::shared_ptr<ProbeLog> log, ProbeCore::Mutator mutator,
               ProbeBehavior behavior = ProbeBehavior::kSuccess,
               sonare::arrangement::SourceId audio_source = 0,
               sonare::arrangement::WarpRefId warp_source = 0, bool dynamic_audio = false,
               std::size_t fail_allocation_nth = 0)
      : core_(id, std::move(log), std::move(mutator), behavior, audio_source, warp_source,
              dynamic_audio, fail_allocation_nth) {}

  bool apply(Project& project, MidiContentStore& store) override {
    return core_.apply(project, store);
  }
  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& store_before) const override {
    return core_.inverse(before, store_before);
  }
  const char* type_name() const noexcept override { return "AssistProbe"; }
  std::size_t retained_bytes() const noexcept override { return core_.retained_bytes(); }
  void rollback_apply(Project& project, MidiContentStore& store) noexcept override {
    core_.rollback(project, store);
  }
  bool affects_audio_source(sonare::arrangement::SourceId source_id) const noexcept override {
    return core_.affects_audio_source(source_id);
  }
  bool affects_warp_source(sonare::arrangement::WarpRefId warp_source) const noexcept override {
    return core_.affects_warp_source(warp_source);
  }

 private:
  ProbeCore core_;
};

class UnknownRollbackProbe final : public sonare::arrangement::EditCommand,
                                   public EditCommandRollback {
 public:
  UnknownRollbackProbe(int id, std::shared_ptr<ProbeLog> log, ProbeCore::Mutator mutator,
                       ProbeBehavior behavior = ProbeBehavior::kSuccess)
      : core_(id, std::move(log), std::move(mutator), behavior) {}

  bool apply(Project& project, MidiContentStore& store) override {
    return core_.apply(project, store);
  }
  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& store_before) const override {
    return core_.inverse(before, store_before);
  }
  const char* type_name() const noexcept override { return "AssistUnknownRollbackProbe"; }
  std::size_t retained_bytes() const noexcept override { return core_.retained_bytes(); }
  void rollback_apply(Project& project, MidiContentStore& store) noexcept override {
    core_.rollback(project, store);
  }

 private:
  ProbeCore core_;
};

}  // namespace

// ===========================================================================
// No module registered -> empty run, project byte-identical after apply.
// ===========================================================================

TEST_CASE("assist run with no modules is empty and leaves the project untouched", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);

  AssistRegistry registry;
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);

  AssistRequest req;
  req.seed = 1234;
  AssistResult result = assist.run(view, req);

  REQUIRE(result.commands.empty());
  REQUIRE(result.diagnostics.status == AssistStatus::kEmpty);

  // Applying an empty transaction is a no-op; the project is byte-identical.
  REQUIRE(apply_result(&f.history, std::move(result)));
  REQUIRE(serialize(f.history) == before);
}

// ===========================================================================
// register / clear -> only registered slots drive.
// ===========================================================================

TEST_CASE("assist registry register and clear gate the slots", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);

  MockNoteGenerator gen;
  AssistRegistry registry;
  ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
  AssistRequest req;
  req.seed = 7;

  // Cleared registry -> empty.
  {
    CompositionAssist assist(registry);
    AssistResult r = assist.run(view, req);
    REQUIRE(r.commands.empty());
  }

  // Registered generator -> non-empty.
  registry.register_generator(&gen);
  {
    CompositionAssist assist(registry);
    AssistResult r = assist.run(view, req);
    REQUIRE_FALSE(r.commands.empty());
    REQUIRE(r.diagnostics.status == AssistStatus::kOk);
  }

  // Clearing the registry drops the slot again.
  registry.clear();
  REQUIRE(registry.generator() == nullptr);
  {
    CompositionAssist assist(registry);
    AssistResult r = assist.run(view, req);
    REQUIRE(r.commands.empty());
  }

  // The driver never mutated the project.
  REQUIRE(serialize(f.history) == before);
}

// ===========================================================================
// Mock module: deterministic round-trip through EditHistory.
// ===========================================================================

TEST_CASE("assist mock generator is deterministic for a fixed seed", "[assist]") {
  MockNoteGenerator gen;

  auto run_into_fresh = [&](uint32_t seed) {
    Fixture f = make_fixture();
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
    AssistRequest req;
    req.seed = seed;
    AssistResult r = assist.run(view, req);
    REQUIRE(apply_result(&f.history, std::move(r)));
    return serialize(f.history);
  };

  const std::string a = run_into_fresh(2024);
  const std::string b = run_into_fresh(2024);
  const std::string c = run_into_fresh(99);

  // Same seed + same request -> identical serialized project.
  REQUIRE(a == b);
  // A different seed produces a different line.
  REQUIRE(a != c);
}

TEST_CASE("assist mock candidate payload re-applies as a deterministic branch", "[assist]") {
  MockNoteGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);

  Fixture f = make_fixture();
  CompositionAssist assist(registry);
  AssistRequest req;
  req.seed = 555;

  // First run: capture the candidate payload.
  std::string candidate;
  {
    ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
    AssistResult r = assist.run(view, req);
    candidate = r.candidate_payload;
    REQUIRE_FALSE(candidate.empty());
    REQUIRE(apply_result(&f.history, std::move(r)));
  }
  const std::string applied = serialize(f.history);

  // The candidate payload encodes the seed; re-deriving from it on a fresh
  // project reproduces the same render (a history branch the host can keep).
  Fixture f2 = make_fixture();
  {
    ProjectView view(f2.history.project(), f2.history.midi_content(), kMockModuleId);
    AssistResult r = assist.run(view, req);
    REQUIRE(r.candidate_payload == candidate);
    REQUIRE(apply_result(&f2.history, std::move(r)));
  }
  REQUIRE(serialize(f2.history) == applied);
}

TEST_CASE("assist passes query slots and validates multi-slot commands cumulatively", "[assist]") {
  Fixture f = make_fixture();

  ScratchClipGenerator gen;
  QueryAwareCounterpoint counterpoint;
  DummyHarmonyContext harmony;
  DummyDissonanceAnalyzer dissonance;
  DummyPlacementJudge judge;

  AssistRegistry registry;
  registry.register_generator(&gen);
  registry.register_counterpoint(&counterpoint);
  registry.register_harmony_context(&harmony);
  registry.register_dissonance_analyzer(&dissonance);
  registry.register_judge(&judge);

  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "scratch-gen");
  AssistRequest req;
  AssistResult result = assist.run(view, req);

  REQUIRE(result.diagnostics.status == AssistStatus::kOk);
  REQUIRE(counterpoint.saw_queries);
  REQUIRE(result.candidate_payloads.size() == 2);
  REQUIRE(result.candidate_payloads[0] == "scratch-generator");
  REQUIRE(result.candidate_payloads[1] == "counterpoint");
  REQUIRE(result.candidate_payload.find("scratch-generator") != std::string::npos);
  REQUIRE(result.candidate_payload.find("counterpoint") != std::string::npos);
  REQUIRE(apply_result(&f.history, std::move(result)));

  const auto events_it = f.history.midi_content().events.find(ScratchClipGenerator::kClipId);
  REQUIRE(events_it != f.history.midi_content().events.end());
  REQUIRE(events_it->second.size() == 2);
}

// ===========================================================================
// Error contract: throwing / invalid module -> discarded, state unchanged,
// registry preserved.
// ===========================================================================

TEST_CASE("assist discards a throwing module and preserves state + registry", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);

  ThrowingGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
  AssistRequest req;
  req.seed = 1;

  AssistResult r = assist.run(view, req);  // Must NOT throw.
  REQUIRE(r.commands.empty());
  REQUIRE(r.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(r.diagnostics.slots_discarded == 1);

  // Project unchanged; registry still holds the module.
  REQUIRE(serialize(f.history) == before);
  REQUIRE(registry.generator() == &gen);
}

TEST_CASE("assist discards an invalid patch and preserves state", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);

  InvalidPatchGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
  AssistRequest req;

  AssistResult r = assist.run(view, req);
  REQUIRE(r.commands.empty());
  REQUIRE(r.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(r.diagnostics.slots_discarded == 1);
  REQUIRE(serialize(f.history) == before);
  REQUIRE(registry.generator() == &gen);
}

// ===========================================================================
// Budget overrun -> truncated, returns promptly.
// ===========================================================================

TEST_CASE("assist budget truncates and returns promptly", "[assist]") {
  Fixture f = make_fixture();

  MockNoteGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);
  registry.register_rhythm(nullptr);  // explicit: unregistered = no-op
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);

  AssistRequest req;
  req.seed = 3;
  req.budget.max_iterations = 0;  // 0 = no cap; ensure the no-cap path is fine.
  AssistResult full = assist.run(view, req);
  REQUIRE_FALSE(full.commands.empty());

  // A tiny per-module iteration cap truncates the generated notes cooperatively;
  // the run still returns (no hang).
  AssistRequest tiny = req;
  tiny.budget.max_iterations = 1;
  AssistResult truncated = assist.run(view, tiny);
  // The mock self-truncates to <= 1 note per clip; fewer events than the full run.
  REQUIRE(truncated.diagnostics.iterations_consumed <= 1);
}

TEST_CASE("assist time budget stops dispatching later slots", "[assist]") {
  Fixture f = make_fixture();

  SlowGenerator gen;
  QueryAwareCounterpoint counterpoint;
  AssistRegistry registry;
  registry.register_generator(&gen);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "slow");

  AssistRequest req;
  req.budget.max_time_ms = 1;
  AssistResult result = assist.run(view, req);

  REQUIRE(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(result.diagnostics.reason == "time budget exhausted");
  REQUIRE(result.candidate_payloads.size() == 1);
  REQUIRE(result.candidate_payloads[0] == "slow");
  REQUIRE_FALSE(counterpoint.saw_queries);
}

TEST_CASE("assist iteration budget gates later slots on consumed iterations, not slot count",
          "[assist]") {
  Fixture f = make_fixture();

  HeavyGenerator gen;
  QueryAwareCounterpoint counterpoint;
  AssistRegistry registry;
  registry.register_generator(&gen);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "heavy");

  AssistRequest req;
  // The generator alone consumes 5 iterations in its single slot, over this cap.
  // A driver that gated on the slot count (dispatched == 1 < 3) would still
  // dispatch the counterpoint slot; gating on consumed iterations must not.
  req.budget.max_iterations = 3;
  AssistResult result = assist.run(view, req);

  REQUIRE(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(result.diagnostics.reason == "iteration budget exhausted");
  REQUIRE(result.candidate_payloads.empty());
  REQUIRE_FALSE(counterpoint.saw_queries);
}

TEST_CASE("assist run-wide iteration budget propagates remaining work", "[assist]") {
  Fixture f = make_fixture();
  BudgetObservation generator_observation;
  BudgetObservation counterpoint_observation;
  BudgetGenerator generator("budget-generator", &generator_observation,
                            [](const AssistRequest&) { return budget_result(90, "generator"); });
  BudgetCounterpoint counterpoint(
      "budget-counterpoint", &counterpoint_observation,
      [](const AssistRequest&) { return budget_result(10, "counterpoint"); });
  AssistRegistry registry;
  registry.register_generator(&generator);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "budget-generator");

  AssistRequest capped;
  capped.budget.max_iterations = 100;
  const AssistResult capped_result = assist.run(view, capped);
  REQUIRE(capped_result.diagnostics.status == AssistStatus::kEmpty);
  REQUIRE(capped_result.diagnostics.iterations_consumed == 100);
  REQUIRE(generator_observation.max_iterations == std::vector<uint32_t>{100});
  REQUIRE(counterpoint_observation.max_iterations == std::vector<uint32_t>{10});
  REQUIRE(capped_result.candidate_payloads ==
          std::vector<std::string>{"generator", "counterpoint"});

  // With both axes unlimited, the same modules see zero in every slot and
  // produce the same payload/counter telemetry as the capped exact-fit run.
  generator_observation.max_iterations.clear();
  counterpoint_observation.max_iterations.clear();
  const AssistResult unlimited_result = assist.run(view, AssistRequest{});
  REQUIRE(unlimited_result.diagnostics.status == AssistStatus::kEmpty);
  REQUIRE(unlimited_result.diagnostics.iterations_consumed == 100);
  REQUIRE(generator_observation.max_iterations == std::vector<uint32_t>{0});
  REQUIRE(counterpoint_observation.max_iterations == std::vector<uint32_t>{0});
  REQUIRE(unlimited_result.candidate_payloads == capped_result.candidate_payloads);
}

TEST_CASE("assist overreported work discards a slot atomically", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);
  auto log = std::make_shared<ProbeLog>();
  BudgetObservation counterpoint_observation;
  BudgetGenerator generator("overreport", nullptr, [&, log](const AssistRequest&) {
    AssistResult result = budget_result(101, "overreported");
    result.commands.push_back(std::make_unique<ProbeCommand>(
        1, log, [track_id = f.track_id](Project& project, MidiContentStore&) {
          project.find_track_mutable(track_id)->gain = 0.99f;
        }));
    return result;
  });
  BudgetCounterpoint counterpoint("should-not-run", &counterpoint_observation,
                                  [](const AssistRequest&) { return budget_result(1, "later"); });
  AssistRegistry registry;
  registry.register_generator(&generator);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "overreport");
  AssistRequest request;
  request.budget.max_iterations = 100;

  const AssistResult result = assist.run(view, request);
  REQUIRE(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(result.diagnostics.reason == "iteration budget exhausted");
  REQUIRE(result.diagnostics.iterations_consumed == 100);
  REQUIRE(result.diagnostics.slots_discarded == 1);
  REQUIRE(result.commands.empty());
  REQUIRE(result.candidate_payloads.empty());
  REQUIRE(counterpoint_observation.max_iterations.empty());
  REQUIRE(log->apply_count == 0);
  REQUIRE(log->external == 0);
  REQUIRE(serialize(f.history) == before);
}

TEST_CASE("assist discarded work still consumes the shared iteration budget", "[assist]") {
  Fixture f = make_fixture();
  const std::string before = serialize(f.history);
  BudgetObservation counterpoint_observation;
  BudgetGenerator generator("invalid-consumer", nullptr, [](const AssistRequest&) {
    AssistResult result = budget_result(20, "discarded");
    result.commands.push_back(std::make_unique<ReplaceMidiClipEvents>(
        999999u, MidiClipEventList{MidiClipEvent{0.0, 0x20903C64u, 0u}}));
    return result;
  });
  BudgetCounterpoint counterpoint(
      "remaining-consumer", &counterpoint_observation,
      [](const AssistRequest&) { return budget_result(0, "remaining"); });
  AssistRegistry registry;
  registry.register_generator(&generator);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "invalid-consumer");
  AssistRequest request;
  request.budget.max_iterations = 100;

  const AssistResult result = assist.run(view, request);
  REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(result.diagnostics.iterations_consumed == 20);
  REQUIRE(result.diagnostics.slots_discarded == 1);
  REQUIRE(counterpoint_observation.max_iterations == std::vector<uint32_t>{80});
  REQUIRE(result.candidate_payloads == std::vector<std::string>{"remaining"});
  REQUIRE(serialize(f.history) == before);
}

TEST_CASE("assist unlimited iteration accounting saturates at UINT32_MAX", "[assist]") {
  Fixture f = make_fixture();
  BudgetObservation generator_observation;
  BudgetObservation counterpoint_observation;
  BudgetGenerator generator("saturating-generator", &generator_observation,
                            [](const AssistRequest&) {
                              return budget_result(std::numeric_limits<uint32_t>::max(), "maximum");
                            });
  BudgetCounterpoint counterpoint("saturating-counterpoint", &counterpoint_observation,
                                  [](const AssistRequest&) { return budget_result(1, "one"); });
  AssistRegistry registry;
  registry.register_generator(&generator);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "saturating-generator");

  const AssistResult result = assist.run(view, AssistRequest{});
  REQUIRE(result.diagnostics.iterations_consumed == std::numeric_limits<uint32_t>::max());
  REQUIRE(generator_observation.max_iterations == std::vector<uint32_t>{0});
  REQUIRE(counterpoint_observation.max_iterations == std::vector<uint32_t>{0});
}

TEST_CASE("assist slots share one absolute fake deadline", "[assist]") {
  using namespace std::chrono_literals;
  sonare::midi::assist::detail::NowFnScope now_scope(fake_assist_now);
  g_fake_assist_now = {};

  Fixture f = make_fixture();
  BudgetObservation generator_observation;
  BudgetObservation counterpoint_observation;
  BudgetGenerator generator("timed-generator", &generator_observation, [](const AssistRequest&) {
    g_fake_assist_now += 7ms;
    return budget_result(0, "generator");
  });
  BudgetCounterpoint counterpoint(
      "timed-counterpoint", &counterpoint_observation,
      [](const AssistRequest&) { return budget_result(0, "counterpoint"); });
  AssistRegistry registry;
  registry.register_generator(&generator);
  registry.register_counterpoint(&counterpoint);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "timed-generator");
  AssistRequest request;
  request.budget.max_time_ms = 10;

  const AssistResult result = assist.run(view, request);
  REQUIRE(result.diagnostics.status == AssistStatus::kEmpty);
  REQUIRE(generator_observation.max_time_ms == std::vector<uint32_t>{10});
  REQUIRE(counterpoint_observation.max_time_ms == std::vector<uint32_t>{3});

  g_fake_assist_now = {};
  BudgetObservation ceil_generator_observation;
  BudgetObservation ceil_counterpoint_observation;
  BudgetGenerator ceil_generator("ceil-generator", &ceil_generator_observation,
                                 [](const AssistRequest&) {
                                   g_fake_assist_now += 9500us;
                                   return budget_result(0, "generator");
                                 });
  BudgetCounterpoint ceil_counterpoint(
      "ceil-counterpoint", &ceil_counterpoint_observation,
      [](const AssistRequest&) { return budget_result(0, "counterpoint"); });
  AssistRegistry ceil_registry;
  ceil_registry.register_generator(&ceil_generator);
  ceil_registry.register_counterpoint(&ceil_counterpoint);
  const AssistResult ceil_result = CompositionAssist(ceil_registry).run(view, [&] {
    AssistRequest ceil_request;
    ceil_request.budget.max_time_ms = 10;
    return ceil_request;
  }());
  REQUIRE(ceil_result.diagnostics.status == AssistStatus::kEmpty);
  REQUIRE(ceil_counterpoint_observation.max_time_ms == std::vector<uint32_t>{1});
}

TEST_CASE("assist does not dispatch after allowance calculation crosses deadline", "[assist]") {
  sonare::midi::assist::detail::NowFnScope now_scope(scripted_assist_now);
  g_scripted_assist_now_calls = 0;
  Fixture f = make_fixture();
  BudgetObservation observation;
  bool called = false;
  BudgetGenerator generator("scripted-boundary", &observation, [&called](const AssistRequest&) {
    called = true;
    return budget_result(0, "must-not-run");
  });
  AssistRegistry registry;
  registry.register_generator(&generator);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "scripted-boundary");
  AssistRequest request;
  request.budget.max_time_ms = 10;

  const AssistResult result = assist.run(view, request);
  REQUIRE(result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(result.diagnostics.reason == "time budget exhausted");
  REQUIRE_FALSE(called);
  REQUIRE(observation.max_time_ms.empty());
  REQUIRE(g_scripted_assist_now_calls >= 4);
}

TEST_CASE("assist post-slot deadlines and iteration tie use deterministic reasons", "[assist]") {
  using namespace std::chrono_literals;
  sonare::midi::assist::detail::NowFnScope now_scope(fake_assist_now);
  g_fake_assist_now = {};
  Fixture f = make_fixture();
  BudgetGenerator overrun("last-slot-overrun", nullptr, [](const AssistRequest&) {
    g_fake_assist_now += 11ms;
    return budget_result(0, "partial");
  });
  AssistRegistry registry;
  registry.register_generator(&overrun);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "last-slot-overrun");
  AssistRequest timed;
  timed.budget.max_time_ms = 10;
  const AssistResult overrun_result = assist.run(view, timed);
  REQUIRE(overrun_result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(overrun_result.diagnostics.reason == "time budget exhausted");
  REQUIRE(overrun_result.candidate_payloads == std::vector<std::string>{"partial"});

  g_fake_assist_now = {};
  BudgetGenerator tie("tie", nullptr, [](const AssistRequest&) {
    g_fake_assist_now += 10ms;
    return budget_result(1, "tie");
  });
  AssistRegistry tie_registry;
  tie_registry.register_generator(&tie);
  CompositionAssist tie_assist(tie_registry);
  AssistRequest tie_request;
  tie_request.budget.max_time_ms = 10;
  tie_request.budget.max_iterations = 1;
  const AssistResult tie_result = tie_assist.run(view, tie_request);
  REQUIRE(tie_result.diagnostics.status == AssistStatus::kBudgetTruncated);
  REQUIRE(tie_result.diagnostics.reason == "iteration budget exhausted");
}

TEST_CASE("assist nested runs restore the parent's deadline state", "[assist]") {
  using namespace std::chrono_literals;
  sonare::midi::assist::detail::NowFnScope now_scope(fake_assist_now);
  g_fake_assist_now = {};
  Fixture f = make_fixture();
  BudgetObservation nested_observation;
  BudgetGenerator nested_module("nested-child", &nested_observation,
                                [](const AssistRequest&) { return budget_result(0, "child"); });
  AssistRegistry nested_registry;
  nested_registry.register_generator(&nested_module);
  bool restored = false;
  bool parent_active = false;
  NestedRunGenerator outer(&nested_registry, &restored, &parent_active);
  AssistRegistry registry;
  registry.register_generator(&outer);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "nested-run");
  AssistRequest request;
  request.budget.max_time_ms = 10;
  const AssistResult result = assist.run(view, request);
  REQUIRE(result.diagnostics.status == AssistStatus::kEmpty);
  REQUIRE(restored);
  REQUIRE(parent_active);
  REQUIRE(nested_observation.max_time_ms == std::vector<uint32_t>{10});
}

TEST_CASE("assist deadline state handles saturated and negative clock boundaries", "[assist]") {
  using namespace std::chrono_literals;
  using sonare::midi::assist::detail::AssistClock;
  using sonare::midi::assist::detail::AssistTimePoint;
  using sonare::midi::assist::detail::current_deadline;
  using sonare::midi::assist::detail::deadline_expired;
  using sonare::midi::assist::detail::DeadlineScope;
  using sonare::midi::assist::detail::DeadlineState;
  using sonare::midi::assist::detail::effective_deadline;
  using sonare::midi::assist::detail::NowFnScope;
  using sonare::midi::assist::detail::remaining_time_ms;
  using sonare::midi::assist::detail::saturating_deadline;

  NowFnScope now_scope(fake_assist_now);
  const auto one_tick = AssistClock::duration(1);
  g_fake_assist_now = AssistTimePoint::max() - one_tick;

  // An unlimited request deliberately retains the max sentinel but remains
  // inactive, while a one-millisecond cap saturates to the same endpoint and
  // remains active until the fake clock reaches it.
  const AssistRequest unlimited_request;
  DeadlineState unlimited = effective_deadline(unlimited_request);
  REQUIRE_FALSE(unlimited.active());
  REQUIRE(unlimited.deadline == AssistTimePoint::max());
  {
    DeadlineScope scope(unlimited);
    REQUIRE_FALSE(deadline_expired());
    REQUIRE(remaining_time_ms() == 0);
  }

  AssistRequest capped_request;
  capped_request.budget.max_time_ms = 1;
  DeadlineState saturated = effective_deadline(capped_request);
  REQUIRE(saturated.active());
  REQUIRE(saturated.deadline == AssistTimePoint::max());
  {
    DeadlineScope scope(saturated);
    REQUIRE_FALSE(deadline_expired());
    REQUIRE(remaining_time_ms() == 1);
    g_fake_assist_now = AssistTimePoint::max();
    REQUIRE(deadline_expired());
    REQUIRE(remaining_time_ms() == 0);
  }

  // Both the saturating addition and remaining-time calculation must stay
  // defined when a valid monotonic origin is the minimum representable tick.
  g_fake_assist_now = AssistTimePoint::min();
  const AssistTimePoint negative_deadline = saturating_deadline(g_fake_assist_now, 1);
  REQUIRE(negative_deadline > g_fake_assist_now);
  DeadlineState negative_state;
  negative_state.deadline = negative_deadline;
  negative_state.has_deadline = true;
  {
    DeadlineScope scope(negative_state);
    REQUIRE(remaining_time_ms() == 1);
  }

  // A deliberately broad active state exercises the negative-to-positive
  // duration path without simulating a backwards clock during a run.
  DeadlineState broad_state;
  broad_state.deadline = AssistTimePoint::max();
  broad_state.has_deadline = true;
  {
    DeadlineScope scope(broad_state);
    REQUIRE(remaining_time_ms() == std::numeric_limits<uint32_t>::max());
  }

  // A nested request inherits an active parent's saturated endpoint and still
  // expires when the shared clock reaches that endpoint.
  DeadlineState parent_state;
  parent_state.deadline = AssistTimePoint::max();
  parent_state.has_deadline = true;
  {
    DeadlineScope parent_scope(parent_state);
    g_fake_assist_now = AssistTimePoint::max() - one_tick;
    AssistRequest child_request;
    child_request.budget.max_time_ms = 1;
    DeadlineState child_state = effective_deadline(child_request);
    REQUIRE(child_state.active());
    REQUIRE(child_state.deadline == AssistTimePoint::max());
    DeadlineScope child_scope(child_state);
    REQUIRE(current_deadline() == &child_state);
    g_fake_assist_now = AssistTimePoint::max();
    REQUIRE(deadline_expired());
  }
  REQUIRE(current_deadline() == nullptr);
}

TEST_CASE("assist rejects commands that mutate out-of-scope tracks", "[assist]") {
  Fixture f = make_fixture();
  TrackId track_a = f.track_id;

  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "track-b";
    t.kind = Track::Kind::kMidi;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f.history.apply(std::move(add_track)));
  const TrackId track_b = add_track_ptr->allocated_id();

  MidiSourceRef ref;
  ref.name = "b-src";
  auto attach = std::make_unique<AttachMidiSource>(ref);
  auto* attach_ptr = attach.get();
  REQUIRE(f.history.apply(std::move(attach)));
  EditClip clip_b;
  clip_b.track_id = track_b;
  clip_b.source_id = attach_ptr->allocated_id();
  clip_b.length_ppq = 480.0;
  auto add_clip = std::make_unique<AddClip>(clip_b);
  auto* add_clip_ptr = add_clip.get();
  REQUIRE(f.history.apply(std::move(add_clip)));
  const auto clip_b_id = add_clip_ptr->allocated_id();

  const std::string before = serialize(f.history);
  ScopeBreakingGenerator gen(clip_b_id);
  AssistRegistry registry;
  registry.register_generator(&gen);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "scope-breaking");

  AssistRequest req;
  req.scope.track_ids = {track_a};
  AssistResult result = assist.run(view, req);

  REQUIRE(result.commands.empty());
  REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(result.diagnostics.slots_discarded == 1);
  REQUIRE(serialize(f.history) == before);
}

TEST_CASE("assist protects every outside track field and accepts inside controls", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  auto* outside_track = f.history.project().find_track_mutable(extra.track_id);
  REQUIRE(outside_track != nullptr);
  sonare::automation::AutomationLane outside_lane(
      77, sonare::automation::AutomationTargetKind::kTrackFaderDb);
  outside_lane.set_points({{0.0, -3.0f, sonare::automation::CurveType::Linear},
                           {480.0, 2.0f, sonare::automation::CurveType::Hold}});
  outside_track->automation_lanes.push_back(outside_lane);
  const std::string before = serialize(f.history);

  auto run = [&](std::function<EditCommandPtr()> factory) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = {f.track_id};
    return assist.run(view, request);
  };

  const std::vector<std::pair<const char*, std::function<EditCommandPtr()>>> outside_controls = {
      {"name",
       [&] {
         return std::make_unique<sonare::arrangement::RenameTrack>(extra.track_id,
                                                                   "outside-renamed");
       }},
      {"kind",
       [&] {
         return std::make_unique<ProbeCommand>(
             1, std::make_shared<ProbeLog>(),
             [track_id = extra.track_id](Project& project, MidiContentStore&) {
               auto* track = project.find_track_mutable(track_id);
               if (track == nullptr) throw std::runtime_error("outside track missing");
               track->kind = Track::Kind::kAudio;
             });
       }},
      {"gain", [&] { return std::make_unique<SetTrackGain>(extra.track_id, 0.25f); }},
      {"mute", [&] { return std::make_unique<SetTrackMute>(extra.track_id, true); }},
      {"solo", [&] { return std::make_unique<SetTrackSolo>(extra.track_id, true); }},
      {"pan", [&] { return std::make_unique<SetTrackPan>(extra.track_id, -0.4f); }},
      {"route",
       [&] {
         return std::make_unique<SetTrackRoute>(extra.track_id, "outside-strip", "outside-bus");
       }},
      {"midi destination",
       [&] { return std::make_unique<SetTrackMidiDestination>(extra.track_id, 44u); }},
      {"automation",
       [&] {
         sonare::automation::AutomationLane lane(
             78, sonare::automation::AutomationTargetKind::kTrackFaderDb);
         lane.set_points({{0.0, -9.0f, sonare::automation::CurveType::Linear},
                          {960.0, 4.0f, sonare::automation::CurveType::Linear}});
         return std::make_unique<AddAutomationLane>(extra.track_id, std::move(lane));
       }},
  };
  for (const auto& [label, factory] : outside_controls) {
    INFO("outside track field: " << label);
    AssistResult outside = run(factory);
    REQUIRE(outside.commands.empty());
    REQUIRE(outside.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(outside.diagnostics.slots_discarded == 1);
    REQUIRE(serialize(f.history) == before);
  }

  AssistResult inside = run([&] { return std::make_unique<SetTrackGain>(f.track_id, 0.25f); });
  REQUIRE_FALSE(inside.commands.empty());
  REQUIRE(inside.diagnostics.status == AssistStatus::kOk);
  REQUIRE(serialize(f.history) == before);

  AssistResult inside_mute = run([&] { return std::make_unique<SetTrackMute>(f.track_id, true); });
  REQUIRE_FALSE(inside_mute.commands.empty());
  REQUIRE(inside_mute.diagnostics.status == AssistStatus::kOk);
}

TEST_CASE("assist rejects clip boundary crossings and protects project globals", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);

  auto run = [&](std::function<EditCommandPtr()> factory, std::vector<TrackId> tracks) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = std::move(tracks);
    return assist.run(view, request);
  };

  AssistResult crossing = run(
      [&] {
        return std::make_unique<sonare::arrangement::MoveClip>(f.clip_id, 0.0, extra.track_id);
      },
      {f.track_id});
  REQUIRE(crossing.commands.empty());
  REQUIRE(crossing.diagnostics.status == AssistStatus::kDiscarded);

  AssistResult delete_outside =
      run([&] { return std::make_unique<RemoveClip>(extra.clip_id); }, {f.track_id});
  REQUIRE(delete_outside.commands.empty());
  REQUIRE(delete_outside.diagnostics.status == AssistStatus::kDiscarded);

  EditClip new_outside_clip;
  new_outside_clip.track_id = extra.track_id;
  new_outside_clip.source_id = f.history.project().find_clip(extra.clip_id)->source_id;
  new_outside_clip.start_ppq = 2400.0;
  new_outside_clip.length_ppq = 480.0;
  AssistResult add_outside = run(
      [&, new_outside_clip] { return std::make_unique<AddClip>(new_outside_clip); }, {f.track_id});
  REQUIRE(add_outside.commands.empty());
  REQUIRE(add_outside.diagnostics.status == AssistStatus::kDiscarded);

  EditClip new_inside_clip = new_outside_clip;
  new_inside_clip.track_id = f.track_id;
  AssistResult add_inside = run(
      [&, new_inside_clip] { return std::make_unique<AddClip>(new_inside_clip); }, {f.track_id});
  REQUIRE_FALSE(add_inside.commands.empty());
  REQUIRE(add_inside.diagnostics.status == AssistStatus::kOk);

  AssistResult inside_move = run(
      [&] { return std::make_unique<sonare::arrangement::MoveClip>(f.clip_id, 480.0, f.track_id); },
      {f.track_id});
  REQUIRE_FALSE(inside_move.commands.empty());

  AssistResult global = run([&] { return std::make_unique<SetSampleRate>(44100.0); }, {f.track_id});
  REQUIRE(global.commands.empty());
  REQUIRE(global.diagnostics.status == AssistStatus::kDiscarded);

  AssistResult whole =
      run([&] { return std::make_unique<SetSampleRate>(44100.0); }, {f.track_id, extra.track_id});
  REQUIRE_FALSE(whole.commands.empty());
  REQUIRE(whole.diagnostics.status == AssistStatus::kOk);
}

TEST_CASE("assist compares every outside clip value and both move directions", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  const std::string before = serialize(f.history);

  auto run = [&](std::function<EditCommandPtr()> factory, TrackId scope_track) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = {scope_track};
    return assist.run(view, request);
  };

  AssistResult a_to_b = run(
      [&] {
        return std::make_unique<sonare::arrangement::MoveClip>(f.clip_id, 0.0, extra.track_id);
      },
      f.track_id);
  REQUIRE(a_to_b.commands.empty());
  REQUIRE(a_to_b.diagnostics.status == AssistStatus::kDiscarded);

  AssistResult b_to_a = run(
      [&] {
        return std::make_unique<sonare::arrangement::MoveClip>(extra.clip_id, 0.0, f.track_id);
      },
      extra.track_id);
  REQUIRE(b_to_a.commands.empty());
  REQUIRE(b_to_a.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(serialize(f.history) == before);

  Fixture audio_history = make_fixture();
  const ExtraAudioFixture audio = add_extra_audio_fixture(&audio_history);
  auto* audio_clip = audio_history.history.project().find_clip_mutable(audio.clip_id);
  REQUIRE(audio_clip != nullptr);
  audio_clip->loop_mode = sonare::arrangement::LoopMode::kLoop;
  audio_clip->loop_length_ppq = 960.0;
  const std::string audio_before = serialize(audio_history.history);

  auto run_audio = [&](std::function<EditCommandPtr()> factory, TrackId scope_track) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(audio_history.history.project(), audio_history.history.midi_content(),
                     "one-command");
    AssistRequest request;
    request.scope.track_ids = {scope_track};
    return assist.run(view, request);
  };

  const auto outside_clip = [&](const char* label, std::function<void(EditClip&)> mutate) {
    INFO("outside clip field: " << label);
    return run_audio(
        [&, mutate = std::move(mutate)] {
          return std::make_unique<ProbeCommand>(
              1, std::make_shared<ProbeLog>(),
              [&, mutate = std::move(mutate)](Project& project, MidiContentStore&) {
                EditClip* clip = project.find_clip_mutable(audio.clip_id);
                if (clip == nullptr) throw std::runtime_error("outside clip missing");
                mutate(*clip);
              });
        },
        audio_history.track_id);
  };

  const std::vector<std::pair<const char*, std::function<void(EditClip&)>>> clip_fields = {
      {"source", [&](EditClip& clip) { clip.source_id = audio.take_source_id; }},
      {"start", [](EditClip& clip) { clip.start_ppq = 120.0; }},
      {"length", [](EditClip& clip) { clip.length_ppq = 1440.0; }},
      {"source offset", [](EditClip& clip) { clip.source_offset_ppq = 60.0; }},
      {"gain", [](EditClip& clip) { clip.gain = 0.5f; }},
      {"fade in", [](EditClip& clip) { clip.fade_in.length_ppq = 120.0; }},
      {"fade out", [](EditClip& clip) { clip.fade_out.length_ppq = 80.0; }},
      {"loop mode", [](EditClip& clip) { clip.loop_mode = sonare::arrangement::LoopMode::kOff; }},
      {"loop length", [](EditClip& clip) { clip.loop_length_ppq = 720.0; }},
      {"loop crossfade", [](EditClip& clip) { clip.loop_crossfade_ppq = 90.0; }},
      {"warp ref", [](EditClip& clip) { clip.warp_ref_id = 9; }},
      {"warp mode",
       [](EditClip& clip) { clip.warp_mode = sonare::arrangement::WarpMode::kTimeStretch; }},
      {"takes",
       [](EditClip& clip) {
         clip.takes = {{2, 0, 30.0, "alternate", 0.75, true}};
         clip.active_take_id = 2;
       }},
      {"active take", [](EditClip& clip) { clip.active_take_id = 0; }},
      {"comp segments", [](EditClip& clip) { clip.comp_segments = {{0.0, 960.0, 1, 32.0}}; }},
      {"physical source offset", [](EditClip& clip) { clip.source_offset_seconds = 1.25; }},
      {"loop anchor",
       [](EditClip& clip) { clip.loop_anchor = sonare::arrangement::LoopAnchor{2.0, 0.5}; }},
      {"render parts",
       [](EditClip& clip) {
         clip.comp_render_parts = {{0.0, 960.0, -32.0, 992.0, 1, 16.0, 8.0, false}};
       }},
  };
  for (const auto& [label, mutate] : clip_fields) {
    AssistResult outside = outside_clip(label, mutate);
    REQUIRE(outside.commands.empty());
    REQUIRE(outside.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(outside.diagnostics.slots_discarded == 1);
    REQUIRE(serialize(audio_history.history) == audio_before);
  }

  AssistResult inside_hidden_fields = run_audio(
      [&] {
        return std::make_unique<ProbeCommand>(
            2, std::make_shared<ProbeLog>(),
            [clip_id = audio.clip_id](Project& project, MidiContentStore&) {
              EditClip* clip = project.find_clip_mutable(clip_id);
              if (clip == nullptr) throw std::runtime_error("inside clip missing");
              clip->source_offset_seconds = 0.5;
              clip->loop_anchor = sonare::arrangement::LoopAnchor{2.0, 0.25};
              clip->comp_render_parts = {{0.0, 960.0, -16.0, 976.0, 1, 8.0, 4.0, false}};
            });
      },
      audio.track_id);
  REQUIRE_FALSE(inside_hidden_fields.commands.empty());
  REQUIRE(inside_hidden_fields.diagnostics.status == AssistStatus::kOk);

  AssistResult loop_command = run_audio(
      [&] {
        return std::make_unique<SetClipLoop>(audio.clip_id, sonare::arrangement::LoopMode::kLoop,
                                             960.0, 120.0);
      },
      audio.track_id);
  REQUIRE_FALSE(loop_command.commands.empty());
  REQUIRE(loop_command.diagnostics.status == AssistStatus::kOk);

  AssistResult warp_command = run_audio(
      [&] {
        return std::make_unique<SetClipWarpMode>(audio.clip_id,
                                                 sonare::arrangement::WarpMode::kTimeStretch);
      },
      audio.track_id);
  REQUIRE_FALSE(warp_command.commands.empty());
  REQUIRE(warp_command.diagnostics.status == AssistStatus::kOk);

  AssistResult takes_command = run_audio(
      [&] {
        std::vector<sonare::arrangement::ClipTake> takes = {
            {2, audio.take_source_id, 120.0, "alternate", 0.75, false}};
        return std::make_unique<SetClipTakes>(audio.clip_id, std::move(takes), 2);
      },
      audio.track_id);
  REQUIRE_FALSE(takes_command.commands.empty());
  REQUIRE(takes_command.diagnostics.status == AssistStatus::kOk);

  AssistResult comp_command = run_audio(
      [&] {
        return std::make_unique<SetClipCompSegments>(
            audio.clip_id,
            std::vector<sonare::arrangement::ClipCompSegment>{{0.0, 1920.0, 1, 0.0}});
      },
      audio.track_id);
  REQUIRE_FALSE(comp_command.commands.empty());
  REQUIRE(comp_command.diagnostics.status == AssistStatus::kOk);
}

TEST_CASE("assist protects every project global while allowing sidecars", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  const uint32_t marker_id = f.history.project().add_marker(0.0, "old-marker");
  REQUIRE(marker_id != 0);
  const std::string before = serialize(f.history);

  auto run = [&](std::function<EditCommandPtr()> factory, std::vector<TrackId> tracks) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = std::move(tracks);
    return assist.run(view, request);
  };

  sonare::transport::TempoSegment tempo;
  tempo.start_ppq = 0.0;
  tempo.bpm = 132.0;
  tempo.start_sample = 10;
  tempo.end_bpm = 140.0;
  tempo.end_ppq = 1920.0;

  sonare::transport::TimeSignatureSegment time_signature;
  time_signature.start_ppq = 0.0;
  time_signature.time_sig.numerator = 7;
  time_signature.time_sig.denominator = 8;
  time_signature.clocks_per_metronome_click = 36;
  time_signature.thirty_seconds_per_quarter = 12;

  sonare::mixing::api::Scene scene;
  scene.version = 3;
  sonare::mixing::api::Strip strip;
  strip.id = "strip-1";
  strip.input_trim_db = -2.0f;
  strip.fader_db = -1.0f;
  strip.inserts.emplace_back(sonare::mixing::api::InsertSlot::PostFader, "compressor", "{}",
                             "sidechain");
  strip.sends.push_back({"send-1", "bus-1", -4.0f, sonare::mixing::api::SendTiming::PreFader});
  strip.eq.enabled = false;
  scene.strips.push_back(strip);
  sonare::mixing::api::Bus bus("bus-1", "aux");
  bus.input_trim_db = -1.0f;
  bus.inserts.emplace_back(sonare::mixing::api::InsertSlot::PreFader, "limiter", "{}", "");
  bus.eq.enabled = false;
  scene.buses.push_back(bus);
  scene.vca_groups.push_back({"vca-1", -1.5f, {"strip-1"}});
  scene.connections.push_back({"strip-1", "bus-1"});

  sonare::arrangement::ProjectAnnotation annotation;
  annotation.tempo_confidence = 0.75f;
  annotation.keys.push_back({0.0, 960.0, 2, sonare::arrangement::KeyMode::kMinor});
  annotation.chords.push_back(
      {0.0, 960.0, 0, sonare::arrangement::ChordQuality::kMajor, {7, 9}, 7, "V", true});
  annotation.sections.push_back({0.0, 960.0, "verse"});
  annotation.onsets.push_back({480.0, 0.9f});

  sonare::midi::PartRig rig;
  rig.mode = sonare::midi::PartRigMode::kChain;
  rig.stages.push_back({"chorus", "{\"mix\":0.3}"});

  AssistSidecar sidecar;
  sidecar.module_id = "scope-sidecar";
  sidecar.schema_version = 2;
  sidecar.target_track_id = f.track_id;
  sidecar.payload = {4, 5, 6};

  const std::vector<std::pair<const char*, std::function<EditCommandPtr()>>> globals = {
      {"sample rate", [&] { return std::make_unique<SetSampleRate>(44100.0); }},
      {"overlap", [&] { return std::make_unique<SetOverlapPolicy>(OverlapPolicy::kAllow); }},
      {"tempo",
       [&] {
         return std::make_unique<SetTempoSegment>(
             std::vector<sonare::transport::TempoSegment>{tempo});
       }},
      {"time signature",
       [&] {
         return std::make_unique<SetTimeSignatureSegment>(
             std::vector<sonare::transport::TimeSignatureSegment>{time_signature});
       }},
      {"scene", [&] { return std::make_unique<SetScene>(scene); }},
      {"marker",
       [&] { return std::make_unique<SetMarker>(marker_id, 480.0, "new-marker", 1, -2, true); }},
      {"annotation", [&] { return std::make_unique<SetAnnotation>(annotation); }},
      {"part rig", [&] { return std::make_unique<SetPartRig>(44u, 3u, rig); }},
  };
  for (const auto& [label, factory] : globals) {
    INFO("global field: " << label);
    AssistResult partial = run(factory, {f.track_id});
    REQUIRE(partial.commands.empty());
    REQUIRE(partial.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(partial.diagnostics.slots_discarded == 1);
    REQUIRE(serialize(f.history) == before);

    AssistResult all = run(factory, {f.track_id, extra.track_id});
    REQUIRE_FALSE(all.commands.empty());
    REQUIRE(all.diagnostics.status == AssistStatus::kOk);

    AssistResult empty = run(factory, {});
    REQUIRE_FALSE(empty.commands.empty());
    REQUIRE(empty.diagnostics.status == AssistStatus::kOk);
  }

  AssistResult sidecar_result =
      run([&] { return std::make_unique<SetAssistSidecar>(sidecar); }, {f.track_id});
  REQUIRE_FALSE(sidecar_result.commands.empty());
  REQUIRE(sidecar_result.diagnostics.status == AssistStatus::kOk);
  REQUIRE(serialize(f.history) == before);
}

TEST_CASE("assist protects shared base, take, and warp dependencies", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  auto* clip_a = f.history.project().find_clip_mutable(f.clip_id);
  auto* clip_b = f.history.project().find_clip_mutable(extra.clip_id);
  REQUIRE(clip_a != nullptr);
  REQUIRE(clip_b != nullptr);

  const sonare::arrangement::SourceId base_source = clip_a->source_id;
  // Make B consume A's base source explicitly.  The fixture starts B with its
  // own source, so without this assignment the "shared base" control is only
  // an in-scope mutation and the production dependency walk correctly accepts
  // it.
  clip_b->source_id = base_source;
  const sonare::arrangement::SourceId unreferenced_source =
      f.history.project().add_midi_source(MidiSourceRef{});
  const sonare::arrangement::SourceId b_take_source =
      f.history.project().add_midi_source(MidiSourceRef{});
  const sonare::arrangement::SourceId a_take_source =
      f.history.project().add_midi_source(MidiSourceRef{});
  REQUIRE(unreferenced_source != 0);
  REQUIRE(b_take_source != 0);
  REQUIRE(a_take_source != 0);

  const sonare::arrangement::WarpRefId shared_warp_id = 71;
  const sonare::arrangement::WarpRefId inside_warp_id = 72;
  REQUIRE(f.history.project().set_warp_map(
      WarpMapRef{shared_warp_id, "shared", {{0.0, 0.0}, {480.0, 512.0}}}));
  REQUIRE(f.history.project().set_warp_map(
      WarpMapRef{inside_warp_id, "inside", {{0.0, 0.0}, {240.0, 250.0}}}));
  clip_a->warp_ref_id = shared_warp_id;
  clip_b->warp_ref_id = shared_warp_id;
  clip_b->takes = {{10, b_take_source, 0.0, "b-take", std::nullopt, false}};
  clip_b->active_take_id = 10;
  clip_a->takes = {{11, a_take_source, 0.0, "a-take", std::nullopt, false}};
  clip_a->active_take_id = 11;
  const std::string before = serialize(f.history);

  auto run = [&](std::function<EditCommandPtr()> factory) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = {f.track_id};
    return assist.run(view, request);
  };

  auto reject = [&](const char* label, std::function<EditCommandPtr()> factory) {
    INFO("protected dependency: " << label);
    AssistResult result = run(std::move(factory));
    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(result.diagnostics.slots_discarded == 1);
    REQUIRE(serialize(f.history) == before);
  };

  reject("shared base source", [&] {
    return std::make_unique<ReplaceSource>(base_source, MidiSourceRef{0, "changed", 9});
  });

  MidiSourceRef changed_take;
  changed_take.name = "b-take-changed";
  reject("outside take source",
         [&] { return std::make_unique<ReplaceSource>(b_take_source, changed_take); });

  reject("shared warp map", [&] {
    return std::make_unique<SetWarpMap>(
        WarpMapRef{shared_warp_id, "shared-changed", {{0.0, 0.0}, {480.0, 640.0}}});
  });

  AssistResult inside_source = run([&] {
    return std::make_unique<ReplaceSource>(a_take_source,
                                           MidiSourceRef{0, "inside-take-changed", 4});
  });
  REQUIRE_FALSE(inside_source.commands.empty());
  REQUIRE(inside_source.diagnostics.status == AssistStatus::kOk);

  AssistResult unreferenced = run([&] {
    return std::make_unique<ReplaceSource>(unreferenced_source,
                                           MidiSourceRef{0, "unreferenced-changed", 5});
  });
  REQUIRE_FALSE(unreferenced.commands.empty());
  REQUIRE(unreferenced.diagnostics.status == AssistStatus::kOk);

  AssistResult add_unreferenced = run([&] {
    MidiSourceRef new_source;
    new_source.name = "new-unreferenced";
    return std::make_unique<AttachMidiSource>(new_source);
  });
  REQUIRE_FALSE(add_unreferenced.commands.empty());
  REQUIRE(add_unreferenced.diagnostics.status == AssistStatus::kOk);

  clip_a->warp_ref_id = inside_warp_id;
  clip_b->warp_ref_id = shared_warp_id;
  AssistResult inside_warp = run([&] {
    return std::make_unique<SetWarpMap>(
        WarpMapRef{inside_warp_id, "inside-changed", {{0.0, 0.0}, {240.0, 300.0}}});
  });
  REQUIRE_FALSE(inside_warp.commands.empty());
  REQUIRE(inside_warp.diagnostics.status == AssistStatus::kOk);
}

TEST_CASE("assist protects shared SysEx payloads while preserving the full store", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  constexpr uint32_t kSharedHandle = 17;
  constexpr uint32_t kInsideHandle = 23;
  f.history.midi_content().events[f.clip_id] = {{0.0, 0, 0, kSharedHandle}};
  f.history.midi_content().events[extra.clip_id] = {{0.0, 0, 0, kSharedHandle}};
  f.history.midi_content().sysex_payloads[kSharedHandle] = {1, 2, 3};

  auto run = [&](uint32_t handle, std::vector<uint8_t> payload) {
    OneCommandGenerator gen([&, handle, payload = std::move(payload)] {
      MidiClipEventList events = {{0.0, 0, 0, handle}};
      std::map<uint32_t, std::vector<uint8_t>> payloads;
      payloads.emplace(handle, payload);
      return std::make_unique<ReplaceMidiClipEvents>(f.clip_id, std::move(events),
                                                     std::move(payloads));
    });
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = {f.track_id};
    return assist.run(view, request);
  };

  const MidiContentStore before = f.history.midi_content();
  AssistResult shared = run(kSharedHandle, {9, 9, 9});
  REQUIRE(shared.commands.empty());
  REQUIRE(shared.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(f.history.midi_content() == before);

  // A payload referenced only by the in-scope clip is allowed; the outside
  // clip's event and payload remain part of the copied store.
  f.history.midi_content().events[f.clip_id] = {{0.0, 0, 0, kInsideHandle}};
  f.history.midi_content().sysex_payloads[kInsideHandle] = {4, 5};
  const auto outside_events_before = f.history.midi_content().events.at(extra.clip_id);
  const auto shared_payload_before = f.history.midi_content().sysex_payloads.at(kSharedHandle);
  const MidiContentStore inside_preview_before = f.history.midi_content();
  AssistResult inside = run(kInsideHandle, {6, 7});
  REQUIRE_FALSE(inside.commands.empty());
  REQUIRE(inside.diagnostics.status == AssistStatus::kOk);
  REQUIRE(f.history.midi_content() == inside_preview_before);
  REQUIRE(apply_result(&f.history, std::move(inside)));
  REQUIRE(f.history.midi_content().events.at(extra.clip_id) == outside_events_before);
  REQUIRE(f.history.midi_content().sysex_payloads.at(kSharedHandle) == shared_payload_before);
  REQUIRE(f.history.midi_content().events.at(f.clip_id).front().sysex_handle == kInsideHandle);
  REQUIRE(f.history.midi_content().sysex_payloads.at(kInsideHandle) == std::vector<uint8_t>{6, 7});
}

TEST_CASE("assist preflights unknown and protected external effects", "[assist]") {
  Fixture f = make_fixture();
  const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
  const auto source_a = f.history.project().find_clip(f.clip_id)->source_id;
  const auto source_b = f.history.project().find_clip(extra.clip_id)->source_id;
  constexpr sonare::arrangement::WarpRefId kProtectedWarp = 88;
  REQUIRE(f.history.project().set_warp_map(
      WarpMapRef{kProtectedWarp, "protected", {{0.0, 0.0}, {480.0, 480.0}}}));
  f.history.project().find_clip_mutable(extra.clip_id)->warp_ref_id = kProtectedWarp;
  const std::string before = serialize(f.history);

  auto run = [&](std::function<EditCommandPtr()> factory, std::vector<TrackId> tracks) {
    OneCommandGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids = std::move(tracks);
    return assist.run(view, request);
  };

  auto mutate_inside = [&](Project& project, MidiContentStore&) {
    auto* track = project.find_track_mutable(f.track_id);
    if (track == nullptr) throw std::runtime_error("missing probe track");
    track->gain = 0.2f;
  };

  auto unknown_log = std::make_shared<ProbeLog>();
  AssistResult unknown = run(
      [&, unknown_log] {
        return std::make_unique<UnknownRollbackProbe>(1, unknown_log, mutate_inside);
      },
      {f.track_id});
  REQUIRE(unknown.commands.empty());
  REQUIRE(unknown.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(unknown_log->apply_count == 0);
  REQUIRE(unknown_log->rollback_count == 0);
  REQUIRE(unknown_log->external == 0);
  REQUIRE(serialize(f.history) == before);

  auto protected_log = std::make_shared<ProbeLog>();
  AssistResult protected_source = run(
      [&, protected_log] {
        return std::make_unique<ProbeCommand>(2, protected_log, mutate_inside,
                                              ProbeBehavior::kSuccess, source_b);
      },
      {f.track_id});
  REQUIRE(protected_source.commands.empty());
  REQUIRE(protected_source.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(protected_log->apply_count == 0);
  REQUIRE(protected_log->rollback_count == 0);
  REQUIRE(protected_log->external == 0);
  REQUIRE(serialize(f.history) == before);

  auto protected_warp_log = std::make_shared<ProbeLog>();
  AssistResult protected_warp = run(
      [&, protected_warp_log] {
        return std::make_unique<ProbeCommand>(3, protected_warp_log, mutate_inside,
                                              ProbeBehavior::kSuccess, 0, kProtectedWarp);
      },
      {f.track_id});
  REQUIRE(protected_warp.commands.empty());
  REQUIRE(protected_warp.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(protected_warp_log->apply_count == 0);
  REQUIRE(protected_warp_log->rollback_count == 0);
  REQUIRE(protected_warp_log->external == 0);
  REQUIRE(serialize(f.history) == before);

  auto dynamic_log = std::make_shared<ProbeLog>();
  AssistResult dynamic = run(
      [&, dynamic_log] {
        return std::make_unique<ProbeCommand>(4, dynamic_log, mutate_inside,
                                              ProbeBehavior::kSuccess, 0, 0, true);
      },
      {f.track_id});
  REQUIRE(dynamic.commands.empty());
  REQUIRE(dynamic.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(dynamic_log->apply_count == 0);
  REQUIRE(dynamic_log->rollback_count == 0);
  REQUIRE(dynamic_log->external == 0);
  REQUIRE(serialize(f.history) == before);

  auto inside_log = std::make_shared<ProbeLog>();
  AssistResult inside = run(
      [&, inside_log] {
        return std::make_unique<ProbeCommand>(5, inside_log, mutate_inside, ProbeBehavior::kSuccess,
                                              source_a);
      },
      {f.track_id});
  REQUIRE_FALSE(inside.commands.empty());
  REQUIRE(inside.diagnostics.status == AssistStatus::kOk);
  REQUIRE(inside_log->apply_count == 1);
  REQUIRE(inside_log->rollback_count == 1);
  REQUIRE(inside_log->external == 0);
  REQUIRE(serialize(f.history) == before);

  auto empty_log = std::make_shared<ProbeLog>();
  AssistResult empty_scope = run(
      [&, empty_log] {
        return std::make_unique<UnknownRollbackProbe>(6, empty_log, mutate_inside);
      },
      {});
  REQUIRE_FALSE(empty_scope.commands.empty());
  REQUIRE(empty_scope.diagnostics.status == AssistStatus::kOk);
  REQUIRE(empty_log->apply_count == 1);
  REQUIRE(empty_log->rollback_count == 1);
  REQUIRE(empty_log->external == 0);

  auto all_log = std::make_shared<ProbeLog>();
  AssistResult all_scope = run(
      [&, all_log] { return std::make_unique<UnknownRollbackProbe>(7, all_log, mutate_inside); },
      {f.track_id, extra.track_id});
  REQUIRE_FALSE(all_scope.commands.empty());
  REQUIRE(all_scope.diagnostics.status == AssistStatus::kOk);
  REQUIRE(all_log->apply_count == 1);
  REQUIRE(all_log->rollback_count == 1);
  REQUIRE(all_log->external == 0);
  REQUIRE(serialize(f.history) == before);
}

TEST_CASE("assist rollback guard covers every failure exit and replays success", "[assist]") {
  auto run_sequence = [](Fixture& fixture, std::function<std::vector<EditCommandPtr>()> factory,
                         std::vector<TrackId> tracks) {
    SequenceGenerator gen(std::move(factory));
    AssistRegistry registry;
    registry.register_generator(&gen);
    CompositionAssist assist(registry);
    ProjectView view(fixture.history.project(), fixture.history.midi_content(), "sequence");
    AssistRequest request;
    request.scope.track_ids = std::move(tracks);
    return assist.run(view, request);
  };

  auto make_mutator = [](TrackId track_id, sonare::arrangement::ClipId clip_id, uint8_t note) {
    return [track_id, clip_id, note](Project& project, MidiContentStore& store) {
      auto* track = project.find_track_mutable(track_id);
      if (track == nullptr) throw std::runtime_error("probe track missing");
      track->gain = static_cast<float>(note) / 100.0f;
      store.events[clip_id] = {{0.0, note, 0, 0}};
    };
  };

  {
    Fixture f = make_fixture();
    const std::string before = serialize(f.history);
    auto log = std::make_shared<ProbeLog>();
    const auto first_mutation = make_mutator(f.track_id, f.clip_id, 61);
    const auto second_mutation = [log, track_id = f.track_id, clip_id = f.clip_id](
                                     Project& project, MidiContentStore& store) {
      auto* track = project.find_track_mutable(track_id);
      const auto event_it = store.events.find(clip_id);
      log->observed_prior_state = track != nullptr && track->gain == 0.61f &&
                                  event_it != store.events.end() && event_it->second.size() == 1;
      if (track == nullptr) throw std::runtime_error("probe track missing");
      track->pan = 0.2f;
      store.events[clip_id].push_back({240.0, 62, 0, 0});
    };
    AssistResult result =
        run_sequence(f,
                     [=] {
                       std::vector<EditCommandPtr> commands;
                       commands.push_back(std::make_unique<ProbeCommand>(1, log, first_mutation));
                       commands.push_back(std::make_unique<ProbeCommand>(2, log, second_mutation));
                       return commands;
                     },
                     {f.track_id});
    REQUIRE(result.commands.size() == 2);
    REQUIRE(result.diagnostics.status == AssistStatus::kOk);
    REQUIRE(log->apply_count == 2);
    REQUIRE(log->rollback_count == 2);
    REQUIRE(log->rollback_ids[0] == 2);
    REQUIRE(log->rollback_ids[1] == 1);
    REQUIRE(log->external == 0);
    REQUIRE(log->observed_prior_state);
    REQUIRE(serialize(f.history) == before);

    REQUIRE(apply_result(&f.history, std::move(result)));
    REQUIRE(f.history.project().find_track(f.track_id)->gain == 0.61f);
    REQUIRE(f.history.midi_content().events.at(f.clip_id).size() == 2);
    REQUIRE(log->external == 2);
    const std::string applied = serialize(f.history);
    REQUIRE(f.history.undo());
    REQUIRE(serialize(f.history) == before);
    REQUIRE(log->external == 0);
    REQUIRE(f.history.redo());
    REQUIRE(serialize(f.history) == applied);
    REQUIRE(log->external == 2);
  }

  {
    Fixture f = make_fixture();
    const std::string before = serialize(f.history);
    auto log = std::make_shared<ProbeLog>();
    const auto first_mutation = make_mutator(f.track_id, f.clip_id, 61);
    const auto second_mutation = [log, track_id = f.track_id, clip_id = f.clip_id](
                                     Project& project, MidiContentStore& store) {
      auto* track = project.find_track_mutable(track_id);
      const auto event_it = store.events.find(clip_id);
      log->observed_prior_state = track != nullptr && track->gain == 0.61f &&
                                  event_it != store.events.end() && event_it->second.size() == 1;
      if (track == nullptr) throw std::runtime_error("probe track missing");
      track->pan = 0.2f;
      store.events[clip_id].push_back({240.0, 62, 0, 0});
    };
    OneCommandGenerator generator(
        [=] { return std::make_unique<ProbeCommand>(1, log, first_mutation); });
    OneCommandCounterpoint counterpoint(
        [=] { return std::make_unique<ProbeCommand>(2, log, second_mutation); });
    AssistRegistry registry;
    registry.register_generator(&generator);
    registry.register_counterpoint(&counterpoint);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "successive-slots");
    AssistRequest request;
    request.scope.track_ids = {f.track_id};
    AssistResult result = assist.run(view, request);
    REQUIRE(result.commands.size() == 2);
    REQUIRE(result.diagnostics.status == AssistStatus::kOk);
    // Each dispatch slot owns a separate guard.  Their rollback order is
    // therefore [1, 2], while the same-slot transaction above proves [2, 1].
    REQUIRE(log->apply_count == 2);
    REQUIRE(log->rollback_count == 2);
    REQUIRE(log->rollback_ids[0] == 1);
    REQUIRE(log->rollback_ids[1] == 2);
    REQUIRE(log->external == 0);
    REQUIRE(log->observed_prior_state);
    REQUIRE(serialize(f.history) == before);

    REQUIRE(apply_result(&f.history, std::move(result)));
    REQUIRE(f.history.project().find_track(f.track_id)->gain == 0.61f);
    REQUIRE(f.history.midi_content().events.at(f.clip_id).size() == 2);
    const std::string applied = serialize(f.history);
    REQUIRE(f.history.undo());
    REQUIRE(serialize(f.history) == before);
    REQUIRE(log->external == 0);
    REQUIRE(f.history.redo());
    REQUIRE(serialize(f.history) == applied);
    REQUIRE(log->external == 2);
  }

  auto run_failure_case = [&](ProbeBehavior behavior) {
    Fixture f = make_fixture();
    const std::string before = serialize(f.history);
    auto log = std::make_shared<ProbeLog>();
    AssistResult result =
        run_sequence(f,
                     [&, log] {
                       std::vector<EditCommandPtr> commands;
                       commands.push_back(std::make_unique<ProbeCommand>(
                           1, log, make_mutator(f.track_id, f.clip_id, 63)));
                       commands.push_back(std::make_unique<ProbeCommand>(
                           2, log, make_mutator(f.track_id, f.clip_id, 64), behavior));
                       return commands;
                     },
                     {f.track_id});
    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(result.diagnostics.slots_discarded == 1);
    REQUIRE(log->rollback_count == 2);
    REQUIRE(log->rollback_ids[0] == 2);
    REQUIRE(log->rollback_ids[1] == 1);
    REQUIRE(log->external == 0);
    REQUIRE(serialize(f.history) == before);
  };
  run_failure_case(ProbeBehavior::kFalse);
  run_failure_case(ProbeBehavior::kThrow);

  {
    Fixture f = make_fixture();
    const std::string before = serialize(f.history);
    auto log = std::make_shared<ProbeLog>();
    AssistResult result = run_sequence(f,
                                       [&, log] {
                                         std::vector<EditCommandPtr> commands;
                                         commands.push_back(std::make_unique<ProbeCommand>(
                                             1, log, make_mutator(f.track_id, f.clip_id, 65)));
                                         commands.emplace_back(nullptr);
                                         return commands;
                                       },
                                       {f.track_id});
    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(log->rollback_count == 1);
    REQUIRE(log->rollback_ids[0] == 1);
    REQUIRE(log->external == 0);
    REQUIRE(serialize(f.history) == before);
  }

  {
    Fixture f = make_fixture();
    const ExtraMidiFixture extra = add_extra_midi_fixture(&f);
    const std::string before = serialize(f.history);
    auto log = std::make_shared<ProbeLog>();
    AssistResult result =
        run_sequence(f,
                     [&, log] {
                       std::vector<EditCommandPtr> commands;
                       commands.push_back(std::make_unique<ProbeCommand>(
                           1, log, make_mutator(f.track_id, f.clip_id, 66)));
                       commands.push_back(std::make_unique<ProbeCommand>(
                           2, log, make_mutator(extra.track_id, extra.clip_id, 67)));
                       return commands;
                     },
                     {f.track_id});
    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(log->rollback_count == 2);
    REQUIRE(log->rollback_ids[0] == 2);
    REQUIRE(log->rollback_ids[1] == 1);
    REQUIRE(log->external == 0);
    REQUIRE(serialize(f.history) == before);
  }
}

TEST_CASE("assist accepted Project and MIDI copies fail independently", "[assist]") {
  auto clear_failpoint = []() noexcept {
    sonare::test::g_fail_allocation_number.store(0, std::memory_order_relaxed);
    sonare::test::g_allocation_attempts.store(0, std::memory_order_relaxed);
  };
  struct FailpointReset final {
    ~FailpointReset() noexcept {
      sonare::test::g_fail_allocation_number.store(0, std::memory_order_relaxed);
      sonare::test::g_allocation_attempts.store(0, std::memory_order_relaxed);
    }
  } failpoint_reset;
  clear_failpoint();

  auto project_copy_allocations = [](const Project& project) {
    sonare::test::AllocationGuard count_guard;
    Project copy = project;
    (void)copy;
    return count_guard.count();
  };
  auto project_and_midi_copy_allocations = [](const Project& project,
                                              const MidiContentStore& midi) {
    sonare::test::AllocationGuard count_guard;
    Project project_copy = project;
    MidiContentStore midi_copy = midi;
    (void)project_copy;
    (void)midi_copy;
    return count_guard.count();
  };

  auto run_copy_failure = [&](std::size_t fail_nth, bool with_midi_payload) {
    Fixture f = make_fixture();
    if (with_midi_payload) {
      f.history.midi_content().events[f.clip_id] = {{0.0, 0x1234u, 0u, 91u}};
      f.history.midi_content().sysex_payloads[91u] = {1, 2, 3, 4};
    }
    const std::string before_project = serialize(f.history);
    const MidiContentStore before_midi = f.history.midi_content();
    auto log = std::make_shared<ProbeLog>();

    OneCommandGenerator generator([&, log] {
      return std::make_unique<ProbeCommand>(
          1, log,
          [clip_id = f.clip_id, track_id = f.track_id](Project& project, MidiContentStore& store) {
            auto* track = project.find_track_mutable(track_id);
            if (track == nullptr) throw std::runtime_error("copy probe track missing");
            track->gain = 0.33f;
            store.events[clip_id] = {{0.0, 0x5678u, 0u, 91u}};
            store.sysex_payloads[91u] = {8, 9};
          },
          ProbeBehavior::kSuccess, 0, 0, false, fail_nth);
    });
    AssistRegistry registry;
    registry.register_generator(&generator);
    CompositionAssist assist(registry);
    ProjectView view(f.history.project(), f.history.midi_content(), "one-command");
    AssistRequest request;
    request.scope.track_ids.clear();
    AssistResult result = assist.run(view, request);
    clear_failpoint();

    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
    REQUIRE(result.diagnostics.slots_discarded == 1);
    REQUIRE(log->apply_count == 1);
    REQUIRE(log->rollback_count == 1);
    REQUIRE(log->external == 0);
    REQUIRE(serialize(f.history) == before_project);
    REQUIRE(f.history.midi_content() == before_midi);
  };

  {
    Fixture f = make_fixture();
    const std::size_t project_copy_count = project_copy_allocations(f.history.project());
    REQUIRE(project_copy_count > 0);
  }

  // The first allocation after apply is in accepted Project copy. The command
  // itself arms the one-shot failure only after candidate apply completes.
  run_copy_failure(1, false);

  Fixture midi_fixture = make_fixture();
  midi_fixture.history.midi_content().events[midi_fixture.clip_id] = {{0.0, 0x1234u, 0u, 91u}};
  midi_fixture.history.midi_content().sysex_payloads[91u] = {1, 2, 3, 4};
  const std::size_t project_copy_count = project_copy_allocations(midi_fixture.history.project());
  const std::size_t project_and_midi_copy_count = project_and_midi_copy_allocations(
      midi_fixture.history.project(), midi_fixture.history.midi_content());
  REQUIRE(project_copy_count > 0);
  REQUIRE(project_and_midi_copy_count > project_copy_count);
  clear_failpoint();

  const std::string before_project = serialize(midi_fixture.history);
  const MidiContentStore before_midi = midi_fixture.history.midi_content();
  auto log = std::make_shared<ProbeLog>();
  OneCommandGenerator generator([&, log] {
    return std::make_unique<ProbeCommand>(
        1, log,
        [clip_id = midi_fixture.clip_id](Project& project, MidiContentStore& store) {
          auto* track = project.find_track_mutable(1);
          if (track == nullptr) throw std::runtime_error("MIDI copy probe track missing");
          track->gain = 0.44f;
          store.events[clip_id] = {{0.0, 0x5678u, 0u, 91u}};
          store.sysex_payloads[91u] = {8, 9};
        },
        ProbeBehavior::kSuccess, 0, 0, false, project_copy_count + 1);
  });
  AssistRegistry registry;
  registry.register_generator(&generator);
  CompositionAssist assist(registry);
  ProjectView view(midi_fixture.history.project(), midi_fixture.history.midi_content(),
                   "one-command");
  AssistRequest request;
  AssistResult result = assist.run(view, request);
  clear_failpoint();

  REQUIRE(result.commands.empty());
  REQUIRE(result.diagnostics.status == AssistStatus::kDiscarded);
  REQUIRE(result.diagnostics.slots_discarded == 1);
  REQUIRE(log->apply_count == 1);
  REQUIRE(log->rollback_count == 1);
  REQUIRE(log->external == 0);
  REQUIRE(serialize(midi_fixture.history) == before_project);
  REQUIRE(midi_fixture.history.midi_content() == before_midi);
}

TEST_CASE("production audio transfer commands expose stable effect metadata", "[assist]") {
  constexpr sonare::arrangement::SourceId kSourceId = 42;
  constexpr sonare::arrangement::SourceId kOtherSourceId = 43;
  AudioContentStore audio;
  sonare::arrangement::AudioSourceSamples samples;
  samples.channels = {{0.1f, 0.2f, 0.3f}};
  samples.sample_rate = 48000.0;
  std::map<sonare::arrangement::SourceId, sonare::arrangement::AudioSourceSamples> contents;
  contents.emplace(kSourceId, samples);
  Project project;
  MidiContentStore midi;

  auto check_metadata = [&](const EditCommandPtr& command) {
    auto* effects = dynamic_cast<const EditCommandExternalEffects*>(command.get());
    REQUIRE(effects != nullptr);
    REQUIRE(effects->affects_audio_source(kSourceId));
    REQUIRE_FALSE(effects->affects_audio_source(kOtherSourceId));
    REQUIRE_FALSE(effects->affects_warp_source(7));
  };
  auto rollback_command = [&](const EditCommandPtr& command) {
    auto* rollback = dynamic_cast<EditCommandRollback*>(command.get());
    REQUIRE(rollback != nullptr);
    rollback->rollback_apply(project, midi);
  };

  EditCommandPtr store_command =
      sonare_project_make_store_audio_content_command(&audio, std::move(contents));
  REQUIRE(store_command != nullptr);
  check_metadata(store_command);
  REQUIRE(audio.sources.empty());
  REQUIRE(store_command->apply(project, midi));
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  check_metadata(store_command);
  rollback_command(store_command);
  REQUIRE(audio.sources.empty());
  check_metadata(store_command);

  REQUIRE(store_command->apply(project, midi));
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  EditCommandPtr store_inverse = store_command->invert(project, midi);
  REQUIRE(store_inverse != nullptr);
  check_metadata(store_inverse);
  REQUIRE(store_inverse->apply(project, midi));
  REQUIRE(audio.sources.empty());
  check_metadata(store_inverse);
  rollback_command(store_inverse);
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  check_metadata(store_inverse);

  // The removal factory has the opposite initial direction and shares the same
  // stable source-id metadata contract across its inverse.
  REQUIRE(store_inverse->apply(project, midi));
  REQUIRE(audio.sources.empty());
  rollback_command(store_inverse);
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  EditCommandPtr remove_command = sonare_project_make_remove_audio_content_command(
      &audio, std::vector<sonare::arrangement::SourceId>{kSourceId});
  REQUIRE(remove_command != nullptr);
  check_metadata(remove_command);
  REQUIRE(remove_command->apply(project, midi));
  REQUIRE(audio.sources.empty());
  rollback_command(remove_command);
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  check_metadata(remove_command);
  REQUIRE(remove_command->apply(project, midi));
  REQUIRE(audio.sources.empty());
  EditCommandPtr remove_inverse = remove_command->invert(project, midi);
  REQUIRE(remove_inverse != nullptr);
  check_metadata(remove_inverse);
  REQUIRE(remove_inverse->apply(project, midi));
  REQUIRE(audio.sources.find(kSourceId) != audio.sources.end());
  rollback_command(remove_inverse);
  REQUIRE(audio.sources.empty());
  check_metadata(remove_inverse);
}

// ===========================================================================
// Partial regen: scope = one track leaves out-of-scope tracks unchanged.
// ===========================================================================

TEST_CASE("assist partial regen leaves out-of-scope tracks unchanged", "[assist]") {
  // Two MIDI tracks, each with its own clip.
  Fixture f = make_fixture();
  TrackId track_a = f.track_id;

  // Second track + source + clip.
  auto add_track = std::make_unique<AddTrack>([] {
    Track t;
    t.name = "track-b";
    t.kind = Track::Kind::kMidi;
    return t;
  }());
  auto* add_track_ptr = add_track.get();
  REQUIRE(f.history.apply(std::move(add_track)));
  const TrackId track_b = add_track_ptr->allocated_id();

  MidiSourceRef ref;
  ref.name = "b-src";
  auto attach = std::make_unique<AttachMidiSource>(ref);
  auto* attach_ptr = attach.get();
  REQUIRE(f.history.apply(std::move(attach)));
  EditClip clip_b;
  clip_b.track_id = track_b;
  clip_b.source_id = attach_ptr->allocated_id();
  clip_b.length_ppq = 1920.0;
  REQUIRE(f.history.apply(std::make_unique<AddClip>(clip_b)));

  // Generate over BOTH tracks first so track B has content.
  MockNoteGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);
  CompositionAssist assist(registry);
  {
    ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
    AssistRequest req;
    req.seed = 10;
    AssistResult r = assist.run(view, req);
    REQUIRE(apply_result(&f.history, std::move(r)));
  }

  // Snapshot track B's content (its clip's events) before the scoped regen.
  const MidiContentStore store_before = f.history.midi_content();

  // Regenerate ONLY track A with a different seed.
  {
    ProjectView view(f.history.project(), f.history.midi_content(), kMockModuleId);
    AssistRequest req;
    req.seed = 20;
    req.scope.track_ids = {track_a};
    AssistResult r = assist.run(view, req);
    REQUIRE_FALSE(r.commands.empty());
    REQUIRE(apply_result(&f.history, std::move(r)));
  }

  // Track B's clip events are unchanged; track A's changed.
  // Identify each track's clip id.
  sonare::arrangement::ClipId clip_a_id = 0, clip_b_id = 0;
  for (const auto& c : f.history.project().clips()) {
    if (c.track_id == track_a) clip_a_id = c.id;
    if (c.track_id == track_b) clip_b_id = c.id;
  }
  REQUIRE(clip_a_id != 0);
  REQUIRE(clip_b_id != 0);
  REQUIRE(f.history.midi_content().events.at(clip_b_id) == store_before.events.at(clip_b_id));
  REQUIRE(f.history.midi_content().events.at(clip_a_id) != store_before.events.at(clip_a_id));
}

// ===========================================================================
// Multi-track batch: one run emits a coherent set (track add + MIDI clip +
// sidecar) without conflict.
// ===========================================================================

namespace {
// A generator that, in one run, adds a NEW track + source + clip and fills it,
// plus a sidecar — a coherent multi-command batch the caller applies atomically.
class BatchGenerator final : public INoteGenerator {
 public:
  const char* module_id() const noexcept override { return "batch"; }
  AssistResult generate(const ProjectView& view, const AssistRequest& request) override {
    AssistResult result;
    (void)request;
    // Add a new track.
    Track t;
    t.name = "assist-added";
    t.kind = Track::Kind::kMidi;
    result.commands.push_back(std::make_unique<AddTrack>(t));

    // Update a sidecar to record the batch (kept simple; uses the existing
    // track scope so the command is independent of allocated ids).
    AssistSidecar sc;
    sc.module_id = "batch";
    sc.schema_version = 1;
    sc.payload = {0x01};
    if (!view.tracks().empty()) sc.target_track_id = view.tracks().front().id;
    result.commands.push_back(std::make_unique<SetAssistSidecar>(sc));

    result.diagnostics.status = AssistStatus::kOk;
    return result;
  }
};
}  // namespace

TEST_CASE("assist multi-command batch applies as one coherent transaction", "[assist]") {
  Fixture f = make_fixture();
  const size_t tracks_before = f.history.project().tracks().size();
  const size_t sidecars_before = f.history.project().assist_sidecars().size();

  BatchGenerator gen;
  AssistRegistry registry;
  registry.register_generator(&gen);
  CompositionAssist assist(registry);
  ProjectView view(f.history.project(), f.history.midi_content(), "batch");
  AssistRequest req;

  AssistResult r = assist.run(view, req);
  REQUIRE(r.commands.size() == 2);
  REQUIRE(apply_result(&f.history, std::move(r)));

  REQUIRE(f.history.project().tracks().size() == tracks_before + 1);
  REQUIRE(f.history.project().assist_sidecars().size() == sidecars_before + 1);

  // The whole batch is one undoable transaction.
  REQUIRE(f.history.undo());
  REQUIRE(f.history.project().tracks().size() == tracks_before);
  REQUIRE(f.history.project().assist_sidecars().size() == sidecars_before);
}

// ===========================================================================
// STATIC LAYERING GUARD: rt/ and engine/ must NOT include "midi/assist/", and
// compiler/rt/engine must not reference AssistSidecar. Mirrors the host SDK guard.
// ===========================================================================

namespace {

// Returns every regular file under `dir` (recursively) with a C/C++ extension.
std::vector<std::filesystem::path> source_files(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  if (!fs::is_directory(dir)) return out;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension();
    if (ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".cc") out.push_back(entry.path());
  }
  return out;
}

bool file_contains(const std::filesystem::path& path, const std::string& needle) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("assist seam is not reachable from rt/ or engine/", "[assist]") {
  namespace fs = std::filesystem;
  const fs::path src = fs::path(__FILE__).parent_path().parent_path().parent_path() / "src";

  // rt/ and engine/ MUST NOT include midi/assist/ (the seam is control/offline
  // only and must never be pulled onto the RT path).
  for (const char* sub : {"rt", "engine"}) {
    const fs::path dir = src / sub;
    const auto files = source_files(dir);
    REQUIRE_FALSE(files.empty());
    for (const auto& f : files) {
      INFO("RT/engine file must not include midi/assist/: " << f.string());
      REQUIRE_FALSE(file_contains(f, "midi/assist/"));
    }
  }

  // compiler / rt / engine MUST NOT read AssistSidecar. The compiler lives
  // in arrangement/edit_compiler.*; rt/ and engine/ are scanned above.
  const std::vector<fs::path> no_sidecar_dirs = {src / "rt", src / "engine"};
  for (const auto& dir : no_sidecar_dirs) {
    for (const auto& f : source_files(dir)) {
      INFO("RT/engine file must not reference AssistSidecar: " << f.string());
      REQUIRE_FALSE(file_contains(f, "AssistSidecar"));
    }
  }
  for (const char* compiler_file : {"edit_compiler.h", "edit_compiler.cpp"}) {
    const fs::path cf = src / "arrangement" / compiler_file;
    REQUIRE(fs::is_regular_file(cf));
    INFO("compiler must not reference AssistSidecar: " << cf.string());
    REQUIRE_FALSE(file_contains(cf, "AssistSidecar"));
  }
}
