// Definitions of the mixing C ABI for a build without mixing support. The library lists this
// file in place of the mixing translation units, so the exported symbol set does not depend on
// the build options: every entry refuses with SONARE_ERROR_NOT_SUPPORTED and clears its outputs.

#include <sonare/sonare_c.h>

#include <cstddef>
#include <cstdint>

#include "sonare_c_internal.h"

using namespace sonare_c_detail;

namespace {

constexpr const char* kNotCompiledIn = "mixing support is not compiled in";

}  // namespace

SonareError sonare_mixer_create(int sample_rate, int max_block_size, SonareMixer** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  (void)sample_rate;
  (void)max_block_size;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return nullptr;
}

SonareStrip* sonare_mixer_add_strip_ex(SonareMixer* mixer, const char* id, int enable_metering,
                                       int measure_lufs, int measure_true_peak,
                                       int true_peak_oversample) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  (void)enable_metering;
  (void)measure_lufs;
  (void)measure_true_peak;
  (void)true_peak_oversample;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return nullptr;
}

SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)db;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)db;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)pan;
  (void)pan_mode;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)left_pan;
  (void)right_pan;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_surround_pan(SonareStrip* strip, const SonareSurroundPan* pan) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)pan;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_width(SonareStrip* strip, float width) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)width;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_muted(SonareStrip* strip, int muted) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)muted;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)soloed;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)solo_safe;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left,
                                             int invert_right) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)invert_left;
  (void)invert_right;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)pan_law;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)delay_samples;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)offset_db;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_settle(SonareStrip* strip) {
  SONARE_C_API_ENTRY;
  (void)strip;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(index_out);
  (void)strip;
  (void)id;
  (void)destination_bus_id;
  (void)send_db;
  (void)timing;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)index;
  (void)send_db;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_remove_send(SonareStrip* strip, unsigned int index) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)index;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_meter(const SonareStrip* strip, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  (void)strip;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_meter_tap(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  (void)strip;
  (void)tap;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_bus_meter(SonareMixer* mixer, const char* bus_id,
                                   SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  (void)mixer;
  (void)bus_id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_bus_non_finite_discard_count(const SonareMixer* mixer, const char* bus_id,
                                                      uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  (void)mixer;
  (void)bus_id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points) {
  (void)strip;
  (void)out;
  (void)max_points;
  return 0;
}

SonareError sonare_strip_non_finite_discard_count(const SonareStrip* strip, uint32_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  (void)strip;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

size_t sonare_mixer_strip_count(const SonareMixer* mixer) {
  (void)mixer;
  return 0;
}

SonareError sonare_mixer_get_strip_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_add_bus(SonareMixer* mixer, const char* id, const char* role) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  (void)role;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_remove_bus(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_set_output_bus(SonareMixer* mixer, const char* source_id,
                                        const char* bus_id) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)source_id;
  (void)bus_id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_bus_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_add_vca_group(SonareMixer* mixer, const char* id, float gain_db,
                                       const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  (void)gain_db;
  (void)members;
  (void)member_count;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_set_vca_group_gain_db(SonareMixer* mixer, const char* id, float gain_db) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  (void)gain_db;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_set_vca_group_members(SonareMixer* mixer, const char* id,
                                               const char* const* members, size_t member_count) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  (void)members;
  (void)member_count;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_remove_vca_group(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_vca_group_count(const SonareMixer* mixer, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareStrip* sonare_mixer_strip_at(SonareMixer* mixer, size_t index) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)index;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return nullptr;
}

SonareStrip* sonare_mixer_strip_by_id(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)id;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return nullptr;
}

SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)insert_index;
  (void)param_id;
  (void)sample_pos;
  (void)value;
  (void)curve;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)sample_pos;
  (void)fader_db;
  (void)curve;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)sample_pos;
  (void)pan;
  (void)curve;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)sample_pos;
  (void)width;
  (void)curve;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve) {
  SONARE_C_API_ENTRY;
  (void)strip;
  (void)send_index;
  (void)sample_pos;
  (void)db;
  (void)curve;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareMixer* sonare_mixer_from_scene_json(const char* json, int sample_rate, int max_block_size) {
  SONARE_C_API_ENTRY;
  (void)json;
  (void)sample_rate;
  (void)max_block_size;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return nullptr;
}

SonareError sonare_mixer_to_scene_json(const SonareMixer* mixer, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_compile(SonareMixer* mixer) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_latency_samples(SonareMixer* mixer, int* out_latency_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_latency_samples);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_max_block_size(const SonareMixer* mixer, int* out_max_block_size) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_max_block_size);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_tail_samples(SonareMixer* mixer, int* out_tail_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_tail_samples);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_process_stereo(SonareMixer* mixer, const float* const* input_left,
                                        const float* const* input_right, size_t input_count,
                                        float* output_left, float* output_right,
                                        size_t num_samples) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)input_left;
  (void)input_right;
  (void)input_count;
  (void)output_left;
  (void)output_right;
  (void)num_samples;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_drain_tail_stereo(SonareMixer* mixer, float* output_left,
                                           float* output_right, size_t num_samples) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)output_left;
  (void)output_right;
  (void)num_samples;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

void sonare_mixer_master_meter_config_init(SonareMixerMasterMeterConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
}

SonareError sonare_mixer_configure_master_meter(SonareMixer* mixer,
                                                const SonareMixerMasterMeterConfig* config) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  (void)config;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_master_meter(SonareMixer* mixer, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

SonareError sonare_mixer_flush_meters(SonareMixer* mixer) {
  SONARE_C_API_ENTRY;
  (void)mixer;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

const char* sonare_mixing_scene_preset_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

SonareError sonare_mixing_scene_preset_json(const char* preset_name, char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  (void)preset_name;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

void sonare_mixer_destroy(SonareMixer* mixer) { (void)mixer; }

SonareError sonare_mixing_assistant_suggest(
    const float* const* input_left, const float* const* input_right, const char* const* track_ids,
    const char* const* track_names, const size_t* track_lengths, const int* track_sample_rates,
    size_t input_count, int sample_rate, const SonareMasteringParam* params, size_t param_count,
    char** json_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(json_out);
  (void)input_left;
  (void)input_right;
  (void)track_ids;
  (void)track_names;
  (void)track_lengths;
  (void)track_sample_rates;
  (void)input_count;
  (void)sample_rate;
  (void)params;
  (void)param_count;
  set_last_error_static(SONARE_ERROR_NOT_SUPPORTED, kNotCompiledIn);
  return SONARE_ERROR_NOT_SUPPORTED;
}

const char* sonare_mixing_assistant_source_class_names(void) {
  SONARE_C_API_ENTRY;
  return "";
}

int sonare_mixing_assistant_source_class_from_name(const char* name) {
  SONARE_C_API_ENTRY;
  (void)name;
  return -1;
}
