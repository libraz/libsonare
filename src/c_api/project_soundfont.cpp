#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

#include "midi/synth/sf2_player.h"
#include "util/resource_limits.h"

namespace {

namespace synth = sonare::midi::synth;

/// SF2 drum bank (channel 10 percussion per the GS convention).
constexpr uint16_t kManifestDrumBank = 128;
constexpr uint8_t kGm2MelodicBankMsb = 0x79;
constexpr uint8_t kGm2PercussionBankMsb = 0x78;

/// Per-(destination, channel) program/bank state while scanning the compiled
/// event streams in time order. Mirrors Sf2Player's channel state subset that
/// affects preset resolution.
struct ScanChannelState {
  uint8_t bank_msb = 0;
  uint8_t bank_lsb = 0;
  uint8_t program = 0;
  bool drums = false;
};

/// Copies a preset name into the fixed manifest field (truncating, always
/// NUL-terminated).
void copy_preset_name(char (&dest)[64], const std::string& name) {
  sonare_c_detail::copy_text(dest, sizeof(dest), name.c_str());
}

uint16_t effective_scan_bank(const ScanChannelState& state) noexcept {
  if (state.drums || state.bank_msb == kGm2PercussionBankMsb) return kManifestDrumBank;
  if (state.bank_msb == kGm2MelodicBankMsb) return state.bank_lsb;
  return state.bank_msb;
}

/// Returns whether @p preset_index has at least one playable sample zone for
/// this note. This is the same preset/instrument zone traversal as
/// Sf2Player::note_on, without allocating a voice: a preset can exist while a
/// key- or velocity-limited zone leaves a particular note to the synth fallback.
bool has_renderable_zone(const synth::Sf2File& soundfont, int preset_index, uint8_t note,
                         uint8_t velocity) noexcept {
  if (preset_index < 0 || static_cast<size_t>(preset_index) >= soundfont.presets().size()) {
    return false;
  }
  const synth::Sf2Preset& preset = soundfont.presets()[static_cast<size_t>(preset_index)];
  const auto& instruments = soundfont.instruments();
  const auto& samples = soundfont.samples();
  const size_t pool_size = soundfont.sample_pool().size();
  for (const synth::Sf2Zone& preset_zone : preset.zones) {
    if (preset_zone.is_global() || !preset_zone.matches(note, velocity) ||
        preset_zone.instrument < 0 ||
        static_cast<size_t>(preset_zone.instrument) >= instruments.size()) {
      continue;
    }
    const synth::Sf2Instrument& instrument =
        instruments[static_cast<size_t>(preset_zone.instrument)];
    for (const synth::Sf2Zone& instrument_zone : instrument.zones) {
      if (instrument_zone.is_global() || !instrument_zone.matches(note, velocity) ||
          instrument_zone.sample < 0 ||
          static_cast<size_t>(instrument_zone.sample) >= samples.size()) {
        continue;
      }
      const synth::Sf2Sample& sample = samples[static_cast<size_t>(instrument_zone.sample)];
      if (!sample.is_rom() && synth::valid_sf2_sample_rate(sample.sample_rate) &&
          sample.end > sample.start && sample.end <= pool_size) {
        return true;
      }
    }
  }
  return false;
}

/// Builds the bounce manifest from the compiled timeline: every
/// (channel, effective bank, program) combination a note-on actually plays
/// through, in first-use order, resolved against the loaded SoundFont with the
/// same GS fallback rule and note/velocity zone checks the player uses.
std::vector<SonareSf2ProgramStatus> build_manifest(const arr::CompiledTimeline& timeline,
                                                   const synth::Sf2File* soundfont) {
  // Merge all clip events into one (render_frame, destination, ump) stream.
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

  // (destination, channel) -> scan state. Channel 10 (index 9) is a drum part
  // by GS power-on convention.
  std::map<std::pair<uint32_t, uint8_t>, ScanChannelState> states;
  const auto state_for = [&](uint32_t destination, uint8_t channel) -> ScanChannelState& {
    auto [it, inserted] = states.try_emplace({destination, channel});
    if (inserted && channel == 9) it->second.drums = true;
    return it->second;
  };

  std::vector<SonareSf2ProgramStatus> manifest;
  std::map<std::tuple<uint8_t, uint16_t, uint8_t>, size_t> manifest_index;
  using sonare::midi::UmpMessageType;
  using sonare::midi::UmpStatus;

  for (const ScanEvent& scan : events) {
    const sonare::midi::Ump& u = scan.event->ump;
    if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
        u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
      // GS/GM SysEx affects drum assignment ("use for rhythm part") and resets.
      if (scan.event->sysex_payload != nullptr && scan.event->sysex_payload_size > 0) {
        const synth::GsSysEx msg =
            synth::parse_gs_sysex(scan.event->sysex_payload, scan.event->sysex_payload_size);
        switch (msg.kind) {
          case synth::GsSysExKind::kGmReset:
          case synth::GsSysExKind::kGsReset:
            for (auto& [key, state] : states) {
              if (key.first != scan.destination_id) continue;
              state = ScanChannelState{};
              if (key.second == 9) state.drums = true;
            }
            break;
          case synth::GsSysExKind::kUseForRhythm:
            state_for(scan.destination_id, msg.channel & 0x0Fu).drums = msg.value != 0;
            break;
          case synth::GsSysExKind::kEfxPartSwitch:
          case synth::GsSysExKind::kNone:
            break;
        }
      }
      continue;
    }
    const uint8_t channel = u.channel() & 0x0Fu;
    ScanChannelState& state = state_for(scan.destination_id, channel);
    if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
      if (u.message_type() == UmpMessageType::kMidi2ChannelVoice) {
        state.program = static_cast<uint8_t>((u.words[1] >> 24) & 0x7Fu);
        if ((u.words[0] & 0x01u) != 0) {
          state.bank_msb = static_cast<uint8_t>((u.words[1] >> 8) & 0x7Fu);
          state.bank_lsb = static_cast<uint8_t>(u.words[1] & 0x7Fu);
        }
      } else {
        state.program = u.note_number();
      }
    } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
      if (u.note_number() == 0) {  // CC0 bank select MSB (GS variation bank)
        state.bank_msb = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                             ? u.data2_7bit()
                             : sonare::midi::scale_cc_32_to_7(u.words[1]);
      } else if (u.note_number() == 32) {
        state.bank_lsb = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                             ? u.data2_7bit()
                             : sonare::midi::scale_cc_32_to_7(u.words[1]);
      }
    } else if (u.is_note_on()) {
      const uint16_t bank = effective_scan_bank(state);
      const auto key = std::make_tuple(channel, bank, state.program);
      const uint8_t velocity = u.message_type() == UmpMessageType::kMidi2ChannelVoice
                                   ? sonare::midi::scale_note_on_velocity_16_to_7(
                                         static_cast<uint16_t>(u.words[1] >> 16))
                                   : u.data2_7bit();
      int preset = -1;
      bool sf2_renders_note = false;
      if (soundfont != nullptr) {
        preset = synth::resolve_gs_preset(*soundfont, bank, state.program);
        sf2_renders_note = has_renderable_zone(*soundfont, preset, u.note_number(), velocity);
      }
      const auto existing = manifest_index.find(key);
      if (existing != manifest_index.end()) {
        // One ABI entry represents all note-ons for this channel/bank/program.
        // Be conservative: if any played note escapes the SF2's zones, callers
        // must not be told the complete program is SF2-covered.
        if (!sf2_renders_note) {
          SonareSf2ProgramStatus& entry = manifest[existing->second];
          entry.backend = SONARE_SOURCE_BACKEND_SYNTH;
          entry.preset_name[0] = '\0';
        }
        continue;
      }
      SonareSf2ProgramStatus entry{};
      entry.channel = channel;
      entry.program = state.program;
      entry.bank = bank;
      entry.backend = SONARE_SOURCE_BACKEND_SYNTH;
      if (sf2_renders_note) {
        entry.backend = SONARE_SOURCE_BACKEND_SF2;
        copy_preset_name(entry.preset_name, soundfont->presets()[static_cast<size_t>(preset)].name);
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
  if (!sonare::resource::sf2_file_fits(size)) {
    sonare_c_detail::set_last_error("sf2: file resource limit exceeded");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto soundfont = std::make_shared<synth::Sf2File>();
  std::string error;
  if (!soundfont->parse(data, size, &error)) {
    sonare_c_detail::set_last_error(error.c_str());
    return SONARE_ERROR_INVALID_PARAMETER;
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
  const std::vector<SonareSf2ProgramStatus> manifest =
      build_manifest(*compiled.timeline, project->soundfont.get());
  *out_count = manifest.size();
  const size_t to_write = std::min(max_entries, manifest.size());
  for (size_t i = 0; i < to_write; ++i) out[i] = manifest[i];
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out, max_entries, out_count);
#endif
}
