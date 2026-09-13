#include <cctype>

#include "sonare_cli.h"

// Offline-effect output contract: a command that renders an audio buffer
// requires -o/--output. Running it without a destination has no useful result
// (the render would be computed and thrown away), so the missing-output case is
// a hard error mapped to the invalid-parameter exit code, not a silent no-op.
// trim-silence is the deliberate exception: it doubles as an analysis command
// (it reports the trimmed length), so its output stays optional.
//
// The requirement is declared once, in the CLI registry (required_output()),
// and enforced by validate_cli_arguments before dispatch. A handler below
// therefore never re-checks it: when one did, the same missing option produced
// a different message and a different exit code depending on which command the
// caller happened to run.

int cmd_pitch_shift(const CliArgs& args, const Audio& audio) {
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
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_time_stretch(const CliArgs& args, const Audio& audio) {
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
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_pitch_correct(const CliArgs& args, const Audio& audio) {
  const float current_midi = args.get_float("current-midi", 69.0f);
  const float target_midi = args.get_float("target-midi", 69.0f);
  editing::pitch_editor::PitchCorrector corrector;
  // The constant-pitch overload, not the time-varying one. Feeding a synthetic
  // single-frame F0Track to the time-varying path runs the retune IIR, whose
  // per-hop coefficient depends on retune_speed_ms, the hop, and the sample
  // rate: one frame of it applies only a fraction of the requested interval
  // (about 20% at 44.1 kHz with the defaults) and a different fraction at every
  // other sample rate. This command states the interval outright, so the whole
  // (target_midi - current_midi) must be applied, unsmoothed and unclamped --
  // the same facade the Python CLI selects for the same command.
  Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("current_midi", current_midi)
        .kv("target_midi", target_midi)
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_note_stretch(const CliArgs& args, const Audio& audio) {
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
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// The two polyphonic commands below stay registered in the CLI's command table
// regardless of BUILD_PITCH_EDITOR (see get_commands() in
// tools/cli/sonare_cli.cpp), so a build without the pitch editor must still
// answer the subcommand -- with a NotImplemented diagnostic mapped to the CLI's
// not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_PITCH_EDITOR)

namespace {

// The ridge's salience, not the note's: read through the ridge's own start, so a
// frame the ridge does not reach reads 0 rather than the neighbour's value. The
// pairing holds by construction here, the analysis coming straight from the
// chain rather than from a caller.
std::vector<float> polyphonic_note_salience(const editing::polyphony::PolyphonicAnalysis& analysis,
                                            size_t note) {
  const auto& object = analysis.notes[note];
  std::vector<float> span(static_cast<size_t>(std::max(0, object.frame_end - object.frame_start)),
                          0.0f);
  const auto& masks = analysis.masks.notes;
  if (note >= masks.size()) return span;
  const int ridge_index = masks[note].ridge_index;
  const auto& ridges = analysis.track.ridges;
  if (ridge_index < 0 || static_cast<size_t>(ridge_index) >= ridges.size()) return span;
  const auto& ridge = ridges[static_cast<size_t>(ridge_index)];
  for (size_t i = 0; i < span.size(); ++i) {
    const long long frame = static_cast<long long>(object.frame_start) + static_cast<long long>(i) -
                            static_cast<long long>(ridge.frame_start);
    if (frame < 0 || static_cast<size_t>(frame) >= ridge.salience.size()) continue;
    span[i] = ridge.salience[static_cast<size_t>(frame)];
  }
  return span;
}

// The flag literals the parser's own `--flag=false` form accepts, so one spelling
// does not mean two things depending on where it is written.
bool parse_edit_bool(const std::string& field, const std::string& value) {
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") return true;
  if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") return false;
  throw std::invalid_argument("invalid boolean value for --edit " + field + ": " + value);
}

// One --edit occurrence is one NOTE.FIELD=VALUE assignment: the dot path and the
// single assignment per occurrence are --set's grammar, with the note index as
// the path's first element, because an edit addresses a note rather than a JSON
// document and nothing else about the spelling has to differ.
void apply_polyphonic_note_edit(std::vector<editing::note_model::NoteObject>& notes,
                                const std::string& assignment) {
  const auto equals = assignment.find('=');
  if (equals == std::string::npos || equals == 0) {
    throw std::invalid_argument("invalid --edit assignment: " + assignment);
  }
  const std::string path = assignment.substr(0, equals);
  const std::string value = assignment.substr(equals + 1);
  const auto dot = path.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == path.size()) {
    throw std::invalid_argument("--edit path must be NOTE.FIELD: " + assignment);
  }
  const std::string index_text = path.substr(0, dot);
  const int index = parse_int_strict("edit", index_text);
  if (index < 0 || static_cast<size_t>(index) >= notes.size()) {
    throw std::invalid_argument("--edit note index out of range: " + index_text +
                                " (the analysis found " + std::to_string(notes.size()) + " notes)");
  }
  const std::string field = path.substr(dot + 1);
  auto& edit = notes[static_cast<size_t>(index)].edit;
  if (field == "pitch_shift_semitones") {
    edit.pitch_shift_semitones = parse_float_strict("edit", value);
  } else if (field == "gain_db") {
    edit.gain_db = parse_float_strict("edit", value);
  } else if (field == "time_offset_samples") {
    // An int is the whole reachable range: the chain refuses audio longer than
    // an int can index, so a wider offset only ever moves a note off the end.
    edit.time_offset_samples = parse_int_strict("edit", value);
  } else if (field == "time_stretch_ratio") {
    edit.time_stretch_ratio = parse_float_strict("edit", value);
  } else if (field == "formant_shift_semitones") {
    edit.formant_shift_semitones = parse_float_strict("edit", value);
  } else if (field == "vibrato_depth_change") {
    edit.vibrato_depth_change = parse_float_strict("edit", value);
  } else if (field == "drift_change") {
    edit.drift_change = parse_float_strict("edit", value);
  } else if (field == "muted") {
    edit.muted = parse_edit_bool(field, value);
  } else {
    // The accepted set is named at the refusal because the option inventory the
    // help prints carries no per-option text, so this is the only place a caller
    // can read it. The amplitude envelope is the one edit field missing from it:
    // it is a curve, and nothing on a command line states one.
    throw std::invalid_argument(
        "unknown --edit field: " + field +
        " (expected one of pitch_shift_semitones, gain_db, time_offset_samples, "
        "time_stretch_ratio, formant_shift_semitones, vibrato_depth_change, drift_change, muted)");
  }
}

}  // namespace

// Both commands run the chain at its own defaults, so the indices reported here
// are the ones polyphonic-render edits. A framing option on one of the two would
// have to be spelled identically on the other, and a caller spelling it
// differently would silently renumber the notes.
int cmd_polyphonic_notes(const CliArgs& args, const Audio& audio) {
  if (!args.quiet && !args.json_output) {
    std::cerr << color::blue << "Analyzing polyphony..." << color::reset << "\n";
  }
  const auto analysis = editing::polyphony::analyze_polyphonic(audio);
  const int frame_count = analysis.spectrum.n_frames();

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("sample_rate", audio.sample_rate())
        .kv("frame_count", frame_count)
        .kv("note_count", analysis.notes.size())
        .key("polyphony")
        .begin_array();
    for (int count : analysis.track.polyphony) json.value(count);
    json.end_array().key("notes").begin_array();
    for (size_t i = 0; i < analysis.notes.size(); ++i) {
      const auto& note = analysis.notes[i];
      json.begin_object()
          .kv("index", i)
          .kv("onset_sample", static_cast<size_t>(note.onset_sample))
          .kv("offset_sample", static_cast<size_t>(note.offset_sample))
          .kv("frame_start", note.frame_start)
          .kv("frame_end", note.frame_end)
          .kv("median_hz", note.median_hz)
          .kv("median_cents", note.median_cents)
          .kv("f0_stability", note.f0_stability)
          .key("f0_hz")
          .float_array(note.f0_hz.values)
          .key("amplitude")
          .float_array(note.amplitude.values)
          .key("salience")
          .float_array(polyphonic_note_salience(analysis, i))
          .end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "Polyphonic notes: " << analysis.notes.size() << "\n";
    printf("  Frames: %d @ %d Hz\n", frame_count, audio.sample_rate());
    for (size_t i = 0; i < analysis.notes.size(); ++i) {
      const auto& note = analysis.notes[i];
      printf("  [%zu] samples %lld-%lld  frames %d-%d  %.2f Hz  stability %.3f\n", i,
             static_cast<long long>(note.onset_sample), static_cast<long long>(note.offset_sample),
             note.frame_start, note.frame_end, note.median_hz, note.f0_stability);
    }
  }
  return 0;
}

int cmd_polyphonic_render(const CliArgs& args, const Audio& audio) {
  const auto assignments = args.get_string_list("edit");
  if (!args.quiet && !args.json_output) {
    std::cerr << color::blue << "Analyzing polyphony..." << color::reset << "\n";
  }
  // The same defaults polyphonic-notes numbered the notes against.
  auto analysis = editing::polyphony::analyze_polyphonic(audio);
  for (const auto& assignment : assignments) {
    apply_polyphonic_note_edit(analysis.notes, assignment);
  }

  const Audio result = editing::polyphony::render_polyphonic(analysis);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("note_count", analysis.notes.size())
        .kv("edits", assignments.size())
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

#else  // !SONARE_WITH_PITCH_EDITOR

int cmd_polyphonic_notes(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_polyphonic_render(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

#endif  // SONARE_WITH_PITCH_EDITOR

// The four voice-changer commands below stay registered in the CLI's command
// table regardless of BUILD_VOICE_CHANGER (see get_commands() in
// tools/cli/sonare_cli.cpp), so a build without the voice changer must still
// answer the subcommand -- with a NotImplemented diagnostic mapped to the
// CLI's not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_VOICE_CHANGER)

int cmd_voice_change(const CliArgs& args, const Audio& audio) {
  const bool has_preset = args.has("preset");
  const bool has_preset_json = args.has("preset-json");
  const bool has_preset_pack = args.has("preset-pack");
  const int selector_count = static_cast<int>(has_preset) + static_cast<int>(has_preset_json);
  if (selector_count > 1) {
    throw std::invalid_argument(
        "voice-change preset selectors are mutually exclusive: choose one of --preset, "
        "--preset-json, or --preset-pack");
  }
  if (has_preset_pack && has_preset_json) {
    throw std::invalid_argument("--preset-pack and --preset-json are mutually exclusive");
  }
  // A pack names the file, --preset names the entry inside it, so the pair is
  // one selector. This check precedes the ones below so that a pack without an
  // entry reports the missing --preset rather than a downstream rule that reads
  // as if no selector had been given at all.
  if (has_preset_pack && !has_preset) {
    throw std::invalid_argument("--preset-pack requires --preset to select an entry");
  }
  if (selector_count > 0 && (args.has("pitch-semitones") || args.has("formant-factor"))) {
    throw std::invalid_argument(
        "--pitch-semitones/--formant-factor cannot be combined with a realtime preset");
  }
  if (args.has("set") && selector_count == 0) {
    throw std::invalid_argument("--set requires --preset, --preset-json, or --preset-pack");
  }
  const bool uses_realtime_preset = selector_count > 0 || args.has("set");

  Audio result;
  std::string preset_id;
  int latency_samples = 0;
  float pitch_semitones = 0.0f;
  float formant_factor = 1.0f;
  if (uses_realtime_preset) {
    const std::string requested_preset = args.get_string("preset", "");
    // Only advertise an ID when it identifies the selected source. A
    // --preset-json document has its own identity (or may be anonymous), so
    // falling back to the neutral preset here would be misleading. A
    // --preset-pack entry remains identified by the explicit --preset value.
    if (has_preset) preset_id = requested_preset;
    std::string config_text = requested_preset;
    if (args.has("preset-json")) {
      config_text = read_plain_text_file(args.get_string("preset-json"));
    } else if (args.has("preset-pack")) {
      config_text = find_voice_preset_in_pack(read_plain_text_file(args.get_string("preset-pack")),
                                              requested_preset);
    } else if (args.has("set")) {
      const auto id =
          editing::voice_changer::realtime_voice_changer_preset_from_id(requested_preset);
      config_text = editing::voice_changer::realtime_voice_changer_preset_json(id);
    }
    if (args.has("set"))
      config_text = apply_voice_preset_sets(config_text, args.get_string_list("set"));

    // Route through the same strict validator as the C ABI / Python entry
    // points (realtime_voice_changer_config_from_input) instead of the
    // tolerant realtime_voice_changer_config_from_json: a mistyped section
    // name, a missing "dsp" wrapper, or a partial hand-written preset must
    // fail loudly rather than silently render with unrelated defaults.
    editing::voice_changer::RealtimeVoiceChangerConfig config;
    std::string config_error;
    if (!editing::voice_changer::realtime_voice_changer_config_from_input(config_text, &config,
                                                                          &config_error)) {
      throw std::invalid_argument("invalid voice preset: " + config_error);
    }
    editing::voice_changer::RealtimeVoiceChanger changer(config);
    // Block size and pre-roll/drop latency compensation mirror the C-ABI
    // oracle (process_realtime_voice_change_compensated in
    // src/c_api/sonare_c_voice_changer.cpp): pad the input by the chain
    // latency, process in fixed 128-sample blocks for bit-identical DSP
    // across surfaces, then drop the leading pre-roll so output sample k
    // corresponds to input sample k and the output length equals the input
    // length.
    constexpr int kBlock = 128;
    changer.prepare(audio.sample_rate(), kBlock, 1);
    latency_samples = std::max(changer.latency_samples(), 0);
    const size_t latency_frames = static_cast<size_t>(latency_samples);
    const size_t total = audio.size() + latency_frames;
    std::vector<float> padded_input(total, 0.0f);
    std::copy(audio.data(), audio.data() + audio.size(), padded_input.begin());
    std::vector<float> padded_output(total, 0.0f);
    for (size_t pos = 0; pos < total; pos += kBlock) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, total - pos));
      changer.process_block(padded_input.data() + pos, padded_output.data() + pos, n);
    }
    std::vector<float> output(
        padded_output.begin() + static_cast<std::ptrdiff_t>(latency_frames),
        padded_output.begin() + static_cast<std::ptrdiff_t>(latency_frames + audio.size()));
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

  // Pitch/formant processing uses spectral transforms whose boundary
  // convention can produce one extra (or one missing) sample.  The CLI's
  // voice-change contract is sample-preserving, so normalize both branches
  // to the input length before writing the artifact or reporting metadata.
  if (result.size() != audio.size()) {
    std::vector<float> sized_result(audio.size(), 0.0f);
    const size_t copy_size = std::min(result.size(), audio.size());
    if (copy_size > 0) {
      std::copy(result.data(), result.data() + copy_size, sized_result.begin());
    }
    result = Audio::from_vector(std::move(sized_result), audio.sample_rate());
  }
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("output", args.output_file)
        .kv("length", result.size())
        .kv("duration", result.duration())
        .kv("sample_rate", result.sample_rate())
        .kv("latency_samples", latency_samples);
    if (uses_realtime_preset && !preset_id.empty()) {
      json.kv("preset", preset_id);
    } else if (!uses_realtime_preset) {
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
  if (path.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::FileNotFound,
                                  "voice-preset-validate requires a JSON file");
  }
  // read_plain_text_file historically reports open failures as
  // std::invalid_argument. Classify the pre-validation missing-file case as
  // FileNotFound so it maps to exit 4 (or legacy exit 1) and never emits a
  // JSON validation envelope.
  {
    std::ifstream input(path);
    if (!input.is_open()) {
      throw sonare::SonareException(sonare::ErrorCode::FileNotFound,
                                    "cannot open text file: " + path);
    }
  }
  std::string config_text = read_plain_text_file(path);
  if (args.has("preset")) {
    config_text = find_voice_preset_in_pack(config_text, args.get_string("preset"));
  }
  if (args.has("set")) {
    config_text = apply_voice_preset_sets(config_text, args.get_string_list("set"));
  }
  std::string normalized;
  std::string error;
  if (!editing::voice_changer::validate_realtime_voice_changer_preset_json(config_text, &normalized,
                                                                           &error)) {
    if (error.empty()) error = "invalid voice preset";
    if (args.json_output) {
      JsonBuilder().begin_object().kv("ok", false).kv("error", error).end_object().print();
    } else {
      std::cerr << error << "\n";
    }
    return 3;
  }
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object().kv("ok", true).kv("normalized_json", normalized).end_object().print();
  } else {
    std::cout << normalized << "\n";
  }
  return 0;
}

#else  // !SONARE_WITH_VOICE_CHANGER

int cmd_voice_change(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_presets(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset_validate(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "voice changer support is not compiled in");
}

#endif  // SONARE_WITH_VOICE_CHANGER

int cmd_hpss(const CliArgs& args, const Audio& audio) {
  const int output_mode_count = static_cast<int>(args.has("harmonic-only")) +
                                static_cast<int>(args.has("percussive-only")) +
                                static_cast<int>(args.has("with-residual"));
  if (output_mode_count > 1) {
    throw std::invalid_argument(
        "hpss output modes are mutually exclusive: choose one of --harmonic-only, "
        "--percussive-only, or --with-residual");
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

  // The suffix is stripped case-insensitively: a caller writing `-o out.WAV`
  // means the same thing as `-o out.wav`, and the Python CLI already reads it
  // that way, so a case-sensitive compare made the two CLIs write differently
  // named artifacts from one command line.
  std::string base = args.output_file;
  if (base.size() > 4) {
    std::string suffix = base.substr(base.size() - 4);
    for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (suffix == ".wav") base = base.substr(0, base.size() - 4);
  }

  auto save_audio = [](const std::string& path, const Audio& a) {
    save_wav(path, a.data(), a.size(), a.sample_rate());
  };
  // Mean absolute amplitude, the level summary both CLIs publish per component.
  auto component_energy = [](const Audio& a) {
    if (a.empty()) return 0.0f;
    float total = 0.0f;
    for (float sample : a) total += std::fabs(sample);
    return total / static_cast<float>(a.size());
  };

  if (args.has("harmonic-only")) {
    std::string path = base + ".wav";
    const Audio result = harmonic(audio, config, stft);
    save_audio(path, result);
    if (!args.quiet) {
      std::cerr << color::green << "Saved harmonic to " << path << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", result.size())
          .kv("sample_rate", result.sample_rate())
          .kv("harmonic_energy", component_energy(result))
          .kv("harmonic", path)
          .end_object()
          .print();
  } else if (args.has("percussive-only")) {
    std::string path = base + ".wav";
    const Audio result = percussive(audio, config, stft);
    save_audio(path, result);
    if (!args.quiet) {
      std::cerr << color::green << "Saved percussive to " << path << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", result.size())
          .kv("sample_rate", result.sample_rate())
          .kv("percussive_energy", component_energy(result))
          .kv("percussive", path)
          .end_object()
          .print();
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
          .kv("length", r.harmonic.size())
          .kv("sample_rate", r.harmonic.sample_rate())
          .kv("harmonic_energy", component_energy(r.harmonic))
          .kv("percussive_energy", component_energy(r.percussive))
          .kv("residual_energy", component_energy(r.residual))
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
      JsonBuilder()
          .begin_object()
          .kv("length", r.harmonic.size())
          .kv("sample_rate", r.harmonic.sample_rate())
          .kv("harmonic_energy", component_energy(r.harmonic))
          .kv("percussive_energy", component_energy(r.percussive))
          .kv("harmonic", h)
          .kv("percussive", p)
          .end_object()
          .print();
  }
  return 0;
}

int cmd_preemphasis(const CliArgs& args, const Audio& audio) {
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
  // Match Python's public trim() command: an absolute dB threshold. --top-db
  // remains a native compatibility alias for the legacy relative algorithm.
  if (args.has("threshold-db") && args.has("top-db")) {
    throw std::invalid_argument("--threshold-db and --top-db are mutually exclusive");
  }
  const bool use_legacy_top_db = args.has("top-db") && !args.has("threshold-db");
  const float threshold_db = args.get_float("threshold-db", -60.0f);
  Audio result =
      use_legacy_top_db
          ? Audio::from_vector(
                sonare::trim(std::vector<float>(audio.begin(), audio.end()),
                             args.get_float("top-db", 60.0f), args.n_fft, args.hop_length)
                    .audio,
                audio.sample_rate())
          : trim_absolute(audio, threshold_db, args.n_fft, args.hop_length);

  if (!args.output_file.empty()) {
    save_wav(args.output_file, result.data(), result.size(), audio.sample_rate());
  }

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("length", result.size())
        .kv("sample_rate", audio.sample_rate())
        .kv("duration", result.duration());
    // Report the parameter that actually drove the trim: the relative top_db for
    // the legacy algorithm, the absolute threshold_db otherwise.
    if (use_legacy_top_db) {
      json.kv("top_db", args.get_float("top-db", 60.0f));
    } else {
      json.kv("threshold_db", threshold_db);
    }
    json.kv("n_fft", args.n_fft).kv("hop_length", args.hop_length);
    if (!args.output_file.empty()) json.kv("output", args.output_file);
    json.end_object().print();
  } else {
    std::cout << "Silence Trim:\n";
    printf("  Samples: %zu\n", result.size());
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
