#include "arrangement/vocal_edit_sidecar.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace sonare::arrangement::vocal_sidecar {
namespace {

constexpr size_t kEnvelopeHeaderBytes = 112;
constexpr uint16_t kEnvelopeVersion = 1;
constexpr uint16_t kEnvelopeFlags = 1;

bool has_prefix(std::string_view value) noexcept {
  return value.size() >= kPrefix.size() && value.compare(0, kPrefix.size(), kPrefix) == 0;
}

template <typename UInt>
bool parse_decimal(std::string_view text, UInt* out) noexcept {
  if (out == nullptr || text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  UInt parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
  if (parsed > static_cast<UInt>(std::numeric_limits<uint32_t>::max() - 1u)) return false;
  *out = parsed;
  return true;
}

template <typename UInt>
void append_le(std::vector<uint8_t>* bytes, UInt value) {
  for (unsigned shift = 0; shift < sizeof(UInt) * 8; shift += 8) {
    bytes->push_back(static_cast<uint8_t>(value >> shift));
  }
}

template <typename UInt>
UInt read_le(const std::vector<uint8_t>& bytes, size_t offset) noexcept {
  UInt value = 0;
  for (unsigned shift = 0; shift < sizeof(UInt) * 8; shift += 8) {
    value |= static_cast<UInt>(bytes[offset + shift / 8]) << shift;
  }
  return value;
}

bool parseable_vocal(const AssistSidecar& sidecar, Key* key = nullptr) noexcept {
  const auto parsed = parse_key(sidecar.module_id);
  if (!parsed.has_value()) return false;
  if (key != nullptr) *key = *parsed;
  return true;
}

struct CapturedVocal {
  std::vector<ClipId> scope;
  std::vector<AssistSidecar> entries;
  std::vector<size_t> out_of_scope_anchors;
};

bool in_scope(const AssistSidecar& sidecar, const std::vector<ClipId>& scope) noexcept {
  Key key;
  return parseable_vocal(sidecar, &key) &&
         std::find(scope.begin(), scope.end(), key.clip_id) != scope.end();
}

CapturedVocal capture_vocal(const Project& project, std::vector<ClipId> scope) {
  CapturedVocal captured;
  captured.scope = std::move(scope);
  size_t out_of_scope_count = 0;
  for (const AssistSidecar& sidecar : project.assist_sidecars()) {
    if (in_scope(sidecar, captured.scope)) {
      captured.entries.push_back(sidecar);
      captured.out_of_scope_anchors.push_back(out_of_scope_count);
    } else {
      ++out_of_scope_count;
    }
  }
  return captured;
}

std::vector<AssistSidecar> merged_vocal(const Project& project, const CapturedVocal& captured) {
  std::vector<AssistSidecar> restored;
  restored.reserve(project.assist_sidecars().size() + captured.entries.size());
  size_t out_of_scope_count = 0;
  size_t captured_index = 0;
  for (const AssistSidecar& current : project.assist_sidecars()) {
    if (in_scope(current, captured.scope)) continue;
    while (captured_index < captured.entries.size() &&
           captured.out_of_scope_anchors[captured_index] <= out_of_scope_count) {
      restored.push_back(captured.entries[captured_index]);
      ++captured_index;
    }
    restored.push_back(current);
    ++out_of_scope_count;
  }
  while (captured_index < captured.entries.size()) {
    restored.push_back(captured.entries[captured_index]);
    ++captured_index;
  }
  return restored;
}

size_t captured_bytes(const CapturedVocal& captured) noexcept {
  size_t total = retained::dynamic_bytes(captured.entries);
  total = retained::saturating_add(total, retained::dynamic_bytes(captured.scope));
  return retained::saturating_add(total, retained::dynamic_bytes(captured.out_of_scope_anchors));
}

SourceId resolved_source(const EditClip& clip, TakeId take_id, bool* exists) noexcept {
  if (take_id == 0) {
    *exists = true;
    return clip.source_id;
  }
  for (const ClipTake& take : clip.takes) {
    if (take.id != take_id) continue;
    *exists = true;
    return take.source_id == 0 ? clip.source_id : take.source_id;
  }
  *exists = false;
  return 0;
}

class RestoreVocalCommand final : public EditCommand {
 public:
  RestoreVocalCommand(EditCommandPtr inner, CapturedVocal captured)
      : inner_(std::move(inner)), captured_(std::move(captured)) {}

  bool apply(Project& project, MidiContentStore& store) override {
    // Built before the inner inverse so the final swap is the no-throw publication point.
    std::vector<AssistSidecar> restored = merged_vocal(project, captured_);
    if (inner_ != nullptr && !inner_->apply(project, store)) return false;
    project.assist_sidecars_mutable().swap(restored);
    return true;
  }

  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& store_before) const override {
    if (inner_ == nullptr) return nullptr;
    EditCommandPtr inverse = inner_->invert(before, store_before);
    if (inverse == nullptr) return nullptr;
    return std::make_unique<RestoreVocalCommand>(std::move(inverse),
                                                 capture_vocal(before, captured_.scope));
  }

  const char* type_name() const noexcept override { return "RestoreVocalSidecars"; }

  bool mutates_midi_store() const noexcept override {
    return inner_ != nullptr && inner_->mutates_midi_store();
  }

  size_t retained_bytes() const noexcept override {
    size_t total = retained::saturating_add(sizeof(*this), captured_bytes(captured_));
    if (inner_ != nullptr) total = retained::saturating_add(total, inner_->retained_bytes());
    return total;
  }

 private:
  EditCommandPtr inner_;
  CapturedVocal captured_;
};

class UpsertCommand final : public EditCommand {
 public:
  explicit UpsertCommand(AssistSidecar sidecar) : sidecar_(std::move(sidecar)) {}

  bool apply(Project& project, MidiContentStore& /*store*/) override {
    const auto key = parse_key(sidecar_.module_id);
    if (!key.has_value() || sidecar_.schema_version != kSchemaVersion) return false;
    std::vector<AssistSidecar> next;
    next.reserve(project.assist_sidecars().size() + 1);
    for (const AssistSidecar& current : project.assist_sidecars()) {
      Key current_key;
      if (parseable_vocal(current, &current_key) && current_key.clip_id == key->clip_id &&
          current_key.take_id == key->take_id) {
        continue;
      }
      next.push_back(current);
    }
    next.push_back(sidecar_);
    project.assist_sidecars_mutable().swap(next);
    return true;
  }

  EditCommandPtr invert(const Project& before,
                        const MidiContentStore& /*store_before*/) const override {
    const auto key = parse_key(sidecar_.module_id);
    if (!key.has_value()) return nullptr;
    return std::make_unique<RestoreVocalCommand>(nullptr, capture_vocal(before, {key->clip_id}));
  }

  const char* type_name() const noexcept override { return "UpsertVocalSidecar"; }
  bool mutates_midi_store() const noexcept override { return false; }
  size_t retained_bytes() const noexcept override {
    return retained::saturating_add(sizeof(*this), retained::dynamic_bytes(sidecar_));
  }

 private:
  AssistSidecar sidecar_;
};

}  // namespace

std::optional<Key> parse_key(std::string_view module_id) noexcept {
  try {
    if (!has_prefix(module_id)) return std::nullopt;
    const std::string_view rest = module_id.substr(kPrefix.size());
    constexpr std::string_view separator = "/take/";
    const size_t split = rest.find(separator);
    if (split == std::string_view::npos) return std::nullopt;
    uint32_t clip_id = 0;
    uint32_t take_id = 0;
    if (!parse_decimal(rest.substr(0, split), &clip_id) ||
        !parse_decimal(rest.substr(split + separator.size()), &take_id)) {
      return std::nullopt;
    }
    if (clip_id == 0) return std::nullopt;
    return Key{clip_id, take_id};
  } catch (...) {
    return std::nullopt;
  }
}

std::string make_key(Key key) {
  if (key.clip_id == 0 || key.clip_id == std::numeric_limits<uint32_t>::max() ||
      key.take_id == std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("invalid vocal sidecar key");
  }
  return std::string(kPrefix) + std::to_string(key.clip_id) + "/take/" +
         std::to_string(key.take_id);
}

bool decode_envelope(const AssistSidecar& sidecar, Envelope* out) noexcept {
  if (out == nullptr || sidecar.schema_version != kSchemaVersion ||
      sidecar.payload.size() < kEnvelopeHeaderBytes) {
    return false;
  }
  const std::vector<uint8_t>& bytes = sidecar.payload;
  if (bytes[0] != 'S' || bytes[1] != 'V' || bytes[2] != 'P' || bytes[3] != '1' ||
      read_le<uint16_t>(bytes, 4) != kEnvelopeVersion ||
      read_le<uint16_t>(bytes, 6) != kEnvelopeFlags) {
    return false;
  }
  const uint32_t original_source_id = read_le<uint32_t>(bytes, 8);
  const uint32_t derived_source_id = read_le<uint32_t>(bytes, 12);
  const uint32_t sample_rate = read_le<uint32_t>(bytes, 16);
  const uint32_t profile_id = read_le<uint32_t>(bytes, 20);
  const int64_t sample_count = static_cast<int64_t>(read_le<uint64_t>(bytes, 24));
  const uint64_t revision = read_le<uint64_t>(bytes, 32);
  const uint64_t sve_size = read_le<uint64_t>(bytes, 104);
  if (original_source_id == 0 || derived_source_id == 0 || sample_rate == 0 || sample_count <= 0 ||
      sve_size > static_cast<uint64_t>(bytes.size() - kEnvelopeHeaderBytes) ||
      kEnvelopeHeaderBytes + static_cast<size_t>(sve_size) != bytes.size()) {
    return false;
  }
  Envelope decoded;
  decoded.original_source_id = original_source_id;
  decoded.derived_source_id = derived_source_id;
  decoded.source_sample_rate = sample_rate;
  decoded.profile_id = profile_id;
  decoded.source_sample_count = sample_count;
  decoded.committed_revision = revision;
  std::memcpy(decoded.original_digest.data(), bytes.data() + 40, decoded.original_digest.size());
  std::memcpy(decoded.derived_digest.data(), bytes.data() + 72, decoded.derived_digest.size());
  try {
    decoded.sve1.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kEnvelopeHeaderBytes),
                        bytes.end());
    *out = std::move(decoded);
  } catch (...) {
    return false;
  }
  return true;
}

AssistSidecar encode_envelope(Key key, const Envelope& envelope) {
  AssistSidecar sidecar;
  sidecar.module_id = make_key(key);
  sidecar.schema_version = kSchemaVersion;
  sidecar.target_track_id = 0;
  sidecar.region_start_ppq = 0.0;
  sidecar.region_end_ppq = 0.0;
  sidecar.payload.reserve(kEnvelopeHeaderBytes + envelope.sve1.size());
  sidecar.payload.insert(sidecar.payload.end(), {'S', 'V', 'P', '1'});
  append_le<uint16_t>(&sidecar.payload, kEnvelopeVersion);
  append_le<uint16_t>(&sidecar.payload, kEnvelopeFlags);
  append_le<uint32_t>(&sidecar.payload, envelope.original_source_id);
  append_le<uint32_t>(&sidecar.payload, envelope.derived_source_id);
  append_le<uint32_t>(&sidecar.payload, envelope.source_sample_rate);
  append_le<uint32_t>(&sidecar.payload, envelope.profile_id);
  append_le<uint64_t>(&sidecar.payload, static_cast<uint64_t>(envelope.source_sample_count));
  append_le<uint64_t>(&sidecar.payload, envelope.committed_revision);
  sidecar.payload.insert(sidecar.payload.end(), envelope.original_digest.begin(),
                         envelope.original_digest.end());
  sidecar.payload.insert(sidecar.payload.end(), envelope.derived_digest.begin(),
                         envelope.derived_digest.end());
  append_le<uint64_t>(&sidecar.payload, static_cast<uint64_t>(envelope.sve1.size()));
  sidecar.payload.insert(sidecar.payload.end(), envelope.sve1.begin(), envelope.sve1.end());
  return sidecar;
}

void remove_clip_sidecars(Project* project, ClipId clip_id) {
  if (project == nullptr) return;
  auto& sidecars = project->assist_sidecars_mutable();
  sidecars.erase(std::remove_if(sidecars.begin(), sidecars.end(),
                                [clip_id](const AssistSidecar& sidecar) {
                                  const auto key = parse_key(sidecar.module_id);
                                  return key.has_value() && key->clip_id == clip_id;
                                }),
                 sidecars.end());
}

void clone_clip_sidecars(Project* project, ClipId from, ClipId to) {
  if (project == nullptr || from == 0 || to == 0 || from == to) return;
  std::vector<AssistSidecar> source_entries;
  std::set<TakeId> source_takes;
  for (const AssistSidecar& sidecar : project->assist_sidecars()) {
    const auto key = parse_key(sidecar.module_id);
    if (key.has_value() && key->clip_id == from) {
      source_entries.push_back(sidecar);
      source_takes.insert(key->take_id);
    }
  }
  if (source_entries.empty()) return;
  std::vector<AssistSidecar> next;
  next.reserve(project->assist_sidecars().size() + source_entries.size());
  for (const AssistSidecar& sidecar : project->assist_sidecars()) {
    const auto key = parse_key(sidecar.module_id);
    if (key.has_value() && key->clip_id == to && source_takes.count(key->take_id) != 0) continue;
    next.push_back(sidecar);
  }
  for (const AssistSidecar& source : source_entries) {
    const auto key = parse_key(source.module_id);
    AssistSidecar clone = source;
    clone.module_id = make_key({to, key->take_id});
    next.push_back(std::move(clone));
  }
  project->assist_sidecars_mutable().swap(next);
}

void prune_changed_bindings(Project* project, const EditClip& before, const EditClip& after) {
  if (project == nullptr || before.id != after.id) return;
  auto& sidecars = project->assist_sidecars_mutable();
  sidecars.erase(std::remove_if(sidecars.begin(), sidecars.end(),
                                [&](const AssistSidecar& sidecar) {
                                  const auto key = parse_key(sidecar.module_id);
                                  if (!key.has_value() || key->clip_id != after.id) return false;
                                  bool before_exists = false;
                                  bool after_exists = false;
                                  const SourceId before_source =
                                      resolved_source(before, key->take_id, &before_exists);
                                  const SourceId after_source =
                                      resolved_source(after, key->take_id, &after_exists);
                                  return !before_exists || !after_exists ||
                                         before_source != after_source;
                                }),
                 sidecars.end());
}

EditCommandPtr wrap_inverse(EditCommandPtr ordinary_inverse, const Project& before,
                            std::vector<ClipId> affected_clip_ids) {
  if (ordinary_inverse == nullptr) return nullptr;
  return std::make_unique<RestoreVocalCommand>(std::move(ordinary_inverse),
                                               capture_vocal(before, std::move(affected_clip_ids)));
}

EditCommandPtr make_upsert_command(AssistSidecar canonical_sidecar) {
  return std::make_unique<UpsertCommand>(std::move(canonical_sidecar));
}

std::vector<SourceId> collect_orphaned_sources_after_removing_clips(
    const Project& project, const std::vector<ClipId>& removed_clip_ids) {
  std::unordered_set<ClipId> removed(removed_clip_ids.begin(), removed_clip_ids.end());
  std::set<SourceId> candidates;
  std::set<SourceId> live;
  for (const EditClip& clip : project.clips()) {
    const bool is_removed = removed.count(clip.id) != 0;
    auto add_binding = [&](SourceId source_id) {
      if (source_id == 0) return;
      (is_removed ? candidates : live).insert(source_id);
    };
    add_binding(clip.source_id);
    for (const ClipTake& take : clip.takes) {
      add_binding(take.source_id == 0 ? clip.source_id : take.source_id);
    }
  }

  for (const AssistSidecar& sidecar : project.assist_sidecars()) {
    if (!has_prefix(sidecar.module_id)) continue;
    const auto key = parse_key(sidecar.module_id);
    if (!key.has_value()) return {};
    Envelope envelope;
    if (!decode_envelope(sidecar, &envelope)) return {};
    const bool is_removed = removed.count(key->clip_id) != 0;
    auto add_reference = [&](SourceId source_id) {
      if (source_id == 0) return;
      (is_removed ? candidates : live).insert(source_id);
    };
    add_reference(envelope.original_source_id);
    add_reference(envelope.derived_source_id);
  }

  std::vector<SourceId> orphaned;
  for (const ClipSource& source : project.sources()) {
    const SourceId id = source_id(source);
    if (candidates.count(id) != 0 && live.count(id) == 0) orphaned.push_back(id);
  }
  return orphaned;
}

}  // namespace sonare::arrangement::vocal_sidecar
