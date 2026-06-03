#include "sonare_cli.h"

int cmd_pitch_shift(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: pitch-shift requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("semitones")) {
    std::cerr << color::red << "Error: --semitones required" << color::reset << "\n";
    return 1;
  }

  float semitones = args.get_float("semitones", 0.0f);
  PitchShiftConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Pitch shifting by " << semitones << " semitones..." << color::reset
              << "\n";
  }

  Audio result = pitch_shift(audio, semitones, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("semitones", semitones)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_time_stretch(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: time-stretch requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("rate")) {
    std::cerr << color::red << "Error: --rate required" << color::reset << "\n";
    return 1;
  }

  float rate = args.get_float("rate", 1.0f);
  TimeStretchConfig config{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Time stretching with rate " << rate << "..." << color::reset
              << "\n";
  }

  Audio result = time_stretch(audio, rate, config);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("rate", rate)
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_pitch_correct(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: pitch-correct requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("current-midi") || !args.has("target-midi")) {
    std::cerr << color::red << "Error: --current-midi and --target-midi required" << color::reset
              << "\n";
    return 1;
  }

  const float current_midi = args.get_float("current-midi", 69.0f);
  const float target_midi = args.get_float("target-midi", 69.0f);
  editing::pitch_editor::PitchCorrector corrector;
  editing::pitch_editor::F0Track track;
  track.sample_rate = audio.sample_rate();
  track.hop_length = args.hop_length;
  track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};

  Audio result = corrector.correct_to_midi(audio, track, target_midi);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("current_midi", current_midi)
        .kv("target_midi", target_midi)
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_note_stretch(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: note-stretch requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  if (!args.has("onset") || !args.has("offset") || !args.has("ratio")) {
    std::cerr << color::red << "Error: --onset, --offset and --ratio required" << color::reset
              << "\n";
    return 1;
  }

  editing::pitch_editor::NoteRegion region;
  region.onset_sample = args.get_int("onset", 0);
  region.offset_sample = args.get_int("offset", 0);
  const float ratio = args.get_float("ratio", 1.0f);

  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, ratio);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("onset_sample", region.onset_sample)
        .kv("offset_sample", region.offset_sample)
        .kv("ratio", ratio)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_voice_change(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: voice-change requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }

  Audio result;
  std::string preset_id;
  int latency_samples = 0;
  float pitch_semitones = 0.0f;
  float formant_factor = 1.0f;
  if (args.has("preset") || args.has("preset-json") || args.has("preset-pack") || args.has("set")) {
    preset_id = args.get_string("preset", "neutral-monitor");
    std::string config_text = preset_id;
    if (args.has("preset-json")) {
      config_text = read_plain_text_file(args.get_string("preset-json"));
    } else if (args.has("preset-pack")) {
      config_text = find_voice_preset_in_pack(read_plain_text_file(args.get_string("preset-pack")),
                                              preset_id);
    } else if (args.has("set")) {
      const auto id = editing::voice_changer::realtime_voice_changer_preset_from_id(preset_id);
      config_text = editing::voice_changer::realtime_voice_changer_preset_json(id);
    }
    if (args.has("set")) config_text = apply_voice_preset_sets(config_text, args.get_string("set"));
    auto config = editing::voice_changer::realtime_voice_changer_config_from_json(config_text);
    editing::voice_changer::RealtimeVoiceChanger changer(config);
    constexpr int kBlock = 512;
    changer.prepare(audio.sample_rate(), kBlock, 1);
    std::vector<float> output(audio.size(), 0.0f);
    for (size_t pos = 0; pos < audio.size(); pos += kBlock) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, audio.size() - pos));
      changer.process_block(audio.data() + pos, output.data() + pos, n);
    }
    latency_samples = changer.latency_samples();
    result = Audio::from_vector(std::move(output), audio.sample_rate());
  } else {
    pitch_semitones = args.get_float("pitch-semitones", 0.0f);
    formant_factor = args.get_float("formant-factor", 1.0f);
    editing::voice_changer::VoiceChangerConfig config;
    config.pitch_semitones = pitch_semitones;
    config.formant_factor = formant_factor;
    editing::voice_changer::VoiceChanger changer(config);
    result = changer.process(audio);
  }
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("output", args.output_file)
        .kv("durationSec", result.duration())
        .kv("sampleRate", result.sample_rate())
        .kv("latencySamples", latency_samples);
    if (!preset_id.empty()) {
      json.kv("presetId", preset_id);
    } else {
      // Offline voice-change path: echo the simple pitch/formant knobs the
      // caller supplied so JSON consumers can correlate input args with the
      // result without re-parsing CLI flags.
      json.kv("pitch_semitones", pitch_semitones).kv("formant_factor", formant_factor);
    }
    json.end_object().print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_voice_presets(const CliArgs& args, const Audio&) {
  const auto names = editing::voice_changer::realtime_voice_changer_preset_names();
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().key("presets").begin_array();
    for (const auto& name : names) json.value(name);
    json.end_array().end_object().print();
  } else {
    for (const auto& name : names) std::cout << name << "\n";
  }
  return 0;
}

int cmd_voice_preset(const CliArgs& args, const Audio&) {
  const std::string preset = args.get_string("preset", "neutral-monitor");
  const auto id = editing::voice_changer::realtime_voice_changer_preset_from_id(preset);
  std::cout << editing::voice_changer::realtime_voice_changer_preset_json(id) << "\n";
  return 0;
}

int cmd_voice_preset_validate(const CliArgs& args, const Audio&) {
  const std::string path = args.get_string("preset-json", args.input_file);
  if (path.empty()) throw std::invalid_argument("voice-preset-validate requires a JSON file");
  std::string config_text = read_plain_text_file(path);
  if (args.has("preset")) {
    config_text = find_voice_preset_in_pack(config_text, args.get_string("preset"));
  }
  if (args.has("set")) config_text = apply_voice_preset_sets(config_text, args.get_string("set"));
  const auto config = editing::voice_changer::realtime_voice_changer_config_from_json(config_text);
  std::cout << editing::voice_changer::realtime_voice_changer_config_to_json(config) << "\n";
  return 0;
}

int cmd_hpss(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: hpss requires output prefix (-o)" << color::reset << "\n";
    return 1;
  }

  HpssConfig config;
  config.kernel_size_harmonic = args.get_int("kernel-harmonic", 31);
  config.kernel_size_percussive = args.get_int("kernel-percussive", 31);
  config.use_soft_mask = !args.has("hard-mask");

  StftConfig stft{args.n_fft, args.hop_length};

  if (!args.quiet) {
    std::cerr << color::blue << "Performing harmonic-percussive separation..." << color::reset
              << "\n";
  }

  std::string base = args.output_file;
  if (base.size() > 4 && base.substr(base.size() - 4) == ".wav") {
    base = base.substr(0, base.size() - 4);
  }

  auto save_audio = [](const std::string& path, const Audio& a) {
    save_wav(path, a.data(), a.size(), a.sample_rate());
  };

  if (args.has("harmonic-only")) {
    std::string path = base + ".wav";
    save_audio(path, harmonic(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved harmonic to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("harmonic", path).end_object().print();
  } else if (args.has("percussive-only")) {
    std::string path = base + ".wav";
    save_audio(path, percussive(audio, config, stft));
    if (!args.quiet) {
      std::cerr << color::green << "Saved percussive to " << path << color::reset << "\n";
    }
    if (args.json_output) JsonBuilder().begin_object().kv("percussive", path).end_object().print();
  } else if (args.has("with-residual")) {
    auto r = hpss_with_residual(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav",
                res = base + "_residual.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    save_audio(res, r.residual);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << ", " << res << color::reset
                << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("harmonic", h)
          .kv("percussive", p)
          .kv("residual", res)
          .end_object()
          .print();
  } else {
    auto r = hpss(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav";
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder().begin_object().kv("harmonic", h).kv("percussive", p).end_object().print();
  }
  return 0;
}

int cmd_preemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: preemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = preemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_deemphasis(const CliArgs& args, const Audio& audio) {
  if (args.output_file.empty()) {
    std::cerr << color::red << "Error: deemphasis requires output file (-o)" << color::reset
              << "\n";
    return 1;
  }
  const float coef = args.get_float("coef", 0.97f);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = deemphasis(input, coef);
  save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("coef", coef)
        .kv("samples", result.size())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_trim_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  std::vector<float> input(audio.begin(), audio.end());
  TrimResult result = sonare::trim(input, top_db, args.n_fft, args.hop_length);

  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.audio.data(), result.audio.size(), audio.sample_rate());
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("start_sample", result.start_sample)
        .kv("end_sample", result.end_sample)
        .kv("samples", result.audio.size())
        .kv("top_db", top_db)
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else {
    std::cout << "Silence Trim:\n";
    printf("  Range:   %d - %d\n", result.start_sample, result.end_sample);
    printf("  Samples: %zu\n", result.audio.size());
    if (!args.output_file.empty()) std::cout << "  Output:  " << args.output_file << "\n";
  }
  return 0;
}

int cmd_split_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  std::vector<float> input(audio.begin(), audio.end());
  auto ranges = sonare::split(input, top_db, args.n_fft, args.hop_length);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_array();
    for (const auto& range : ranges) {
      json.begin_object()
          .kv("start_sample", range.first)
          .kv("end_sample", range.second)
          .end_object();
    }
    json.end_array().print();
  } else {
    std::cout << "Non-silent intervals: " << ranges.size() << "\n";
    for (const auto& range : ranges) {
      printf("  %d - %d\n", range.first, range.second);
    }
  }
  return 0;
}
