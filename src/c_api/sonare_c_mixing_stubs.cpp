// Definitions of the mixing C ABI for a build without mixing support. The library lists this
// file in place of the mixing translation units, so the exported symbol set does not depend on
// the build options: every entry refuses with SONARE_ERROR_NOT_SUPPORTED and clears its outputs.

#include <sonare/sonare_c.h>

#include <cstddef>
#include <cstdint>

#include "sonare_c_internal.h"

using namespace sonare_c_detail;

SonareError sonare_mixer_create(int sample_rate, int max_block_size, SonareMixer** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMixing, sample_rate, max_block_size);
}

void sonare_mixer_add_strip_options_init(SonareMixerAddStripOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id,
                                    const SonareMixerAddStripOptions* options) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in_handle<SonareStrip>(Feature::kMixing, mixer, id, options);
}

SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, db);
}

SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, db);
}

SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, pan, pan_mode);
}

SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, left_pan, right_pan);
}

SonareError sonare_strip_set_surround_pan(SonareStrip* strip, const SonareSurroundPan* pan) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, pan);
}

SonareError sonare_strip_set_width(SonareStrip* strip, float width) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, width);
}

SonareError sonare_strip_set_muted(SonareStrip* strip, int muted) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, muted);
}

SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, soloed);
}

SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, solo_safe);
}

SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left,
                                             int invert_right) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, invert_left, invert_right);
}

SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, pan_law);
}

SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, delay_samples);
}

SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, offset_db);
}

SonareError sonare_strip_settle(SonareStrip* strip) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip);
}

SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(index_out);
  return refuse_not_compiled_in(Feature::kMixing, strip, id, destination_bus_id, send_db, timing);
}

SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, index, send_db);
}

SonareError sonare_strip_remove_send(SonareStrip* strip, unsigned int index) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, index);
}

SonareError sonare_strip_meter(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMixing, strip, tap);
}

SonareError sonare_mixer_bus_meter(SonareMixer* mixer, const char* bus_id,
                                   SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMixing, mixer, bus_id);
}

SonareError sonare_mixer_bus_non_finite_discard_count(const SonareMixer* mixer, const char* bus_id,
                                                      size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMixing, mixer, bus_id);
}

size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points) {
  (void)strip;
  (void)out;
  (void)max_points;
  return 0;
}

SonareError sonare_strip_non_finite_discard_count(const SonareStrip* strip, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMixing, strip);
}

size_t sonare_mixer_strip_count(const SonareMixer* mixer) {
  (void)mixer;
  return 0;
}

SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id, role);
}

SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id);
}

SonareError sonare_mixer_set_output_bus(SonareMixer* mixer, const char* source_id,
                                        const char* bus_id) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, source_id, bus_id);
}

SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id, gain_db, members, member_count);
}

SonareError sonare_mixer_set_vca_group_gain_db(SonareMixer* mixer, const char* id, float gain_db) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id, gain_db);
}

SonareError sonare_mixer_set_vca_group_members(SonareMixer* mixer, const char* id,
                                               const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id, members, member_count);
}

SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, id);
}

SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in_handle<SonareStrip>(Feature::kMixing, mixer, index);
}

SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in_handle<SonareStrip>(Feature::kMixing, mixer, id);
}

SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, insert_index, param_id, sample_pos, value,
                                curve);
}

SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, sample_pos, fader_db, curve);
}

SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, sample_pos, pan, curve);
}

SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, sample_pos, width, curve);
}

SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, strip, send_index, sample_pos, db, curve);
}

SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in_handle<SonareMixer>(Feature::kMixing, json, sample_rate,
                                                    max_block_size);
}

SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_compile(SonareMixer* mixer) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_latency_samples(SonareMixer* mixer, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_latency_samples);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_max_block_size(const SonareMixer* mixer, int* out_max_block_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_max_block_size);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_tail_samples(SonareMixer* mixer, int* out_tail_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_tail_samples);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, input_left, input_right, input_count,
                                output_left, output_right, num_samples);
}

SonareError sonare_mixer_drain_tail_stereo(SonareMixer* mixer, float* output_left,
                                           float* output_right, size_t num_samples) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, output_left, output_right, num_samples);
}

void sonare_mixer_master_meter_config_init(SonareMixerMasterMeterConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mixer_configure_master_meter(SonareMixer* mixer,
                                                const SonareMixerMasterMeterConfig* config) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer, config);
}

SonareError sonare_mixer_master_meter(SonareMixer* mixer, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

SonareError sonare_mixer_flush_meters(SonareMixer* mixer) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, mixer);
}

const char* sonare_mixing_scene_preset_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMixing, preset_name);
}

void sonare_mixer_destroy(SonareMixer* mixer) { (void)mixer; }

SonareError sonare_validate_mixing_assistant_config(int sample_rate,
                                                    const SonareMasteringParam* params,
                                                    size_t param_count) {
  SONARE_C_API_ENTRY;
  return refuse_not_compiled_in(Feature::kMixing, sample_rate, params, param_count);
}

SonareError sonare_mixing_assistant_suggest(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, const int* track_sample_rates,
    size_t input_count, int sample_rate, const SonareMasteringParam* params, size_t param_count,
    char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  return refuse_not_compiled_in(Feature::kMixing, input_left, input_right, track_ids, track_names,
                                track_lengths, track_sample_rates, input_count, sample_rate, params,
                                param_count);
}

const char* sonare_mixing_assistant_source_class_names(void) {
  SONARE_C_API_ENTRY;
  (void)refuse_not_compiled_in(Feature::kMixing);
  return "";
}

int sonare_mixing_assistant_source_class_from_name(const char* name) {
  SONARE_C_API_ENTRY;
  (void)refuse_not_compiled_in(Feature::kMixing, name);
  return -1;
}
