#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

#include "arrangement/external_stems.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace {

size_t channel_count(uint32_t layout) noexcept {
  switch (layout) {
    case SONARE_EXTERNAL_STEM_MONO:
      return 1;
    case SONARE_EXTERNAL_STEM_STEREO:
      return 2;
    default:
      return 0;
  }
}

// PCM lives alongside EditHistory rather than inside Project, so a regular
// arrangement command alone cannot restore an external-stem import. Keep one
// shared state object for the staged Project and only the newly imported PCM.
// The Project slot swaps between the live and staged values; the PCM map moves
// through std::map node handles. Undo therefore retains the exact source nodes,
// while discarding a redo branch destroys detached samples instead of leaving
// an orphaned AudioContentStore entry.
struct ExternalStemImportState {
  arr::AudioContentStore* audio = nullptr;
  arr::Project staged_project;
  std::vector<arr::SourceId> source_ids;
  std::map<arr::SourceId, arr::AudioSourceSamples> detached_audio;
  bool audio_in_store = false;

  // All callers preflight the complete id set before the first node move.
  // Consequently each branch below is a sequence of non-allocating map-node
  // transfers followed by a value swap, and is safe to use from the history's
  // noexcept rollback path.
  bool toggle(arr::Project& project) noexcept {
    if (audio == nullptr || source_ids.empty()) return false;
    auto& store = audio->sources;
    if (!audio_in_store) {
      if (detached_audio.size() != source_ids.size()) return false;
      for (arr::SourceId source_id : source_ids) {
        if (detached_audio.find(source_id) == detached_audio.end() ||
            store.find(source_id) != store.end()) {
          return false;
        }
      }
      for (arr::SourceId source_id : source_ids) {
        const auto inserted = store.insert(detached_audio.extract(source_id));
        if (!inserted.inserted) return false;
      }
      std::swap(project, staged_project);
      audio_in_store = true;
      return true;
    }

    if (!detached_audio.empty()) return false;
    for (arr::SourceId source_id : source_ids) {
      if (store.find(source_id) == store.end()) return false;
    }
    for (arr::SourceId source_id : source_ids) {
      const auto inserted = detached_audio.insert(store.extract(source_id));
      if (!inserted.inserted) return false;
    }
    std::swap(project, staged_project);
    audio_in_store = false;
    return true;
  }
};

std::size_t retained_external_stem_state_bytes(const ExternalStemImportState& state) noexcept {
  std::size_t total = sizeof(state);
  total = arr::retained::saturating_add(total, arr::retained::bytes(state.staged_project));
  total = arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.source_ids));
  return arr::retained::saturating_add(total, arr::retained::dynamic_bytes(state.detached_audio));
}

class ExternalStemImportCommand final : public arr::EditCommand,
                                        public arr::EditCommandExternalEffects {
 public:
  explicit ExternalStemImportCommand(std::shared_ptr<ExternalStemImportState> state)
      : state_(std::move(state)) {}

  bool apply(arr::Project& project, arr::MidiContentStore&) override {
    if (state_ == nullptr || state_->audio == nullptr || state_->source_ids.empty()) {
      return false;
    }
    applied_ = false;
    applied_ = state_->toggle(project);
    return applied_;
  }

  void rollback_apply(arr::Project& project, arr::MidiContentStore&) noexcept override {
    if (!applied_ || state_ == nullptr) return;
    // The command's preflight guarantees this cannot fail.  Keep the hook
    // noexcept so it remains usable while unwinding a bad_alloc from invert().
    (void)state_->toggle(project);
    applied_ = false;
  }

  bool affects_audio_source(arr::SourceId source_id) const noexcept override {
    if (state_ == nullptr) return false;
    return std::find(state_->source_ids.begin(), state_->source_ids.end(), source_id) !=
           state_->source_ids.end();
  }

  bool affects_warp_source(arr::WarpRefId /*warp_id*/) const noexcept override { return false; }

  arr::EditCommandPtr invert(const arr::Project&, const arr::MidiContentStore&) const override {
    if (state_ == nullptr) return nullptr;
    return std::make_unique<ExternalStemImportCommand>(state_);
  }

  const char* type_name() const noexcept override { return "ImportExternalStems"; }
  bool mutates_midi_store() const noexcept override { return false; }

  std::size_t retained_bytes() const noexcept override {
    std::size_t total = sizeof(*this);
    if (state_ != nullptr) {
      total = arr::retained::saturating_add(total, retained_external_stem_state_bytes(*state_));
    }
    return total;
  }

 private:
  std::shared_ptr<ExternalStemImportState> state_;
  bool applied_ = false;
};

}  // namespace

#endif

SonareError sonare_project_import_external_stems(SonareProject* project,
                                                 const SonareExternalStemImportRequest* request,
                                                 SonareExternalStemImportResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out != nullptr) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(request);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  namespace check = sonare::util::check;
  constexpr size_t kMaxSamples = sonare::resource::kMaxOfflineAudioSamples;
  check::one_of(request->struct_version, std::array<int, 2>{0, 1}, "request.struct_version");
  check::non_null_when_count(request->stems, request->stem_count, "request.stems",
                             "request.stem_count");
  if (request->stem_count == 0 || request->stem_count > kMaxSamples) {
    check::refuse("request.stem_count", sonare::util::refusal::integer_range_predicate(
                                            1, static_cast<double>(kMaxSamples)));
  }
  check::integer_at_least(request->sample_rate, 1, "request.sample_rate");
  sonare::arrangement::ExternalSeparatedStemSet input;
  input.sample_rate = request->sample_rate;
  std::set<std::string> names;
  input.stems.reserve(request->stem_count);
  size_t total_samples = 0;
  for (size_t i = 0; i < request->stem_count; ++i) {
    const SonareExternalStemDesc& desc = request->stems[i];
    sonare::arrangement::ExternalSeparatedStemLayout layout{};
    const auto member = [i](const char* field) {
      return check::indexed_subject("request.stems", i, field);
    };
    check::non_null(desc.name, member("name").c_str());
    if (!names.insert(desc.name).second) {
      check::refuse(member("name").c_str(), "must not repeat the name of another stem");
    }
    check::integer_at_least<int64_t>(desc.start_frame, 0, member("start_frame").c_str());
    check::integer_in_range(desc.layout, SONARE_EXTERNAL_STEM_MONO, SONARE_EXTERNAL_STEM_STEREO,
                            member("layout").c_str());
    layout =
        admitted_ordinal_to_enum<sonare::arrangement::ExternalSeparatedStemLayout>(desc.layout);
    check::non_null(desc.planar_samples, member("planar_samples").c_str());
    check::integer_at_least<int64_t>(desc.frame_count, 1, member("frame_count").c_str());
    const size_t channels = channel_count(desc.layout);
    const size_t frames = static_cast<size_t>(desc.frame_count);
    if (frames > kMaxSamples / channels || total_samples > kMaxSamples - frames * channels) {
      check::refuse("request.stems",
                    "must hold at most " + std::to_string(kMaxSamples) + " samples in total");
    }
    sonare::arrangement::ExternalSeparatedStem stem;
    stem.name = desc.name;
    if (desc.role != nullptr) stem.role = std::string(desc.role);
    stem.layout = layout;
    stem.start_frame = desc.start_frame;
    stem.planar_samples.reserve(channels);
    for (size_t channel = 0; channel < channels; ++channel) {
      check::non_null(desc.planar_samples[channel],
                      (member("planar_samples") + "[" + std::to_string(channel) + "]").c_str());
      for (size_t k = 0; k < frames; ++k) {
        if (!std::isfinite(desc.planar_samples[channel][k])) {
          check::refuse((member("planar_samples") + "[" + std::to_string(channel) + "]").c_str(),
                        "contains a non-finite sample at index " + std::to_string(k));
        }
      }
      stem.planar_samples.emplace_back(desc.planar_samples[channel],
                                       desc.planar_samples[channel] + frames);
    }
    total_samples += frames * channels;
    input.stems.push_back(std::move(stem));
  }

  // Preserve the public import error taxonomy before handing the mutation to
  // EditHistory: its bool-only command API cannot distinguish malformed host
  // input from a failed state transition. Stage the Project and newly-created
  // PCM only; the live audio map is deliberately never copied.
  arr::Project validation_project = project->history.project();
  arr::AudioContentStore validation_audio;
  const auto validation =
      arr::import_external_separated_stems(&validation_project, &validation_audio, input);
  using ImportError = sonare::arrangement::ExternalSeparatedStemImportError;
  if (validation.error == ImportError::kSampleRateMismatch) {
    check::refuse("request.sample_rate", "must equal the sample rate of the project");
  }
  if (validation.error == ImportError::kFramePositionNotRepresentable) {
    check::refuse("request.stems",
                  "must start and end where the project's tempo map can place them");
  }
  if (validation.error == ImportError::kInvalidArgument) {
    check::refuse("request.stems", "must carry names and roles that are valid UTF-8");
  }
  // Every argument cause is refused above; what is left is a failed staging.
  if (validation.error != ImportError::kOk) return SONARE_ERROR_INVALID_STATE;

  // The project allocator and the sidecar map normally advance together, but
  // callers can construct an inconsistent store through the C++ seam. Reject
  // an id collision before history sees the command so no history entry or
  // live state is changed.
  for (const auto& [source_id, samples] : validation_audio.sources) {
    (void)samples;
    if (project->audio.sources.find(source_id) != project->audio.sources.end()) {
      return SONARE_ERROR_INVALID_STATE;
    }
  }

  // Marshal the result before committing history. Once apply() succeeds the
  // C ABI path performs no allocations; a caller therefore cannot observe a
  // successful structural edit followed by an output-buffer failure.
  auto track_ids = std::make_unique<uint32_t[]>(validation.track_ids.size());
  auto clip_ids = std::make_unique<uint32_t[]>(validation.clip_ids.size());
  std::copy(validation.track_ids.begin(), validation.track_ids.end(), track_ids.get());
  std::copy(validation.clip_ids.begin(), validation.clip_ids.end(), clip_ids.get());

  auto state = std::make_shared<ExternalStemImportState>();
  state->audio = &project->audio;
  state->staged_project = std::move(validation_project);
  state->detached_audio = std::move(validation_audio.sources);
  state->source_ids.reserve(state->detached_audio.size());
  for (const auto& [source_id, samples] : state->detached_audio) {
    (void)samples;
    state->source_ids.push_back(source_id);
  }
  if (state->source_ids.empty() ||
      !project->history.apply(std::make_unique<ExternalStemImportCommand>(state))) {
    return SONARE_ERROR_INVALID_STATE;
  }

  out->count = validation.track_ids.size();
  out->track_ids = track_ids.release();
  out->clip_ids = clip_ids.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, request, out);
#endif
}

void sonare_free_external_stem_import_result(SonareExternalStemImportResult* result) {
  if (result == nullptr) return;
  delete[] result->track_ids;
  delete[] result->clip_ids;
  *result = {};
}
