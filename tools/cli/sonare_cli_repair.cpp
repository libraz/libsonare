#include <algorithm>
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

/// `--params` on `repair` is documented as repair config overrides, so a key
/// reaching any other stage would let an override silently master -- refusing
/// it here is what makes that promise actual rather than aspirational.
void reject_non_repair_overrides(const std::vector<mastering::api::Param>& overrides) {
  for (const auto& param : overrides) {
    if (param.key.rfind("repair.", 0) == 0) continue;
    throw std::invalid_argument("--params key for repair must start with 'repair.': " + param.key);
  }
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
float fit_repair_output_to_full_scale(std::vector<float>& samples) {
  float peak = 0.0f;
  for (const float sample : samples) {
    // A non-finite sample would make every comparison below false and leave the
    // peak at 0, so the scale is skipped rather than turned into a NaN gain.
    if (std::isfinite(sample)) peak = std::max(peak, std::abs(sample));
  }
  if (peak <= 1.0f) return 0.0f;
  const float gain = 1.0f / peak;
  for (float& sample : samples) sample *= gain;
  return 20.0f * std::log10(gain);
}

}  // namespace

int cmd_repair(const CliArgs& args, const Audio& audio) {
  const bool has_preset = args.has("preset");
  const bool detect_only = args.has("detect");

  // Why each stage was chosen only exists on the default (measure-and-choose)
  // path: a named preset picked its stages by name, and --detect never picks
  // any at all, so the flag would report on a decision that never ran.
  if (args.has("explain") && (has_preset || detect_only)) {
    throw std::invalid_argument(
        "--explain requires the default preset selection (omit --preset and --detect)");
  }

  if (detect_only) {
    mastering::assistant::AudioProfileConfig profile_config;
    profile_config.detect_defects = true;
    const auto profile = mastering::assistant::analyze_audio_profile(audio, profile_config);
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

  if (has_preset) {
    preset_name = catalog_token(args.get_string("preset"), mastering::api::preset_names());
    source_config = mastering::api::preset_config(mastering::api::preset_from_string(preset_name));
    mode = "preset";
    // The report covers every mode, so a named preset pays for its own
    // measurement pass even though nothing here uses the result to choose a
    // stage -- the report is half of what this command is for.
    mastering::assistant::AudioProfileConfig profile_config;
    profile_config.detect_defects = true;
    defects = mastering::assistant::analyze_audio_profile(audio, profile_config).defects;
  } else {
    mastering::assistant::AssistantConfig assistant_config;
    assistant_config.enable_repair = true;
    const auto suggestion = mastering::assistant::suggest_chain(audio, assistant_config);
    source_config = suggestion.config;
    explanation = repair_only_explanation(suggestion.explanation);
    defects = suggestion.profile.defects;
    mode = "assistant";
  }

  auto repair_config = repair_only_config(source_config);
  const auto overrides = parse_chain_params(args.get_string("params"));
  if (!overrides.empty()) {
    reject_non_repair_overrides(overrides);
    mastering::api::apply_chain_config_overrides(repair_config, overrides.data(), overrides.size());
  }

  mastering::api::MasteringChain chain(std::move(repair_config));
  const auto result = chain.process_mono(audio.data(), audio.size(), audio.sample_rate());
  std::vector<float> samples = result.samples;
  const float applied_gain_db = fit_repair_output_to_full_scale(samples);
  save_wav(args.output_file, samples.data(), samples.size(), result.sample_rate,
           args.get_int("bits", 16));

  const bool show_explanation = args.has("explain") && !explanation.empty();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("mode", mode);
    if (!preset_name.empty()) json.kv("preset", preset_name);
    json.key("stages").begin_array();
    for (const auto& stage : result.stages) json.value(stage);
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
    std::cout << "  Stages:          " << result.stages.size() << "\n"
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
