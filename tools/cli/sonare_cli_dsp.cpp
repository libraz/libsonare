#include "sonare_cli.h"

int cmd_normalize(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: normalize requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  const std::string mode = args.get_string("mode", "peak");
  Audio result;
  float target_db = 0.0f;
  if (mode == "rms") {
    target_db = args.get_float("target-db", -20.0f);
    result = normalize_rms(audio, target_db);
  } else if (mode == "peak") {
    target_db = args.get_float("target-db", 0.0f);
    result = normalize(audio, target_db);
  } else {
    std::cerr << color::red << "Error: --mode must be 'peak' or 'rms'" << color::reset << "\n";
    return 1;
  }

  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("mode", mode)
        .kv("target_db", target_db)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_gain(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: gain requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("gain-db")) {
    std::cerr << color::red << "Error: --gain-db required" << color::reset << "\n";
    return 1;
  }
  const float gain_db = args.get_float("gain-db", 0.0f);
  Audio result = apply_gain(audio, gain_db);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("gain_db", gain_db)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_fade(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: fade requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("fade-in") && !args.has("fade-out")) {
    std::cerr << color::red << "Error: --fade-in and/or --fade-out required" << color::reset
              << "\n";
    return 1;
  }
  const float fade_in_sec = args.get_float("fade-in", 0.0f);
  const float fade_out_sec = args.get_float("fade-out", 0.0f);

  Audio result = audio;
  if (args.has("fade-in")) result = fade_in(result, fade_in_sec);
  if (args.has("fade-out")) result = fade_out(result, fade_out_sec);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("fade_in", fade_in_sec)
        .kv("fade_out", fade_out_sec)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_filter(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: filter requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  const std::string type = args.get_string("type", "");
  const bool is_hp = (type == "hp" || type == "highpass");
  const bool is_lp = (type == "lp" || type == "lowpass");
  const bool is_bp = (type == "bp" || type == "bandpass");
  const bool is_notch = (type == "notch");
  if (!is_hp && !is_lp && !is_bp && !is_notch) {
    std::cerr << color::red << "Error: --type must be hp|lp|bp|notch" << color::reset << "\n";
    return 1;
  }

  const int sr = audio.sample_rate();
  const int order = args.get_int("order", 2);
  const bool zero_phase = args.has("zero-phase");
  std::vector<float> result;

  float cutoff = 0.0f;
  float center = 0.0f;
  float bandwidth = 0.0f;

  if (is_hp || is_lp) {
    if (!args.has("cutoff")) {
      std::cerr << color::red << "Error: --cutoff required" << color::reset << "\n";
      return 1;
    }
    cutoff = args.get_float("cutoff", 0.0f);
    if (order != 2 && order != 4) {
      std::cerr << color::red << "Error: --order must be 2 or 4" << color::reset << "\n";
      return 1;
    }
    if (order == 4) {
      CascadedBiquad cascade =
          is_hp ? highpass_coeffs_4th(cutoff, sr) : lowpass_coeffs_4th(cutoff, sr);
      result = apply_cascade_filtfilt(audio.data(), audio.size(), cascade);
    } else {
      BiquadCoeffs coeffs = is_hp ? highpass_coeffs(cutoff, sr) : lowpass_coeffs(cutoff, sr);
      result = zero_phase ? apply_biquad_filtfilt(audio.data(), audio.size(), coeffs)
                          : apply_biquad(audio.data(), audio.size(), coeffs);
    }
  } else {
    if (!args.has("center") || !args.has("bandwidth")) {
      std::cerr << color::red << "Error: --center and --bandwidth required" << color::reset << "\n";
      return 1;
    }
    if (order != 2) {
      std::cerr << color::red << "Error: --order 4 is only allowed for hp/lp" << color::reset
                << "\n";
      return 1;
    }
    center = args.get_float("center", 0.0f);
    bandwidth = args.get_float("bandwidth", 0.0f);
    BiquadCoeffs coeffs =
        is_bp ? bandpass_coeffs(center, bandwidth, sr) : notch_coeffs(center, bandwidth, sr);
    result = zero_phase ? apply_biquad_filtfilt(audio.data(), audio.size(), coeffs)
                        : apply_biquad(audio.data(), audio.size(), coeffs);
  }

  save_wav(args.output_file, result.data(), result.size(), sr);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("output", args.output_file).kv("type", type);
    if (is_hp || is_lp) {
      json.kv("cutoff", cutoff);
    } else {
      json.kv("center", center).kv("bandwidth", bandwidth);
    }
    json.kv("duration", static_cast<float>(result.size()) / static_cast<float>(sr))
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_resample(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: resample requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("target-sr")) {
    std::cerr << color::red << "Error: --target-sr required" << color::reset << "\n";
    return 1;
  }
  const int source_sr = audio.sample_rate();
  const int target_sr = args.get_int("target-sr", source_sr);
  Audio result = resample(audio, target_sr);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("source_sr", source_sr)
        .kv("target_sr", target_sr)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_tone(const CliArgs& args, const Audio&) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: tone requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("frequency")) {
    std::cerr << color::red << "Error: --frequency required" << color::reset << "\n";
    return 1;
  }
  const float frequency = args.get_float("frequency", 0.0f);
  const int sr = args.get_int("sr", 22050);
  const float duration = args.get_float("duration", 1.0f);
  const float phase = args.get_float("phase", 0.0f);
  const float amplitude = args.get_float("amplitude", 1.0f);

  Audio result = tone(frequency, sr, duration, phase, amplitude);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("frequency", frequency)
        .kv("sr", sr)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_chirp(const CliArgs& args, const Audio&) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: chirp requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  const float fmin = args.fmin;
  const float fmax = args.fmax;
  const bool linear = !args.has("exponential");
  if (fmax <= 0.0f) {
    std::cerr << color::red << "Error: --fmax must be > 0" << color::reset << "\n";
    return 1;
  }
  if (!linear && fmin <= 0.0f) {
    std::cerr << color::red << "Error: --fmin must be > 0 for an exponential sweep" << color::reset
              << "\n";
    return 1;
  }
  const int sr = args.get_int("sr", 22050);
  const float duration = args.get_float("duration", 1.0f);

  Audio result = chirp(fmin, fmax, sr, duration, linear);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("fmin", fmin)
        .kv("fmax", fmax)
        .kv("sr", sr)
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
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: clicks requires output file (-o)" << color::reset << "\n";
    return 1;
  }
  if (!args.has("times")) {
    std::cerr << color::red << "Error: --times required" << color::reset << "\n";
    return 1;
  }
  const std::vector<float> times = parse_float_list(args.get_string("times"));
  const int sr = args.get_int("sr", 22050);
  const int length = args.get_int("length", 0);
  const float frequency = args.get_float("frequency", 1000.0f);
  const float click_duration = args.get_float("click-duration", 0.1f);

  Audio result = clicks(times, sr, length, frequency, click_duration);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("n_clicks", times.size())
        .kv("sr", sr)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}
