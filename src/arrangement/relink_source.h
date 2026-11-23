#pragma once

/// @file relink_source.h
/// @brief Undoable source relinking for arrangement projects.

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"

namespace sonare::arrangement {

enum class RelinkSourceError {
  kNone,
  kNullAudioContentStore,
  kNullUri,
  kEmptyUri,
  kUnknownSource,
  kNotAudioSource,
};

struct RelinkSourceCommands {
  std::vector<EditCommandPtr> commands;
  RelinkSourceError error = RelinkSourceError::kNone;

  explicit operator bool() const noexcept { return error == RelinkSourceError::kNone; }
};

namespace detail {

struct RelinkAudioContentState {
  AudioContentStore* store = nullptr;
  SourceId source_id = 0;
  std::map<SourceId, AudioSourceSamples> contents;
  std::map<WarpedAudioKey, AudioSourceSamples> warped_contents;
};

template <typename Map, typename Key>
void swap_audio_content_entry(Map& live, Map& saved, const Key& key) noexcept {
  using Value = typename Map::mapped_type;
  static_assert(std::is_nothrow_swappable_v<Value>);
  const auto live_entry = live.find(key);
  const auto saved_entry = saved.find(key);
  if (live_entry != live.end() && saved_entry != saved.end()) {
    using std::swap;
    swap(live_entry->second, saved_entry->second);
  } else if (live_entry != live.end()) {
    saved.insert(live.extract(live_entry));
  } else if (saved_entry != saved.end()) {
    live.insert(saved.extract(saved_entry));
  }
}

inline void swap_relinked_audio_content(RelinkAudioContentState& state) noexcept {
  if (state.store == nullptr || state.source_id == 0) return;
  swap_audio_content_entry(state.store->sources, state.contents, state.source_id);

  auto& live = state.store->warped_sources;
  auto& saved = state.warped_contents;
  const WarpedAudioKey first{state.source_id, 0};
  auto live_it = live.lower_bound(first);
  auto saved_it = saved.lower_bound(first);
  while ((live_it != live.end() && live_it->first.source_id == state.source_id) ||
         (saved_it != saved.end() && saved_it->first.source_id == state.source_id)) {
    const bool has_live = live_it != live.end() && live_it->first.source_id == state.source_id;
    const bool has_saved = saved_it != saved.end() && saved_it->first.source_id == state.source_id;
    if (!has_saved || (has_live && live_it->first < saved_it->first)) {
      auto node = live.extract(live_it++);
      saved.insert(std::move(node));
    } else if (!has_live || saved_it->first < live_it->first) {
      auto node = saved.extract(saved_it++);
      live.insert(std::move(node));
    } else {
      using std::swap;
      swap(live_it->second, saved_it->second);
      ++live_it;
      ++saved_it;
    }
  }
}

class RelinkAudioContent final : public EditCommand, public EditCommandExternalEffects {
 public:
  explicit RelinkAudioContent(std::shared_ptr<RelinkAudioContentState> state)
      : state_(std::move(state)) {}

  bool apply(Project& /*project*/, MidiContentStore& /*midi*/) override {
    applied_ = false;
    if (state_ == nullptr || state_->store == nullptr || state_->source_id == 0) return false;
    swap_relinked_audio_content(*state_);
    applied_ = true;
    return true;
  }

  void rollback_apply(Project& /*project*/, MidiContentStore& /*midi*/) noexcept override {
    if (!applied_ || state_ == nullptr || state_->store == nullptr) return;
    swap_relinked_audio_content(*state_);
    applied_ = false;
  }

  bool affects_audio_source(SourceId source_id) const noexcept override {
    return state_ != nullptr && state_->source_id == source_id;
  }

  // Relinking changes source content, not the project's shared warp map definitions.
  bool affects_warp_source(WarpRefId /*warp_id*/) const noexcept override { return false; }

  EditCommandPtr invert(const Project& /*before*/,
                        const MidiContentStore& /*midi_before*/) const override {
    return std::make_unique<RelinkAudioContent>(state_);
  }

  const char* type_name() const noexcept override { return "RelinkAudioContent"; }

  std::size_t retained_bytes() const noexcept override {
    std::size_t total = sizeof(*this);
    if (state_ != nullptr) {
      total = retained::saturating_add(total, sizeof(*state_));
      total = retained::saturating_add(total, retained::dynamic_bytes(state_->contents));
      total = retained::saturating_add(total, retained::dynamic_bytes(state_->warped_contents));
    }
    return total;
  }

  bool mutates_midi_store() const noexcept override { return false; }
  ProjectMemberMask affected_members() const noexcept override { return 0; }
  ProjectMemberMask invert_reads_members() const noexcept override { return 0; }
  bool invert_reads_store() const noexcept override { return false; }

 private:
  std::shared_ptr<RelinkAudioContentState> state_;
  bool applied_ = false;
};

}  // namespace detail

inline RelinkSourceCommands make_relink_source_commands(const Project& project,
                                                        AudioContentStore* audio,
                                                        SourceId source_id, const char* uri) {
  RelinkSourceCommands result;
  if (audio == nullptr) {
    result.error = RelinkSourceError::kNullAudioContentStore;
    return result;
  }
  if (uri == nullptr) {
    result.error = RelinkSourceError::kNullUri;
    return result;
  }
  if (uri[0] == '\0') {
    result.error = RelinkSourceError::kEmptyUri;
    return result;
  }

  const ClipSource* source = project.find_source(source_id);
  if (source == nullptr) {
    result.error = RelinkSourceError::kUnknownSource;
    return result;
  }
  const auto* audio_ref = std::get_if<AudioSourceRef>(source);
  if (audio_ref == nullptr) {
    result.error = RelinkSourceError::kNotAudioSource;
    return result;
  }

  AudioSourceRef replacement = *audio_ref;
  replacement.uri = uri;
  replacement.content_hash.clear();
  replacement.channel_count = 0;
  replacement.sample_rate_hint = 0.0;

  auto state = std::make_shared<detail::RelinkAudioContentState>();
  state->store = audio;
  state->source_id = source_id;

  result.commands.reserve(2);
  result.commands.push_back(
      std::make_unique<ReplaceSource>(source_id, ClipSource{std::move(replacement)}));
  result.commands.push_back(std::make_unique<detail::RelinkAudioContent>(std::move(state)));
  return result;
}

}  // namespace sonare::arrangement
