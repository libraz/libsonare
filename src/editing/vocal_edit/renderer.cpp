#include "editing/vocal_edit/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/vocal_edit/render_cache.h"
#include "effects/formant_warp.h"
#include "effects/time_stretch.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/numeric_validation.h"

namespace sonare::editing::vocal_edit {
namespace {

using sonare::editing::pitch_editor::F0Track;
using sonare::editing::pitch_editor::PitchCorrector;

[[noreturn]] void invalid(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

[[noreturn]] void invalid_state(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidState, message, field);
}

[[noreturn]] void cancelled() {
  throw VocalEditException(VocalReason::kCancelled, "vocal render was cancelled", "render");
}

std::size_t checked_size(int64_t value, const char* field) {
  if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<std::size_t>::max()) {
    invalid(field, "range is not representable as a host size");
  }
  return static_cast<std::size_t>(value);
}

int64_t checked_end(int64_t start, int64_t length, const char* field) {
  if (start < 0 || length < 0 || length > std::numeric_limits<int64_t>::max() - start) {
    invalid(field, "range is outside the signed sample domain");
  }
  return start + length;
}

int64_t fade_samples(double fade_ms, int sample_rate, int64_t length) noexcept {
  if (!(fade_ms > 0.0) || sample_rate <= 0 || length <= 1) return 0;
  const double requested = std::round(fade_ms * 0.001 * static_cast<double>(sample_rate));
  const int64_t limit = std::max<int64_t>(0, length / 2);
  if (!(requested > 0.0)) return 0;
  if (requested >= static_cast<double>(limit)) return limit;
  return static_cast<int64_t>(requested);
}

float fade_phase(int64_t offset, int64_t length, int64_t fade) noexcept {
  if (fade <= 0 || length <= 0) return 1.0f;
  if (offset < fade) return (static_cast<float>(offset) + 0.5f) / static_cast<float>(fade);
  if (offset >= length - fade) {
    return (static_cast<float>(length - offset) - 0.5f) / static_cast<float>(fade);
  }
  return 1.0f;
}

void erase_source(std::vector<float>& output, const Audio& source, SampleRange span,
                  SampleRange request, int64_t fade) {
  const int64_t begin = std::max(span.start, request.start);
  const int64_t end = std::min(span.end, request.end);
  for (int64_t sample = begin; sample < end; ++sample) {
    const int64_t local = sample - span.start;
    const float phase = fade_phase(local, span.length(), fade);
    const float source_gain =
        phase >= 1.0f ? 0.0f : std::cos(static_cast<float>(sonare::constants::kHalfPi) * phase);
    output[checked_size(sample - request.start, "render.range")] =
        source_gain * source[checked_size(sample, "source")];
  }
}

/// True when the rendered segment replaces the note's own source span sample for sample.
bool renders_in_place(const VocalNote& note) noexcept {
  return !note.edit.muted && note.edit.destination_start_sample == note.source_range.start &&
         note.edit.destination_length_samples == note.source_range.length();
}

/// Crossfades @p segment over @p output. A segment sitting on the source it was cut from is
/// coherent with it, so complementary gains keep a no-op edit at unity; elsewhere the two are
/// independent and the gains are equal-power.
void overlay(std::vector<float>& output, const std::vector<float>& segment,
             int64_t destination_start, SampleRange request, int64_t fade, bool coherent) {
  const int64_t segment_length = static_cast<int64_t>(segment.size());
  if (segment_length <= 0) return;
  const int64_t destination_end = checked_end(destination_start, segment_length, "destination");
  const int64_t begin = std::max(destination_start, request.start);
  const int64_t end = std::min(destination_end, request.end);
  if (begin >= end) return;
  for (int64_t sample = begin; sample < end; ++sample) {
    const int64_t local = sample - destination_start;
    const float phase = fade_phase(local, segment_length, fade);
    const float segment_gain =
        phase >= 1.0f ? 1.0f : std::sin(static_cast<float>(sonare::constants::kHalfPi) * phase);
    const float existing_gain =
        phase >= 1.0f ? 0.0f
        : coherent    ? 1.0f - segment_gain
                      : std::cos(static_cast<float>(sonare::constants::kHalfPi) * phase);
    const std::size_t output_index = checked_size(sample - request.start, "render.range");
    output[output_index] = segment_gain * segment[checked_size(local, "destination")] +
                           existing_gain * output[output_index];
  }
}

float envelope_at(const std::vector<float>& envelope, std::size_t index, std::size_t count) {
  if (envelope.empty()) return 1.0f;
  if (envelope.size() == 1 || count <= 1) return envelope.front();
  const double position = static_cast<double>(index) * static_cast<double>(envelope.size() - 1) /
                          static_cast<double>(count - 1);
  const std::size_t lower =
      std::min<std::size_t>(static_cast<std::size_t>(position), envelope.size() - 1);
  const std::size_t upper = std::min(lower + 1, envelope.size() - 1);
  const double fraction = position - static_cast<double>(lower);
  return static_cast<float>(static_cast<double>(envelope[lower]) * (1.0 - fraction) +
                            static_cast<double>(envelope[upper]) * fraction);
}

void apply_gain_and_envelope(std::vector<float>& samples, const VocalNoteEdit& edit) {
  const double gain = db_to_linear(edit.gain_db);
  if (!std::isfinite(gain)) invalid("gain_db", "linear gain is not finite");
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double scaled =
        static_cast<double>(samples[i]) *
        static_cast<double>(envelope_at(edit.amplitude_envelope, i, samples.size())) * gain;
    if (!std::isfinite(scaled) || std::abs(scaled) > max_float) {
      invalid("amplitude_envelope", "combined gain is outside float range");
    }
    samples[i] = static_cast<float>(scaled);
  }
}

std::vector<float> stretch_note(const std::vector<float>& input, std::size_t output_size,
                                int sample_rate) {
  if (output_size == input.size()) return input;
  if (input.empty() || output_size == 0) invalid_state("time_stretch", "empty note");
  const float rate = detail::stretch_rate_for_length(input.size(), output_size);
  auto stretched = time_stretch(Audio::from_vector(input, sample_rate), rate);
  std::vector<float> output(stretched.begin(), stretched.end());
  return detail::fit_stretched_note_output(std::move(output), output_size);
}

const PitchTransition* incoming_transition(const VocalEditState& state, VocalNoteId id) {
  for (const auto& transition : state.transitions) {
    if (transition.right_note_id == id) return &transition;
  }
  return nullptr;
}

const PitchTransition* outgoing_transition(const VocalEditState& state, VocalNoteId id) {
  for (const auto& transition : state.transitions) {
    if (transition.left_note_id == id) return &transition;
  }
  return nullptr;
}

std::shared_ptr<VocalRenderCache> cache_for(const VocalRenderSnapshotData& data) {
  if (!data.cache) invalid_state("snapshot.cache", "snapshot has no session cache");
  return data.cache;
}

struct NoteArtifactResult {
  std::shared_ptr<const RenderArtifact> artifact;
  PitchPlan plan;
};

NoteArtifactResult render_note(const VocalRenderSnapshotData& snapshot, const VocalNote& note,
                               PitchPlan plan, const PitchTransition* incoming,
                               const PitchTransition* outgoing) {
  if (!snapshot.analysis) invalid_state("snapshot.analysis", "render snapshot has no analysis");
  const auto& analysis = *snapshot.analysis;

  const bool effective_identity =
      !note.edit.muted && plan.pitch_identity &&
      note.edit.destination_start_sample == note.source_range.start &&
      note.edit.destination_length_samples == note.source_range.length() &&
      note.edit.gain_db == 0.0 && note.edit.amplitude_envelope.empty() &&
      note.edit.formant.mode == FormantMode::kPreserve && note.edit.formant.shift_semitones == 0.0;
  if (effective_identity) {
    for (auto& point : plan.points) {
      point.delta_semitones = 0.0f;
      point.effective_midi = point.measured_midi;
    }
    plan.pitch_identity = true;
  }

  RenderArtifact artifact;
  artifact.note_id = note.id;
  artifact.destination_range = {note.edit.destination_start_sample,
                                checked_end(note.edit.destination_start_sample,
                                            note.edit.destination_length_samples, "destination")};
  artifact.key = render_artifact_key(snapshot, note, plan, incoming, outgoing);

  std::vector<float> rendered;
  rendered.resize(checked_size(artifact.destination_range.length(), "destination"), 0.0f);
  if (note.edit.muted) {
    artifact.samples = std::make_shared<const std::vector<float>>(std::move(rendered));
    return {std::make_shared<const RenderArtifact>(std::move(artifact)), std::move(plan)};
  }

  const int64_t source_length = note.source_range.length();
  if (source_length <= 0) invalid_state("source_range", "note has an empty source range");
  std::vector<float> source_segment;
  source_segment.reserve(checked_size(source_length, "source_range"));
  for (int64_t i = note.source_range.start; i < note.source_range.end; ++i) {
    source_segment.push_back(snapshot.source[checked_size(i, "source_range")]);
  }

  if (effective_identity) {
    rendered = source_segment;
    artifact.samples = std::make_shared<const std::vector<float>>(std::move(rendered));
    return {std::make_shared<const RenderArtifact>(std::move(artifact)), std::move(plan)};
  }

  std::vector<float> pitch_segment = source_segment;
  if (!plan.pitch_identity) {
    const int64_t sample_count = static_cast<int64_t>(snapshot.source.size());
    double minimum_voiced_f0 = std::numeric_limits<double>::infinity();
    for (std::size_t frame = 0; frame < analysis.f0_hz.size(); ++frame) {
      if (analysis.voiced[frame] != 0 && std::isfinite(analysis.f0_hz[frame]) &&
          analysis.f0_hz[frame] > 0.0f) {
        minimum_voiced_f0 = std::min(minimum_voiced_f0, static_cast<double>(analysis.f0_hz[frame]));
      }
    }
    if (!std::isfinite(minimum_voiced_f0) || !(minimum_voiced_f0 > 0.0)) {
      minimum_voiced_f0 = 65.0;
    }
    const double period_context =
        std::ceil(3.0 * snapshot.source.sample_rate() / minimum_voiced_f0);
    const double context_double =
        std::max(static_cast<double>(analysis.grid.frame_length_samples), period_context);
    const int64_t context =
        context_double > static_cast<double>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::max()
            : static_cast<int64_t>(context_double);
    if (context > static_cast<int64_t>(kMaxAudioBufferSize) ||
        source_length > static_cast<int64_t>(kMaxAudioBufferSize) - 2 * context)
      invalid("render.context", "padded note exceeds audio resource limit");
    const int64_t context_start = note.source_range.start - context;
    const int64_t context_end = note.source_range.end + context;
    std::vector<float> padded(checked_size(context_end - context_start, "render.context"), 0.0f);
    const int64_t read_begin = std::max<int64_t>(0, context_start);
    const int64_t read_end = std::min<int64_t>(sample_count, context_end);
    std::copy(snapshot.source.begin() + read_begin, snapshot.source.begin() + read_end,
              padded.begin() + (read_begin - context_start));
    const Audio context_audio =
        Audio::from_vector(std::move(padded), snapshot.source.sample_rate());
    const double min_backend_hz = static_cast<double>(snapshot.source.sample_rate()) /
                                  static_cast<double>(context_audio.size());
    const double max_backend_hz = static_cast<double>(snapshot.source.sample_rate()) * 0.5;
    for (std::size_t offset = 0; offset < plan.points.size(); ++offset) {
      const auto& point = plan.points[offset];
      if (!point.voiced) continue;
      const std::size_t frame = static_cast<std::size_t>(plan.analysis_frame_start) + offset;
      if (frame >= analysis.f0_hz.size()) continue;
      const double target_hz = static_cast<double>(analysis.f0_hz[frame]) *
                               std::exp2(static_cast<double>(point.delta_semitones) /
                                         sonare::constants::kSemitonesPerOctave);
      if (!(target_hz >= min_backend_hz && target_hz <= max_backend_hz)) {
        ++artifact.dry_passed_frames;
      }
    }
    F0Track track;
    track.f0_hz = analysis.f0_hz;
    track.voiced.resize(analysis.voiced.size(), false);
    for (std::size_t i = 0; i < analysis.voiced.size(); ++i) {
      track.voiced[i] = analysis.voiced[i] != 0;
    }
    track.sample_rate = snapshot.source.sample_rate();
    track.hop_length = analysis.grid.samples_per_frame >= 1.0 &&
                               analysis.grid.samples_per_frame <=
                                   static_cast<double>(std::numeric_limits<int>::max())
                           ? static_cast<int>(std::llround(analysis.grid.samples_per_frame))
                           : 1;
    track.frame_rate_hz = static_cast<float>(static_cast<double>(track.sample_rate) /
                                             analysis.grid.samples_per_frame);
    std::vector<float> deltas(track.f0_hz.size(), 0.0f);
    for (std::size_t offset = 0; offset < plan.points.size(); ++offset) {
      const std::size_t frame = static_cast<std::size_t>(plan.analysis_frame_start) + offset;
      if (frame < deltas.size()) deltas[frame] = plan.points[offset].delta_semitones;
    }
    const double local_origin =
        analysis.grid.frame_origin_sample - static_cast<double>(context_start);
    const Audio corrected = PitchCorrector().resynthesize(
        context_audio, track, deltas, local_origin, analysis.grid.samples_per_frame);
    const int64_t local_start = note.source_range.start - context_start;
    const int64_t local_end = local_start + source_length;
    if (local_start < 0 || local_end > static_cast<int64_t>(corrected.size())) {
      invalid_state("render.context", "pitch backend returned a short context buffer");
    }
    pitch_segment.assign(corrected.begin() + local_start, corrected.begin() + local_end);
  }

  rendered = stretch_note(pitch_segment, rendered.size(), snapshot.source.sample_rate());
  if (note.edit.formant.mode == FormantMode::kShift && note.edit.formant.shift_semitones != 0.0) {
    const float factor = static_cast<float>(
        std::pow(2.0, note.edit.formant.shift_semitones / sonare::constants::kSemitonesPerOctave));
    if (!std::isfinite(factor) || factor < sonare::kFormantFactorMin ||
        factor > sonare::kFormantFactorMax) {
      invalid("formant.shift_semitones", "requested formant shift is outside renderer capability");
    }
    const Audio warped =
        sonare::FormantWarp({factor, 12, 1.0f})
            .process(Audio::from_vector(std::move(rendered), snapshot.source.sample_rate()));
    rendered.assign(warped.begin(), warped.end());
    if (rendered.size() != checked_size(artifact.destination_range.length(), "destination"))
      invalid_state("formant", "backend changed note duration");
  }
  apply_gain_and_envelope(rendered, note.edit);
  artifact.samples = std::make_shared<const std::vector<float>>(std::move(rendered));
  return {std::make_shared<const RenderArtifact>(std::move(artifact)), plan};
}

bool effective_identity(const VocalNote& note, const PitchPlan& plan) {
  return !note.edit.muted && plan.pitch_identity &&
         note.edit.destination_start_sample == note.source_range.start &&
         note.edit.destination_length_samples == note.source_range.length() &&
         note.edit.gain_db == 0.0 && note.edit.amplitude_envelope.empty() &&
         note.edit.formant.mode == FormantMode::kPreserve &&
         note.edit.formant.shift_semitones == 0.0;
}

void add_range(std::vector<SampleRange>& ranges, SampleRange range) {
  if (range.start < range.end) ranges.push_back(range);
}

void normalize_ranges(std::vector<SampleRange>& ranges) {
  insertion_sort(ranges.begin(), ranges.end(), [](SampleRange lhs, SampleRange rhs) {
    return lhs.start < rhs.start || (lhs.start == rhs.start && lhs.end < rhs.end);
  });
  std::vector<SampleRange> merged;
  for (const auto range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  ranges = std::move(merged);
}

}  // namespace

float detail::stretch_rate_for_length(std::size_t input_size, std::size_t output_size) {
  if (input_size == 0 || output_size == 0) invalid_state("time_stretch", "empty note");
  const double ideal = static_cast<double>(input_size) / static_cast<double>(output_size);
  const float nearest = static_cast<float>(ideal);
  float best = nearest;
  std::size_t best_distance = std::numeric_limits<std::size_t>::max();
  double best_rate_distance = std::numeric_limits<double>::infinity();
  for (const float candidate : {nearest, std::nextafter(nearest, 0.0f),
                                std::nextafter(nearest, std::numeric_limits<float>::infinity())}) {
    std::size_t projected = 0;
    if (!numeric::checked_projected_count(input_size, candidate,
                                          std::numeric_limits<std::size_t>::max(), &projected))
      continue;
    const auto distance =
        projected > output_size ? projected - output_size : output_size - projected;
    const double rate_distance = std::abs(static_cast<double>(candidate) - ideal);
    if (distance < best_distance ||
        (distance == best_distance && rate_distance < best_rate_distance)) {
      best = candidate;
      best_distance = distance;
      best_rate_distance = rate_distance;
    }
  }
  if (best_distance == std::numeric_limits<std::size_t>::max())
    invalid_state("time_stretch", "note duration is not representable");
  return best;
}

std::vector<float> detail::fit_stretched_note_output(std::vector<float> output,
                                                     std::size_t output_size) {
  if (output.size() > output_size) output.resize(output_size);
  if (output.size() < output_size) {
    if (output.empty() || !numeric::all_finite(output.data(), output.size()))
      invalid_state("time_stretch", "backend returned empty or non-finite samples");
    output.resize(output_size, output.back());
  }
  return output;
}

struct VocalRenderJob::Impl {
  struct ActiveCallGuard final {
    explicit ActiveCallGuard(Impl& impl) : impl_(impl) {
      if (impl_.active_call) invalid_state("render", "render operation is already active");
      impl_.active_call = true;
    }
    ActiveCallGuard(const ActiveCallGuard&) = delete;
    ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;
    ~ActiveCallGuard() noexcept { impl_.active_call = false; }

   private:
    Impl& impl_;
  };

  std::shared_ptr<const VocalRenderSnapshot> snapshot;
  VocalRenderRequest request{};
  std::shared_ptr<VocalRenderCache> cache;
  std::vector<std::shared_ptr<const RenderArtifact>> artifacts;
  std::vector<PitchPlan> plans;
  std::vector<PitchPlan> compiled_plans;
  std::vector<SampleRange> processed_ranges;
  VocalRenderResult result{};
  VocalRenderJobState state = VocalRenderJobState::kRenderingUnits;
  uint64_t completed_units = 0;
  uint64_t total_units = 0;
  uint64_t cache_hits = 0;
  bool result_taken = false;
  bool active_call = false;
  int64_t assembly_cursor = 0;
  std::shared_ptr<std::atomic<uint32_t>> job_count;
  void release_job() noexcept {
    if (job_count) {
      job_count->fetch_sub(1, std::memory_order_relaxed);
      job_count.reset();
    }
  }
  ~Impl() { release_job(); }
};

VocalRenderJob::VocalRenderJob(std::shared_ptr<const VocalRenderSnapshot> snapshot,
                               VocalRenderRequest request)
    : impl_(std::make_unique<Impl>()) {
  if (!snapshot || !snapshot->valid()) invalid("snapshot", "render snapshot is invalid");
  if (request.range.start < 0 || request.range.end <= request.range.start ||
      request.range.end > snapshot->data().output_length_samples) {
    invalid("request.range", "render range is outside output bounds");
  }
  impl_->snapshot = std::move(snapshot);
  impl_->request = request;
  const auto& data = impl_->snapshot->data();
  if (!data.analysis || !data.state || data.output_length_samples <= 0) {
    invalid("snapshot", "render snapshot is incomplete");
  }
  impl_->cache = cache_for(data);
  const auto& state = *data.state;
  constexpr int64_t kAssemblyTileSamples = 4096;
  impl_->total_units =
      static_cast<uint64_t>(state.notes.size()) +
      static_cast<uint64_t>((request.range.length() - 1) / kAssemblyTileSamples + 1);
  impl_->assembly_cursor = request.range.start;
  impl_->result.samples.reserve(checked_size(request.range.length(), "request.range"));
  if (!data.render_job_count) invalid_state("snapshot.jobs", "snapshot has no job counter");
  auto count = data.render_job_count->load(std::memory_order_relaxed);
  for (;;) {
    if (count >= data.max_render_jobs) invalid_state("render.jobs", "render job limit reached");
    if (data.render_job_count->compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
      break;
  }
  impl_->job_count = data.render_job_count;
  impl_->artifacts.resize(state.notes.size());
  impl_->plans.resize(state.notes.size());
  impl_->compiled_plans = compile_pitch_plans(*data.analysis, state, data.render_settings,
                                              data.source_descriptor.sample_rate);
  if (impl_->compiled_plans.size() != state.notes.size())
    invalid_state("pitch_plan", "planner returned an unexpected number of note plans");
  impl_->result.start_sample = request.range.start;
  impl_->result.state_token = data.token;
  impl_->result.request_id = request.request_id;
  impl_->result.profile = data.render_settings.profile;
}

VocalRenderJob::VocalRenderJob(VocalRenderJob&& other) noexcept = default;

VocalRenderJob& VocalRenderJob::operator=(VocalRenderJob&& other) noexcept = default;

VocalRenderJob::~VocalRenderJob() = default;

VocalRenderProgress VocalRenderJob::progress() const noexcept {
  if (!impl_) return {VocalRenderJobState::kAborted, 0, 0};
  return {impl_->state, impl_->completed_units, impl_->total_units};
}

bool VocalRenderJob::next(const VocalCancelProbe& cancel) {
  if (!impl_) return false;
  Impl::ActiveCallGuard active_call(*impl_);
  if (impl_->state == VocalRenderJobState::kAborted ||
      impl_->state == VocalRenderJobState::kComplete)
    return false;
  if (cancel && cancel()) {
    abort();
    return false;
  }
  if (impl_->state == VocalRenderJobState::kAborted) return false;
  try {
    const auto& data = impl_->snapshot->data();
    const auto& state = *data.state;
    if (impl_->state == VocalRenderJobState::kRenderingUnits) {
      const std::size_t index = static_cast<std::size_t>(impl_->completed_units);
      if (index < state.notes.size()) {
        const auto& note = state.notes[index];
        const auto* incoming = incoming_transition(state, note.id);
        const auto* outgoing = outgoing_transition(state, note.id);
        const auto& key_plan = impl_->compiled_plans[index];
        const bool plan_identity = key_plan.pitch_identity;
        PitchPlan lookup_plan = key_plan;
        if (plan_identity) {
          lookup_plan.pitch_identity = true;
          for (auto& point : lookup_plan.points) {
            point.delta_semitones = 0.0f;
            point.effective_midi = point.measured_midi;
          }
        }
        const Sha256Digest key = render_artifact_key(data, note, lookup_plan, incoming, outgoing);
        if (auto cached = impl_->cache->find(key)) {
          impl_->artifacts[index] = std::move(cached);
          impl_->plans[index] = std::move(lookup_plan);
          ++impl_->cache_hits;
        } else {
          const auto rendered = render_note(data, note, lookup_plan, incoming, outgoing);
          impl_->artifacts[index] = rendered.artifact;
          impl_->plans[index] = rendered.plan;
        }
        add_range(impl_->processed_ranges, note.source_range);
        add_range(impl_->processed_ranges, impl_->artifacts[index]->destination_range);
        ++impl_->completed_units;
        if (cancel && cancel()) {
          abort();
          return false;
        }
        if (impl_->state == VocalRenderJobState::kAborted) return false;
        if (impl_->completed_units == state.notes.size()) {
          impl_->state = VocalRenderJobState::kAssembling;
        }
        return true;
      }
      impl_->state = VocalRenderJobState::kAssembling;
    }

    if (impl_->state == VocalRenderJobState::kAssembling) {
      if (cancel && cancel()) {
        abort();
        return false;
      }
      if (impl_->state == VocalRenderJobState::kAborted) return false;
      const int64_t remaining = impl_->request.range.end - impl_->assembly_cursor;
      const SampleRange request{impl_->assembly_cursor,
                                impl_->assembly_cursor + std::min<int64_t>(4096, remaining)};
      const std::size_t output_size = checked_size(request.length(), "request.range");
      std::vector<float> tile(output_size);
      const Audio& source = data.source;
      for (std::size_t i = 0; i < output_size; ++i) {
        const int64_t sample = request.start + static_cast<int64_t>(i);
        tile[i] = sample >= 0 && sample < static_cast<int64_t>(source.size())
                      ? source[static_cast<std::size_t>(sample)]
                      : 0.0f;
      }
      const int sample_rate = source.sample_rate();
      for (std::size_t i = 0; i < state.notes.size(); ++i) {
        const auto& note = state.notes[i];
        if (effective_identity(note, impl_->plans[i])) {
          continue;
        }
        // An in-place segment crossfades against the source through overlay() alone; fading the
        // source here as well would weight it twice across the seam.
        if (renders_in_place(note)) continue;
        erase_source(tile, source, note.source_range, request,
                     fade_samples(data.render_settings.edge_fade_ms, sample_rate,
                                  note.source_range.length()));
      }
      std::vector<std::size_t> order(state.notes.size());
      for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
      insertion_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return state.notes[lhs].source_range.start < state.notes[rhs].source_range.start;
      });
      for (const std::size_t index : order) {
        const auto& note = state.notes[index];
        if (effective_identity(note, impl_->plans[index])) {
          continue;
        }
        if (note.edit.muted) continue;
        const auto& artifact = *impl_->artifacts[index];
        overlay(tile, *artifact.samples, artifact.destination_range.start, request,
                fade_samples(data.render_settings.edge_fade_ms, sample_rate,
                             artifact.destination_range.length()),
                renders_in_place(note));
      }
      if (cancel && cancel()) {
        abort();
        return false;
      }
      if (impl_->state == VocalRenderJobState::kAborted) return false;
      impl_->result.samples.insert(impl_->result.samples.end(), tile.begin(), tile.end());
      impl_->assembly_cursor = request.end;
      ++impl_->completed_units;
      if (request.end != impl_->request.range.end) return true;
      for (std::size_t i = 0; i < impl_->artifacts.size(); ++i) {
        if (impl_->artifacts[i]) impl_->cache->publish(impl_->artifacts[i]);
        impl_->result.diagnostics.limited_correction_frames +=
            impl_->plans[i].diagnostics.limited_correction_frames;
        impl_->result.diagnostics.dry_passed_frames += impl_->artifacts[i]->dry_passed_frames;
      }
      normalize_ranges(impl_->processed_ranges);
      impl_->result.processed_ranges = impl_->processed_ranges;
      impl_->result.cache_hit_units = impl_->cache_hits;
      impl_->state = VocalRenderJobState::kComplete;
      impl_->release_job();
      return true;
    }
  } catch (...) {
    abort();
    throw;
  }
  return false;
}

VocalRenderResult VocalRenderJob::finalize(const VocalCancelProbe& cancel) {
  if (!impl_) invalid_state("render", "render job is not complete");
  Impl::ActiveCallGuard active_call(*impl_);
  if (impl_->state != VocalRenderJobState::kComplete || impl_->result_taken) {
    invalid_state("render", "render job is not complete");
  }
  if (cancel && cancel()) {
    abort();
    cancelled();
  }
  if (impl_->state == VocalRenderJobState::kAborted) cancelled();
  impl_->result_taken = true;
  return std::move(impl_->result);
}

void VocalRenderJob::abort() noexcept {
  if (!impl_) return;
  impl_->state = VocalRenderJobState::kAborted;
  impl_->artifacts.clear();
  impl_->result.samples.clear();
  impl_->release_job();
}

VocalRenderResult render_snapshot(std::shared_ptr<const VocalRenderSnapshot> snapshot,
                                  const VocalRenderRequest& request,
                                  const VocalCancelProbe& cancel) {
  VocalRenderJob job(std::move(snapshot), request);
  for (;;) {
    const auto state = job.progress().state;
    if (state == VocalRenderJobState::kComplete) break;
    if (state == VocalRenderJobState::kAborted || !job.next(cancel)) cancelled();
  }
  return job.finalize(cancel);
}

}  // namespace sonare::editing::vocal_edit
