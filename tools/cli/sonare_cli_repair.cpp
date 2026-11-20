#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <utility>

#include "sonare_cli.h"

#ifdef SONARE_WITH_MASTERING
namespace {

/// Keeps only the repair stages of `source`; every mastering stage (eq,
/// dynamics, saturation, spectral, stereo, maximizer, loudness) comes back
/// disabled regardless of what the source config turned on. `repair` must
/// never master, so this is the one place that guarantee is enforced,
/// whichever selector (`--preset` or the default assistant suggestion) built
/// the source config.
mastering::api::MasteringChainConfig repair_only_config(
    const mastering::api::MasteringChainConfig& source) {
  mastering::api::MasteringChainConfig config;
  config.repair = source.repair;
  return config;
}

/// Appends the six repair detectors' measurements, one group per detector in
/// the shape of the audio-profile JSON. The object is omitted when no detector
/// ran, because nobody looking is not the same as clean. A large
/// `click.rejected` beside a zero `click.count` means the detector could not
/// decide, and a `hum.fundamental_prominence` (a linear energy ratio) of 1.0
/// means no stationary peak was found.
void append_defect_profile_json(JsonBuilder& json,
                                const mastering::assistant::DefectProfile& defects) {
  if (!defects.measured) return;
  json.key("defects").begin_object();
  json.key("click")
      .begin_object()
      .kv("count", defects.click.count)
      .kv("rejected", defects.click.rejected)
      .kv("longest_run_samples", defects.click.longest_run_samples)
      .kv("per_second", defects.click.per_second)
      .end_object();
  json.key("clip")
      .begin_object()
      .kv("clipped_samples", defects.clip.sample_count)
      .kv("clipped_ratio", defects.clip.sample_fraction)
      .kv("run_count", defects.clip.run_count)
      .kv("longest_run_samples", defects.clip.longest_run_samples)
      .kv("flat_run_count", defects.clip.flat_run_count)
      .kv("longest_flat_run_samples", defects.clip.longest_flat_run_samples)
      .kv("flat_sample_count", defects.clip.flat_sample_count);
  // A level is only measured where a flat run was found.
  if (defects.clip.flat_run_count > 0) json.kv("flat_level", defects.clip.flat_level);
  json.end_object();
  json.key("crackle")
      .begin_object()
      .kv("sample_count", defects.crackle.sample_count)
      .kv("sample_fraction", defects.crackle.sample_fraction)
      .kv("per_second", defects.crackle.per_second)
      .end_object();
  json.key("noise").begin_object().kv("floor_dbfs", defects.noise.floor_dbfs);
  json.key("band_floor_dbfs").begin_array();
  for (const float level : defects.noise.band_floor_dbfs) json.value(level);
  json.end_array().end_object();
  json.key("hum")
      .begin_object()
      .kv("fundamental_hz", defects.hum.fundamental_hz)
      .kv("fundamental_prominence", defects.hum.fundamental_prominence)
      .kv("harmonics", defects.hum.harmonics);
  json.key("harmonic_dbfs").begin_array();
  for (const float level : defects.hum.harmonic_dbfs) json.value(level);
  json.end_array().end_object();
  json.key("reverb")
      .begin_object()
      .kv("late_decay_ratio_db", defects.reverb.late_decay_ratio_db)
      .end_object();
  json.end_object();
}

/// Human-readable counterpart to @ref append_defect_profile_json, with the same
/// traps spelled out as a sentence rather than a raw sentinel value.
void print_defect_profile_text(const mastering::assistant::DefectProfile& defects) {
  std::cout << "\n" << color::cyan << color::bold << "Defect Detection" << color::reset << "\n";
  if (!defects.measured) {
    std::cout << "  Measured:        no (input too short for the detectors to run -- nobody "
                 "looked, which is not the same as clean)\n\n";
    return;
  }
  const auto& click = defects.click;
  const auto& clip = defects.clip;
  const auto& crackle = defects.crackle;
  const auto& hum = defects.hum;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Clicks:          " << click.count << " detected, " << click.rejected
            << " rejected as undecided (longest run " << click.longest_run_samples << " samples, "
            << click.per_second << "/s)\n";
  std::cout << "  Crackle:         " << crackle.sample_count << " samples ("
            << (crackle.sample_fraction * 100.0f) << "% of signal, " << crackle.per_second
            << "/s)\n";
  std::cout << "  Clipping:        " << clip.sample_count << " samples in " << clip.run_count
            << " runs (" << (clip.sample_fraction * 100.0f) << "% of signal, longest "
            << clip.longest_run_samples << " samples)\n";
  if (clip.flat_run_count > 0) {
    std::cout << "  Flat tops:       " << clip.flat_run_count << " runs pinned at "
              << clip.flat_level << " (" << clip.flat_sample_count << " samples, longest "
              << clip.longest_flat_run_samples
              << ") -- these survive a later gain change, so they are clipping rather than a "
                 "peak that merely reaches the ceiling\n";
  } else {
    std::cout << "  Flat tops:       none (an unclipped peak reaches the ceiling too; "
                 "resampling or a lossy codec erases a real one, so this is not proof of no "
                 "clipping)\n";
  }
  const auto* band_floor = std::begin(defects.noise.band_floor_dbfs);
  const auto* loudest_band = std::max_element(band_floor, std::end(defects.noise.band_floor_dbfs));
  std::cout << "  Noise floor:     " << defects.noise.floor_dbfs << " dBFS, loudest band "
            << *loudest_band << " dBFS (band " << (loudest_band - band_floor) << ")\n";
  if (hum.fundamental_prominence > 1.0f) {
    const float loudest_harmonic =
        *std::max_element(std::begin(hum.harmonic_dbfs), std::end(hum.harmonic_dbfs));
    std::cout << "  Hum:             " << hum.fundamental_hz << " Hz fundamental, " << hum.harmonics
              << " harmonics, prominence " << hum.fundamental_prominence << "x energy ratio ("
              << hum.harmonic_dbfs[0] << " dBFS at f0, " << loudest_harmonic
              << " dBFS loudest harmonic)\n";
  } else {
    std::cout << "  Hum:             no fundamental peak found at 50 or 60 Hz (inconclusive, not "
                 "the same as no hum)\n";
  }
  std::cout << "  Late decay:      " << defects.reverb.late_decay_ratio_db
            << " dB (dereverb-window decay ratio, not RT60 -- less negative means more "
               "reverberant)\n\n";
}

/// The suggester explains its whole suggestion, mastering stages included; a
/// line about air band or tape drive read under `--explain` here would claim
/// credit for a stage `repair_only_config` just dropped. Every repair line
/// suggester.cpp emits opens with its stage name, so keeping only those (plus
/// the no-detector-ran fallback, which carries no stage name) is exact rather
/// than a guess at the suggester's wording.
std::vector<std::string> repair_only_explanation(const std::vector<std::string>& explanation) {
  static const char* const kRepairPrefixes[] = {
      "declick:", "declip:", "decrackle:", "dehum:", "denoise:", "dereverb:"};
  std::vector<std::string> repair_lines;
  for (const auto& line : explanation) {
    const bool is_repair_stage =
        std::any_of(std::begin(kRepairPrefixes), std::end(kRepairPrefixes),
                    [&](const char* prefix) { return line.rfind(prefix, 0) == 0; });
    if (is_repair_stage || line.rfind("repair requested but nothing measured", 0) == 0) {
      repair_lines.push_back(line);
    }
  }
  return repair_lines;
}

/// Scales @p samples so their peak lands at full scale, and returns the applied
/// gain in dB (0 when the peak already fit and nothing was touched).
///
/// Declipping reconstructs the peaks a clipper cut off, so its output routinely
/// exceeds full scale -- and this command deliberately runs no limiter. The
/// integer writer clamps, which pins exactly the samples the repair just
/// rebuilt back onto the ceiling they were rescued from, undoing the stage that
/// was asked for. One gain for the whole file keeps the reconstructed waveform
/// intact; a per-sample fit would be the clipper again.
float fit_repair_output_to_full_scale(std::initializer_list<std::vector<float>*> channels) {
  float peak = 0.0f;
  for (const auto* channel : channels) {
    for (const float sample : *channel) {
      // A non-finite sample would make every comparison below false and leave
      // the peak at 0, so the scale is skipped rather than turned into a NaN gain.
      if (std::isfinite(sample)) peak = std::max(peak, std::abs(sample));
    }
  }
  if (peak <= 1.0f) return 0.0f;
  // One gain across the channels, so the fit leaves the stereo image alone.
  const float gain = 1.0f / peak;
  for (auto* channel : channels) {
    for (float& sample : *channel) sample *= gain;
  }
  return 20.0f * std::log10(gain);
}

/// Interleaves a stereo pair for the profiler's multi-channel entry.
std::vector<float> interleave_pair(const StereoPlanes& planes) {
  std::vector<float> interleaved(planes.left.size() * 2);
  for (std::size_t i = 0; i < planes.left.size(); ++i) {
    interleaved[2 * i] = planes.left[i];
    interleaved[2 * i + 1] = planes.right[i];
  }
  return interleaved;
}

}  // namespace

int cmd_repair(const CliArgs& args, const Audio& audio) {
  // repair_argument_refusal() has refused every invalid combination before the input was read.
  const bool has_preset = args.has("preset");
  const bool has_stages = args.has("stages");
  const bool detect_only = args.has("detect");
  const std::vector<std::string> stage_list =
      has_stages ? parse_repair_stages(args.get_string("stages")) : std::vector<std::string>{};

  // main() probed the channel count; a two-channel input is measured and
  // repaired as a pair, its detectors run per channel and aggregated.
  const bool stereo_input = args.source_channels == 2;
  StereoPlanes planes;
  std::vector<float> interleaved;
  if (stereo_input) {
    planes = load_stereo_planes(args, audio);
    interleaved = interleave_pair(planes);
  }
  const auto measure_defects = [&] {
    mastering::assistant::AudioProfileConfig profile_config;
    profile_config.detect_defects = true;
    if (!stereo_input) return mastering::assistant::analyze_audio_profile(audio, profile_config);
    return mastering::assistant::analyze_audio_profile_interleaved(
        interleaved.data(), planes.left.size(), 2, audio.sample_rate(), profile_config);
  };

  if (detect_only) {
    const auto profile = measure_defects();
    if (args.json_output) {
      JsonBuilder json;
      json.begin_object().kv("mode", "detect");
      append_defect_profile_json(json, profile.defects);
      json.end_object().print();
    } else {
      print_defect_profile_text(profile.defects);
    }
    return 0;
  }

  mastering::api::MasteringChainConfig source_config;
  std::string mode;
  std::string preset_name;
  std::vector<std::string> explanation;
  mastering::assistant::DefectProfile defects;

  if (has_stages) {
    // Exactly the listed stages at the library's defaults; the measurement
    // pass is the report's, as under a preset.
    mode = "stages";
    defects = measure_defects().defects;
  } else if (has_preset) {
    preset_name = catalog_token(args.get_string("preset"), mastering::api::preset_names());
    source_config = mastering::api::preset_config(mastering::api::preset_from_string(preset_name));
    mode = "preset";
    // The report covers every mode, so a named preset pays for its own
    // measurement pass even though nothing here uses the result to choose a
    // stage -- the report is half of what this command is for.
    defects = measure_defects().defects;
  } else {
    mastering::assistant::AssistantConfig assistant_config;
    assistant_config.enable_repair = true;
    const auto suggestion =
        stereo_input
            ? mastering::assistant::suggest_chain_interleaved(
                  interleaved.data(), planes.left.size(), 2, audio.sample_rate(), assistant_config)
            : mastering::assistant::suggest_chain(audio, assistant_config);
    source_config = suggestion.config;
    explanation = repair_only_explanation(suggestion.explanation);
    defects = suggestion.profile.defects;
    mode = "assistant";
  }

  auto repair_config = repair_only_config(source_config);
  const auto user_overrides = parse_chain_params(args.get_string("params"));
  std::vector<mastering::api::Param> overrides;
  for (const auto& stage : stage_list) overrides.push_back({"repair." + stage + ".enabled", 1.0});
  overrides.insert(overrides.end(), user_overrides.begin(), user_overrides.end());
  if (!overrides.empty()) {
    mastering::api::apply_chain_config_overrides(repair_config, overrides.data(), overrides.size());
  }

  mastering::api::MasteringChain chain(std::move(repair_config));
  std::vector<std::string> stages;
  float applied_gain_db = 0.0f;
  if (stereo_input) {
    auto result = chain.process_stereo(planes.left.data(), planes.right.data(), planes.left.size(),
                                       audio.sample_rate());
    applied_gain_db = fit_repair_output_to_full_scale({&result.left, &result.right});
    save_stereo_wav(args.output_file, result.left, result.right, result.sample_rate,
                    args.get_int("bits", 16));
    stages = result.stages;
  } else {
    auto result = chain.process_mono(audio.data(), audio.size(), audio.sample_rate());
    applied_gain_db = fit_repair_output_to_full_scale({&result.samples});
    save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate,
             args.get_int("bits", 16));
    stages = result.stages;
  }

  const bool show_explanation = args.has("explain") && !explanation.empty();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("mode", mode);
    if (!preset_name.empty()) json.kv("preset", preset_name);
    json.key("stages").begin_array();
    for (const auto& stage : stages) json.value(stage);
    json.end_array();
    if (show_explanation) {
      json.key("explanation").begin_array();
      for (const auto& item : explanation) json.value(item);
      json.end_array();
    }
    json.kv("output", args.output_file);
    // Always emitted, 0 when the peak already fit: a key that appeared only on
    // the files it acted on would make its absence mean both "did not clip" and
    // "this build does not report it".
    json.kv("output_gain_db", applied_gain_db);
    append_defect_profile_json(json, defects);
    json.end_object().print();
  } else {
    std::cout << "\n"
              << color::cyan << color::bold << "Repair" << color::reset << "\n"
              << "  Mode:            " << mode << "\n";
    if (!preset_name.empty()) std::cout << "  Preset:          " << preset_name << "\n";
    std::cout << "  Stages:          " << stages.size() << "\n"
              << "  Output:          " << args.output_file << "\n";
    if (applied_gain_db != 0.0f) {
      std::cout << "  Output gain:     " << applied_gain_db
                << " dB (the repair rebuilt peaks past full scale)\n";
    }
    if (show_explanation) {
      std::cout << "  Explanation:\n";
      for (const auto& item : explanation) std::cout << "    - " << item << "\n";
    }
    print_defect_profile_text(defects);
  }
  return 0;
}
#endif
