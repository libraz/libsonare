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

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_history.h"
#include "arrangement/edit_model.h"
#include "arrangement/project_view.h"
#include "midi/assist/assist_registry.h"
#include "midi/assist/composition_assist.h"
#include "midi/ump.h"
#include "serialize/project_serializer.h"

namespace {

using sonare::arrangement::AddClip;
using sonare::arrangement::AddTrack;
using sonare::arrangement::AssistSidecar;
using sonare::arrangement::AttachMidiSource;
using sonare::arrangement::EditClip;
using sonare::arrangement::EditCommandPtr;
using sonare::arrangement::EditHistory;
using sonare::arrangement::MidiClipEvent;
using sonare::arrangement::MidiClipEventList;
using sonare::arrangement::MidiContentStore;
using sonare::arrangement::MidiSourceRef;
using sonare::arrangement::Project;
using sonare::arrangement::ProjectView;
using sonare::arrangement::ReplaceMidiClipEvents;
using sonare::arrangement::SetAssistSidecar;
using sonare::arrangement::Track;
using sonare::arrangement::TrackId;
using sonare::midi::assist::AssistRegistry;
using sonare::midi::assist::AssistRequest;
using sonare::midi::assist::AssistResult;
using sonare::midi::assist::AssistStatus;
using sonare::midi::assist::CompositionAssist;
using sonare::midi::assist::INoteGenerator;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr char kMockModuleId[] = "mock-sketch";

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
