#include <limits>

#include "sonare_cli.h"

int cmd_normalize(const CliArgs& args, const Audio& audio) {
  const std::string mode = args.get_string("mode", "peak");
  const bool rms = mode == "rms";
  const float target_db =
      args.get_float("target-db", rms ? sonare::kDefaultRmsTargetDb : sonare::kDefaultPeakTargetDb);

  size_t length = 0;
  double duration = 0.0;
  if (args.source_channels == 2) {
    // The planes bypass the offline-input policy Audio::from_file applied to the
    // mono decode; a non-finite sample in either channel reaches the downmix as
    // non-finite, so that decode has already refused such a file.
    StereoPlanes planes = load_stereo_planes(args, audio);
    const int sample_rate = audio.sample_rate();
    const Audio left = Audio::from_vector(std::move(planes.left), sample_rate);
    const Audio right = Audio::from_vector(std::move(planes.right), sample_rate);
    // One gain for the pair: a per-channel gain would raise the quieter side
    // further and collapse the image toward centre.
    const NormalizeStereoResult result = rms ? normalize_rms_stereo(left, right, target_db)
                                             : normalize_stereo(left, right, target_db);
    save_stereo_wav(args.output_file, result.left, result.right, result.left.sample_rate());
    length = result.left.size();
    duration = result.left.duration();
  } else {
    const Audio result = rms ? normalize_rms(audio, target_db) : normalize(audio, target_db);
    save_wav(args.output_file, result.data(), result.size(), result.sample_rate());
    length = result.size();
    duration = result.duration();
  }

  if (args.json_output) {
    // `length` is per channel and no channel count is published: the payload is a
    // cross-front-end contract, and how many channels the file carries is a
    // property of the file.
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("mode", mode)
        .kv("target_db", target_db)
        .kv("length", length)
        .kv("sample_rate", audio.sample_rate())
        .kv("duration", duration)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

namespace {

/// @brief Takes ownership of a C entry's output buffer as an Audio at @p sample_rate.
Audio take_c_samples(float* samples, size_t length, int sample_rate) {
  std::vector<float> owned(samples, samples + length);
  sonare_free_floats(samples);
  return Audio::from_vector(std::move(owned), sample_rate);
}

void report_saved(const CliArgs& args) {
  if (!args.json_output && !args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
}

}  // namespace

#ifdef SONARE_WITH_MASTERING
int cmd_gain(const CliArgs& args, const Audio& audio) {
  const float gain_db = args.get_float("gain-db", 0.0f);
  const auto result = mastering::api::apply_named_processor(
      "utility.gain", audio.data(), audio.size(), audio.sample_rate(), {{"levelDb", gain_db}});
  save_wav(args.output_file, result.samples.data(), result.samples.size(), result.sample_rate);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("gain_db", gain_db)
        .kv("duration",
            static_cast<float>(result.samples.size()) / static_cast<float>(result.sample_rate))
        .end_object()
        .print();
  }
  report_saved(args);
  return 0;
}
#endif

int cmd_fade(const CliArgs& args, const Audio& audio) {
  // A side left out is no fade, which the library spells as a length of 0.
  const float fade_in_sec = args.get_float("fade-in-sec", 0.0f);
  const float fade_out_sec = args.get_float("fade-out-sec", 0.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_fade(audio.data(), audio.size(), audio.sample_rate(), fade_in_sec,
                                      fade_out_sec, &out, &out_length);
  if (err != SONARE_OK) throw_c_failure(err, nullptr);
  const Audio result = take_c_samples(out, out_length, audio.sample_rate());
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("fade_in_sec", fade_in_sec)
        .kv("fade_out_sec", fade_out_sec)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  report_saved(args);
  return 0;
}

int cmd_filter(const CliArgs& args, const Audio& audio) {
  const std::string type = args.get_string("type", "");
  const auto& tokens = filter_type_tokens();
  SonareBiquadFilterConfig config;
  sonare_biquad_filter_config_init(&config);
  config.type =
      static_cast<int32_t>(std::find(tokens.begin(), tokens.end(), type) - tokens.begin());
  config.order = args.get_int("order", config.order);
  config.zero_phase = args.has("zero-phase") ? 1 : 0;
  // The validator has refused a frequency the type does not read; one left out stays not given.
  if (args.has("cutoff-hz")) config.cutoff_hz = args.get_float("cutoff-hz", 0.0f);
  if (args.has("center-hz")) config.center_hz = args.get_float("center-hz", 0.0f);
  if (args.has("bandwidth-hz")) config.bandwidth_hz = args.get_float("bandwidth-hz", 0.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_biquad_filter(audio.data(), audio.size(), audio.sample_rate(),
                                               &config, &out, &out_length);
  if (err != SONARE_OK) throw_c_failure(err, "config");
  const Audio result = take_c_samples(out, out_length, audio.sample_rate());
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("output", args.output_file).kv("type", type).kv("order", config.order);
    // A frequency the type does not read is absent, as every record field without a value is.
    const auto frequency = [&](const char* key, const char* option, float value) {
      if (args.has(option)) json.kv(key, value);
    };
    frequency("cutoff_hz", "cutoff-hz", config.cutoff_hz);
    frequency("center_hz", "center-hz", config.center_hz);
    frequency("bandwidth_hz", "bandwidth-hz", config.bandwidth_hz);
    json.kv("zero_phase", config.zero_phase != 0)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  report_saved(args);
  return 0;
}

int cmd_resample(const CliArgs& args, const Audio& audio) {
  if (!args.has("target-sample-rate")) {
    std::cerr << color::red << "Error: --target-sample-rate required" << color::reset << "\n";
    return 1;
  }
  const int source_sr = audio.sample_rate();
  const int target_sr = args.get_int("target-sample-rate", source_sr);
  Audio result = resample(audio, target_sr);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("length", result.size())
        .kv("source_sample_rate", source_sr)
        .kv("target_sample_rate", target_sr);
    if (!args.output_file.empty()) json.kv("output", args.output_file);
    json.end_object().print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_tone(const CliArgs& args, const Audio&) {
  const float frequency = args.get_float("frequency-hz", 0.0f);
  const int sr = args.get_int("sample-rate", constants::kDefaultSampleRate);
  const float duration = args.get_float("duration-sec", kDefaultSynthesisDurationSec);
  const float phase = args.get_float("phase", kDefaultTonePhase);
  const float amplitude = args.get_float("amplitude", kDefaultToneAmplitude);

  Audio result = tone(frequency, sr, duration, phase, amplitude);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("frequency_hz", frequency)
        .kv("sample_rate", sr)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_chirp(const CliArgs& args, const Audio&) {
  const float fmin = args.fmin;
  const float fmax = args.fmax;
  const bool linear = !args.has("exponential");
  const int sr = args.get_int("sample-rate", constants::kDefaultSampleRate);
  const float duration = args.get_float("duration-sec", kDefaultSynthesisDurationSec);

  Audio result = chirp(fmin, fmax, sr, duration, linear);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("fmin_hz", fmin)
        .kv("fmax_hz", fmax)
        .kv("sample_rate", sr)
        .kv("duration", result.duration())
        .kv("linear", linear)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_clicks(const CliArgs& args, const Audio&) {
  const std::vector<float> times = parse_float_list(args.get_string("times"));
  const int sr = args.get_int("sample-rate", constants::kDefaultSampleRate);
  const int length = args.get_int("length", kClicksFitLength);
  const float frequency = args.get_float("frequency-hz", kDefaultClickFrequencyHz);
  const float click_duration = args.get_float("click-duration-sec", kDefaultClickDurationSec);

  Audio result = clicks(times, sr, length, frequency, click_duration);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("n_clicks", times.size())
        .kv("sample_rate", sr)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}
