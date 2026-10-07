#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

#include "midi/channel_voice_decode.h"
#include "midi/synth/sf2_player.h"
#include "util/resource_limits.h"

namespace {

namespace synth = sonare::midi::synth;

/// Copies a preset name into the fixed manifest field (truncating, always
/// NUL-terminated).
void copy_preset_name(char (&dest)[64], const std::string& name) {
  sonare_c_detail::copy_text(dest, sizeof(dest), name.c_str());
}

/// A player that only answers resolve_note_on: the default SF2-first config
/// the manifest reports, with the smallest voice pool, since it never sounds.
std::unique_ptr<synth::Sf2Player> make_scan_player(
    const std::shared_ptr<const synth::Sf2File>& soundfont, double sample_rate) {
  synth::Sf2PlayerConfig cfg;
  cfg.polyphony = 1;
  auto player = std::make_unique<synth::Sf2Player>(cfg);
  player->set_soundfont(soundfont);
  player->prepare(sample_rate, 1);
  return player;
}

/// Builds the bounce manifest from the compiled timeline: every
/// (channel, effective bank, program) combination a note-on actually plays
/// through, in first-use order. Each destination's events drive a player of its
/// own in time order, and every note-on is asked of that player, so the bank,
/// the preset and whether a zone renders are the player's own resolution.
std::vector<SonareSf2ProgramStatus> build_manifest(
    const arr::CompiledTimeline& timeline, const std::shared_ptr<const synth::Sf2File>& soundfont,
    double sample_rate) {
  // Merge all clip events into one (render_frame, destination, event) stream.
  struct ScanEvent {
    int64_t render_frame = 0;
    uint32_t destination_id = 0;
    const sonare::midi::MidiEvent* event = nullptr;
  };
  std::vector<ScanEvent> events;
  for (const auto& clip : timeline.midi_clips) {
    events.reserve(events.size() + clip.events.size());
    for (const auto& event : clip.events) {
      events.push_back({event.render_frame, clip.destination_id, &event});
    }
  }
  std::stable_sort(events.begin(), events.end(), [](const ScanEvent& a, const ScanEvent& b) {
    return a.render_frame < b.render_frame;
  });

  std::map<uint32_t, std::unique_ptr<synth::Sf2Player>> players;
  const auto player_for = [&](uint32_t destination) -> synth::Sf2Player& {
    auto [it, inserted] = players.try_emplace(destination);
    if (inserted) it->second = make_scan_player(soundfont, sample_rate);
    return *it->second;
  };

  std::vector<SonareSf2ProgramStatus> manifest;
  std::map<std::tuple<uint8_t, uint16_t, uint8_t>, size_t> manifest_index;
  using sonare::midi::ChannelVoiceKind;
  using synth::Sf2Player;

  for (const ScanEvent& scan : events) {
    Sf2Player& player = player_for(scan.destination_id);
    sonare::midi::ChannelVoiceEvent ev;
    const bool channel_voice = sonare::midi::decode_channel_voice(scan.event->ump, &ev);
    // Only a note-on is asked rather than played, so the scan player never
    // sounds; a note-off changes nothing a later note-on resolves through.
    if (!channel_voice || ev.kind != ChannelVoiceKind::NoteOn) {
      if (!channel_voice || ev.kind != ChannelVoiceKind::NoteOff) {
        player.on_event(scan.destination_id, *scan.event);
      }
      continue;
    }
    const uint8_t channel = ev.channel & 0x0Fu;
    const uint16_t parts = player.parts_receiving(scan.event->ump);
    for (uint8_t part = 0; part < 16; ++part) {
      if ((parts & (1u << part)) == 0) continue;
      const Sf2Player::NoteResolution note =
          player.resolve_note_on(part, ev.note, ev.velocity, ev.index, ev.attribute_data);
      // A note the part refuses plays nothing, so it neither adds an entry nor
      // demotes one.
      if (note.backend == Sf2Player::NoteBackend::kNone) continue;
      const bool sf2 = note.backend == Sf2Player::NoteBackend::kSoundFont;
      const auto key = std::make_tuple(channel, note.bank, note.program);
      const auto existing = manifest_index.find(key);
      if (existing != manifest_index.end()) {
        // One ABI entry represents all note-ons for this channel/bank/program.
        // Be conservative: if any played note escapes the SF2's zones, callers
        // must not be told the complete program is SF2-covered.
        if (!sf2) {
          SonareSf2ProgramStatus& entry = manifest[existing->second];
          entry.backend = SONARE_SOURCE_BACKEND_SYNTH;
          entry.preset_name[0] = '\0';
        }
        continue;
      }
      SonareSf2ProgramStatus entry{};
      entry.channel = channel;
      entry.program = note.program;
      entry.bank = note.bank;
      entry.backend = SONARE_SOURCE_BACKEND_SYNTH;
      if (sf2) {
        entry.backend = SONARE_SOURCE_BACKEND_SF2;
        copy_preset_name(entry.preset_name,
                         soundfont->presets()[static_cast<size_t>(note.preset_index)].name);
      }
      manifest_index.emplace(key, manifest.size());
      manifest.push_back(entry);
    }
  }
  return manifest;
}

}  // namespace
#endif

SonareError sonare_project_load_soundfont(SonareProject* project, const uint8_t* data,
                                          size_t size) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !data || size == 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  // Both rejections are statements about the supplied data rather than about the
  // call's arguments, so they carry INVALID_FORMAT and leave INVALID_PARAMETER
  // to the null/empty check above. That is the rule the project MIDI import
  // documents for its own budget rejection, and the code the engine's soundfont
  // loader already returns for these two conditions.
  if (!sonare::resource::sf2_file_fits(size)) {
    sonare_c_detail::set_last_error("sf2: file resource limit exceeded");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto soundfont = std::make_shared<synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    sonare_c_detail::set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  project->soundfont = std::move(soundfont);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, data, size);
#endif
}

SonareError sonare_project_clear_soundfont(SonareProject* project) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  project->soundfont.reset();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project);
#endif
}

SonareError sonare_project_soundfont_preset_count(SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = project->soundfont ? project->soundfont->presets().size() : 0;
  return SONARE_OK;
#else
  if (out_count) *out_count = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_soundfont_manifest(SonareProject* project, SonareSf2ProgramStatus* out,
                                              size_t max_entries, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_count || (max_entries > 0 && out == nullptr)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = 0;
  SONARE_C_TRY
  arr::CompileResult compiled =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio, {});
  if (!compiled.timeline.has_value()) return SONARE_ERROR_INVALID_STATE;
  const std::vector<SonareSf2ProgramStatus> manifest = build_manifest(
      *compiled.timeline, project->soundfont, project->history.project().sample_rate());
  *out_count = manifest.size();
  const size_t to_write = std::min(max_entries, manifest.size());
  for (size_t i = 0; i < to_write; ++i) out[i] = manifest[i];
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  SONARE_C_STUB_NOT_SUPPORTED(project, out, max_entries, out_count);
#endif
}
