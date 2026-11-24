#include <sonare/sonare_c_effects.h>
#include <sonare/sonare_c_vocal_edit.h>

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "analysis/analysis_rate.h"
#include "c_api/sonare_c_error_mapping.h"
#include "sonare_cli.h"
#include "util/refusal_predicates.h"
#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_target.h"
#include "midi/note_targets.h"
#endif

// Offline-effect output contract: a command that renders an audio buffer
// requires -o/--output. Running it without a destination has no useful result
// (the render would be computed and thrown away), so the missing-output case is
// refused before dispatch, not a silent no-op.
// trim-silence is the deliberate exception: it doubles as an analysis command
// (it reports the trimmed length), so its output stays optional.
//
// The requirement is declared once, in the CLI registry (required_output()),
// and enforced by validate_cli_arguments before dispatch. A handler below
// therefore never re-checks it: when one did, the same missing option produced
// a different message and a different exit code depending on which command the
// caller happened to run.

int cmd_pitch_shift(const CliArgs& args, const Audio& audio) {
  float semitones = args.get_float("pitch-semitones", 0.0f);
  PitchShiftConfig config;
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);

  if (!args.quiet) {
    std::cerr << color::blue << "Pitch shifting by " << semitones << " semitones..." << color::reset
              << "\n";
  }

  Audio result = pitch_shift(audio, semitones, config);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .kv("pitch_semitones", semitones)
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .end_object()
        .print();
  }
  return 0;
}

int cmd_time_stretch(const CliArgs& args, const Audio& audio) {
  float rate = args.get_float("rate", 1.0f);
  TimeStretchConfig config;
  config.n_fft = args.get_int("n-fft", config.n_fft);
  config.hop_length = args.get_int("hop-length", config.hop_length);

  if (!args.quiet) {
    std::cerr << color::blue << "Time stretching with rate " << rate << "..." << color::reset
              << "\n";
  }

  Audio result = time_stretch(audio, rate, config);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
// answer the subcommand -- with a NotSupported diagnostic mapped to the CLI's
// not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_PITCH_EDITOR)

int cmd_pitch_correct(const CliArgs& args, const Audio& audio) {
  // Both pitches are required options (the registry refuses an absent one), as in the library.
  const float current_midi = args.get_float("current-midi", 0.0f);
  const float target_midi = args.get_float("target-midi", 0.0f);
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
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
  const std::string subject = std::string("argument ") + name + ":";
  try {
    // Held to the float range the quantizer reads, as an option value is; the
    // payload echoes the double the caller wrote.
    (void)parse_float_text(subject, text);
    return parse_double_text(subject, text);
  } catch (const std::invalid_argument& error) {
    throw CliUsageError(error.what());
  }
}

// The mask arrives as an integer or a mode name; the registry has already
// checked both spellings and the 12-bit range, so this only resolves it.
int mode_mask_option(const CliArgs& args, const char* name) {
  return args.has(name) ? parse_mode_mask_option(name, args.get_string(name))
                        : args.get_int(name, 0xAB5);
}

}  // namespace

int cmd_scale_quantize(const CliArgs& args, const Audio&) {
  const double midi = required_positional_number(args, "midi");
  editing::pitch_editor::ScaleQuantizerConfig config;
  config.root = args.get_int("root", 0);
  config.mode_mask = static_cast<uint16_t>(mode_mask_option(args, "mode-mask"));
  // An absent flag is the registry default (69) and a given value, 0 included, is the anchor.
  config.reference_midi = args.get_float("reference-midi", 69.0f);

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
  pitch_config.hop_length = args.get_int("hop-length", pitch_config.hop_length);
  // The pYIN frame is 2048 samples at 22050 Hz, held as a duration.
  pitch_config.frame_length = window_length_at_rate(pitch_config.frame_length, audio.sample_rate());
  const editing::pitch_editor::F0Track track =
      editing::pitch_editor::pyin_f0_track(audio, pitch_config);

  editing::pitch_editor::PitchCorrectionConfig config;
  config.scale.root = args.get_int("scale-root", config.scale.root);
  if (args.has("scale-mode-mask")) {
    config.scale.mode_mask = static_cast<uint16_t>(mode_mask_option(args, "scale-mode-mask"));
  }
  // Assigned as given, with no zero-is-default sentinel: this path validates the
  // anchor for finiteness only, so an explicit 0 anchors the grid at MIDI 0.
  config.scale.reference_midi = args.get_float("reference-midi", config.scale.reference_midi);
  // The registry holds the library defaults and the ranges the correction
  // config refuses outside of, so these are read as given.
  config.retune_amount = args.get_float("retune-amount", config.retune_amount);
  config.max_correction_semitones =
      args.get_float("max-correction-semitones", config.max_correction_semitones);
  config.retune_speed_ms = args.get_float("retune-speed-ms", config.retune_speed_ms);
  config.vibrato_threshold_cents =
      args.get_float("vibrato-threshold-cents", config.vibrato_threshold_cents);

  const editing::pitch_editor::PitchCorrector corrector(config);
  const bool to_scale = args.get_string("mode", "midi") == "scale";
  const Audio result = to_scale ? corrector.correct_to_scale_timevarying(audio, track)
                                : corrector.correct_to_midi_timevarying(
                                      audio, track, args.get_float("target-midi", 69.0f));
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

// Resolves a position the caller gave as `--<bound>-sample` or `--<bound>-sec`
// (the registry refuses both at once and a negative value), rounding seconds to
// the nearest sample as the library does. Absent means `fallback`.
static int note_bound_sample(const CliArgs& args, const std::string& bound, int sample_rate,
                             int fallback) {
  const std::string sec = bound + "-sec";
  if (args.has(sec)) {
    const double samples = std::floor(args.get_double(sec, 0.0) * sample_rate + 0.5);
    if (samples > static_cast<double>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument("--" + sec + " is out of range at " +
                                  std::to_string(sample_rate) + " Hz");
    }
    return static_cast<int>(samples);
  }
  const std::string sample = bound + "-sample";
  return args.has(sample) ? args.get_int(sample, 0) : fallback;
}

int cmd_note_move(const CliArgs& args, const Audio& audio) {
  const int sample_rate = audio.sample_rate();
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = note_bound_sample(args, "onset", sample_rate, 0);
  region.offset_sample =
      note_bound_sample(args, "offset", sample_rate, static_cast<int>(audio.size()));
  const int target_onset = note_bound_sample(args, "target-onset", sample_rate, 0);

  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.move_note(audio, region, target_onset);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("length", result.size())
        .kv("sample_rate", result.sample_rate())
        .kv("duration", result.duration())
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
        .end_object()
        .print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_note_stretch(const CliArgs& args, const Audio& audio) {
  const int sample_rate = audio.sample_rate();
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = note_bound_sample(args, "onset", sample_rate, 0);
  region.offset_sample =
      note_bound_sample(args, "offset", sample_rate, static_cast<int>(audio.size()));
  const float ratio = args.get_float("ratio", 1.0f);

  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, ratio);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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

// Applies one assignment parse_polyphonic_edit has already read; only the note range is left to
// check, because it depends on what the analysis found.
void apply_polyphonic_note_edit(std::vector<editing::note_model::NoteObject>& notes,
                                const PolyphonicEdit& parsed, const std::string& path) {
  const std::string subject = cli_bag_subject("--edit", path);
  if (notes.empty()) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            "the analysis found no notes")
        .as_caller_spelled();
  }
  if (parsed.note < 0 || static_cast<size_t>(parsed.note) >= notes.size()) {
    throw SonareParameterRefusal::predicate(
              ErrorCode::InvalidParameter, subject,
              "must name a note index in [0, " + std::to_string(notes.size() - 1) + "]")
        .as_caller_spelled();
  }
  auto& edit = notes[static_cast<size_t>(parsed.note)].edit;
  const std::string& field = parsed.field;
  if (field == "pitch_shift_semitones") {
    edit.pitch_shift_semitones = parsed.number;
  } else if (field == "gain_db") {
    edit.gain_db = parsed.number;
  } else if (field == "time_offset_samples") {
    edit.time_offset_samples = parsed.integer;
  } else if (field == "time_stretch_ratio") {
    edit.time_stretch_ratio = parsed.number;
  } else if (field == "formant_shift_semitones") {
    edit.formant_shift_semitones = parsed.number;
  } else if (field == "vibrato_depth_change") {
    edit.vibrato_depth_change = parsed.number;
  } else if (field == "drift_change") {
    edit.drift_change = parsed.number;
  } else {
    edit.muted = parsed.flag;
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
          .kv("frame_end", note.frame_end);
      // A note with no usable pitch carries a median of 0, which is not a frequency.
      if (note.median_hz > 0.0f) json.kv("median_hz", note.median_hz);
      json.kv("median_cents", note.median_cents)
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
      printf("  [%zu] samples %lld-%lld  frames %d-%d  ", i,
             static_cast<long long>(note.onset_sample), static_cast<long long>(note.offset_sample),
             note.frame_start, note.frame_end);
      if (note.median_hz > 0.0f) {
        printf("%.2f Hz", note.median_hz);
      } else {
        printf("-");
      }
      printf("  stability %.3f\n", note.f0_stability);
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
  std::map<std::string, std::string> caller_paths;
  for (const auto& assignment : assignments) {
    const PolyphonicEdit parsed = parse_polyphonic_edit(assignment);
    const std::string path = assignment.substr(0, assignment.find('='));
    caller_paths["analysis.notes[" + std::to_string(parsed.note) + "].edit." + parsed.field] =
        path;
    apply_polyphonic_note_edit(analysis.notes, parsed, path);
  }

  const Audio result = [&] {
    try {
      return editing::polyphony::render_polyphonic(analysis);
    } catch (const SonareParameterRefusal& refusal) {
      const auto shown = refusal.respelled(
          refusal.code(),
          [&](const std::string& subject) {
            const auto it = caller_paths.find(subject);
            return it != caller_paths.end() ? cli_bag_subject("--edit", it->second) : subject;
          },
          true);
      throw shown;
    }
  }();
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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

namespace {

// The nearest equal-tempered note of a pitch given as a fractional MIDI number,
// with the signed distance to it in cents. Ties round up, as the Python CLI does.
struct NearestNote {
  std::string name;
  double cents;
};

NearestNote nearest_note(double midi) {
  const long nearest = static_cast<long>(std::floor(midi + 0.5));
  const long pitch_class = ((nearest % 12) + 12) % 12;
  const long octave = (nearest - pitch_class) / 12 - 1;
  return {std::string(sonare::pitch_class_name(static_cast<sonare::PitchClass>(pitch_class))) +
              std::to_string(octave),
          (midi - static_cast<double>(nearest)) * 100.0};
}

[[noreturn]] void throw_c_error(SonareError err, const char* what) {
  const char* message = sonare_error_message(err);
  throw sonare::SonareException(sonare_c_detail::error_code_from_c_error(err),
                                message != nullptr ? message : what);
}

}  // namespace

// A read-only view of what a vocal-edit session finds in a take: the session is
// created with the analysis settings the caller spelled, its notes are read, and
// nothing is edited or rendered. An unspelled setting is left at the session's
// own default rather than restated here.
int cmd_vocal_notes(const CliArgs& args, const Audio& audio) {
  SonareVocalCreateOptions options;
  sonare_vocal_create_options_init(&options);
  if (args.has("segmentation-threshold-cents"))
    options.segmentation_threshold_cents = args.get_double("segmentation-threshold-cents", 0.0);
  if (args.has("min-note-ms")) options.min_note_ms = args.get_double("min-note-ms", 0.0);
  if (args.has("frame-length-samples"))
    options.frame_length_samples = args.get_uint32("frame-length-samples", 0);
  if (args.has("hop-length-samples"))
    options.hop_length_samples = args.get_uint32("hop-length-samples", 0);
  if (args.has("f0-min-hz")) options.fmin_hz = args.get_double("f0-min-hz", 0.0);
  if (args.has("f0-max-hz")) options.fmax_hz = args.get_double("f0-max-hz", 0.0);
  if (args.has("yin-threshold")) options.yin_threshold = args.get_double("yin-threshold", 0.0);
  if (args.has("voiced-threshold"))
    options.voiced_threshold = args.get_double("voiced-threshold", 0.0);
  if (args.has("no-centered")) options.centered = 0;
  if (args.has("reference-hz")) options.reference_hz = args.get_double("reference-hz", 0.0);

  if (!args.quiet && !args.json_output) {
    std::cerr << color::blue << "Analyzing vocal notes..." << color::reset << "\n";
  }
  SonareVocalSession* raw_session = nullptr;
  const SonareError created =
      sonare_vocal_session_create(audio.data(), static_cast<int64_t>(audio.size()), 1,
                                  audio.sample_rate(), &options, &raw_session);
  std::unique_ptr<SonareVocalSession, decltype(&sonare_vocal_session_destroy)> session(
      raw_session, &sonare_vocal_session_destroy);
  if (created != SONARE_OK) throw_c_error(created, "vocal session creation failed");

  SonareVocalNotesResult notes;
  sonare_vocal_notes_result_init(&notes);
  const SonareError read = sonare_vocal_session_notes(session.get(), &notes);
  std::unique_ptr<SonareVocalNotesResult, decltype(&sonare_free_vocal_notes_result)> owned(
      &notes, &sonare_free_vocal_notes_result);
  if (read != SONARE_OK) throw_c_error(read, "vocal note read failed");

  const int sample_rate = audio.sample_rate();
  const size_t note_count = static_cast<size_t>(notes.note_count);
  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("sample_rate", sample_rate)
        .kv("note_count", note_count)
        .key("notes")
        .begin_array();
    for (size_t i = 0; i < note_count; ++i) {
      const SonareVocalNote& note = notes.notes[i];
      json.begin_object()
          .kv("index", i)
          .kv("id", static_cast<size_t>(note.id))
          .kv("source_start_sample", static_cast<size_t>(note.source_start_sample))
          .kv("source_end_sample", static_cast<size_t>(note.source_end_sample))
          .kv("start_sec", static_cast<double>(note.source_start_sample) / sample_rate)
          .kv("end_sec", static_cast<double>(note.source_end_sample) / sample_rate)
          .kv("analysis_frame_start", static_cast<size_t>(note.analysis_frame_start))
          .kv("analysis_frame_end", static_cast<size_t>(note.analysis_frame_end));
      // An unpitched note has no pitch to report, so its pitch keys are omitted.
      if (note.has_pitch != 0) {
        const NearestNote nearest = nearest_note(note.center_midi);
        json.kv("median_hz", note.median_hz)
            .kv("center_midi", note.center_midi)
            .kv("f0_stability", note.f0_stability)
            .kv("note_name", nearest.name)
            .kv("cents", nearest.cents);
      }
      std::vector<float> amplitude(note.amplitude, note.amplitude + note.amplitude_count);
      json.key("amplitude").float_array(amplitude).end_object();
    }
    json.end_array().end_object().print();
  } else {
    std::cout << "Vocal notes: " << note_count << "\n";
    for (size_t i = 0; i < note_count; ++i) {
      const SonareVocalNote& note = notes.notes[i];
      printf("  [%zu] %.3f-%.3f s  ", i,
             static_cast<double>(note.source_start_sample) / sample_rate,
             static_cast<double>(note.source_end_sample) / sample_rate);
      if (note.has_pitch != 0) {
        const NearestNote nearest = nearest_note(note.center_midi);
        printf("%-4s %+6.1f cents  midi %.2f  %.2f Hz  stability %.3f\n", nearest.name.c_str(),
               nearest.cents, note.center_midi, note.median_hz, note.f0_stability);
      } else {
        printf("unpitched\n");
      }
    }
  }
  return 0;
}

#else  // !SONARE_WITH_PITCH_EDITOR

int cmd_pitch_correct(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_scale_quantize(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_pitch_correct_timevarying(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_note_move(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_note_stretch(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_polyphonic_notes(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_polyphonic_render(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

int cmd_vocal_notes(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "pitch editor support is not compiled in");
}

#endif  // SONARE_WITH_PITCH_EDITOR

// `tune-to-midi in.wav --reference-smf ref.mid -o out.wav` -- the take follows a
// written melody instead of a single stated interval. Unlike the family above,
// this one is absent from the command table when either gate is off rather than
// answering with a NotSupported diagnostic: the SMF reader lives in the
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
  throw std::invalid_argument("argument --unmatched-policy: must be one of leave, mute, nearest");
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
  if (args.has("unmatched-policy")) {
    config.unmatched_policy = parse_unmatched_policy(args.get_string("unmatched-policy"));
  }
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
  PitchConfig pitch_config;
  pitch_config.frame_length = window_length_at_rate(pitch_config.frame_length, audio.sample_rate());
  const editing::pitch_editor::F0Track track =
      editing::pitch_editor::pyin_f0_track(audio, pitch_config);
  std::vector<editing::note_model::NoteObject> notes =
      editing::note_model::extract_notes(audio, track);
  const size_t assigned =
      editing::note_model::assign_note_targets(notes, audio.sample_rate(), targets, config);

  const Audio result = editing::note_model::render_notes(audio, notes);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
// answer the subcommand -- with a NotSupported diagnostic mapped to the
// CLI's not-supported exit code -- instead of failing to link.
#if defined(SONARE_WITH_VOICE_CHANGER)

int cmd_voice_change(const CliArgs& args, const Audio& audio) {
  // The selector relations are validate_voice_change's, decided before the input is read.
  const bool has_preset = args.has("preset");
  const bool has_preset_pack = args.has("preset-pack");
  const bool uses_realtime_preset = has_preset || args.has("preset-json") || args.has("set");

  Audio result;
  std::string preset_id;
  int latency_samples = 0;
  float pitch_semitones = 0.0f;
  float formant_factor = 1.0f;
  editing::voice_changer::FormantMode formant_mode = editing::voice_changer::FormantMode::Relative;
  if (uses_realtime_preset) {
    // A built-in preset is named by its library id in any letter case, and the library refuses
    // an unknown one; a pack entry keeps the id its author gave it.
    const std::string requested_preset =
        has_preset && !has_preset_pack
            ? catalog_token(args.get_string("preset"),
                            editing::voice_changer::realtime_voice_changer_preset_names())
            : args.get_string("preset", "");
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
    } else {
      // A built-in id is a parameter, so an unknown or empty one is refused as one.
      const auto id =
          editing::voice_changer::realtime_voice_changer_preset_from_id(requested_preset);
      config_text = editing::voice_changer::realtime_voice_changer_preset_json(id);
    }
    if (args.has("set"))
      config_text = apply_voice_preset_sets(config_text, args.get_string_list("set"));

    // The library's one preset reader, as the C ABI and Python use it: an unknown,
    // missing or duplicate key and any value outside its domain are refused, so a
    // hand-written preset never renders with defaults or a clamped value.
    editing::voice_changer::RealtimeVoiceChangerConfig config;
    std::string config_error;
    if (!editing::voice_changer::realtime_voice_changer_config_from_input(config_text, &config,
                                                                          &config_error)) {
      // A refused preset document is a format error, as the C ABI reports it; the reader's
      // text already names the key from the document root.
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, config_error);
    }
    // The library runs the latency-compensated one-shot, so the CLI keeps no block loop of its
    // own; the probe handle is prepared at the library's default block size.
    SonareRealtimeVoiceChangerConfig c_config;
    const SonareError parse_err =
        sonare_realtime_voice_changer_config_from_json(config_text.c_str(), &c_config);
    if (parse_err != SONARE_OK) throw_c_error(parse_err, "voice changer config failed");
    SonareRealtimeVoiceChanger* raw_probe = nullptr;
    const SonareError probe_err =
        sonare_realtime_voice_changer_create(&c_config, audio.sample_rate(), 0, 1, &raw_probe);
    if (probe_err != SONARE_OK) throw_c_error(probe_err, "voice changer creation failed");
    std::unique_ptr<SonareRealtimeVoiceChanger, decltype(&sonare_realtime_voice_changer_destroy)>
        probe(raw_probe, sonare_realtime_voice_changer_destroy);
    const SonareError latency_err =
        sonare_realtime_voice_changer_latency_samples(probe.get(), &latency_samples);
    if (latency_err != SONARE_OK) throw_c_error(latency_err, "voice changer latency read failed");
    latency_samples = std::max(latency_samples, 0);

    float* raw_output = nullptr;
    size_t output_length = 0;
    const SonareError render_err = sonare_realtime_voice_changer_render(
        audio.data(), audio.size(), audio.sample_rate(), &c_config, 1, &raw_output, &output_length);
    if (render_err != SONARE_OK) throw_c_error(render_err, "voice change failed");
    std::unique_ptr<float, decltype(&sonare_free_floats)> output(raw_output, sonare_free_floats);
    result = Audio::from_vector(std::vector<float>(raw_output, raw_output + output_length),
                                audio.sample_rate());
  } else {
    pitch_semitones = args.get_float("pitch-semitones", 0.0f);
    formant_factor = args.get_float("formant-factor", 1.0f);
    editing::voice_changer::VoiceChangerConfig config;
    config.pitch_semitones = pitch_semitones;
    config.formant_factor = formant_factor;
    formant_mode =
        editing::voice_changer::parse_formant_mode(args.get_string("formant-mode", "relative"));
    config.formant_mode = formant_mode;
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
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result, 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder json;
    json.begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
      json.kv("pitch_semitones", pitch_semitones)
          .kv("formant_factor", formant_factor)
          .kv("formant_mode", editing::voice_changer::formant_mode_name(formant_mode));
    }
    json.end_object().print();
  } else if (!args.quiet) {
    std::cerr << color::green << "Saved to " << args.output_file << color::reset << "\n";
  }
  return 0;
}

int cmd_voice_presets(const CliArgs& args, const Audio&) {
  return print_name_catalog(args, "presets",
                            editing::voice_changer::realtime_voice_changer_preset_names());
}

int cmd_voice_preset(const CliArgs& args, const Audio&) {
  const std::string preset =
      catalog_token(args.get_string("preset", "neutralMonitor"),
                    editing::voice_changer::realtime_voice_changer_preset_names());
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
  // FileNotFound so it maps to exit 4 and never emits a JSON validation
  // envelope.
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
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "voice changer support is not compiled in");
}

int cmd_voice_presets(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "voice changer support is not compiled in");
}

int cmd_voice_preset_validate(const CliArgs&, const Audio&) {
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "voice changer support is not compiled in");
}

#endif  // SONARE_WITH_VOICE_CHANGER

int cmd_decompose_stems(const CliArgs& args, const Audio& audio) {
  DecomposeStemsConfig config;
  config.n_components = args.get_int("n-components", config.n_components);
  config.n_fft = args.get_int("n-fft", config.n_fft);
  // The core config spells "a quarter of the FFT size" as a zero hop, so only an absent flag may
  // send it; the registry domain refuses an explicit zero.
  if (args.has("hop-length")) config.hop_length = args.get_int("hop-length", 0);
  config.n_iter = args.get_int("n-iter", config.n_iter);
  config.beta = args.get_float("beta", config.beta);
  config.init = args.get_string("init", config.init);
  config.mask_power = args.get_float("mask-power", config.mask_power);

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
    const std::string suffix = lowercase_cli_value(base.substr(base.size() - 4));
    if (suffix == ".wav") base = base.substr(0, base.size() - 4);
  }

  // Mean absolute amplitude, the level summary both CLIs publish per component.
  auto component_energy = [](const std::vector<float>& component) {
    if (component.empty()) return 0.0f;
    double total = 0.0;
    for (float sample : component) total += std::fabs(sample);
    return static_cast<float>(total / static_cast<double>(component.size()));
  };

  // The components sum to the input, so they share one gain and keep summing to it.
  OutputFit fit;
  for (const std::vector<float>& component : result.components) fit.include(component);
  std::vector<std::string> paths;
  std::vector<float> energies;
  paths.reserve(result.components.size());
  energies.reserve(result.components.size());
  for (size_t index = 0; index < result.components.size(); ++index) {
    const std::vector<float>& component = result.components[index];
    std::string path = base + "_component" + std::to_string(index + 1) + ".wav";
    write_wav(path, component.data(), component.size(), audio.sample_rate(), 16, fit);
    paths.push_back(std::move(path));
    energies.push_back(component_energy(component) * fit.gain());
  }
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder builder;
    builder.begin_object()
        .kv("count", paths.size())
        .kv("output_gain_db", fit.gain_db())
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
  HpssConfig config;
  config.kernel_size_harmonic = args.get_int("kernel-harmonic", config.kernel_size_harmonic);
  config.kernel_size_percussive = args.get_int("kernel-percussive", config.kernel_size_percussive);
  config.use_soft_mask = !args.has("hard-mask");

  StftConfig stft;
  stft.n_fft = args.get_int("n-fft", stft.n_fft);
  // An absent hop is sent as 0, which the library derives from the FFT size.
  stft.hop_length = args.get_int("hop-length", 0);

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
    const std::string suffix = lowercase_cli_value(base.substr(base.size() - 4));
    if (suffix == ".wav") base = base.substr(0, base.size() - 4);
  }

  // The components sum to the input, so the files one run writes share one gain.
  OutputFit fit;
  auto save_audio = [&](const std::string& path, const Audio& a) { write_wav(path, a, 16, fit); };
  // Mean absolute amplitude of the component as written, the level summary both CLIs publish.
  auto component_energy = [&](const Audio& a) {
    if (a.empty()) return 0.0f;
    double total = 0.0;
    for (float sample : a) total += std::fabs(sample);
    return static_cast<float>(total / static_cast<double>(a.size())) * fit.gain();
  };

  if (args.has("harmonic-only")) {
    std::string path = base + ".wav";
    const Audio result = harmonic(audio, config, stft);
    fit.include(result);
    save_audio(path, result);
    note_output_fit(args, fit);
    if (!args.quiet) {
      std::cerr << color::green << "Saved harmonic to " << path << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", result.size())
          .kv("output_gain_db", fit.gain_db())
          .kv("sample_rate", result.sample_rate())
          .kv("harmonic_mean_abs", component_energy(result))
          .kv("harmonic", path)
          .end_object()
          .print();
  } else if (args.has("percussive-only")) {
    std::string path = base + ".wav";
    const Audio result = percussive(audio, config, stft);
    fit.include(result);
    save_audio(path, result);
    note_output_fit(args, fit);
    if (!args.quiet) {
      std::cerr << color::green << "Saved percussive to " << path << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", result.size())
          .kv("output_gain_db", fit.gain_db())
          .kv("sample_rate", result.sample_rate())
          .kv("percussive_mean_abs", component_energy(result))
          .kv("percussive", path)
          .end_object()
          .print();
  } else if (args.has("with-residual")) {
    auto r = hpss_with_residual(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav",
                res = base + "_residual.wav";
    fit.include(r.harmonic).include(r.percussive).include(r.residual);
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    save_audio(res, r.residual);
    note_output_fit(args, fit);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << ", " << res << color::reset
                << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", r.harmonic.size())
          .kv("output_gain_db", fit.gain_db())
          .kv("sample_rate", r.harmonic.sample_rate())
          .kv("harmonic_mean_abs", component_energy(r.harmonic))
          .kv("percussive_mean_abs", component_energy(r.percussive))
          .kv("residual_mean_abs", component_energy(r.residual))
          .kv("harmonic", h)
          .kv("percussive", p)
          .kv("residual", res)
          .end_object()
          .print();
  } else {
    auto r = hpss(audio, config, stft);
    std::string h = base + "_harmonic.wav", p = base + "_percussive.wav";
    fit.include(r.harmonic).include(r.percussive);
    save_audio(h, r.harmonic);
    save_audio(p, r.percussive);
    note_output_fit(args, fit);
    if (!args.quiet) {
      std::cerr << color::green << "Saved: " << h << ", " << p << color::reset << "\n";
    }
    if (args.json_output)
      JsonBuilder()
          .begin_object()
          .kv("length", r.harmonic.size())
          .kv("output_gain_db", fit.gain_db())
          .kv("sample_rate", r.harmonic.sample_rate())
          .kv("harmonic_mean_abs", component_energy(r.harmonic))
          .kv("percussive_mean_abs", component_energy(r.percussive))
          .kv("harmonic", h)
          .kv("percussive", p)
          .end_object()
          .print();
  }
  return 0;
}

int cmd_preemphasis(const CliArgs& args, const Audio& audio) {
  const float coef = args.get_float("coef", kDefaultEmphasisCoef);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = preemphasis(input, coef);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result.data(), result.size(), audio.sample_rate(), 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
  const float coef = args.get_float("coef", kDefaultEmphasisCoef);
  std::vector<float> input(audio.begin(), audio.end());
  std::vector<float> result = deemphasis(input, coef);
  const OutputFit fit = OutputFit().include(result.data(), result.size());
  write_wav(args.output_file, result.data(), result.size(), audio.sample_rate(), 16, fit);
  note_output_fit(args, fit);

  if (args.json_output) {
    JsonBuilder()
        .begin_object()
        .kv("output", args.output_file)
        .kv("output_gain_db", fit.gain_db())
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
  const bool use_legacy_top_db = args.has("top-db");
  const float threshold_db = args.get_float("threshold-db", sonare::kDefaultAbsoluteSilenceDb);
  const float top_db = args.get_float("top-db", sonare::kDefaultSilenceTopDb);
  const int frame_length = args.get_int("n-fft", sonare::kDefaultSilenceFrameLength);
  const int hop_length = args.get_int("hop-length", sonare::kDefaultSilenceHopLength);
  Audio result =
      use_legacy_top_db
          ? Audio::from_vector(sonare::trim(std::vector<float>(audio.begin(), audio.end()), top_db,
                                            frame_length, hop_length)
                                   .audio,
                               audio.sample_rate())
          : trim_absolute(audio, threshold_db, frame_length, hop_length);

  const OutputFit fit = OutputFit().include(result.data(), result.size());
  if (!args.output_file.empty()) {
    write_wav(args.output_file, result.data(), result.size(), audio.sample_rate(), 16, fit);
    note_output_fit(args, fit);
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
      json.kv("top_db", top_db);
    } else {
      json.kv("threshold_db", threshold_db);
    }
    json.kv("n_fft", frame_length).kv("hop_length", hop_length);
    if (!args.output_file.empty()) {
      json.kv("output", args.output_file).kv("output_gain_db", fit.gain_db());
    }
    json.end_object().print();
  } else {
    std::cout << "Silence Trim:\n";
    printf("  Samples: %zu\n", result.size());
    if (!args.output_file.empty()) std::cout << "  Output:  " << args.output_file << "\n";
  }
  return 0;
}

namespace {

// The takes `split-silence` measures together: the first positional, already
// loaded as `audio`, then every further positional in command-line order.
std::vector<std::vector<float>> load_split_silence_takes(const CliArgs& args, const Audio& audio) {
  std::vector<std::vector<float>> takes;
  takes.reserve(args.positionals.size());
  takes.emplace_back(audio.begin(), audio.end());
  for (size_t index = 1; index < args.positionals.size(); ++index) {
    const std::string& path = args.positionals[index];
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

// Zero-pads to `width` digits and grows past it rather than truncating, so the
// filenames sort naturally at the expected counts and stay distinct beyond them.
std::string pad_index(size_t value, int width) {
  std::ostringstream text;
  text << std::setw(width) << std::setfill('0') << value;
  return text.str();
}

// Writes every take sliced at every interval as `{prefix}{take}_{interval}.wav`,
// both indices 1-based and zero-padded. A take that ended before an interval is
// silent there, which is the rule the union was built on, so its slice is padded
// rather than shortened and every take's file for one interval is the same
// length.
// The slices are parts of takes meant to be compared, so they share one gain.
OutputFit write_split_silence_takes(const std::string& prefix,
                                    const std::vector<std::vector<float>>& takes,
                                    const std::vector<std::pair<int, int>>& ranges,
                                    int sample_rate) {
  OutputFit fit;
  for (const auto& range : ranges) {
    for (const std::vector<float>& take : takes) {
      const size_t start = static_cast<size_t>(range.first);
      const size_t end = std::min(static_cast<size_t>(range.second), take.size());
      if (start < end) fit.include(take.data() + start, end - start);
    }
  }
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
      write_wav(prefix + pad_index(take + 1, 2) + "_" + pad_index(interval + 1, 3) + ".wav",
                slice.data(), slice.size(), sample_rate, 16, fit);
    }
  }
  return fit;
}

}  // namespace

int cmd_split_silence(const CliArgs& args, const Audio& audio) {
  const float top_db = args.get_float("top-db", sonare::kDefaultSilenceTopDb);
  const int frame_length = args.get_int("n-fft", sonare::kDefaultSilenceFrameLength);
  const int hop_length = args.get_int("hop-length", sonare::kDefaultSilenceHopLength);
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
  const SonareError err = sonare_split_silence_common(
      signals.data(), signals.size(), lengths.data(), top_db, static_cast<size_t>(frame_length),
      static_cast<size_t>(hop_length), &flat, &flat_count, &report);
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
  OutputFit fit;
  if (!write_takes.empty()) {
    fit = write_split_silence_takes(write_takes, takes, ranges, audio.sample_rate());
    note_output_fit(args, fit);
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
    // One object whatever the flags; the gain and the report are present when their flag is.
    JsonBuilder json;
    json.begin_object().key("intervals");
    append_intervals(json);
    if (!write_takes.empty()) json.kv("output_gain_db", fit.gain_db());
    if (with_report) {
      json.key("report")
          .begin_object()
          .kv("silence_ceiling_db", report.silence_ceiling_db)
          .kv("max_signal_intervals", report.max_signal_intervals)
          .kv("min_signal_intervals", report.min_signal_intervals)
          .end_object();
    }
    json.end_object().print();
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
      printf("  silence ceiling %s at --top-db %.2f\n",
             measured_text(report.silence_ceiling_db, 2, "dB").c_str(),
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
