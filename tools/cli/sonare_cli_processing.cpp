#include <cctype>

#include "c_api/sonare_c_error_mapping.h"
#include "sonare_cli.h"
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_target.h"
#include "midi/note_targets.h"
#endif

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

// Every command below up to the #else stays registered in the CLI's command
// table regardless of BUILD_PITCH_EDITOR (see get_commands() in
// tools/cli/sonare_cli.cpp), so a build without the pitch editor must still
// answer the subcommand -- with a NotImplemented diagnostic mapped to the CLI's
// not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_PITCH_EDITOR)

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

namespace {

// The MIDI value this command reads is a positional, not an option, so the
// registry's option domains cannot refuse it. An absent or unreadable one is
// the usage failure the Python parser reports for the same argument, which is
// why it travels as CliUsageError rather than as a handler status.
double required_positional_number(const CliArgs& args, const char* name) {
  if (args.positionals.empty()) {
    throw CliUsageError(std::string("Missing required argument '") + name + "'");
  }
  const std::string& text = args.positionals.front();
  const CliUsageError rejected(std::string("argument ") + name + ": must be a finite number");
  size_t consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &consumed);
  } catch (const std::exception&) {
    throw rejected;
  }
  // stod stops at the first character it cannot read, so a trailing tail is a
  // value the caller misspelled rather than one this accepts a prefix of.
  if (consumed != text.size() || !std::isfinite(value)) throw rejected;
  return value;
}

}  // namespace

int cmd_scale_quantize(const CliArgs& args, const Audio&) {
  const double midi = required_positional_number(args, "midi");
  editing::pitch_editor::ScaleQuantizerConfig config;
  config.root = args.get_int("root", 0);
  config.mode_mask = static_cast<uint16_t>(args.get_int("mode-mask", 0xAB5));
  // Zero is the sentinel for "use the library default", which is the anchor the
  // config already carries. The C ABI applies it and the Python CLI inherits it
  // from there, so a `--reference-midi 0` that anchored the grid at MIDI 0 here
  // would answer a different pitch for the same command line.
  const float reference_midi = args.get_float("reference-midi", 69.0f);
  if (reference_midi != 0.0f) config.reference_midi = reference_midi;

  const editing::pitch_editor::ScaleQuantizer quantizer(config);
  const float quantized = quantizer.quantize_midi(static_cast<float>(midi));

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("input_midi", midi)
        .kv("quantized_midi", quantized)
        .end_object()
        .print();
  } else {
    std::cout << std::fixed << std::setprecision(6) << quantized << "\n";
  }
  return 0;
}

int cmd_pitch_correct_timevarying(const CliArgs& args, const Audio& audio) {
  // The contour comes from pYIN at the requested hop and otherwise library
  // defaults, which is the track the Python CLI measures for the same command.
  PitchConfig pitch_config;
  pitch_config.hop_length = args.get_int("hop-length", 512);
  const editing::pitch_editor::F0Track track =
      editing::pitch_editor::PyinF0Provider(pitch_config).detect(audio);

  editing::pitch_editor::PitchCorrectionConfig config;
  config.scale.root = args.get_int("scale-root", 0);
  config.scale.mode_mask = static_cast<uint16_t>(args.get_int("scale-mode-mask", 0xAB5));
  // Assigned as given, with no zero-is-default sentinel: this path validates the
  // anchor for finiteness only, so an explicit 0 anchors the grid at MIDI 0.
  config.scale.reference_midi = args.get_float("reference-midi", 69.0f);

  const editing::pitch_editor::PitchCorrector corrector(config);
  const bool to_scale = args.get_string("mode", "midi") == "scale";
  const Audio result = to_scale ? corrector.correct_to_scale_timevarying(audio, track)
                                : corrector.correct_to_midi_timevarying(
                                      audio, track, args.get_float("target-midi", 69.0f));
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .kv("output", args.output_file)
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_note_move(const CliArgs& args, const Audio& audio) {
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = args.get_int("onset", 0);
  // An absent --offset means the rest of the buffer. The editor reads no
  // sentinel for it, so the resolution happens here, as it does on the other
  // front-end, rather than arriving as a zero-length region.
  region.offset_sample =
      args.has("offset") ? args.get_int("offset", 0) : static_cast<int>(audio.size());

  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.move_note(audio, region, args.get_int("target-onset", 0));
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .kv("output", args.output_file)
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

int cmd_pitch_correct(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_scale_quantize(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_pitch_correct_timevarying(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_note_move(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_note_stretch(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_polyphonic_notes(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

int cmd_polyphonic_render(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "pitch editor support is not compiled in");
}

#endif  // SONARE_WITH_PITCH_EDITOR

// `tune-to-midi in.wav --reference-smf ref.mid -o out.wav` -- the take follows a
// written melody instead of a single stated interval. Unlike the family above,
// this one is absent from the command table when either gate is off rather than
// answering with a NotImplemented diagnostic: the SMF reader lives in the
// arrangement library, which an arrangement-off build does not link at all, so it
// takes the registration shape the other arrangement commands take.
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)

namespace {

// The three policy names, in the order the help, the published domain and the
// refusal all name them. The registry's choices domain refuses every other
// spelling before dispatch, so the throw below is unreachable from a command
// line and is here to keep the mapping total.
editing::note_model::UnmatchedTargetPolicy parse_unmatched_policy(const std::string& name) {
  using editing::note_model::UnmatchedTargetPolicy;
  if (name == "leave") return UnmatchedTargetPolicy::Leave;
  if (name == "mute") return UnmatchedTargetPolicy::Mute;
  if (name == "nearest") return UnmatchedTargetPolicy::Nearest;
  throw std::invalid_argument("invalid value for --unmatched-policy: " + name);
}

}  // namespace

int cmd_tune_to_midi(const CliArgs& args, const Audio& audio) {
  const std::string reference_path = args.get_string("reference-smf");
  std::vector<uint8_t> smf;
  if (!read_binary_file(reference_path, &smf)) {
    throw sonare::SonareException(sonare::ErrorCode::FileNotFound,
                                  "cannot open reference SMF: " + reference_path);
  }

  // Only what the caller spelled is written: 0 is a legal value for both bounds,
  // so neither can double as the sentinel that asks for the core default.
  editing::note_model::NoteTargetAssignConfig config;
  config.unmatched_policy = parse_unmatched_policy(args.get_string("unmatched-policy", "leave"));
  if (args.has("min-overlap-ratio")) {
    config.min_overlap_ratio = args.get_float("min-overlap-ratio", config.min_overlap_ratio);
  }
  if (args.has("max-correction-semitones")) {
    config.max_correction_semitones =
        args.get_float("max-correction-semitones", config.max_correction_semitones);
  }

  // The reference is resolved before the analysis, which is what the command
  // costs: a malformed file and a --track naming no MIDI-bearing track are both
  // answerable from the arguments alone, so neither is worth a pitch track first.
  // The other front-end reads it in the same place, so the two cannot report a
  // different failure for one command line.
  const std::vector<editing::note_model::NoteTarget> targets =
      midi::note_targets_from_smf(smf.data(), smf.size(), args.get_int("track", 0));

  // The command advertises no analysis geometry, so the contour is pYIN at the
  // library's own defaults -- the track the Python CLI measures for it.
  const editing::pitch_editor::F0Track track =
      editing::pitch_editor::PyinF0Provider().detect(audio);
  std::vector<editing::note_model::NoteObject> notes =
      editing::note_model::extract_notes(audio, track);
  const size_t assigned =
      editing::note_model::assign_note_targets(notes, audio.sample_rate(), targets, config);

  const Audio result = editing::note_model::render_notes(audio, notes);
  save_wav(args.output_file, result.data(), result.size(), result.sample_rate());

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        // Zero assigned is a legitimate answer -- a reference that does not line
        // up with the take -- so it is reported rather than raised.
        .kv("assigned_count", assigned)
        .kv("note_count", notes.size())
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

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_PITCH_EDITOR

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
    // The warp resolves the factor into its own range, and the changer leaves the
    // dry/wet amount at its default, so this is the value the audio was made with.
    formant_factor = effective_formant_factor(formant_factor, FormantWarpConfig{}.amount);
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
      // result was made with, so a JSON consumer reading the formant factor back
      // gets the value that shaped the audio rather than the one that was asked for.
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

int cmd_decompose_stems(const CliArgs& args, const Audio& audio) {
  DecomposeStemsConfig config;
  config.n_components = args.get_int("n-components", 4);
  config.n_fft = args.n_fft;
  config.hop_length = args.hop_length;
  config.n_iter = args.get_int("n-iter", 100);
  config.beta = args.get_float("beta", 2.0f);
  config.init = args.get_string("init", "random");
  config.mask_power = args.get_float("mask-power", 1.0f);

  if (!args.quiet) {
    std::cerr << color::blue << "Decomposing into components..." << color::reset << "\n";
  }

  const DecomposeStemsResult result =
      decompose_stems(audio.data(), audio.size(), audio.sample_rate(), config);

  // Same base-name shape hpss uses: the destination names the set rather than a
  // single file, and the suffix is stripped case-insensitively so `-o out.WAV`
  // and `-o out.wav` name the same set.
  std::string base = args.output_file;
  if (base.size() > 4) {
    std::string suffix = base.substr(base.size() - 4);
    for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (suffix == ".wav") base = base.substr(0, base.size() - 4);
  }

  // Mean absolute amplitude, the level summary both CLIs publish per component.
  auto component_energy = [](const std::vector<float>& component) {
    if (component.empty()) return 0.0f;
    float total = 0.0f;
    for (float sample : component) total += std::fabs(sample);
    return total / static_cast<float>(component.size());
  };

  std::vector<std::string> paths;
  std::vector<float> energies;
  paths.reserve(result.components.size());
  energies.reserve(result.components.size());
  for (size_t index = 0; index < result.components.size(); ++index) {
    const std::vector<float>& component = result.components[index];
    std::string path = base + "_component" + std::to_string(index + 1) + ".wav";
    save_wav(path, component.data(), component.size(), audio.sample_rate());
    paths.push_back(std::move(path));
    energies.push_back(component_energy(component));
  }

  if (args.json_output) {
    JsonBuilder builder;
    builder.begin_object()
        .kv("count", paths.size())
        .kv("length", result.components.empty() ? size_t{0} : result.components.front().size())
        .kv("sample_rate", audio.sample_rate())
        .key("energies")
        .begin_array();
    for (float energy : energies) builder.value(energy);
    builder.end_array().key("components").begin_array();
    for (const std::string& path : paths) builder.value(path);
    builder.end_array().end_object().print();
  } else {
    std::cout << "  Stems: " << paths.size() << " components\n";
    for (size_t index = 0; index < paths.size(); ++index) {
      std::cout << "    " << std::setw(2) << (index + 1) << ". energy " << std::fixed
                << std::setprecision(6) << energies[index] << std::defaultfloat << "  "
                << paths[index] << "\n";
    }
    if (!paths.empty()) {
      std::cout << "  Wrote: ";
      for (size_t index = 0; index < paths.size(); ++index) {
        if (index != 0) std::cout << ", ";
        std::cout << paths[index];
      }
      std::cout << "\n";
    }
  }
  return 0;
}

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

namespace {

// The takes `split-silence` measures together: the positional first, then every
// --input in command-line order.
std::vector<std::vector<float>> load_split_silence_takes(const CliArgs& args, const Audio& audio) {
  std::vector<std::vector<float>> takes;
  const std::vector<std::string> paths = args.get_string_list("input");
  takes.reserve(paths.size() + 1);
  takes.emplace_back(audio.begin(), audio.end());
  for (const std::string& path : paths) {
    // Through Audio::from_file, as main() loads the positional, so every take
    // meets the same offline-input policy.
    Audio take = Audio::from_file(path);
    // Frame indices from two rates are not comparable, so a mismatch would
    // report intervals in units the caller cannot map back onto either take.
    if (take.sample_rate() != audio.sample_rate()) {
      throw std::invalid_argument("take sample rate differs: " + path + " is " +
                                  std::to_string(take.sample_rate()) + " Hz, the first take is " +
                                  std::to_string(audio.sample_rate()) + " Hz");
    }
    takes.emplace_back(take.begin(), take.end());
  }
  return takes;
}

// Writes every take sliced at every interval as `{prefix}{take}_{interval}.wav`,
// both indices 1-based and zero-padded. A take that ended before an interval is
// silent there, which is the rule the union was built on, so its slice is padded
// rather than shortened and every take's file for one interval is the same
// length.
void write_split_silence_takes(const std::string& prefix,
                               const std::vector<std::vector<float>>& takes,
                               const std::vector<std::pair<int, int>>& ranges, int sample_rate) {
  for (size_t take = 0; take < takes.size(); ++take) {
    for (size_t interval = 0; interval < ranges.size(); ++interval) {
      const size_t start = static_cast<size_t>(ranges[interval].first);
      const size_t end = static_cast<size_t>(ranges[interval].second);
      std::vector<float> slice(end - start, 0.0f);
      // Guarded rather than clamped to a zero count: a take that ends before the
      // interval starts is the documented unequal-length case, and `begin() +
      // start` past the end is undefined even when nothing is copied from it.
      if (start < takes[take].size()) {
        const size_t available = takes[take].size() - start;
        std::copy_n(takes[take].begin() + static_cast<std::ptrdiff_t>(start),
                    std::min(available, slice.size()), slice.begin());
      }
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), "%02zu_%03zu.wav", take + 1, interval + 1);
      save_wav(prefix + suffix, slice, sample_rate);
    }
  }
}

}  // namespace

int cmd_split_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", 60.0f);
  const std::vector<std::vector<float>> takes = load_split_silence_takes(args, audio);

  std::vector<const float*> signals;
  std::vector<size_t> lengths;
  signals.reserve(takes.size());
  lengths.reserve(takes.size());
  for (const std::vector<float>& take : takes) {
    signals.push_back(take.data());
    lengths.push_back(take.size());
  }

  // The union across the takes, from the one implementation every other surface
  // calls. A single take answers exactly as sonare_split_silence does.
  int* flat = nullptr;
  size_t flat_count = 0;
  // Always asked for, only sometimes printed: the report costs nothing here and
  // taking it unconditionally keeps one call site rather than two that could
  // refuse differently.
  SonareSilenceCommonReport report{};
  const SonareError err =
      sonare_split_silence_common_ex(signals.data(), signals.size(), lengths.data(), top_db,
                                     args.n_fft, args.hop_length, &flat, &flat_count, &report);
  if (err != SONARE_OK) {
    // Mapped rather than raised as a plain runtime error, so the failure keeps
    // the class it carries out to the exit code the two front-ends publish for
    // it. The CLI refuses an unreadable or empty take before this point, so this
    // is the defensive branch; a divergence here would be found by nothing.
    const char* message = sonare_error_message(err);
    throw sonare::SonareException(sonare_c_detail::error_code_from_c_error(err),
                                  message != nullptr ? message : "split-silence failed");
  }
  std::vector<std::pair<int, int>> ranges;
  ranges.reserve(flat_count / 2);
  for (size_t i = 0; i + 1 < flat_count; i += 2) ranges.emplace_back(flat[i], flat[i + 1]);
  sonare_free_ints(flat);

  const std::string write_takes = args.get_string("write-takes");
  if (!write_takes.empty()) {
    write_split_silence_takes(write_takes, takes, ranges, audio.sample_rate());
  }

  const bool with_report = args.has("report");
  const auto append_intervals = [&ranges](JsonBuilder& json) {
    json.begin_array();
    for (const auto& range : ranges) {
      json.begin_object()
          .kv("start_sample", range.first)
          .kv("end_sample", range.second)
          .end_object();
    }
    json.end_array();
  };
  if (args.json_output) {
    JsonBuilder json;
    if (with_report) {
      json.begin_object().key("intervals");
      append_intervals(json);
      json.key("report")
          .begin_object()
          .kv("silence_ceiling_db", report.silence_ceiling_db)
          .kv("max_signal_intervals", report.max_signal_intervals)
          .kv("min_signal_intervals", report.min_signal_intervals)
          .end_object()
          .end_object();
    } else {
      append_intervals(json);
    }
    json.print();
  } else {
    std::cout << "Non-silent intervals: " << ranges.size() << "\n";
    for (const auto& range : ranges) {
      printf("  %d - %d\n", range.first, range.second);
    }
    if (with_report) {
      // Read against the --top-db in use, which is the whole rule: a ceiling
      // under it means the threshold was too loose for the quiet these takes
      // have, and one at or over it means the quiet is there and they do not
      // share it.
      printf("  silence ceiling %.2f dB at --top-db %.2f\n", report.silence_ceiling_db,
             static_cast<double>(top_db));
      printf("  intervals per signal: %d..%d\n", report.min_signal_intervals,
             report.max_signal_intervals);
    }
    if (!write_takes.empty()) {
      std::cout << "Wrote " << takes.size() * ranges.size() << " take files with prefix "
                << write_takes << "\n";
    }
  }
  return 0;
}
