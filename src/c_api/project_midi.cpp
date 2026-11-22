#include "c_api/midi_input_domains.h"
#include "c_api/ppq_domains.h"
#include "c_api/project_internal.h"
#include "util/parameter_checks.h"

using sonare::constants::kDefaultBpm;

namespace check = sonare::util::check;

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// The refusal for a clip_id that names no MIDI clip in the project.
SonareError refuse_unknown_midi_clip() {
  return run_validation([] { check::refuse_unknown_id("clip_id", "a MIDI clip", "project"); });
}

// Refuses a MIDI FX chain the clip's events cannot pass through: one expanding a
// chunk past the chain's output buffer, or an event it cannot place in time.
[[noreturn]] void refuse_unrenderable_midi_fx() {
  check::refuse("config_json",
                "must describe a chain the clip's events pass through without overflowing");
}

// Refuses an event whose position is out of range or that is not a channel voice message.
void check_midi_event_pod(const SonareMidiEventPod& event, const std::string& ppq_subject,
                          const std::string& word0_subject) {
  check::within(event.ppq, sonare_c_detail::kPublicPpqDomain, ppq_subject.c_str());
  const uint8_t mt = static_cast<uint8_t>((event.word0 >> 28u) & 0x0Fu);
  if (mt != static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi1ChannelVoice) &&
      mt != static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi2ChannelVoice)) {
    check::refuse(word0_subject.c_str(), "must be a MIDI 1.0 or MIDI 2.0 channel voice message");
  }
}

void check_u7(uint8_t value, const char* subject) {
  check::integer_within(value, sonare_c_detail::kMidiDataByteDomain, subject);
}

// Checks the fields every MIDI builder carries: the position, the group and the channel.
void check_midi_head(double ppq, uint8_t group, uint8_t channel) {
  check::within(ppq, sonare_c_detail::kPublicPpqDomain, "ppq");
  check::integer_within(group, sonare_c_detail::kMidiUmpGroupDomain, "group");
  check::integer_within(channel, sonare_c_detail::kMidiChannelDomain, "channel");
}

// Refuses through `check`, then `out` being NULL, then packs the event `build` makes into `out`.
template <typename Check, typename Build>
SonareError emit_midi_pod(SonareMidiEventPod* out, double ppq, Check&& check_fields,
                          Build&& build) {
  SONARE_C_TRY
  if (const SonareError err = run_validation(check_fields); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out);
  *out = pod_from_ump(ppq, build());
  return SONARE_OK;
  SONARE_C_CATCH
}

}  // namespace
#endif

// ============================================================================
// MIDI
// ============================================================================

SonareError sonare_project_set_midi_events(SonareProject* project, uint32_t clip_id,
                                           const SonareMidiEventPod* events, size_t count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (find_midi_clip(project, clip_id) == nullptr) return refuse_unknown_midi_clip();
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(events, count);
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), sonare_c_detail::kMidiEventCountDomain,
                              "count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  arr::MidiClipEventList list;
  list.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    check_midi_event_pod(events[i], check::indexed_subject("events", i, "ppq"),
                         check::indexed_subject("events", i, "word0"));
    arr::MidiClipEvent event;
    event.ppq = events[i].ppq;
    event.word0 = events[i].word0;
    event.word1 = events[i].word1;
    list.push_back(event);
  }
  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(list));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 clip_id, events, count);
#endif
}

SonareError sonare_midi_note_on(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                uint8_t velocity, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
        check_u7(velocity, "velocity");
      },
      [&] { return sonare::midi::make_midi1_note_on(group, channel, note, velocity); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, velocity, out);
#endif
}

SonareError sonare_midi_note_off(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                 uint8_t velocity, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
        check_u7(velocity, "velocity");
      },
      [&] { return sonare::midi::make_midi1_note_off(group, channel, note, velocity); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, velocity, out);
#endif
}

SonareError sonare_midi_cc(double ppq, uint8_t group, uint8_t channel, uint8_t controller,
                           uint8_t value, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(controller, "controller");
        check_u7(value, "value");
      },
      [&] { return sonare::midi::make_midi1_control_change(group, channel, controller, value); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, controller, value, out);
#endif
}

SonareError sonare_midi_poly_pressure(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                      uint8_t pressure, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
        check_u7(pressure, "pressure");
      },
      [&] { return sonare::midi::make_midi1_poly_pressure(group, channel, note, pressure); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, pressure, out);
#endif
}

SonareError sonare_midi_program(double ppq, uint8_t group, uint8_t channel, uint8_t program,
                                SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(program, "program");
      },
      [&] { return sonare::midi::make_midi1_program_change(group, channel, program); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, program, out);
#endif
}

SonareError sonare_midi2_note_on(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                 uint16_t velocity16, uint8_t attribute_type,
                                 uint16_t attribute_data, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] {
        return sonare::midi::make_midi2_note_on(group, channel, note, velocity16, attribute_type,
                                                attribute_data);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, velocity16, attribute_type,
                                                 attribute_data, out);
#endif
}

SonareError sonare_midi2_note_off(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                  uint16_t velocity16, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] { return sonare::midi::make_midi2_note_off(group, channel, note, velocity16); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, velocity16, out);
#endif
}

SonareError sonare_midi2_cc(double ppq, uint8_t group, uint8_t channel, uint8_t controller,
                            uint32_t value32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(controller, "controller");
      },
      [&] { return sonare::midi::make_midi2_control_change(group, channel, controller, value32); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, controller, value32, out);
#endif
}

SonareError sonare_midi2_poly_pressure(double ppq, uint8_t group, uint8_t channel, uint8_t note,
                                       uint32_t pressure32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] { return sonare::midi::make_midi2_poly_pressure(group, channel, note, pressure32); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, pressure32, out);
#endif
}

SonareError sonare_midi2_channel_pressure(double ppq, uint8_t group, uint8_t channel,
                                          uint32_t pressure32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq, [&] { check_midi_head(ppq, group, channel); },
      [&] { return sonare::midi::make_midi2_channel_pressure(group, channel, pressure32); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, pressure32, out);
#endif
}

SonareError sonare_midi2_pitch_bend(double ppq, uint8_t group, uint8_t channel, uint32_t bend32,
                                    SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq, [&] { check_midi_head(ppq, group, channel); },
      [&] { return sonare::midi::make_midi2_pitch_bend(group, channel, bend32); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bend32, out);
#endif
}

SonareError sonare_midi2_program(double ppq, uint8_t group, uint8_t channel, uint8_t program,
                                 int bank_valid, uint8_t bank_msb, uint8_t bank_lsb,
                                 SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(program, "program");
        check_u7(bank_msb, "bank_msb");
        check_u7(bank_lsb, "bank_lsb");
        if (bank_valid == 0 && (bank_msb != 0 || bank_lsb != 0)) {
          check::refuse_relation({"bank_valid", "bank_msb", "bank_lsb"},
                                 "{1} and {2} must be 0 when {0} is 0");
        }
      },
      [&] {
        return sonare::midi::make_midi2_program_change(group, channel, program, bank_msb, bank_lsb,
                                                       bank_valid != 0);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, program, bank_valid, bank_msb, bank_lsb,
                                                 out);
#endif
}

SonareError sonare_midi2_registered_controller(double ppq, uint8_t group, uint8_t channel,
                                               uint8_t bank, uint8_t index, uint32_t value32,
                                               SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(bank, "bank");
        check_u7(index, "index");
      },
      [&] {
        return sonare::midi::make_midi2_registered_controller(group, channel, bank, index, value32);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bank, index, value32, out);
#endif
}

SonareError sonare_midi2_assignable_controller(double ppq, uint8_t group, uint8_t channel,
                                               uint8_t bank, uint8_t index, uint32_t value32,
                                               SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(bank, "bank");
        check_u7(index, "index");
      },
      [&] {
        return sonare::midi::make_midi2_assignable_controller(group, channel, bank, index, value32);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bank, index, value32, out);
#endif
}

SonareError sonare_midi2_relative_registered_controller(double ppq, uint8_t group, uint8_t channel,
                                                        uint8_t bank, uint8_t index,
                                                        int32_t delta32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(bank, "bank");
        check_u7(index, "index");
      },
      [&] {
        return sonare::midi::make_midi2_relative_registered_controller(
            group, channel, bank, index, static_cast<uint32_t>(delta32));
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bank, index, delta32, out);
#endif
}

SonareError sonare_midi2_relative_assignable_controller(double ppq, uint8_t group, uint8_t channel,
                                                        uint8_t bank, uint8_t index,
                                                        int32_t delta32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(bank, "bank");
        check_u7(index, "index");
      },
      [&] {
        return sonare::midi::make_midi2_relative_assignable_controller(
            group, channel, bank, index, static_cast<uint32_t>(delta32));
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bank, index, delta32, out);
#endif
}

SonareError sonare_midi2_registered_per_note_controller(double ppq, uint8_t group, uint8_t channel,
                                                        uint8_t note, uint8_t index,
                                                        uint32_t value32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] {
        return sonare::midi::make_midi2_per_note_controller(group, channel, note, index, value32);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, index, value32, out);
#endif
}

SonareError sonare_midi2_assignable_per_note_controller(double ppq, uint8_t group, uint8_t channel,
                                                        uint8_t note, uint8_t index,
                                                        uint32_t value32, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] {
        return sonare::midi::make_midi2_assignable_per_note_controller(group, channel, note, index,
                                                                       value32);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, index, value32, out);
#endif
}

SonareError sonare_midi2_per_note_pitch_bend(double ppq, uint8_t group, uint8_t channel,
                                             uint8_t note, uint32_t bend32,
                                             SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] { return sonare::midi::make_midi2_per_note_pitch_bend(group, channel, note, bend32); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, bend32, out);
#endif
}

SonareError sonare_midi2_per_note_management(double ppq, uint8_t group, uint8_t channel,
                                             uint8_t note, int detach, int reset,
                                             SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(note, "note");
      },
      [&] {
        return sonare::midi::make_midi2_per_note_management(group, channel, note, detach != 0,
                                                            reset != 0);
      });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, note, detach, reset, out);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

const char* view_to_cstr(std::string_view value) { return value.empty() ? nullptr : value.data(); }

sonare::midi::Ump ump_from_pod(const SonareMidiEventPod& pod) noexcept {
  sonare::midi::Ump out{};
  out.words[0] = pod.word0;
  out.words[1] = pod.word1;
  out.word_count = sonare::midi::ump_word_count_for_word0(pod.word0);
  out.group = static_cast<uint8_t>((pod.word0 >> 24u) & 0x0Fu);
  return out;
}

// Refuses a binding row the table cannot hold, naming the row.
void check_cc_binding(const SonareMidiCcBinding& src, size_t index) {
  const auto subject = [index](const char* field) {
    return check::indexed_subject("bindings", index, field);
  };
  check::integer_within(src.cc_number, sonare_c_detail::kMidiDataByteDomain,
                        subject("cc_number").c_str());
  check::integer_within(src.param_id, sonare_c_detail::kMidiTargetParamIdDomain,
                        subject("param_id").c_str());
  if (src.kind > SONARE_MIDI_CC_NRPN) {
    check::refuse(subject("kind").c_str(), sonare::util::refusal::kDeclaredValuePredicate);
  }
  if (src.channel != sonare::midi::kCcAnyChannel && src.channel > 15) {
    check::refuse(subject("channel").c_str(),
                  "must be an integer in [0, 15] or " +
                      std::to_string(static_cast<unsigned>(sonare::midi::kCcAnyChannel)));
  }
  check::finite_float(src.min_value, subject("min_value").c_str());
  check::finite_float(src.max_value, subject("max_value").c_str());
  if (src.max_value < src.min_value) {
    check::refuse_relation({subject("max_value"), subject("min_value")},
                           "{0} must be at least {1}");
  }
  if (src.kind == SONARE_MIDI_CC_CONTROL_CHANGE_14) {
    check::integer_within(src.cc_number, sonare_c_detail::kMidiControl14NumberDomain,
                          subject("cc_number").c_str());
    if (src.cc_lsb_number != src.cc_number + 32u) {
      check::refuse_relation({subject("cc_lsb_number"), subject("cc_number")},
                             "{0} must be {1} + 32");
    }
  }
}

sonare::midi::CcBinding binding_from_c(const SonareMidiCcBinding& src) noexcept {
  sonare::midi::CcBinding binding{};
  binding.kind = static_cast<sonare::midi::CcBindingKind>(src.kind);
  binding.cc_number = src.cc_number;
  binding.channel = src.channel;
  binding.param_id = src.param_id;
  binding.min_value = src.min_value;
  binding.max_value = src.max_value;
  binding.cc_lsb_number = src.cc_lsb_number;
  binding.selector_msb = src.selector_msb;
  binding.selector_lsb = src.selector_lsb;
  return binding;
}

SonareMidiCcBinding binding_to_c(const sonare::midi::CcBinding& binding) noexcept {
  SonareMidiCcBinding out{};
  out.cc_number = binding.cc_number;
  out.channel = binding.channel;
  out.kind = static_cast<uint8_t>(binding.kind);
  out.cc_lsb_number = binding.cc_lsb_number;
  out.selector_msb = binding.selector_msb;
  out.selector_lsb = binding.selector_lsb;
  out.param_id = binding.param_id;
  out.min_value = binding.min_value;
  out.max_value = binding.max_value;
  return out;
}

// Refuses a binding table over the map's capacity, then each row the map cannot hold.
void check_cc_bindings(const SonareMidiCcBinding* bindings, size_t binding_count) {
  check::integer_in_range(static_cast<long long>(binding_count), 0,
                          static_cast<long long>(sonare::midi::CcMap::kMaxBindings),
                          "binding_count");
  for (size_t i = 0; i < binding_count; ++i) check_cc_binding(bindings[i], i);
}

// Binds a table that check_cc_bindings accepted; its count never exceeds the map's capacity.
void populate_cc_map(const SonareMidiCcBinding* bindings, size_t binding_count,
                     sonare::midi::CcMap* out) {
  for (size_t i = 0; i < binding_count; ++i) out->bind(binding_from_c(bindings[i]));
}

}  // namespace
#endif

const char* sonare_midi_gm_instrument_name(int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (program < 0 || program > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm_instrument_name(static_cast<uint8_t>(program)));
#else
  (void)program;
  return nullptr;
#endif
}

int sonare_midi_gm_program_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::gm_program_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_gm_family_name(int family) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (family < 0 || family > 15) return nullptr;
  return view_to_cstr(sonare::midi::gm_family_name(static_cast<uint8_t>(family)));
#else
  (void)family;
  return nullptr;
#endif
}

int sonare_midi_gm_family_first_program(int family) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (family < 0 || family > 15) return -1;
  return sonare::midi::gm_family_first_program(static_cast<uint8_t>(family));
#else
  (void)family;
  return -1;
#endif
}

const char* sonare_midi_gm2_instrument_name(int bank_lsb, int program) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127 || program < 0 || program > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm2_instrument_name(static_cast<uint8_t>(bank_lsb),
                                                        static_cast<uint8_t>(program)));
#else
  (void)bank_lsb;
  (void)program;
  return nullptr;
#endif
}

const char* sonare_midi_gm_drum_name(int note) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (note < 0 || note > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm_drum_name(static_cast<uint8_t>(note)));
#else
  (void)note;
  return nullptr;
#endif
}

int sonare_midi_gm_drum_note_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::gm_drum_note_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_gm2_drum_set_name(int bank_lsb) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127) return nullptr;
  return view_to_cstr(sonare::midi::gm2_drum_set_name(static_cast<uint8_t>(bank_lsb)));
#else
  (void)bank_lsb;
  return nullptr;
#endif
}

const char* sonare_midi_gm2_drum_name(int bank_lsb, int note) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (bank_lsb < 0 || bank_lsb > 127 || note < 0 || note > 127) return nullptr;
  return view_to_cstr(
      sonare::midi::gm2_drum_name(static_cast<uint8_t>(bank_lsb), static_cast<uint8_t>(note)));
#else
  (void)bank_lsb;
  (void)note;
  return nullptr;
#endif
}

const char* sonare_midi_cc_name(int controller) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (controller < 0 || controller > 127) return nullptr;
  return view_to_cstr(sonare::midi::cc_name(static_cast<uint8_t>(controller)));
#else
  (void)controller;
  return nullptr;
#endif
}

int sonare_midi_cc_index_for_name(const char* name) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return name ? sonare::midi::cc_index_for_name(name) : -1;
#else
  (void)name;
  return -1;
#endif
}

const char* sonare_midi_per_note_controller_name(int index) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (index < 0 || index > 255) return nullptr;
  return view_to_cstr(sonare::midi::per_note_controller_name(static_cast<uint8_t>(index)));
#else
  (void)index;
  return nullptr;
#endif
}

SonareError sonare_midi_bank_program(double ppq, uint8_t group, uint8_t channel, int bank_msb,
                                     int bank_lsb, int program, SonareMidiEventPod* out_events,
                                     size_t out_capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (out_count) *out_count = 0;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (const SonareError err = run_validation([&] {
        check_midi_head(ppq, group, channel);
        check::integer_within(bank_msb, sonare_c_detail::kMidiDataByteDomain, "bank_msb");
        check::integer_within(bank_lsb, sonare_c_detail::kMidiDataByteDomain, "bank_lsb");
        check::integer_within(program, sonare_c_detail::kMidiDataByteDomain, "program");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_events);
  if (const SonareError err =
          run_validation([&] { check::integer_at_least<size_t>(out_capacity, 3, "out_capacity"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_count);
  sonare::midi::ProgramSelection selection{};
  selection.bank_msb = static_cast<uint8_t>(bank_msb);
  selection.bank_lsb = static_cast<uint8_t>(bank_lsb);
  selection.program = static_cast<uint8_t>(program);
  const sonare::midi::BankProgramMessages messages =
      sonare::midi::program_to_messages(group, channel, selection);
  for (uint8_t i = 0; i < messages.count; ++i) {
    out_events[i] = pod_from_ump(ppq, messages.messages[i]);
  }
  *out_count = messages.count;
  return SONARE_OK;
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bank_msb, bank_lsb, program, out_events,
                                                 out_capacity, out_count);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
static_assert(sonare::midi::MidiRouteConfig{}.thru,
              "the documented default of SonareMidiRouteConfig.thru");
#endif

void sonare_midi_route_config_init(SonareMidiRouteConfig* config) {
  if (config == nullptr) return;
  *config = SonareMidiRouteConfig{};
  config->struct_size = sizeof(SonareMidiRouteConfig);
  config->filter_group = SONARE_MIDI_ROUTE_ANY;
  config->filter_channel = SONARE_MIDI_ROUTE_ANY;
  config->remap_channel = SONARE_MIDI_ROUTE_NO_REMAP;
  config->thru = 1;
}

SonareError sonare_midi_route_events(const SonareMidiEventPod* events, size_t count,
                                     const SonareMidiRouteConfig* config,
                                     SonareMidiEventPod* out_events, size_t out_capacity,
                                     size_t* out_count, int* out_overflowed,
                                     size_t* out_overflow_count) {
  SONARE_C_API_ENTRY;
  if (out_overflowed) *out_overflowed = 0;
  if (out_overflow_count) *out_overflow_count = 0;
  if (out_count) *out_count = 0;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(events, count);
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), sonare_c_detail::kMidiEventCountDomain,
                              "count");
      });
      err != SONARE_OK) {
    return err;
  }
  SonareMidiRouteConfig default_config;
  sonare_midi_route_config_init(&default_config);
  const SonareMidiRouteConfig& c = config != nullptr ? *config : default_config;
  if (const SonareError err = run_validation([&] {
        sonare_c_detail::require_struct_size(c.struct_size, sizeof(SonareMidiRouteConfig), "config",
                                             "sonare_midi_route_config_init");
        // SONARE_MIDI_ROUTE_ANY / SONARE_MIDI_ROUTE_NO_REMAP leave the field unfiltered (or the
        // channel unmapped).
        const auto route_field = [](int value, const char* subject) {
          if (value != SONARE_MIDI_ROUTE_ANY && (value < 0 || value > 15)) {
            check::refuse(subject, "must be -1 or an integer in [0, 15]");
          }
        };
        route_field(c.filter_group, "config.filter_group");
        route_field(c.filter_channel, "config.filter_channel");
        route_field(c.remap_channel, "config.remap_channel");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(out_events, out_capacity);
  SONARE_C_REQUIRE_NONNULL(out_count);

  SONARE_C_TRY
  std::vector<sonare::midi::MidiEvent> input;
  input.reserve(count);
  std::vector<double> ppq_by_index;
  ppq_by_index.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    check_midi_event_pod(events[i], check::indexed_subject("events", i, "ppq"),
                         check::indexed_subject("events", i, "word0"));
    sonare::midi::MidiEvent event{};
    event.render_frame = static_cast<int64_t>(i);
    event.ump = ump_from_pod(events[i]);
    input.push_back(event);
    ppq_by_index.push_back(events[i].ppq);
  }

  sonare::midi::MidiRouteConfig route{};
  route.filter_group = c.filter_group == SONARE_MIDI_ROUTE_ANY
                           ? sonare::midi::kRouteAnyGroup
                           : static_cast<uint8_t>(c.filter_group);
  route.filter_channel = c.filter_channel == SONARE_MIDI_ROUTE_ANY
                             ? sonare::midi::kRouteAnyChannel
                             : static_cast<uint8_t>(c.filter_channel);
  route.remap_channel = c.remap_channel == SONARE_MIDI_ROUTE_NO_REMAP
                            ? sonare::midi::kRouteNoRemap
                            : static_cast<uint8_t>(c.remap_channel);
  route.thru = c.thru != 0;

  sonare::midi::MidiRouter router;
  router.set_config(route);
  sonare::midi::MidiRouteOutput routed;
  router.process(input.empty() ? nullptr : input.data(), input.size(), &routed);

  const size_t copied = std::min(out_capacity, routed.size);
  for (size_t i = 0; i < copied; ++i) {
    const sonare::midi::MidiEvent& event = routed.events[i];
    const size_t source_index = static_cast<size_t>(event.render_frame);
    out_events[i] = pod_from_ump(
        source_index < ppq_by_index.size() ? ppq_by_index[source_index] : 0.0, event.ump);
  }
  const uint32_t capacity_overflow =
      routed.size > out_capacity ? static_cast<uint32_t>(routed.size - out_capacity) : 0u;
  *out_count = copied;
  if (out_overflowed) *out_overflowed = (routed.overflowed || capacity_overflow > 0u) ? 1 : 0;
  if (out_overflow_count) *out_overflow_count = router.overflow_count() + capacity_overflow;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, events,
                                                 count, config, out_events, out_capacity, out_count,
                                                 out_overflowed, out_overflow_count);
#endif
}

SonareError sonare_midi_cc_learn(const SonareMidiEventPod* events, size_t count, uint32_t param_id,
                                 float min_value, float max_value, uint8_t min_movement,
                                 SonareMidiCcBinding* out_binding) {
  SONARE_C_API_ENTRY;
  if (out_binding) *out_binding = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(events, count);
  // The core compares movement in 7-bit CC units, so a larger threshold could never be met.
  if (const SonareError err = run_validation([&] {
        check::integer_within(static_cast<long long>(count), sonare_c_detail::kMidiEventCountDomain,
                              "count");
        check::integer_within(param_id, sonare_c_detail::kMidiTargetParamIdDomain, "param_id");
        check::finite_float(min_value, "min_value");
        check::finite_float(max_value, "max_value");
        if (max_value < min_value) {
          check::refuse_relation({"max_value", "min_value"}, "{0} must be at least {1}");
        }
        check::integer_within(min_movement, sonare_c_detail::kMidiDataByteDomain, "min_movement");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_binding);
  SONARE_C_TRY
  sonare::midi::CcMap map;
  map.begin_learn(param_id, min_value, max_value, min_movement);
  sonare::midi::CcBinding learned{};
  for (size_t i = 0; i < count; ++i) {
    check_midi_event_pod(events[i], check::indexed_subject("events", i, "ppq"),
                         check::indexed_subject("events", i, "word0"));
    if (map.observe_for_learn(ump_from_pod(events[i]), &learned)) {
      *out_binding = binding_to_c(learned);
      return SONARE_OK;
    }
  }
  return SONARE_ERROR_INVALID_STATE;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, events,
                                                 count, param_id, min_value, max_value,
                                                 min_movement, out_binding);
#endif
}

SonareError sonare_midi_cc_to_breakpoint(const SonareMidiCcBinding* bindings, size_t binding_count,
                                         const SonareMidiEventPod* event,
                                         SonareAutomationPoint* out_point) {
  SONARE_C_API_ENTRY;
  if (out_point) *out_point = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(bindings, binding_count);
  if (const SonareError err = run_validation([&] { check_cc_bindings(bindings, binding_count); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(event);
  if (const SonareError err =
          run_validation([&] { check_midi_event_pod(*event, "event.ppq", "event.word0"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_point);
  SONARE_C_TRY
  sonare::midi::CcMap map;
  populate_cc_map(bindings, binding_count, &map);
  std::vector<sonare::automation::Breakpoint> points;
  if (!map.cc_to_breakpoint(ump_from_pod(*event), event->ppq, &points) || points.empty()) {
    return SONARE_ERROR_INVALID_STATE;
  }
  out_point->ppq = points[0].ppq;
  out_point->value = points[0].value;
  out_point->curve_to_next = 0;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, bindings,
                                                 binding_count, event, out_point);
#endif
}

SonareError sonare_midi_param_to_cc(const SonareMidiCcBinding* bindings, size_t binding_count,
                                    uint32_t param_id, float unit_value, uint8_t group, double ppq,
                                    SonareMidiEventPod* out_event) {
  SONARE_C_API_ENTRY;
  if (out_event) *out_event = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(bindings, binding_count);
  if (const SonareError err = run_validation([&] {
        check_cc_bindings(bindings, binding_count);
        check::integer_within(param_id, sonare_c_detail::kMidiTargetParamIdDomain, "param_id");
        check::finite_float(unit_value, "unit_value");
        check::integer_within(group, sonare_c_detail::kMidiUmpGroupDomain, "group");
        check::within(ppq, sonare_c_detail::kPublicPpqDomain, "ppq");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_event);
  SONARE_C_TRY
  sonare::midi::CcMap map;
  populate_cc_map(bindings, binding_count, &map);
  sonare::midi::Ump ump{};
  if (!map.param_to_cc(param_id, unit_value, group, &ump)) return SONARE_ERROR_INVALID_STATE;
  *out_event = pod_from_ump(ppq, ump);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, bindings,
                                                 binding_count, param_id, unit_value, group, ppq,
                                                 out_event);
#endif
}

SonareError sonare_midi_channel_pressure(double ppq, uint8_t group, uint8_t channel,
                                         uint8_t pressure, SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check_u7(pressure, "pressure");
      },
      [&] { return sonare::midi::make_midi1_channel_pressure(group, channel, pressure); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, pressure, out);
#endif
}

SonareError sonare_midi_pitch_bend(double ppq, uint8_t group, uint8_t channel, uint16_t bend,
                                   SonareMidiEventPod* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  return emit_midi_pod(
      out, ppq,
      [&] {
        check_midi_head(ppq, group, channel);
        check::integer_within(bend, sonare_c_detail::kMidiPitchBend14Domain, "bend");
      },
      [&] { return sonare::midi::make_midi1_pitch_bend(group, channel, bend); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, ppq, group,
                                                 channel, bend, out);
#endif
}

SonareError sonare_project_import_smf(SonareProject* project, const uint8_t* bytes, size_t len,
                                      uint32_t* out_first_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_first_clip_id) *out_first_clip_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(bytes);
  if (const SonareError err = run_validation([&] {
        check::integer_in_range(static_cast<long long>(len), 1,
                                static_cast<long long>(kMaxBufferSize), "len");
      });
      err != SONARE_OK) {
    return err;
  }
  const auto& import_limits = sonare::resource::kProjectMidiImportResourceLimits;
  if (len > import_limits.max_file_bytes) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  SONARE_C_TRY
  sonare::midi::SmfImportResult result = sonare::midi::import_smf(bytes, len, import_limits);
  // A track that ends after a valid prefix (for example, without its optional
  // End-of-Track meta event) is recoverable. Keep the importer's truncated
  // status available to direct C++ callers, while installing that recovered
  // prefix for every public project binding.
  if (!result.recoverable()) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_FORMAT, result.diagnostic.c_str());
    return SONARE_ERROR_INVALID_FORMAT;
  }

  // The per-event PPQ already comes from the importer in quarter-note units, so
  // the shared installer derives each clip length from the largest event
  // position (or the imported end-of-track length). Marker kind / key-signature
  // fields are carried through verbatim via SmfMarker.
  return install_imported_midi(project, result.tempo_segments, result.time_signatures,
                               result.markers, result.clips, result.clip_names,
                               result.clip_lengths_ppq, result.sysex_store, out_first_clip_id,
                               result.sequence_name);
  SONARE_C_CATCH
#else
  if (out_first_clip_id) *out_first_clip_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 bytes, len, out_first_clip_id);
#endif
}

SonareError sonare_project_import_clip_file(SonareProject* project, const uint8_t* bytes,
                                            size_t len, uint32_t* out_first_clip_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_first_clip_id) *out_first_clip_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(bytes);
  if (const SonareError err = run_validation([&] {
        check::integer_in_range(static_cast<long long>(len), 1,
                                static_cast<long long>(kMaxBufferSize), "len");
      });
      err != SONARE_OK) {
    return err;
  }
  const auto& import_limits = sonare::resource::kProjectMidiImportResourceLimits;
  if (len > import_limits.max_file_bytes) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  SONARE_C_TRY
  sonare::midi::Smf2ImportResult result = sonare::midi::import_clip_file(bytes, len, import_limits);
  if (!result.ok()) return SONARE_ERROR_INVALID_FORMAT;

  // MIDI Clip Files carry no SMF "Marker" equivalent.
  const std::vector<sonare::midi::SmfMarker> markers;
  return install_imported_midi(project, result.tempo_segments, result.time_signatures, markers,
                               result.clips, result.clip_names, result.clip_lengths_ppq,
                               result.sysex_store, out_first_clip_id);
  SONARE_C_CATCH
#else
  if (out_first_clip_id) *out_first_clip_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 bytes, len, out_first_clip_id);
#endif
}

SonareError sonare_project_export_clip_file(const SonareProject* project, uint8_t** out_bytes,
                                            size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bytes) *out_bytes = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_bytes);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_TRY
  const arr::Project& model = project->history.project();
  std::vector<sonare::transport::TempoSegment> tempo = model.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, kDefaultBpm, 0.0});
  const std::vector<sonare::transport::TimeSignatureSegment>& sigs = model.time_signatures();

  // The MIDI Clip File is a single-clip container: merge every MIDI clip's
  // events into one clip, positioned at each clip's project start.
  sonare::midi::SysExStore sysex_store;
  std::map<uint32_t, sonare::midi::SysExHandle> exported_sysex_handles;
  sonare::midi::MidiClip merged;
  std::string name;
  double clip_end_ppq = 0.0;
  const arr::MidiContentStore& midi_content = project->history.midi_content();
  for (const arr::EditClip& clip : model.clips()) {
    const arr::ClipSource* source = model.find_source(clip.source_id);
    if (source == nullptr || arr::source_kind(*source) != arr::SourceKind::kMidi) continue;
    // Track the arrangement extent so trailing silence / sustained tails beyond
    // the last event still land inside the exported clip length.
    clip_end_ppq = std::max(clip_end_ppq, clip.end_ppq());
    const auto it = midi_content.events.find(clip.id);
    if (it == midi_content.events.end()) continue;
    const size_t remaining_events = kMaxProjectMidiExportEvents - merged.events().size();
    sonare::midi::MidiClip piece;
    const SonareError build_error =
        make_smf_clip_from_events(clip, it->second, midi_content, &sysex_store,
                                  &exported_sysex_handles, remaining_events, &piece);
    if (build_error != SONARE_OK) return build_error;
    for (const sonare::midi::MidiClipEvent& ev : piece.events()) merged.add_event(ev);
    if (name.empty()) {
      const arr::Track* track = model.find_track(clip.track_id);
      if (track != nullptr) name = track->name;
    }
  }
  uint32_t dropped_events = 0;
  sonare::midi::drop_zero_length_notes(&merged, &dropped_events,
                                       sonare::midi::kDefaultTicksPerQuarter);
  merged.sort_stable();

  sonare::midi::Smf2ExportOptions options;
  options.sysex_store = &sysex_store;
  options.name = name;
  options.length_ppq = clip_end_ppq;
  sonare::midi::Smf2ExportResult result =
      sonare::midi::export_clip_file(merged, tempo, sigs, options);
  if (!result.ok()) return SONARE_ERROR_INVALID_STATE;

  *out_len = result.bytes.size();
  *out_bytes = new uint8_t[result.bytes.size()];
  std::memcpy(*out_bytes, result.bytes.data(), result.bytes.size());
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_bytes) *out_bytes = {};
  if (out_len) *out_len = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_bytes, out_len);
#endif
}

SonareError sonare_project_export_smf(const SonareProject* project, uint8_t** out_bytes,
                                      size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_bytes) *out_bytes = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_bytes);
  SONARE_C_REQUIRE_NONNULL(out_len);
  SONARE_C_TRY
  const arr::Project& model = project->history.project();
  std::vector<sonare::transport::TempoSegment> tempo = model.tempo_segments();
  if (tempo.empty()) tempo.push_back({0.0, kDefaultBpm, 0.0});
  const std::vector<sonare::transport::TimeSignatureSegment>& sigs = model.time_signatures();

  std::vector<sonare::midi::MidiClip> clips;
  std::vector<std::string> names;
  sonare::midi::SysExStore sysex_store;
  std::map<uint32_t, sonare::midi::SysExHandle> exported_sysex_handles;
  const arr::MidiContentStore& midi_content = project->history.midi_content();
  size_t exported_event_count = 0;
  for (const arr::EditClip& clip : model.clips()) {
    const arr::ClipSource* source = model.find_source(clip.source_id);
    if (source == nullptr || arr::source_kind(*source) != arr::SourceKind::kMidi) continue;
    const auto it = midi_content.events.find(clip.id);
    if (it == midi_content.events.end()) continue;
    sonare::midi::MidiClip smf_clip;
    const SonareError build_error = make_smf_clip_from_events(
        clip, it->second, midi_content, &sysex_store, &exported_sysex_handles,
        kMaxProjectMidiExportEvents - exported_event_count, &smf_clip);
    if (build_error != SONARE_OK) return build_error;
    if (smf_clip.events().empty()) continue;
    exported_event_count += smf_clip.events().size();
    clips.push_back(std::move(smf_clip));
    const arr::Track* track = model.find_track(clip.track_id);
    names.push_back(track != nullptr ? track->name : std::string{});
  }

  sonare::midi::SmfExportOptions options;
  options.sysex_store = &sysex_store;
  // Carry project markers (with their kind / key-signature) onto SMF track 0 so
  // marker / text / lyric / cue / key-signature meta round-trip through export.
  options.markers.reserve(model.markers().size());
  for (const arr::ProjectMarker& m : model.markers()) {
    sonare::midi::SmfMarker out;
    out.ppq = m.ppq;
    out.text = m.name;
    out.kind = static_cast<sonare::midi::SmfMarkerKind>(m.kind);
    out.key_fifths = m.key_fifths;
    out.key_minor = m.key_minor;
    options.markers.push_back(std::move(out));
  }
  sonare::midi::SmfExportResult result =
      sonare::midi::export_smf(clips, tempo, sigs, names, options);
  if (!result.ok()) return SONARE_ERROR_INVALID_STATE;

  *out_len = result.bytes.size();
  *out_bytes = new uint8_t[result.bytes.size()];
  std::memcpy(*out_bytes, result.bytes.data(), result.bytes.size());
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_bytes) *out_bytes = {};
  if (out_len) *out_len = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_bytes, out_len);
#endif
}

SonareError sonare_project_set_program(SonareProject* project, uint32_t clip_id, uint8_t group,
                                       uint8_t channel, int program, int bank) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (find_midi_clip(project, clip_id) == nullptr) return refuse_unknown_midi_clip();
  if (const SonareError err = run_validation([&] {
        check::integer_within(group, sonare_c_detail::kMidiUmpGroupDomain, "group");
        check::integer_within(channel, sonare_c_detail::kMidiChannelDomain, "channel");
        check::integer_within(program, sonare_c_detail::kMidiDataByteDomain, "program");
      });
      err != SONARE_OK) {
    return err;
  }
  if (bank < -1 || bank > 16383) {
    return run_validation([] { check::refuse("bank", "must be -1 or an integer in [0, 16383]"); });
  }
  SONARE_C_TRY
  arr::MidiClipEventList next;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) {
    next.reserve(it->second.size() + 3);
    for (const arr::MidiClipEvent& event : it->second) {
      if (!is_bank_or_program_event_for(event, group, channel)) next.push_back(event);
    }
  }

  if (bank >= 0) {
    const uint8_t msb = static_cast<uint8_t>((bank >> 7) & 0x7F);
    const uint8_t lsb = static_cast<uint8_t>(bank & 0x7F);
    next.push_back(
        event_from_ump(0.0, sonare::midi::make_midi1_control_change(group, channel, 0, msb)));
    next.push_back(
        event_from_ump(0.0, sonare::midi::make_midi1_control_change(group, channel, 32, lsb)));
  }
  next.push_back(event_from_ump(
      0.0, sonare::midi::make_midi1_program_change(group, channel, static_cast<uint8_t>(program))));

  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(next));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 clip_id, group, channel, program, bank);
#endif
}

SonareError sonare_project_validate_midi_notes(const SonareProject* project, uint32_t clip_id,
                                               SonareNotePairValidation* out) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  const arr::EditClip* edit_clip = find_midi_clip(project, clip_id);
  if (edit_clip == nullptr) return refuse_unknown_midi_clip();
  SONARE_C_REQUIRE_NONNULL(out);
  out->ok = 1;
  SONARE_C_TRY
  sonare::midi::MidiClip clip;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) {
    // Validate the exact event set that export emits, including the clip's
    // source-offset / half-open length window and loop expansion. A note still
    // sounding at the clip end is closed there, so it validates as a matched pair.
    sonare::midi::SysExStore sysex_store;
    std::map<uint32_t, sonare::midi::SysExHandle> exported_sysex_handles;
    const SonareError build_error = make_smf_clip_from_events(
        *edit_clip, it->second, project->history.midi_content(), &sysex_store,
        &exported_sysex_handles, kMaxProjectMidiExportEvents, &clip);
    if (build_error != SONARE_OK) return build_error;
  }
  clip.sort_stable();
  const sonare::midi::NotePairValidation v = clip.validate_note_pairs();
  out->ok = v.ok ? 1 : 0;
  out->unmatched_note_ons = v.unmatched_note_ons;
  out->unmatched_note_offs = v.unmatched_note_offs;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 clip_id, out);
#endif
}

SonareError sonare_project_bake_midi_fx(SonareProject* project, uint32_t clip_id,
                                        const char* config_json, int32_t* out_source_index,
                                        size_t out_capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (find_midi_clip(project, clip_id) == nullptr) return refuse_unknown_midi_clip();
  SONARE_C_REQUIRE_NONNULL(config_json);
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  midi_fx_chain_from_json(config_json, &chain);

  arr::MidiClipEventList current;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) current = it->second;
  arr::MidiClipEventList transformed;
  // Provenance is tracked only when the caller asked for it: building it drains
  // one input per chunk, which costs more chunk calls on a large clip.
  const bool want_map = out_source_index != nullptr || out_count != nullptr;
  std::vector<int32_t> source_index;
  if (!apply_midi_fx_to_events(current, &chain, &transformed, want_map ? &source_index : nullptr)) {
    refuse_unrenderable_midi_fx();
  }
  auto command = std::make_unique<arr::ReplaceMidiClipEvents>(clip_id, std::move(transformed));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  // Written only after the edit commits, so a caller never sees a map for a
  // bake that did not happen.
  if (out_count) *out_count = source_index.size();
  if (out_source_index) {
    const size_t writable = std::min(out_capacity, source_index.size());
    std::copy(source_index.begin(), source_index.begin() + static_cast<std::ptrdiff_t>(writable),
              out_source_index);
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 clip_id, config_json, out_source_index,
                                                 out_capacity, out_count);
#endif
}

SonareError sonare_project_preview_midi_fx_count(const SonareProject* project, uint32_t clip_id,
                                                 const char* config_json, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  if (find_midi_clip(project, clip_id) == nullptr) return refuse_unknown_midi_clip();
  SONARE_C_REQUIRE_NONNULL(config_json);
  SONARE_C_REQUIRE_NONNULL(out_count);
  SONARE_C_TRY
  sonare::midi::MidiFxChain chain;
  midi_fx_chain_from_json(config_json, &chain);

  arr::MidiClipEventList current;
  const auto it = project->history.midi_content().events.find(clip_id);
  if (it != project->history.midi_content().events.end()) current = it->second;
  arr::MidiClipEventList transformed;
  if (!apply_midi_fx_to_events(current, &chain, &transformed)) {
    refuse_unrenderable_midi_fx();
  }
  *out_count = transformed.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 clip_id, config_json, out_count);
#endif
}
