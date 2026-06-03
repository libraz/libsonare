#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/boundary_detector.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/audio_io.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "core/resample.h"
#include "core/synthesis.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/silence.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#include "feature/vqt.h"
#include "filters/iir.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/lufs.h"
#include "metering/phase_scope.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#ifdef SONARE_WITH_MIXING
#include "mixing/api/presets.h"
#include "mixing/channel_strip.h"
#endif
#ifdef SONARE_WITH_MASTERING
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/maximizer/loudness_optimize.h"
#endif
#ifdef SONARE_WITH_ACOUSTIC_SIM
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/room_estimator.h"
#include "effects/acoustic/room_morph.h"
#endif
#ifdef SONARE_WITH_ARRANGEMENT
#include "sonare_c_project.h"
#include "sonare_c_types.h"
#endif
#include "cli_support.h"
#include "quick.h"
#include "sonare.h"
#include "util/frame.h"
#include "util/json.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/vector_normalize.h"

using namespace sonare;

using CommandHandler = std::function<int(const CliArgs&, const Audio&)>;

struct CommandInfo {
  std::string name;
  std::string description;
  CommandHandler handler;
  bool requires_audio;
};

std::vector<float> parse_float_list(const std::string& text);
std::vector<int> parse_int_list(const std::string& text);
std::string read_plain_text_file(const std::string& path);
std::vector<std::string> split_string(const std::string& text, char delimiter);
void set_json_path(sonare::util::json::Value& root, const std::string& path,
                   sonare::util::json::Value value);
void apply_voice_macro_override(sonare::util::json::Value& root, const std::string& path,
                                const sonare::util::json::Value& value);
std::string find_voice_preset_in_pack(const std::string& pack_json, const std::string& preset_id);
std::string apply_voice_preset_sets(std::string config_text, const std::string& set_options);
PitchClass parse_pitch_class_option(const std::string& value);
Mode parse_mode_option(const std::string& value);
std::vector<Mode> parse_mode_list_option(const std::string& value);
KeyProfileType parse_key_profile_option(const std::string& value);
void print_float_values(const CliArgs& args, const std::vector<float>& values);
void print_int_values(const CliArgs& args, const std::vector<int>& values);
std::vector<float> require_float_values(const CliArgs& args);
std::vector<int> require_int_values(const CliArgs& args);
Audio load_reference_audio(const CliArgs& args, int expected_sample_rate, size_t expected_size);

// Like load_reference_audio but does NOT require the reference to match the
// input length. Used by the two-input "match.*" pair commands, where the
// reference master is commonly a different length than the source. The sample
// rate must still match (the match primitives require equal sample rates).
Audio load_reference_audio_any_length(const CliArgs& args, int expected_sample_rate);

int cmd_version(const CliArgs& args);
int cmd_system_info(const CliArgs& args);
int cmd_frames_to_samples(const CliArgs& args, const Audio& audio);
int cmd_samples_to_frames(const CliArgs& args, const Audio& audio);
int cmd_power_to_db(const CliArgs& args, const Audio& audio);
int cmd_amplitude_to_db(const CliArgs& args, const Audio& audio);
int cmd_db_to_power(const CliArgs& args, const Audio& audio);
int cmd_db_to_amplitude(const CliArgs& args, const Audio& audio);
int cmd_frame_signal(const CliArgs& args, const Audio& audio);
int cmd_pad_center(const CliArgs& args, const Audio& audio);
int cmd_fix_length(const CliArgs& args, const Audio& audio);
int cmd_fix_frames(const CliArgs& args, const Audio& audio);
int cmd_peak_pick(const CliArgs& args, const Audio& audio);
int cmd_vector_normalize(const CliArgs& args, const Audio& audio);
int cmd_pcen(const CliArgs& args, const Audio& audio);
int cmd_info(const CliArgs& args, const Audio& audio);

int cmd_bpm(const CliArgs& args, const Audio& audio);
int cmd_key(const CliArgs& args, const Audio& audio);
int cmd_beats(const CliArgs& args, const Audio& audio);
int cmd_downbeats(const CliArgs& args, const Audio& audio);
int cmd_onsets(const CliArgs& args, const Audio& audio);
int cmd_chords(const CliArgs& args, const Audio& audio);
int cmd_sections(const CliArgs& args, const Audio& audio);
int cmd_timbre(const CliArgs& args, const Audio& audio);
int cmd_dynamics(const CliArgs& args, const Audio& audio);
int cmd_rhythm(const CliArgs& args, const Audio& audio);
int cmd_melody(const CliArgs& args, const Audio& audio);
int cmd_boundaries(const CliArgs& args, const Audio& audio);
int cmd_analyze(const CliArgs& args, const Audio& audio);

int cmd_pitch_shift(const CliArgs& args, const Audio& audio);
int cmd_time_stretch(const CliArgs& args, const Audio& audio);
int cmd_pitch_correct(const CliArgs& args, const Audio& audio);
int cmd_note_stretch(const CliArgs& args, const Audio& audio);
int cmd_voice_change(const CliArgs& args, const Audio& audio);
int cmd_voice_presets(const CliArgs& args, const Audio& audio);
int cmd_voice_preset(const CliArgs& args, const Audio& audio);
int cmd_voice_preset_validate(const CliArgs& args, const Audio& audio);
int cmd_hpss(const CliArgs& args, const Audio& audio);
int cmd_preemphasis(const CliArgs& args, const Audio& audio);
int cmd_deemphasis(const CliArgs& args, const Audio& audio);
int cmd_trim_silence(const CliArgs& args, const Audio& audio);
int cmd_split_silence(const CliArgs& args, const Audio& audio);

#ifdef SONARE_WITH_MASTERING
int cmd_mastering(const CliArgs& args, const Audio& audio);
int cmd_mastering_processor(const CliArgs& args, const Audio& audio);
int cmd_eq(const CliArgs& args, const Audio& audio);
int cmd_mastering_processors(const CliArgs& args, const Audio& audio);
int cmd_mastering_pair_processors(const CliArgs& args, const Audio& audio);
int cmd_mastering_pair_analyses(const CliArgs& args, const Audio& audio);
int cmd_mastering_stereo_analyses(const CliArgs& args, const Audio& audio);
int cmd_mastering_pair_processor(const CliArgs& args, const Audio& audio);
int cmd_mastering_pair_analyze(const CliArgs& args, const Audio& audio);
int cmd_mastering_stereo_analyze(const CliArgs& args, const Audio& audio);
#endif

#ifdef SONARE_WITH_MIXING
int cmd_mixing_presets(const CliArgs& args, const Audio& audio);
int cmd_mixing_preset(const CliArgs& args, const Audio& audio);
int cmd_mix(const CliArgs& args, const Audio& audio);
#endif

int cmd_mel(const CliArgs& args, const Audio& audio);
int cmd_chroma(const CliArgs& args, const Audio& audio);
int cmd_tonnetz(const CliArgs& args, const Audio& audio);
int cmd_spectral(const CliArgs& args, const Audio& audio);
int cmd_pitch(const CliArgs& args, const Audio& audio);
int cmd_onset_env(const CliArgs& args, const Audio& audio);
int cmd_tempogram(const CliArgs& args, const Audio& audio);
int cmd_plp(const CliArgs& args, const Audio& audio);
int cmd_cqt(const CliArgs& args, const Audio& audio);
int cmd_vqt(const CliArgs& args, const Audio& audio);
int cmd_mel_to_audio(const CliArgs& args, const Audio& audio);
int cmd_mfcc_to_audio(const CliArgs& args, const Audio& audio);
int cmd_acoustic(const CliArgs& args, const Audio& audio);
#ifdef SONARE_WITH_ACOUSTIC_SIM
int cmd_synthesize_rir(const CliArgs& args, const Audio& audio);
int cmd_estimate_room(const CliArgs& args, const Audio& audio);
int cmd_room_morph(const CliArgs& args, const Audio& audio);
#endif
int cmd_onset_envelope(const CliArgs& args, const Audio& audio);
int cmd_fourier_tempogram(const CliArgs& args, const Audio& audio);
int cmd_tempogram_ratio(const CliArgs& args, const Audio& audio);
int cmd_nnls_chroma(const CliArgs& args, const Audio& audio);
int cmd_lufs(const CliArgs& args, const Audio& audio);
int cmd_meter(const CliArgs& args, const Audio& audio);
int cmd_clipping(const CliArgs& args, const Audio& audio);
int cmd_dynamic_range(const CliArgs& args, const Audio& audio);
int cmd_stereo(const CliArgs& args, const Audio& audio);
int cmd_phase(const CliArgs& args, const Audio& audio);

int cmd_normalize(const CliArgs& args, const Audio& audio);
int cmd_gain(const CliArgs& args, const Audio& audio);
int cmd_fade(const CliArgs& args, const Audio& audio);
int cmd_filter(const CliArgs& args, const Audio& audio);
int cmd_resample(const CliArgs& args, const Audio& audio);
int cmd_tone(const CliArgs& args, const Audio& audio);
int cmd_chirp(const CliArgs& args, const Audio& audio);
int cmd_clicks(const CliArgs& args, const Audio& audio);

#ifdef SONARE_WITH_ARRANGEMENT
int cmd_project(const CliArgs& args, const Audio& audio);
#endif
