#include "c_api/project_internal.h"

void sonare_project_instrument_init(SonareProjectInstrument* instrument) {
  if (instrument == nullptr) return;
  *instrument = {};
  instrument->struct_size = sizeof(*instrument);
  instrument->kind = SONARE_PROJECT_INSTRUMENT_SYNTH;
  instrument->bank_msb = SONARE_PROJECT_INSTRUMENT_FROM_CLIP;
  instrument->bank_lsb = SONARE_PROJECT_INSTRUMENT_FROM_CLIP;
  instrument->program = SONARE_PROJECT_INSTRUMENT_FROM_CLIP;
}

void sonare_free_project_instrument_list(SonareProjectInstrumentList* list) {
  if (list == nullptr) return;
  for (size_t i = 0; i < list->count; ++i) {
    const auto& instrument = list->items[i].instrument;
    if (const auto* patch = instrument.synth) {
      for (size_t j = 0; j < patch->engine_param_count; ++j) {
        delete[] patch->engine_params[j].key;
      }
      delete[] patch->engine_params;
      delete patch;
    }
    delete instrument.builtin;
    delete instrument.sf2;
    delete[] instrument.sound_font_uri;
  }
  delete[] list->items;
  *list = {};
}

void sonare_free_string_list(SonareStringList* list) {
  if (list == nullptr) return;
  for (size_t i = 0; i < list->count; ++i) delete[] list->items[i];
  delete[] list->items;
  *list = {};
}

#if defined(SONARE_WITH_ARRANGEMENT)
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string_view>

#include "c_api/midi_input_domains.h"
#include "c_api/synth_patch_common.h"
#include "c_api/synth_patch_fields.h"

namespace {
namespace check = sonare::util::check;
namespace names = sonare::c_api::synth_names;

int patch_ordinal(const names::NameTable& values, std::string_view token) {
  const auto* end = values.names + values.count;
  const auto* found = std::find(values.names, end, token);
  if (found == end) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "unknown saved synth patch token '" + std::string(token) + "'");
  }
  return static_cast<int>(found - values.names);
}

arr::DestinationSynthInstrument to_model_patch(const SonareSynthPatch& patch,
                                               bool use_gm_programs) {
  const auto resolved = synth_config_from_patch_c(patch, "instrument.synth");
  if (resolved.patch.mode == sonare::midi::synth::SynthEngineMode::kSample) {
    throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                  "instrument.synth.engine_mode cannot be persisted",
                                  sonare::ErrorReason::NotPersistable);
  }
  check::finite_float(patch.sample_level, "instrument.synth.sample_level");
  check::finite_float(patch.sample_start_offset, "instrument.synth.sample_start_offset");
  arr::DestinationSynthInstrument model;
  model.preset.assign(patch.preset,
                      std::find(std::begin(patch.preset), std::end(patch.preset), '\0'));
  if (patch.engine_mode != SONARE_SYNTH_ENGINE_DEFAULT) {
    model.engine_mode = names::kEngineModes[patch.engine_mode];
  }
  for (const auto& row : kSynthPatchEnumFields) {
    const int ordinal = patch.*row.member;
    if (ordinal != 0) model.fields.emplace(row.name, row.values.names[ordinal]);
  }
  for (const auto& row : kSynthPatchNumericFields) {
    const double value =
        std::visit([&](const auto member) { return double(patch.*member); }, row.member);
    const uint32_t presence = row.presence_fields2 ? patch.present_fields2 : patch.present_fields;
    if (value != 0.0 || (presence & row.presence_bit) != 0) model.fields.emplace(row.name, value);
  }
  for (size_t i = 0; i < patch.engine_param_count; ++i) {
    model.engine_params.emplace(patch.engine_params[i].key, patch.engine_params[i].value);
  }
  for (int i = 0; i < patch.num_mod_routings; ++i) {
    const auto& route = patch.mod_routings[i];
    model.mod_routings.push_back({route.source, route.destination, route.depth});
  }
  model.clears_base_mod_routings =
      patch.num_mod_routings == 0 && (patch.present_fields & SONARE_SYNTH_FIELD_MOD_ROUTINGS) != 0;
  model.use_gm_programs = use_gm_programs;
  return model;
}

void copy_model_binding(const arr::DestinationInstrumentBinding& model,
                        SonareProjectInstrument& out) {
  sonare_project_instrument_init(&out);
  if (model.bank) {
    out.bank_msb = model.bank->msb;
    out.bank_lsb = model.bank->lsb;
  }
  if (model.program) out.program = *model.program;

  if (const auto* synth = std::get_if<arr::DestinationSynthInstrument>(&model.instrument)) {
    out.kind = SONARE_PROJECT_INSTRUMENT_SYNTH;
    out.use_gm_programs = synth->use_gm_programs ? 1 : 0;
    std::vector<SonareSynthEngineParam> borrowed;
    auto patch = std::make_unique<SonareSynthPatch>(project_synth_patch(*synth, borrowed));
    patch->engine_params = nullptr;
    patch->engine_param_count = 0;
    auto* owned = patch.release();
    out.synth = owned;
    if (!borrowed.empty()) {
      auto* params = new SonareSynthEngineParam[borrowed.size()]{};
      owned->engine_params = params;
      owned->engine_param_count = borrowed.size();
      for (size_t i = 0; i < borrowed.size(); ++i) {
        params[i] = {copy_string(std::string(borrowed[i].key)), borrowed[i].value};
      }
    }
  } else if (const auto* builtin =
                 std::get_if<arr::DestinationBuiltinInstrument>(&model.instrument)) {
    out.kind = SONARE_PROJECT_INSTRUMENT_BUILTIN;
    out.builtin = new SonareBuiltinSynthConfig{sizeof(SonareBuiltinSynthConfig),
                                               builtin->waveform,
                                               builtin->gain,
                                               builtin->attack_ms,
                                               builtin->decay_ms,
                                               builtin->sustain,
                                               builtin->release_ms,
                                               builtin->polyphony};
  } else {
    const auto& sf2 = std::get<arr::DestinationSf2Instrument>(model.instrument);
    out.kind = SONARE_PROJECT_INSTRUMENT_SF2;
    out.sf2 = new SonareSf2InstrumentConfig{
        sizeof(SonareSf2InstrumentConfig),     sf2.gain,           sf2.polyphony,
        sf2.prefer_model_for_modeled_families, sf2.clear_bank_rig, sf2.gs_efx_realization};
    if (sf2.sound_font_uri) out.sound_font_uri = copy_string(*sf2.sound_font_uri);
  }
}

arr::DestinationInstrumentBinding to_model_binding(const SonareProjectInstrument& input) {
  require_struct_size(input.struct_size, sizeof(input), "instrument",
                      "sonare_project_instrument_init");
  if (input.kind < SONARE_PROJECT_INSTRUMENT_SYNTH || input.kind > SONARE_PROJECT_INSTRUMENT_SF2) {
    check::refuse("instrument.kind", sonare::util::refusal::kDeclaredValuePredicate);
  }
  const auto require_config = [](const void* config, bool selected, const char* subject) {
    if (selected) {
      check::non_null(config, subject);
    } else if (config != nullptr) {
      check::refuse(subject, "must be NULL for the selected instrument kind");
    }
  };
  require_config(input.synth, input.kind == SONARE_PROJECT_INSTRUMENT_SYNTH, "instrument.synth");
  require_config(input.builtin, input.kind == SONARE_PROJECT_INSTRUMENT_BUILTIN,
                 "instrument.builtin");
  require_config(input.sf2, input.kind == SONARE_PROJECT_INSTRUMENT_SF2, "instrument.sf2");
  if (input.sound_font_uri != nullptr && input.kind != SONARE_PROJECT_INSTRUMENT_SF2) {
    check::refuse("instrument.sound_font_uri", "must be NULL for the selected instrument kind");
  }
  constexpr int absent = SONARE_PROJECT_INSTRUMENT_FROM_CLIP;
  const auto check_midi_byte = [](int value, const char* subject) {
    if (value != SONARE_PROJECT_INSTRUMENT_FROM_CLIP) {
      check::integer_within(value, kMidiDataByteDomain, subject);
    }
  };
  check_midi_byte(input.bank_msb, "instrument.bank_msb");
  check_midi_byte(input.bank_lsb, "instrument.bank_lsb");
  check_midi_byte(input.program, "instrument.program");
  if ((input.bank_msb == absent) != (input.bank_lsb == absent)) {
    check::refuse_relation({"instrument.bank_msb", "instrument.bank_lsb"},
                           "{0} and {1} must both be supplied or both be FROM_CLIP");
  }
  arr::DestinationInstrumentBinding binding;
  if (input.bank_msb != absent) {
    binding.bank = arr::DestinationMidiBank{input.bank_msb, input.bank_lsb};
  }
  if (input.program != absent) binding.program = input.program;
  if (input.synth != nullptr) {
    binding.instrument = to_model_patch(*input.synth, input.use_gm_programs != 0);
  } else if (input.builtin != nullptr) {
    const auto& config = *input.builtin;
    require_struct_size(config.struct_size, sizeof(config), "instrument.builtin",
                        "sonare_builtin_synth_config_init");
    check::integer_in_range(config.waveform, 0, SONARE_SYNTH_WAVEFORM_COUNT - 1,
                            "instrument.builtin.waveform");
    (void)builtin_synth_config_from_c(config, "instrument.builtin.");
    binding.instrument = arr::DestinationBuiltinInstrument{
        config.waveform, config.gain,       config.attack_ms, config.decay_ms,
        config.sustain,  config.release_ms, config.polyphony};
  } else {
    const auto& config = *input.sf2;
    require_struct_size(config.struct_size, sizeof(config), "instrument.sf2",
                        "sonare_sf2_instrument_config_init");
    (void)sf2_player_config_from_c(config, "instrument.sf2.");
    arr::DestinationSf2Instrument model{
        config.gain,           config.polyphony,          config.prefer_model_for_modeled_families,
        config.clear_bank_rig, config.gs_efx_realization, std::nullopt};
    if (input.sound_font_uri != nullptr) {
      check::non_empty(input.sound_font_uri, "instrument.sound_font_uri");
      model.sound_font_uri = input.sound_font_uri;
    }
    binding.instrument = std::move(model);
  }
  return binding;
}
}  // namespace

SonareSynthPatch project_synth_patch(const arr::DestinationSynthInstrument& model,
                                     std::vector<SonareSynthEngineParam>& params) {
  SonareSynthPatch patch{};
  sonare_synth_patch_init(&patch);
  if (model.preset.size() >= sizeof(patch.preset) ||
      model.mod_routings.size() > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "saved synth preset name or mod routing count is out of range");
  }
  std::copy(model.preset.begin(), model.preset.end(), patch.preset);
  if (!model.engine_mode.empty()) {
    patch.engine_mode =
        patch_ordinal(names::kTablesByKind[SONARE_SYNTH_ENUM_ENGINE_MODE], model.engine_mode);
  }
  for (const auto& field : model.fields) {
    const auto& name = field.first;
    const auto& value = field.second;
    const auto enum_row =
        std::find_if(std::begin(kSynthPatchEnumFields), std::end(kSynthPatchEnumFields),
                     [&](const auto& row) { return name == row.name; });
    if (enum_row != std::end(kSynthPatchEnumFields) && std::holds_alternative<std::string>(value)) {
      patch.*enum_row->member = patch_ordinal(enum_row->values, std::get<std::string>(value));
      continue;
    }
    const auto numeric_row =
        std::find_if(std::begin(kSynthPatchNumericFields), std::end(kSynthPatchNumericFields),
                     [&](const auto& row) { return name == row.name; });
    if (numeric_row == std::end(kSynthPatchNumericFields) ||
        !std::holds_alternative<double>(value)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "unknown or incorrectly typed saved synth patch field " + name);
    }
    const double number = std::get<double>(value);
    std::visit(
        [&](const auto member) {
          auto& target = patch.*member;
          using Field = std::remove_reference_t<decltype(target)>;
          if constexpr (std::is_integral_v<Field>) {
            if (!sonare::numeric::checked_integral_cast(number, &target)) {
              throw sonare::SonareException(
                  sonare::ErrorCode::InvalidFormat,
                  "saved synth patch field " + name + " must be an integer");
            }
          } else {
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<Field>::max()) {
              throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                            "saved synth patch field " + name + " is out of range");
            }
            target = static_cast<Field>(number);
          }
        },
        numeric_row->member);
    auto& presence = numeric_row->presence_fields2 ? patch.present_fields2 : patch.present_fields;
    presence |= numeric_row->presence_bit;
  }
  patch.num_mod_routings = static_cast<int>(model.mod_routings.size());
  if (model.clears_base_mod_routings) patch.present_fields |= SONARE_SYNTH_FIELD_MOD_ROUTINGS;
  for (size_t i = 0; i < model.mod_routings.size(); ++i) {
    const auto& route = model.mod_routings[i];
    patch.mod_routings[i] = {route.source, route.destination, route.depth};
  }
  params.reserve(model.engine_params.size());
  for (const auto& [key, value] : model.engine_params) params.push_back({key.c_str(), value});
  patch.engine_params = params.empty() ? nullptr : params.data();
  patch.engine_param_count = params.size();
  return patch;
}
#endif

SonareError sonare_project_set_destination_instrument(SonareProject* project,
                                                      uint32_t destination_id,
                                                      const SonareProjectInstrument* instrument) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  const auto& model = project->history.project();
  std::optional<arr::DestinationInstrumentBinding> value;
  if (instrument != nullptr) {
    const auto& tracks = model.tracks();
    if (std::none_of(tracks.begin(), tracks.end(), [&](const auto& track) {
          return track.midi_destination_id == destination_id;
        })) {
      check::refuse_unknown_id("destination_id", "a MIDI destination", "project");
    }
    value = to_model_binding(*instrument);
  } else if (model.destination_instruments().count(destination_id) == 0) {
    return SONARE_OK;
  }
  auto command = std::make_unique<arr::SetDestinationInstrument>(destination_id, std::move(value));
  if (!project->history.apply(std::move(command))) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "the instrument binding could not be applied");
    return SONARE_ERROR_INVALID_STATE;
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, destination_id, instrument);
#endif
}

SonareError sonare_project_destination_instruments(const SonareProject* project,
                                                   SonareProjectInstrumentList* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& models = project->history.project().destination_instruments();
  StagedOutput<SonareProjectInstrumentList, sonare_free_project_instrument_list> list;
  if (!models.empty()) {
    list->items = new SonareProjectInstrumentBinding[models.size()]{};
    for (const auto& [id, value] : models) {
      auto& row = list->items[list->count++];
      row.destination_id = id;
      copy_model_binding(value, row.instrument);
    }
  }
  list.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, out);
#endif
}

SonareError sonare_project_unresolved_sound_font_uris(const SonareProject* project,
                                                      SonareStringList* out) {
  SONARE_C_API_ENTRY;
  if (out != nullptr) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  std::set<std::string> unresolved;
  for (const auto& [destination, binding] : project->history.project().destination_instruments()) {
    (void)destination;
    const auto* sf2 = std::get_if<arr::DestinationSf2Instrument>(&binding.instrument);
    if (sf2 != nullptr && sf2->sound_font_uri && !project->sound_font(sf2->sound_font_uri)) {
      unresolved.insert(*sf2->sound_font_uri);
    }
  }
  StagedOutput<SonareStringList, sonare_free_string_list> list;
  if (!unresolved.empty()) {
    list->items = new char* [unresolved.size()] {};
    for (const auto& uri : unresolved) list->items[list->count++] = copy_string(uri);
  }
  list.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kArrangement, project, out);
#endif
}
