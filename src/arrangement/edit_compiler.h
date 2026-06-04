#pragma once

/// @file edit_compiler.h
/// @brief Control-thread compiler that turns a mutable arrangement Project into
///        an immutable, RT-readable CompiledTimeline snapshot.
///
/// Threading / ownership contract
/// ------------------------------
/// compile() is CONTROL-THREAD-ONLY. It READS a @ref Project (plus the MIDI and
/// audio content stores) and PRODUCES a fully-allocated, immutable
/// @ref CompiledTimeline value object. The realtime (RT) audio thread NEVER
/// reads a Project: it only ever sees the snapshot's prepared
/// engine::ClipSchedule / automation::AutomationLane / transport data, which is
/// handed over via the engine's existing direct-setter + RtPublisher paths
/// (set_clips / automation().set_lanes / set_markers / tempo+timesig). No RT
/// object pointers ever enter the model or the snapshot.
///
/// Determinism
/// -----------
/// compile() uses NO clock, NO random, NO date. Given the same Project + content
/// stores + CompileConfig, it produces an identical CompiledTimeline and a
/// bit-exact offline bounce within the same build. PPQ->frame conversion runs
/// through a transport::TempoMap built from the Project's tempo/time-signature
/// segments; sample-rate conversion uses the fixed @ref sonare::resample (r8brain
/// 24-bit) path so the baked audio is build-deterministic.
///
/// Diagnostics
/// -----------
/// compile() does NOT throw on bad input. It returns a @ref CompileResult that
/// carries diagnostics. An ERROR diagnostic (dangling/unavailable source,
/// invalid tempo/PPQ, overlap-policy violation) SUPPRESSES the timeline
/// (CompileResult::timeline stays empty). WARNING diagnostics (e.g. a graph /
/// mixer binding requested in a build where SONARE_WITH_GRAPH / SONARE_WITH_MIXING
/// is disabled) do NOT suppress the timeline: a valid CompiledTimeline is still
/// returned alongside the warnings.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"  // MidiContentStore
#include "arrangement/edit_model.h"
#include "automation/automation_lane.h"
#include "engine/clip_player.h"
#include "engine/realtime_engine.h"
#include "midi/midi_clip.h"  // midi::MidiClipSchedule
#include "mixing/api/scene.h"
#include "transport/marker.h"
#include "transport/tempo_map.h"

namespace sonare::arrangement {

/// @brief Compile-time project ABI version. Mirror of the C macro
///        @c SONARE_PROJECT_ABI_VERSION returned by
///        @c sonare_project_abi_version(). Bump on ANY flat POD layout change in
///        the project C ABI. A @c static_assert in the C-ABI bridge
///        (c_api/project_core.cpp) pins this to the macro so the two cannot
///        drift silently. Kept in the arrangement layer (not the public C
///        header) so internal C++ callers can reference it without including
///        sonare_c_project.h.
inline constexpr uint32_t kProjectAbiVersion = 1u;

// ===========================================================================
// Audio content store (decoded samples supplied by the caller before compile)
// ===========================================================================

/// Decoded audio for one registered source: deinterleaved float channels plus
/// the native sample rate. The arrangement model never opens files (the core
/// performs no I/O), so the host decodes audio out-of-band and registers it here
/// keyed by the source id BEFORE calling compile(). This mirrors how
/// @ref MidiContentStore carries MIDI payloads outside the project model.
struct AudioSourceSamples {
  /// Deinterleaved channels; each inner vector is one channel of equal length.
  std::vector<std::vector<float>> channels;
  /// Native sample rate of `channels` in Hz. When this differs from the project
  /// sample rate the compiler resamples to the project rate and bakes the
  /// result; RT never resamples.
  double sample_rate = 0.0;
};

/// Registry mapping source id -> decoded audio samples. Plain value data,
/// control-thread only. A clip whose audio source has no entry here compiles to
/// an ERROR diagnostic (dangling/unavailable), not a crash.
struct AudioContentStore {
  std::map<SourceId, AudioSourceSamples> sources;
  /// Optional pre-baked warped audio, keyed by warp_ref_id. The MIR/host layer
  /// creates these offline from a WarpMap and registers them before compile().
  /// The arrangement compiler can then select already-warped samples without
  /// depending on mir/ (which would create a dependency cycle).
  std::map<WarpRefId, AudioSourceSamples> warped_sources;

  const AudioSourceSamples* find(SourceId id) const noexcept {
    const auto it = sources.find(id);
    return it == sources.end() ? nullptr : &it->second;
  }

  const AudioSourceSamples* find_warped(WarpRefId id) const noexcept {
    if (id == 0) return nullptr;
    const auto it = warped_sources.find(id);
    return it == warped_sources.end() ? nullptr : &it->second;
  }
};

// ===========================================================================
// MIDI clip schedule
// ===========================================================================
//
// CompiledTimeline carries midi::MidiClipSchedule values from
// src/midi/midi_clip.h. The compiler bakes a Project's MIDI clip events (from
// the MidiContentStore) into absolute render-frame UMP events via the TempoMap.

// ===========================================================================
// Graph / mixer binding requests (value descriptors; caller fulfils RT wiring)
// ===========================================================================

/// Reserved graph-replacement request. In this layer nothing in the Project drives a
/// graph swap, so `requested` is always false. The struct exists so the
/// CompiledTimeline shape is stable; later phases set `requested` and populate a
/// node/connection descriptor that the caller turns into a graph::Graph for
/// RealtimeEngine::swap_graph (under SONARE_WITH_GRAPH).
struct GraphRequest {
  bool requested = false;
  /// True when a graph swap was requested but this build lacks SONARE_WITH_GRAPH.
  bool unavailable_in_build = false;
};

/// One Track -> mixing Strip binding (by the Scene Strip's string id). The
/// compiler resolves these from Track::channel_strip_ref; the caller turns each
/// into a live mixing::ChannelStrip and calls RealtimeEngine::bind_mixing_strip
/// (under SONARE_WITH_MIXING). The compiler cannot own RT ChannelStrip objects,
/// so it only emits the binding intent as value data.
struct MixerStripBinding {
  TrackId track_id = 0;
  std::string strip_id;  // mixing::api::Strip::id

  bool operator==(const MixerStripBinding& o) const {
    return track_id == o.track_id && strip_id == o.strip_id;
  }
};

struct MixerAutomationBinding {
  TrackId track_id = 0;
  automation::AutomationLane lane;

  bool operator==(const MixerAutomationBinding& o) const {
    return track_id == o.track_id && lane.target_param_id() == o.lane.target_param_id() &&
           lane.points() == o.lane.points();
  }
};

/// Mixer scene/binding request: a copy of the Project's Scene plus the resolved
/// Track->Strip bindings. The Scene is pure data and is always carried (even when
/// mixing runtime is disabled); the actual bind happens in the caller under
/// SONARE_WITH_MIXING.
struct MixerRequest {
  mixing::api::Scene scene;
  std::vector<MixerStripBinding> bindings;
  std::vector<MixerAutomationBinding> automation_bindings;
  /// True when bindings exist but this build lacks SONARE_WITH_MIXING.
  bool unavailable_in_build = false;
};

// ===========================================================================
// Latency / PDC summary
// ===========================================================================

/// Plugin-delay-compensation summary. In the base timeline there are no host instruments or
/// latency-bearing inserts, so total latency is 0. The field is reserved so
/// Host-instrument latency can populate it without changing the
/// CompiledTimeline shape.
struct LatencySummary {
  /// Total reported latency in samples at the project sample rate.
  int64_t total_latency_samples = 0;
  /// Per-source latency contribution (source id -> samples). Empty without host instruments.
  std::map<SourceId, int64_t> per_source_samples;
};

// ===========================================================================
// CompiledTimeline (immutable value object; fully allocated on control thread)
// ===========================================================================

/// The immutable RT snapshot produced by compile(). Every buffer it references
/// is owned within it (ClipSchedule::storage holds the baked audio; marker name
/// strings are owned by marker_names and the transport::Marker.name pointers
/// point into that stable storage). Safe to hand to the RT engine: nothing here
/// reaches back into a Project.
struct CompiledTimeline {
  /// Audio clip schedules ready for RealtimeEngine::set_clips. Each schedule's
  /// `storage` shared_ptr owns the baked (possibly resampled) audio and the
  /// `buffer.channels` point into it.
  std::vector<engine::ClipSchedule> audio_clips;

  /// MIDI clip schedules baked from the Project's MIDI clips + MidiContentStore
  /// Each carries absolute render-frame UMP events.
  std::vector<midi::MidiClipSchedule> midi_clips;

  /// Automation lanes ready for RealtimeEngine::automation().set_lanes.
  std::vector<automation::AutomationLane> automation_lanes;

  /// Graph replacement request. Currently false unless a future Project field
  /// requests an engine graph swap.
  GraphRequest graph;

  /// Mixer scene + Track->Strip binding request.
  MixerRequest mixer;

  /// Tempo / time-signature segments (full segment vectors copied from the
  /// Project). apply_to_engine() installs the full vectors into RealtimeEngine's
  /// TempoMap before publishing clips and automation.
  std::vector<transport::TempoSegment> tempo_segments;
  std::vector<transport::TimeSignatureSegment> time_signatures;

  /// Markers for RealtimeEngine::set_markers. transport::Marker.name is a
  /// NON-OWNING const char*; this snapshot OWNS the strings in `marker_names`
  /// (a stable deque-like vector that is never resized after build) and each
  /// markers[i].name points into marker_names[i]. The pointers stay valid for
  /// the whole lifetime of this CompiledTimeline. Because copying a
  /// CompiledTimeline would invalidate those pointers, copies re-point them; see
  /// the copy ctor / assignment.
  std::vector<transport::Marker> markers;
  std::vector<std::string> marker_names;

  /// Latency / PDC summary (clip-stage only).
  LatencySummary latency;

  CompiledTimeline() = default;

  // Re-point marker name pointers into THIS object's marker_names after a copy,
  // so a copied snapshot never dangles into the source's storage. The
  // marker/marker_names vectors are kept index-aligned by build().
  CompiledTimeline(const CompiledTimeline& other) { copy_from(other); }
  CompiledTimeline& operator=(const CompiledTimeline& other) {
    if (this != &other) copy_from(other);
    return *this;
  }
  CompiledTimeline(CompiledTimeline&&) = default;
  CompiledTimeline& operator=(CompiledTimeline&&) = default;

 private:
  void copy_from(const CompiledTimeline& other);
};

// ===========================================================================
// Diagnostics
// ===========================================================================

struct Diagnostic {
  enum class Code : uint32_t {
    kOk = 0,
    kDanglingSourceRef = 1,   // clip references a missing/unavailable source
    kClipOverlap = 2,         // overlap-policy violation
    kInvalidTempo = 3,        // negative/zero bpm or non-monotonic PPQ
    kInvalidPpq = 4,          // negative start/length/offset
    kUnsupportedGraph = 5,    // graph requested but SONARE_WITH_GRAPH off
    kUnsupportedMixing = 6,   // mixer bind requested but SONARE_WITH_MIXING off
    kEmptyAudioSource = 7,    // source registered but has no samples
    kInvalidSampleRate = 8,   // project / source sample rate invalid
    kSourceKindMismatch = 9,  // clip source kind does not match its track kind
    kMidiClipNoInstrument =
        10,  // project has MIDI clips; bounce is silent unless an instrument is bound
  };
  // Severity ordinals are a FROZEN WIRE VALUE: they are exposed numerically as
  // SonareProjectDiagnostic.severity through the C ABI (see
  // c_api/project_core.cpp). This ordering (kError=0, kWarning=1) differs from
  // the canonical sonare::Diagnostic ordering (Info<Warning<Error) and from
  // serialize::DiagnosticSeverity (which is inverted); the static_asserts below
  // pin it so a reorder is caught at compile time before it can change a byte on
  // the wire. Do NOT renumber.
  enum class Severity : uint32_t {
    kError = 0,
    kWarning = 1,
  };
  static_assert(static_cast<uint32_t>(Severity::kError) == 0u,
                "arrangement Diagnostic kError ordinal is a frozen C-ABI wire value");
  static_assert(static_cast<uint32_t>(Severity::kWarning) == 1u,
                "arrangement Diagnostic kWarning ordinal is a frozen C-ABI wire value");

  Code code = Code::kOk;
  Severity severity = Severity::kError;
  /// Affected clip / track / source id (0 when not applicable).
  uint32_t target_id = 0;
  std::string message;
};

struct CompileResult {
  /// Present only when compilation produced NO error diagnostics (warnings are
  /// allowed alongside a present timeline).
  std::optional<CompiledTimeline> timeline;
  std::vector<Diagnostic> diagnostics;

  bool has_errors() const noexcept {
    for (const auto& d : diagnostics) {
      if (d.severity == Diagnostic::Severity::kError) return true;
    }
    return false;
  }
};

// ===========================================================================
// CompileConfig
// ===========================================================================

/// Deterministic compile options.
struct CompileConfig {
  /// When true (default) clips on the same track that overlap under the
  /// project's kDisallow policy emit an ERROR. When false the overlap is allowed
  /// (clips simply sum on the RT timeline) and no diagnostic is emitted.
  bool enforce_overlap_policy = true;

  /// Reported latency (in samples, at the project sample rate) of the host
  /// instrument node that will render the project.s MIDI (a single
  /// instrument seam — see midi::MidiInstrument / RealtimeEngine::
  /// set_midi_instrument). The host queries its registered instrument's
  /// latency_samples() and passes it here so the compiler folds it into the
  /// CompiledTimeline PDC / LatencySummary. 0 (default) means no instrument
  /// latency. Only contributes when the project actually has MIDI clips; the
  /// per-source entry is recorded under each MIDI source id.
  int instrument_latency_samples = 0;
};

// ===========================================================================
// compile / apply
// ===========================================================================

/// Compiles `project` into a CompiledTimeline. Reads `midi` for clip event data
/// and `audio` for clip sample data. Never throws; returns diagnostics instead.
CompileResult compile(const Project& project, const MidiContentStore& midi,
                      const AudioContentStore& audio, const CompileConfig& config = {});

/// Installs a CompiledTimeline into a RealtimeEngine via the engine's prescribed
/// CONTROL-THREAD direct-setter order: tempo/time-signature -> markers ->
/// automation lanes -> clips (-> graph swap / mixer bind under their flags).
/// These are all direct-setter / publisher installs, NOT push_command.
///
/// The graph request has no Project authoring surface yet; the mixer binding is
/// value-only (the caller wires live ChannelStrips), so this helper does not
/// call bind_mixing_strip — it leaves that to the caller, matching the
/// "compiler cannot own RT objects" rule.
void apply_to_engine(const CompiledTimeline& timeline, engine::RealtimeEngine& engine);

}  // namespace sonare::arrangement
