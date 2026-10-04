#include "editing/vocal_edit/render_cache.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>

#include "util/sha256.h"

namespace sonare::editing::vocal_edit {
std::shared_ptr<VocalRenderCache> make_vocal_render_cache(uint64_t max_bytes) {
  return std::make_shared<VocalRenderCache>(max_bytes);
}
namespace {

void append_bytes(util::Sha256& digest, const void* data, std::size_t size) {
  digest.update(static_cast<const std::uint8_t*>(data), size);
}

void append_u8(util::Sha256& digest, std::uint8_t value) { append_bytes(digest, &value, 1); }

void append_u32(util::Sha256& digest, std::uint32_t value) {
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8u),
      static_cast<std::uint8_t>(value >> 16u), static_cast<std::uint8_t>(value >> 24u)};
  append_bytes(digest, bytes, sizeof(bytes));
}

void append_u64(util::Sha256& digest, std::uint64_t value) {
  const std::uint8_t bytes[8] = {
      static_cast<std::uint8_t>(value),        static_cast<std::uint8_t>(value >> 8u),
      static_cast<std::uint8_t>(value >> 16u), static_cast<std::uint8_t>(value >> 24u),
      static_cast<std::uint8_t>(value >> 32u), static_cast<std::uint8_t>(value >> 40u),
      static_cast<std::uint8_t>(value >> 48u), static_cast<std::uint8_t>(value >> 56u)};
  append_bytes(digest, bytes, sizeof(bytes));
}

void append_i64(util::Sha256& digest, std::int64_t value) {
  append_u64(digest, static_cast<std::uint64_t>(value));
}

void append_f32(util::Sha256& digest, float value) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  append_u32(digest, bits);
}

void append_f64(util::Sha256& digest, double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  append_u64(digest, bits);
}

void append_range(util::Sha256& digest, SampleRange range) {
  append_i64(digest, range.start);
  append_i64(digest, range.end);
}

void append_string(util::Sha256& digest, std::string_view value) {
  append_u64(digest, static_cast<std::uint64_t>(value.size()));
  append_bytes(digest, value.data(), value.size());
}

void append_target(util::Sha256& digest, const VocalPitchTarget& target) {
  append_u32(digest, static_cast<std::uint32_t>(target.mode));
  append_f64(digest, target.center_midi);
  append_u64(digest, static_cast<std::uint64_t>(target.points.size()));
  for (const auto& point : target.points) {
    append_f64(digest, point.source_sample);
    append_f64(digest, point.target_midi);
  }
}

void append_edit(util::Sha256& digest, const VocalNoteEdit& edit) {
  append_target(digest, edit.pitch.target);
  append_f64(digest, edit.pitch.amount);
  append_f64(digest, edit.pitch.speed_ms);
  append_f64(digest, edit.pitch.max_correction_semitones);
  append_f64(digest, edit.pitch.transpose_semitones);
  append_f64(digest, edit.pitch.drift_scale);
  append_f64(digest, edit.pitch.vibrato_scale);
  append_i64(digest, edit.destination_start_sample);
  append_i64(digest, edit.destination_length_samples);
  append_f64(digest, edit.gain_db);
  append_u8(digest, edit.muted ? 1u : 0u);
  append_u64(digest, static_cast<std::uint64_t>(edit.amplitude_envelope.size()));
  for (const float value : edit.amplitude_envelope) append_f32(digest, value);
  append_u32(digest, static_cast<std::uint32_t>(edit.formant.mode));
  append_f64(digest, edit.formant.shift_semitones);
}

void append_transition(util::Sha256& digest, const PitchTransition* transition) {
  append_u8(digest, transition == nullptr ? 0u : 1u);
  if (transition == nullptr) return;
  append_u32(digest, transition->left_note_id);
  append_u32(digest, transition->right_note_id);
  append_i64(digest, transition->left_window_samples);
  append_i64(digest, transition->right_window_samples);
  append_f64(digest, transition->strength);
  append_u32(digest, static_cast<std::uint32_t>(transition->curve));
}

void append_plan(util::Sha256& digest, const PitchPlan& plan) {
  append_u32(digest, plan.note_id);
  append_u32(digest, plan.analysis_frame_start);
  append_u8(digest, plan.pitch_identity ? 1u : 0u);
  append_u64(digest, plan.points.size());
  for (const auto& point : plan.points) {
    append_f64(digest, point.source_sample);
    append_f64(digest, point.measured_midi);
    append_f64(digest, point.target_midi);
    append_f64(digest, point.effective_midi);
    append_f32(digest, point.delta_semitones);
    append_u8(digest, point.voiced ? 1u : 0u);
    append_u8(digest, point.has_target ? 1u : 0u);
  }
  append_u64(digest, plan.diagnostics.limited_correction_frames);
  append_u64(digest, plan.diagnostics.dry_passed_frames);
}

}  // namespace

struct VocalRenderCache::Impl {
  explicit Impl(uint64_t limit) : max_bytes(limit) {}

  using Map = std::map<Sha256Digest, std::shared_ptr<const RenderArtifact>>;
  mutable std::mutex mutex;
  uint64_t max_bytes = 0;
  uint64_t used_bytes = 0;
  Map artifacts;
  std::deque<Sha256Digest> fifo;
};

VocalRenderCache::VocalRenderCache(uint64_t max_bytes) : impl_(std::make_unique<Impl>(max_bytes)) {}

VocalRenderCache::~VocalRenderCache() = default;

std::shared_ptr<const RenderArtifact> VocalRenderCache::find(const Sha256Digest& key) const {
  std::lock_guard lock(impl_->mutex);
  const auto it = impl_->artifacts.find(key);
  return it == impl_->artifacts.end() ? nullptr : it->second;
}

void VocalRenderCache::publish(std::shared_ptr<const RenderArtifact> artifact) {
  if (!artifact || !artifact->samples) return;
  if (artifact->destination_range.start < 0 ||
      artifact->destination_range.end < artifact->destination_range.start) {
    return;
  }
  const uint64_t sample_count = static_cast<uint64_t>(artifact->samples->size());
  if (sample_count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) return;
  if (artifact->destination_range.length() != static_cast<std::int64_t>(sample_count)) return;
  if (sample_count > std::numeric_limits<uint64_t>::max() / sizeof(float)) return;
  const uint64_t size = static_cast<uint64_t>(sample_count * sizeof(float));

  std::lock_guard lock(impl_->mutex);
  const auto existing = impl_->artifacts.find(artifact->key);
  if (existing != impl_->artifacts.end()) {
    impl_->used_bytes -= static_cast<uint64_t>(existing->second->samples->size() * sizeof(float));
    impl_->artifacts.erase(existing);
    impl_->fifo.erase(std::remove(impl_->fifo.begin(), impl_->fifo.end(), artifact->key),
                      impl_->fifo.end());
  }
  if (size == 0 || size > impl_->max_bytes) return;
  while (!impl_->fifo.empty() && impl_->used_bytes > impl_->max_bytes - size) {
    const Sha256Digest evict_key = impl_->fifo.front();
    impl_->fifo.pop_front();
    const auto it = impl_->artifacts.find(evict_key);
    if (it == impl_->artifacts.end()) continue;
    impl_->used_bytes -= static_cast<uint64_t>(it->second->samples->size() * sizeof(float));
    impl_->artifacts.erase(it);
  }
  if (impl_->used_bytes > impl_->max_bytes - size) return;
  const Sha256Digest key = artifact->key;
  const auto inserted = impl_->artifacts.emplace(key, std::move(artifact));
  try {
    impl_->fifo.push_back(key);
  } catch (...) {
    impl_->artifacts.erase(inserted.first);
    throw;
  }
  impl_->used_bytes += size;
}

uint64_t VocalRenderCache::max_bytes() const noexcept { return impl_->max_bytes; }

uint64_t VocalRenderCache::bytes() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->used_bytes;
}

void VocalRenderCache::clear() {
  std::lock_guard lock(impl_->mutex);
  impl_->artifacts.clear();
  impl_->fifo.clear();
  impl_->used_bytes = 0;
}

Sha256Digest render_artifact_key(const VocalRenderSnapshotData& snapshot, const VocalNote& note,
                                 const PitchPlan& plan, const PitchTransition* incoming,
                                 const PitchTransition* outgoing) {
  util::Sha256 digest;
  static constexpr std::string_view kDomain = "libsonare.vocal-render.v1\0";
  append_string(digest, kDomain);
  append_u32(digest, snapshot.source_descriptor.sample_rate);
  append_i64(digest, snapshot.source_descriptor.sample_count);
  append_bytes(digest, snapshot.source_descriptor.digest.data(),
               snapshot.source_descriptor.digest.size());
  if (snapshot.analysis) {
    append_bytes(digest, snapshot.analysis->digest.data(), snapshot.analysis->digest.size());
  } else {
    const Sha256Digest empty{};
    append_bytes(digest, empty.data(), empty.size());
  }
  append_u32(digest, static_cast<std::uint32_t>(snapshot.render_settings.profile));
  append_u32(digest, snapshot.render_settings.algorithm_version);
  append_f64(digest, snapshot.render_settings.edge_fade_ms);
  append_f64(digest, snapshot.render_settings.vibrato_cutoff_hz);
  append_u32(digest, note.id);
  append_range(digest, note.source_range);
  append_u32(digest, note.analysis_frame_start);
  append_u32(digest, note.analysis_frame_end);
  append_u8(digest, note.has_pitch ? 1u : 0u);
  append_f64(digest, note.centre_midi);
  append_f64(digest, note.median_hz);
  append_f64(digest, note.f0_stability);
  append_edit(digest, note.edit);
  append_plan(digest, plan);
  append_transition(digest, incoming);
  append_transition(digest, outgoing);
  return digest.finalize();
}

}  // namespace sonare::editing::vocal_edit
