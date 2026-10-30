// SONARE_WASM_EXCEPTION_UNWIND: release staged edits and history on exceptions before publication.
#include "editing/vocal_edit/session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/session_internal.h"
#include "editing/vocal_edit/state_codec.h"
#include "effects/formant_warp.h"
#include "util/db.h"
#include "util/insertion_sort.h"
#include "util/sha256.h"

namespace sonare::editing::vocal_edit {
namespace detail {

bool contains_id(const std::set<VocalNoteId>& ids, VocalNoteId id) {
  return ids.find(id) != ids.end();
}

}  // namespace detail

using namespace detail;

namespace {

std::atomic<uint64_t> g_next_session_epoch{1};

VocalSessionId make_session_id(const SourceDescriptor& descriptor, VocalSessionEpoch epoch) {
  VocalSessionId id{};
  // Entropy-seeded so lineage ids differ across processes even if random_device is deterministic.
  std::random_device entropy;
  for (size_t offset = 0; offset < id.size();) {
    const auto word = entropy();
    for (size_t byte = 0; byte < sizeof(word) && offset < id.size(); ++byte, ++offset) {
      id[offset] = static_cast<uint8_t>(word >> (byte * 8));
    }
  }
  for (size_t i = 0; i < id.size(); ++i) id[i] ^= descriptor.digest[i];
  for (size_t i = 0; i < sizeof(epoch) && i < id.size(); ++i) {
    id[i] ^= static_cast<uint8_t>(epoch >> (i * 8));
  }
  const auto timestamp =
      static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  for (size_t i = 0; i < sizeof(timestamp) && i + sizeof(epoch) < id.size(); ++i) {
    id[i + sizeof(epoch)] ^= static_cast<uint8_t>(timestamp >> (i * 8));
  }
  bool all_zero = true;
  for (const auto byte : id) all_zero = all_zero && byte == 0;
  if (all_zero) id.back() = 1;
  return id;
}

VocalSessionEpoch issue_session_epoch() {
  uint64_t current = g_next_session_epoch.load(std::memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == std::numeric_limits<uint64_t>::max()) {
      throw VocalEditException(VocalReason::kCounterExhausted, "session epoch exhausted", "epoch");
    }
    if (g_next_session_epoch.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
      return current;
    }
  }
}

template <typename Impl>
void require_session_alive(const std::shared_ptr<Impl>& impl) {
  if (!impl || !impl->session_alive) invalid_state("session", "session is not alive");
}

[[noreturn]] void draft_not_active() { invalid_state("draft", "draft is not active"); }

template <typename Impl>
void require_draft_active(bool active, const std::shared_ptr<Impl>& impl) {
  if (!active || !impl || !impl->session_alive) draft_not_active();
}

template <typename Impl, typename State>
void require_draft_active(bool active, const std::shared_ptr<Impl>& impl,
                          const std::shared_ptr<State>& candidate) {
  if (!active || !impl || !impl->session_alive || !candidate) draft_not_active();
}

template <typename Impl>
void set_formant_capabilities(Impl& impl) {
  impl.capabilities.min_formant_shift_semitones = kMinFormantShiftSemitones;
  impl.capabilities.max_formant_shift_semitones = kMaxFormantShiftSemitones;
}

[[noreturn]] void revision_conflict(const std::string& field, uint64_t expected, uint64_t actual) {
  throw VocalEditException(VocalReason::kRevisionConflict, "revision or generation conflict", field,
                           std::to_string(expected), std::to_string(actual));
}

bool same_target(const VocalPitchTarget& lhs, const VocalPitchTarget& rhs) {
  if (lhs.mode != rhs.mode || lhs.center_midi != rhs.center_midi ||
      lhs.points.size() != rhs.points.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.points.size(); ++i) {
    if (lhs.points[i].source_sample != rhs.points[i].source_sample ||
        lhs.points[i].target_midi != rhs.points[i].target_midi) {
      return false;
    }
  }
  return true;
}

bool same_pitch_except_target(const VocalPitchEdit& lhs, const VocalPitchEdit& rhs) {
  return lhs.amount == rhs.amount && lhs.speed_ms == rhs.speed_ms &&
         lhs.max_correction_semitones == rhs.max_correction_semitones &&
         lhs.transpose_semitones == rhs.transpose_semitones && lhs.drift_scale == rhs.drift_scale &&
         lhs.vibrato_scale == rhs.vibrato_scale;
}

bool same_edit_content(const VocalNoteEdit& lhs, const VocalNoteEdit& rhs) {
  return same_pitch_except_target(lhs.pitch, rhs.pitch) &&
         same_target(lhs.pitch.target, rhs.pitch.target) &&
         lhs.destination_start_sample == rhs.destination_start_sample &&
         lhs.destination_length_samples == rhs.destination_length_samples &&
         lhs.gain_db == rhs.gain_db && lhs.muted == rhs.muted &&
         lhs.amplitude_envelope == rhs.amplitude_envelope && lhs.formant.mode == rhs.formant.mode &&
         lhs.formant.shift_semitones == rhs.formant.shift_semitones;
}

bool same_note_content(const VocalNote& lhs, const VocalNote& rhs) {
  return lhs.id == rhs.id && lhs.source_range.start == rhs.source_range.start &&
         lhs.source_range.end == rhs.source_range.end &&
         lhs.analysis_frame_start == rhs.analysis_frame_start &&
         lhs.analysis_frame_end == rhs.analysis_frame_end && lhs.has_pitch == rhs.has_pitch &&
         lhs.centre_midi == rhs.centre_midi && lhs.median_hz == rhs.median_hz &&
         lhs.f0_stability == rhs.f0_stability && same_edit_content(lhs.edit, rhs.edit);
}

bool same_transition(const PitchTransition& lhs, const PitchTransition& rhs) {
  return lhs.left_note_id == rhs.left_note_id && lhs.right_note_id == rhs.right_note_id &&
         lhs.left_window_samples == rhs.left_window_samples &&
         lhs.right_window_samples == rhs.right_window_samples && lhs.strength == rhs.strength &&
         lhs.curve == rhs.curve;
}

void append_note_ranges(const VocalNote& note, std::vector<SampleRange>& ranges) {
  ranges.push_back(note.source_range);
  ranges.push_back({note.edit.destination_start_sample,
                    checked_end(note.edit.destination_start_sample,
                                note.edit.destination_length_samples, "destination")});
}

/// Every sample either state can render differently: changed notes, plus both endpoint notes of
/// any transition that changed or touches a changed note. A bridge bends the whole neighbour
/// note's plan, and a plan change re-synthesizes that note end to end.
std::vector<SampleRange> dirty_ranges_for_states(const VocalEditState& old_state,
                                                 const VocalEditState& new_state,
                                                 int64_t output_length) {
  std::map<VocalNoteId, const VocalNote*> old_notes;
  std::map<VocalNoteId, const VocalNote*> new_notes;
  for (const auto& note : old_state.notes) old_notes.emplace(note.id, &note);
  for (const auto& note : new_state.notes) new_notes.emplace(note.id, &note);
  std::set<VocalNoteId> changed;
  for (const auto& entry : old_notes) {
    const auto other = new_notes.find(entry.first);
    if (other == new_notes.end() || !same_note_content(*entry.second, *other->second)) {
      changed.insert(entry.first);
    }
  }
  for (const auto& entry : new_notes) {
    if (old_notes.find(entry.first) == old_notes.end()) changed.insert(entry.first);
  }
  const auto transition_in = [](const std::vector<PitchTransition>& transitions,
                                const PitchTransition& wanted) {
    return std::find_if(transitions.begin(), transitions.end(), [&](const PitchTransition& value) {
             return same_transition(value, wanted);
           }) != transitions.end();
  };
  std::set<VocalNoteId> affected = changed;
  const auto collect = [&](const std::vector<PitchTransition>& transitions,
                           const std::vector<PitchTransition>& other) {
    for (const auto& transition : transitions) {
      if (!transition_in(other, transition) || contains_id(changed, transition.left_note_id) ||
          contains_id(changed, transition.right_note_id)) {
        affected.insert(transition.left_note_id);
        affected.insert(transition.right_note_id);
      }
    }
  };
  collect(old_state.transitions, new_state.transitions);
  collect(new_state.transitions, old_state.transitions);

  std::vector<SampleRange> ranges;
  for (const auto id : affected) {
    const auto old_it = old_notes.find(id);
    const auto new_it = new_notes.find(id);
    if (old_it != old_notes.end()) append_note_ranges(*old_it->second, ranges);
    if (new_it != new_notes.end()) append_note_ranges(*new_it->second, ranges);
  }
  for (auto& range : ranges) {
    range.start = std::clamp<int64_t>(range.start, 0, output_length);
    range.end = std::clamp<int64_t>(range.end, 0, output_length);
  }
  detail::normalize_sample_ranges(ranges);
  return ranges;
}

uint64_t state_storage_bytes(const VocalEditState& state) {
  uint64_t bytes = sizeof(VocalEditState);
  const auto add = [&bytes](uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - bytes) {
      throw VocalEditException(VocalReason::kCounterExhausted, "state size counter exhausted",
                               "history");
    }
    bytes += value;
  };
  const auto add_array = [&add](size_t count, size_t element_size) {
    if (element_size != 0 && count > std::numeric_limits<uint64_t>::max() / element_size) {
      throw VocalEditException(VocalReason::kCounterExhausted, "state size counter exhausted",
                               "history");
    }
    add(static_cast<uint64_t>(count) * static_cast<uint64_t>(element_size));
  };
  for (const auto& note : state.notes) {
    add_array(note.edit.pitch.target.points.capacity(), sizeof(VocalPitchPoint));
    add_array(note.edit.amplitude_envelope.capacity(), sizeof(float));
  }
  add_array(state.notes.capacity(), sizeof(VocalNote));
  add_array(state.transitions.capacity(), sizeof(PitchTransition));
  return bytes;
}

uint64_t retained_history_bytes(const std::vector<std::shared_ptr<const VocalEditState>>& history,
                                size_t current_index) {
  if (current_index >= history.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "history index is outside the retained state list", "history");
  }
  uint64_t bytes = 0;
  for (size_t index = 0; index < history.size(); ++index) {
    // The live state is not retained history, so it does not count against the budget.
    if (index == current_index) continue;
    const uint64_t state_bytes = state_storage_bytes(*history[index]);
    if (state_bytes > std::numeric_limits<uint64_t>::max() - bytes) {
      throw VocalEditException(VocalReason::kCounterExhausted, "history size counter exhausted",
                               "history");
    }
    bytes += state_bytes;
  }
  return bytes;
}

}  // namespace
struct VocalEditSession::Impl {
  Audio source;
  VocalSessionId session_id{};
  SourceDescriptor source_descriptor{};
  std::shared_ptr<const VocalAnalysisData> analysis;
  VocalCapabilities capabilities{};
  RenderSettings render_settings{};
  int64_t output_length_samples = 0;
  VocalSessionLimits limits{};
  VocalSessionEpoch epoch = 0;
  VocalRevision revision = kInitialVocalRevision;
  VocalDraftId next_draft_id = 1;
  uint64_t next_note_id = 1;
  std::shared_ptr<std::atomic<uint32_t>> render_job_count =
      std::make_shared<std::atomic<uint32_t>>(0);
  std::shared_ptr<VocalRenderCache> cache;
  std::shared_ptr<const VocalEditState> committed;
  std::vector<std::shared_ptr<const VocalEditState>> history;
  size_t history_index = 0;
  bool draft_open = false;
  bool session_alive = true;
};

namespace {

std::vector<CompiledPitchPlan> evaluate_state(const VocalAnalysisData& analysis,
                                              const VocalEditState& state,
                                              const RenderSettings& render_settings,
                                              uint32_t source_sample_rate,
                                              const std::vector<VocalNoteId>& requested) {
  std::set<VocalNoteId> wanted(requested.begin(), requested.end());
  const bool all = requested.empty();
  const auto compiled = compile_pitch_plans(analysis, state, render_settings, source_sample_rate);
  if (!all && compiled.size() < wanted.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "pitch evaluation references an unknown note", "note_id");
  }
  if (all) return compiled;
  std::vector<CompiledPitchPlan> plans;
  plans.reserve(wanted.size());
  for (const auto& plan : compiled) {
    if (contains_id(wanted, plan.note_id)) plans.push_back(plan);
  }
  if (plans.size() != wanted.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "pitch evaluation references an unknown note", "note_id");
  }
  return plans;
}

}  // namespace

VocalEditSession VocalEditSession::create(Audio source, const VocalSessionCreateOptions& options) {
  const SourceDescriptor descriptor = describe_source(source);
  VocalAnalysisData analysis = options.analysis.has_value()
                                   ? validate_analysis(source, *options.analysis)
                                   : analyze_vocal(source, options.analysis_options);
  const int64_t output_length =
      options.output_length_samples == 0 ? descriptor.sample_count : options.output_length_samples;
  if (output_length < descriptor.sample_count || output_length <= 0) {
    invalid("output_length_samples", "must cover the source audio");
  }
  if (!std::isfinite(options.render_settings.edge_fade_ms) ||
      options.render_settings.edge_fade_ms < 0.0 ||
      !std::isfinite(options.render_settings.vibrato_cutoff_hz) ||
      options.render_settings.vibrato_cutoff_hz <= 0.0) {
    invalid("render_settings", "render settings are malformed");
  }
  if (options.limits.max_render_jobs == 0) invalid("max_render_jobs", "must be non-zero");
  const uint64_t epoch = issue_session_epoch();

  auto impl = std::make_shared<Impl>();
  impl->source = std::move(source);
  impl->source_descriptor = descriptor;
  impl->session_id = make_session_id(descriptor, epoch);
  impl->analysis = std::make_shared<const VocalAnalysisData>(std::move(analysis));
  set_formant_capabilities(*impl);
  impl->render_settings = options.render_settings;
  impl->output_length_samples = output_length;
  impl->limits = options.limits;
  impl->cache = make_vocal_render_cache(options.limits.max_cache_bytes);
  impl->epoch = epoch;
  auto initial = std::make_shared<VocalEditState>();
  initial->notes = extract_vocal_notes(impl->source, *impl->analysis);
  if (initial->notes.size() > std::numeric_limits<VocalNoteId>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "note ID counter exhausted",
                             "note_id");
  }
  impl->next_note_id = static_cast<uint64_t>(initial->notes.size()) + 1;
  validate_vocal_edit_state(*initial, *impl->analysis, descriptor.sample_count, output_length,
                            impl->render_settings);
  impl->committed = std::shared_ptr<const VocalEditState>(initial);
  impl->history.push_back(impl->committed);
  return VocalEditSession(std::move(impl));
}

VocalEditSession VocalEditSession::restore(Audio source, const std::vector<uint8_t>& bytes,
                                           const VocalSessionLimits& runtime_limits) {
  if (runtime_limits.max_render_jobs == 0) invalid("max_render_jobs", "must be non-zero");
  const VocalPersistedState persisted = decode_vocal_state(bytes);
  const SourceDescriptor descriptor = describe_source(source);
  const bool source_matches = descriptor.sample_rate == persisted.source.sample_rate &&
                              descriptor.sample_count == persisted.source.sample_count &&
                              descriptor.digest == persisted.source.digest;
  if (!source_matches) {
    throw VocalEditException(VocalReason::kSourceMismatch,
                             "state source does not match supplied audio", "source",
                             util::sha256_digest_hex(persisted.source.digest),
                             util::sha256_digest_hex(descriptor.digest));
  }
  const uint64_t maximum_next_id =
      static_cast<uint64_t>(std::numeric_limits<VocalNoteId>::max()) + 1u;
  if (persisted.next_note_id == 0 || persisted.next_note_id > maximum_next_id) {
    throw VocalEditException(VocalReason::kCounterExhausted,
                             "note ID counter is outside its valid high-water range", "note_id");
  }
  if (persisted.output_length_samples < descriptor.sample_count ||
      persisted.output_length_samples <= 0) {
    invalid("output_length_samples", "must cover the source audio");
  }
  const Sha256Digest stored_analysis_digest = persisted.analysis.digest;
  const std::vector<float> stored_amplitude = persisted.analysis.amplitude;
  VocalAnalysisData analysis_input = persisted.analysis;
  analysis_input.amplitude.clear();
  VocalAnalysisData analysis = validate_analysis(source, std::move(analysis_input));
  if (!stored_amplitude.empty() && stored_amplitude.size() != analysis.amplitude.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "analysis amplitude length does not match its grid",
                             "analysis.amplitude");
  }
  if (!stored_amplitude.empty() &&
      !std::equal(stored_amplitude.begin(), stored_amplitude.end(), analysis.amplitude.begin(),
                  [](float lhs, float rhs) {
                    uint32_t left_bits = 0;
                    uint32_t right_bits = 0;
                    std::memcpy(&left_bits, &lhs, sizeof(left_bits));
                    std::memcpy(&right_bits, &rhs, sizeof(right_bits));
                    return left_bits == right_bits;
                  })) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "analysis amplitude does not match supplied source",
                             "analysis.amplitude");
  }
  if (stored_analysis_digest != analysis.digest) {
    throw VocalEditException(
        VocalReason::kInvalidState, "analysis digest does not match its payload", "analysis.digest",
        util::sha256_digest_hex(stored_analysis_digest), util::sha256_digest_hex(analysis.digest));
  }
  validate_vocal_edit_state(persisted.edit_state, analysis, descriptor.sample_count,
                            persisted.output_length_samples, persisted.render_settings);
  VocalNoteId maximum_note_id = 0;
  for (const auto& note : persisted.edit_state.notes)
    maximum_note_id = std::max(maximum_note_id, note.id);
  if (persisted.next_note_id <= static_cast<uint64_t>(maximum_note_id)) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "note ID high-water is behind persisted notes", "note_id");
  }
  const uint64_t epoch = issue_session_epoch();
  auto impl = std::make_shared<Impl>();
  impl->source = std::move(source);
  impl->source_descriptor = descriptor;
  impl->session_id = persisted.session_id;
  impl->analysis = std::make_shared<const VocalAnalysisData>(std::move(analysis));
  impl->render_settings = persisted.render_settings;
  impl->output_length_samples = persisted.output_length_samples;
  impl->limits = runtime_limits;
  impl->cache = make_vocal_render_cache(runtime_limits.max_cache_bytes);
  impl->epoch = epoch;
  impl->revision = persisted.committed_revision;
  impl->next_note_id = persisted.next_note_id;
  set_formant_capabilities(*impl);
  impl->committed = std::make_shared<const VocalEditState>(persisted.edit_state);
  impl->history.push_back(impl->committed);
  return VocalEditSession(std::move(impl));
}

VocalEditSession::~VocalEditSession() {
  if (impl_) {
    impl_->session_alive = false;
    impl_->draft_open = false;
  }
}

VocalEditSession::VocalEditSession(VocalEditSession&& other) noexcept
    : impl_(std::move(other.impl_)) {}

VocalEditSession& VocalEditSession::operator=(VocalEditSession&& other) noexcept {
  if (this == &other) return *this;
  if (impl_) {
    impl_->session_alive = false;
    impl_->draft_open = false;
  }
  impl_ = std::move(other.impl_);
  return *this;
}

std::vector<VocalNote> VocalEditSession::notes() const {
  require_session_alive(impl_);
  return impl_->committed->notes;
}

VocalAnalysisData VocalEditSession::analysis() const {
  require_session_alive(impl_);
  return *impl_->analysis;
}

SourceDescriptor VocalEditSession::source_descriptor() const {
  require_session_alive(impl_);
  return impl_->source_descriptor;
}

VocalCapabilities VocalEditSession::capabilities() const noexcept {
  if (!impl_ || !impl_->session_alive) return {};
  return impl_->capabilities;
}

std::vector<PitchTransition> VocalEditSession::transitions() const {
  require_session_alive(impl_);
  return impl_->committed->transitions;
}

VocalStateToken VocalEditSession::token() const {
  require_session_alive(impl_);
  return {impl_->epoch, impl_->revision, 0, 0};
}

int64_t VocalEditSession::output_length_samples() const {
  require_session_alive(impl_);
  return impl_->output_length_samples;
}

const RenderSettings& VocalEditSession::render_settings() const {
  require_session_alive(impl_);
  return impl_->render_settings;
}

std::unique_ptr<VocalEditDraft> VocalEditSession::begin_edit(VocalRevision expected_revision) {
  require_session_alive(impl_);
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->draft_open) invalid_state("draft", "only one draft may be open");
  if (impl_->next_draft_id == 0) {
    throw VocalEditException(VocalReason::kCounterExhausted, "draft ID counter exhausted",
                             "draft_id");
  }
  auto draft = std::unique_ptr<VocalEditDraft>(new VocalEditDraft(impl_));
  draft->candidate_ = std::make_shared<VocalEditState>(*impl_->committed);
  draft->draft_token_ = {impl_->epoch, impl_->revision, impl_->next_draft_id, 1};
  draft->local_next_note_id_ = impl_->next_note_id;
  impl_->next_draft_id = impl_->next_draft_id == std::numeric_limits<VocalDraftId>::max()
                             ? 0
                             : impl_->next_draft_id + 1;
  impl_->draft_open = true;
  return draft;
}

std::shared_ptr<const VocalRenderSnapshot> VocalEditSession::capture_render_snapshot() const {
  require_session_alive(impl_);
  auto data = std::make_shared<VocalRenderSnapshotData>();
  data->source = impl_->source;
  data->source_descriptor = impl_->source_descriptor;
  data->analysis = impl_->analysis;
  data->state = impl_->committed;
  data->output_length_samples = impl_->output_length_samples;
  data->render_settings = impl_->render_settings;
  data->token = token();
  data->cache = impl_->cache;
  data->render_job_count = impl_->render_job_count;
  data->max_render_jobs = impl_->limits.max_render_jobs;
  return std::make_shared<VocalRenderSnapshot>(std::move(data));
}

std::vector<CompiledPitchPlan> VocalEditSession::evaluate_pitch(
    const std::vector<VocalNoteId>& note_ids) const {
  require_session_alive(impl_);
  return evaluate_state(*impl_->analysis, *impl_->committed, impl_->render_settings,
                        impl_->source_descriptor.sample_rate, note_ids);
}

double VocalEditSession::source_sample_to_destination_sample(VocalNoteId note_id,
                                                             double source_sample) const {
  require_session_alive(impl_);
  const auto& note = impl_->committed->notes[find_note(*impl_->committed, note_id)];
  return ::sonare::editing::vocal_edit::source_sample_to_destination_sample(note, source_sample);
}

double VocalEditSession::destination_sample_to_source_sample(VocalNoteId note_id,
                                                             double destination_sample) const {
  require_session_alive(impl_);
  const auto& note = impl_->committed->notes[find_note(*impl_->committed, note_id)];
  return ::sonare::editing::vocal_edit::destination_sample_to_source_sample(note,
                                                                            destination_sample);
}

StateChangeResult VocalEditSession::undo(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  require_session_alive(impl_);
  if (impl_->draft_open) invalid_state("draft", "undo is not allowed while a draft is open");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->history_index == 0) invalid_state("history", "nothing to undo");
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  const auto old_state = impl_->committed;
  const size_t target_index = impl_->history_index - 1;
  const auto target_state = impl_->history[target_index];
  // Compute all potentially-throwing work before publishing the new model.
  // An allocation failure must leave both the visible state and history
  // cursor untouched.
  auto dirty = dirty_ranges_for_states(*old_state, *target_state, impl_->output_length_samples);
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);
  impl_->history_index = target_index;
  impl_->committed = target_state;
  impl_->revision = next_revision;
  return result;
}

StateChangeResult VocalEditSession::redo(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  require_session_alive(impl_);
  if (impl_->draft_open) invalid_state("draft", "redo is not allowed while a draft is open");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->history_index + 1 >= impl_->history.size())
    invalid_state("history", "nothing to redo");
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  const auto old_state = impl_->committed;
  const size_t target_index = impl_->history_index + 1;
  const auto target_state = impl_->history[target_index];
  // See undo(): build the dirty result before changing the cursor or model.
  auto dirty = dirty_ranges_for_states(*old_state, *target_state, impl_->output_length_samples);
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);
  impl_->history_index = target_index;
  impl_->committed = target_state;
  impl_->revision = next_revision;
  return result;
}

bool VocalEditSession::can_undo() const noexcept {
  return impl_ && impl_->session_alive && !impl_->draft_open && impl_->history_index > 0;
}

bool VocalEditSession::can_redo() const noexcept {
  return impl_ && impl_->session_alive && !impl_->draft_open &&
         impl_->history_index + 1 < impl_->history.size();
}

std::vector<uint8_t> VocalEditSession::export_state() const {
  require_session_alive(impl_);
  if (impl_->draft_open) invalid_state("draft", "cannot export while a draft is open");
  VocalPersistedState persisted;
  persisted.session_id = impl_->session_id;
  persisted.next_note_id = impl_->next_note_id;
  persisted.committed_revision = impl_->revision;
  persisted.source = impl_->source_descriptor;
  persisted.output_length_samples = impl_->output_length_samples;
  persisted.analysis = *impl_->analysis;
  persisted.edit_state = *impl_->committed;
  persisted.render_settings = impl_->render_settings;
  return encode_vocal_state(persisted);
}

VocalEditDraft::VocalEditDraft(std::shared_ptr<VocalEditSession::Impl> impl)
    : impl_(std::move(impl)), active_(true) {}

VocalEditDraft::~VocalEditDraft() { cancel(); }

VocalEditDraft::VocalEditDraft(VocalEditDraft&& other) noexcept
    : impl_(std::move(other.impl_)),
      candidate_(std::move(other.candidate_)),
      draft_token_(other.draft_token_),
      local_next_note_id_(other.local_next_note_id_),
      active_(other.active_) {
  other.active_ = false;
}

VocalEditDraft& VocalEditDraft::operator=(VocalEditDraft&& other) noexcept {
  if (this == &other) return *this;
  cancel();
  impl_ = std::move(other.impl_);
  candidate_ = std::move(other.candidate_);
  draft_token_ = other.draft_token_;
  local_next_note_id_ = other.local_next_note_id_;
  active_ = other.active_;
  other.active_ = false;
  return *this;
}

std::vector<VocalNote> VocalEditDraft::notes() const {
  require_draft_active(active_, impl_, candidate_);
  return candidate_->notes;
}

VocalAnalysisData VocalEditDraft::analysis() const {
  require_draft_active(active_, impl_);
  return *impl_->analysis;
}

std::vector<PitchTransition> VocalEditDraft::transitions() const {
  require_draft_active(active_, impl_, candidate_);
  return candidate_->transitions;
}

VocalStateToken VocalEditDraft::token() const {
  require_draft_active(active_, impl_);
  return draft_token_;
}

DraftApplyResult VocalEditDraft::apply(VocalGeneration expected_generation,
                                       const std::vector<Operation>& operations,
                                       const DraftResultPreparation& before_publish) {
  require_draft_active(active_, impl_, candidate_);
  if (expected_generation != draft_token_.draft_generation) {
    revision_conflict("generation", expected_generation, draft_token_.draft_generation);
  }
  if (operations.empty()) invalid("operations", "at least one operation is required");
  if (draft_token_.draft_generation == std::numeric_limits<VocalGeneration>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "draft generation exhausted",
                             "generation");
  }

  auto candidate = std::make_shared<VocalEditState>(*candidate_);
  uint64_t next_id = local_next_note_id_;
  std::vector<IdChange> changes;
  std::set<VocalNoteId> expanded_curve_ids;
  std::set<VocalNoteId> expanded_envelope_ids;
  // Record expansions against the state at the start of the atomic batch as
  // well as while applying operations below. This closes an ordering hole:
  // a SetNoteEdit that clears a curve before SetNoteSourceSpan must not make
  // the later expansion look as though it never had a curve. The contract
  // requires the final candidate to carry an explicit curve (and envelope)
  // whenever the authored state being expanded had one.
  for (const auto& operation : operations) {
    if (!std::holds_alternative<SetNoteSourceSpanOp>(operation)) continue;
    const auto& source_operation = std::get<SetNoteSourceSpanOp>(operation);
    const auto* original = find_note_or_null(*candidate_, source_operation.note_id);
    if (!original) continue;
    const bool expands = source_operation.source_range.start < original->source_range.start ||
                         source_operation.source_range.end > original->source_range.end;
    if (!expands) continue;
    if (original->edit.pitch.target.mode == PitchTargetMode::kCurve) {
      expanded_curve_ids.insert(source_operation.note_id);
    }
    if (!original->edit.amplitude_envelope.empty()) {
      expanded_envelope_ids.insert(source_operation.note_id);
    }
  }
  for (size_t operation_index = 0; operation_index < operations.size(); ++operation_index) {
    const auto& operation = operations[operation_index];
    const size_t changes_before = changes.size();
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, SetNoteEditOp>) {
            apply_set_edit(*candidate, typed);
          } else if constexpr (std::is_same_v<T, SetNoteSourceSpanOp>) {
            const size_t current_index = find_note(*candidate, typed.note_id);
            const auto& current = candidate->notes[current_index];
            if (current.edit.pitch.target.mode == PitchTargetMode::kCurve &&
                (typed.source_range.start < current.source_range.start ||
                 typed.source_range.end > current.source_range.end)) {
              expanded_curve_ids.insert(typed.note_id);
            }
            if (!current.edit.amplitude_envelope.empty() &&
                (typed.source_range.start < current.source_range.start ||
                 typed.source_range.end > current.source_range.end)) {
              expanded_envelope_ids.insert(typed.note_id);
            }
            apply_set_source(*candidate, typed, impl_->source, *impl_->analysis,
                             impl_->source_descriptor.sample_count);
          } else if constexpr (std::is_same_v<T, SplitNoteOp>) {
            apply_split(*candidate, typed, impl_->source, *impl_->analysis,
                        impl_->source_descriptor.sample_count, impl_->output_length_samples,
                        next_id, changes);
          } else if constexpr (std::is_same_v<T, MergeNotesOp>) {
            apply_merge(*candidate, typed, impl_->source, *impl_->analysis,
                        impl_->source_descriptor.sample_count, impl_->output_length_samples,
                        impl_->render_settings, next_id, changes);
          } else if constexpr (std::is_same_v<T, SetTransitionOp>) {
            apply_set_transition(*candidate, typed);
          } else if constexpr (std::is_same_v<T, RemoveTransitionOp>) {
            apply_remove_transition(*candidate, typed);
          } else if constexpr (std::is_same_v<T, ResetNotesOp>) {
            for (const auto id : typed.note_ids) {
              const size_t index = find_note(*candidate, id);
              candidate->notes[index].edit =
                  VocalNoteEdit::identity_for(candidate->notes[index].source_range);
            }
          }
        },
        operation);
    for (size_t i = changes_before; i < changes.size(); ++i) {
      changes[i].operation_index = static_cast<uint32_t>(operation_index);
    }
  }
  const auto has_companion_set_edit = [&operations](VocalNoteId note_id) {
    return std::any_of(operations.begin(), operations.end(), [note_id](const Operation& operation) {
      return std::holds_alternative<SetNoteEditOp>(operation) &&
             std::get<SetNoteEditOp>(operation).note_id == note_id;
    });
  };
  for (const auto note_id : expanded_curve_ids) {
    const bool has_companion_edit = has_companion_set_edit(note_id);
    if (!has_companion_edit) {
      invalid("source_range", "expanding a curve source span requires a companion SetNoteEdit");
    }
    const auto& final_note = candidate->notes[find_note(*candidate, note_id)];
    if (final_note.edit.pitch.target.mode != PitchTargetMode::kCurve ||
        final_note.edit.pitch.target.points.size() < 2 ||
        final_note.edit.pitch.target.points.front().source_sample !=
            static_cast<double>(final_note.source_range.start) ||
        final_note.edit.pitch.target.points.back().source_sample !=
            static_cast<double>(final_note.source_range.end)) {
      invalid("source_range", "the companion SetNoteEdit must provide the expanded pitch curve");
    }
  }
  for (const auto note_id : expanded_envelope_ids) {
    const bool has_companion_edit = has_companion_set_edit(note_id);
    if (!has_companion_edit ||
        candidate->notes[find_note(*candidate, note_id)].edit.amplitude_envelope.empty()) {
      invalid("source_range",
              "the companion SetNoteEdit must provide the expanded amplitude envelope");
    }
  }
  sort_transitions(*candidate);
  validate_vocal_edit_state(*candidate, *impl_->analysis, impl_->source_descriptor.sample_count,
                            impl_->output_length_samples, impl_->render_settings);
  auto dirty = dirty_ranges_for_states(*candidate_, *candidate, impl_->output_length_samples);
  auto next_token = draft_token_;
  ++next_token.draft_generation;
  DraftApplyResult result{next_token, std::move(dirty), std::move(changes)};
  if (before_publish) before_publish(result);
  candidate_ = std::move(candidate);
  local_next_note_id_ = next_id;
  // IDs issued by a successful apply are consumed even if this draft is
  // cancelled later. Failed batches never reach this assignment.
  impl_->next_note_id = next_id;
  draft_token_ = next_token;
  return result;
}

std::vector<CompiledPitchPlan> VocalEditDraft::evaluate_pitch(
    const std::vector<VocalNoteId>& note_ids) const {
  require_draft_active(active_, impl_, candidate_);
  return evaluate_state(*impl_->analysis, *candidate_, impl_->render_settings,
                        impl_->source_descriptor.sample_rate, note_ids);
}

std::shared_ptr<const VocalRenderSnapshot> VocalEditDraft::capture_render_snapshot() const {
  require_draft_active(active_, impl_, candidate_);
  auto data = std::make_shared<VocalRenderSnapshotData>();
  data->source = impl_->source;
  data->source_descriptor = impl_->source_descriptor;
  data->analysis = impl_->analysis;
  data->state = std::shared_ptr<const VocalEditState>(candidate_);
  data->output_length_samples = impl_->output_length_samples;
  data->render_settings = impl_->render_settings;
  data->token = draft_token_;
  data->cache = impl_->cache;
  data->render_job_count = impl_->render_job_count;
  data->max_render_jobs = impl_->limits.max_render_jobs;
  return std::make_shared<VocalRenderSnapshot>(std::move(data));
}

double VocalEditDraft::source_sample_to_destination_sample(VocalNoteId note_id,
                                                           double source_sample) const {
  require_draft_active(active_, impl_, candidate_);
  const auto& note = candidate_->notes[find_note(*candidate_, note_id)];
  return ::sonare::editing::vocal_edit::source_sample_to_destination_sample(note, source_sample);
}

double VocalEditDraft::destination_sample_to_source_sample(VocalNoteId note_id,
                                                           double destination_sample) const {
  require_draft_active(active_, impl_, candidate_);
  const auto& note = candidate_->notes[find_note(*candidate_, note_id)];
  return ::sonare::editing::vocal_edit::destination_sample_to_source_sample(note,
                                                                            destination_sample);
}

StateChangeResult VocalEditDraft::commit(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  require_draft_active(active_, impl_, candidate_);
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  // A draft that was never applied closes without touching history or redo.
  if (draft_token_.draft_generation == 1) {
    StateChangeResult unchanged{{impl_->epoch, impl_->revision, 0, 0}, {}};
    if (before_publish) before_publish(unchanged);
    impl_->draft_open = false;
    active_ = false;
    draft_token_.draft_id = 0;
    draft_token_.draft_generation = 0;
    return unchanged;
  }
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  // Stage the complete history transition locally. In particular, do not
  // publish the candidate before vector growth, byte accounting, or dirty
  // range construction has succeeded; those operations may allocate or
  // report a counter overflow.
  auto dirty =
      dirty_ranges_for_states(*impl_->committed, *candidate_, impl_->output_length_samples);
  const auto new_committed = std::shared_ptr<const VocalEditState>(candidate_);
  auto new_history = impl_->history;
  if (impl_->history_index + 1 < new_history.size()) {
    new_history.erase(new_history.begin() + static_cast<std::ptrdiff_t>(impl_->history_index + 1),
                      new_history.end());
  }
  new_history.push_back(new_committed);
  size_t new_history_index = new_history.size() - 1;

  size_t max_history_entries = static_cast<size_t>(impl_->limits.max_undo_depth);
  if (max_history_entries < std::numeric_limits<size_t>::max()) ++max_history_entries;
  while (new_history.size() > max_history_entries) {
    new_history.erase(new_history.begin());
    --new_history_index;
  }
  while (new_history.size() > 1 &&
         retained_history_bytes(new_history, new_history_index) > impl_->limits.max_history_bytes) {
    new_history.erase(new_history.begin());
    --new_history_index;
  }
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);

  // These publications are all noexcept after the staged work above.
  impl_->committed = new_committed;
  impl_->history.swap(new_history);
  impl_->history_index = new_history_index;
  impl_->next_note_id = local_next_note_id_;
  impl_->revision = next_revision;
  impl_->draft_open = false;
  active_ = false;
  draft_token_.draft_id = 0;
  draft_token_.draft_generation = 0;
  return result;
}

void VocalEditDraft::cancel() noexcept {
  if (active_ && impl_) {
    impl_->draft_open = false;
    active_ = false;
  }
}

}  // namespace sonare::editing::vocal_edit
