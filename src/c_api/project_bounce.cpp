#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/builtin_synth.h"

namespace {

// Adapts a host's C callback table to a sonare::midi::MidiInstrument so the
// bounce engine can drive an external instrument: events are forwarded to
// on_event at their sample-accurate render frame and render() sums the audio.
// Only opaque UMP words / planar buffers cross the seam (invariant 6).
class CallbackInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit CallbackInstrument(const SonareInstrumentCallbacks& callbacks) : cb_(callbacks) {}

  void prepare(double sample_rate, int max_block_size) override {
    if (cb_.prepare) cb_.prepare(cb_.user_data, sample_rate, max_block_size);
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (cb_.render) cb_.render(cb_.user_data, channels, num_channels, num_samples);
  }
  void reset() override {}
  int latency_samples() const noexcept override { return cb_.latency_samples; }
  void on_event(uint32_t destination_id, const sonare::midi::MidiEvent& event) noexcept override {
    if (cb_.on_event) {
      cb_.on_event(cb_.user_data, destination_id, event.ump.words, event.ump.word_count,
                   event.render_frame);
    }
  }

 private:
  SonareInstrumentCallbacks cb_;
};

// A destination id paired with a borrowed instrument pointer (the owning storage
// outlives the render in the caller). Used by the shared bounce core so the
// callback and built-in-synth paths share one render implementation.
struct HostedInstrument {
  uint32_t destination_id = 0;
  sonare::midi::MidiInstrument* instrument = nullptr;
};

// End of the arrangement in frames at the render sample rate: the latest sample
// touched by any audio or MIDI clip on the compiled timeline. Used to
// auto-derive a bounce length when the caller does not supply total_frames.
int64_t arrangement_end_frames(const arr::CompiledTimeline& timeline) noexcept {
  int64_t end = 0;
  for (const auto& clip : timeline.audio_clips) {
    end = std::max(end, clip.start_sample + clip.length_samples);
  }
  for (const auto& clip : timeline.midi_clips) {
    int64_t clip_end = clip.start_sample + clip.length_samples;
    for (const auto& event : clip.events) {
      clip_end = std::max(clip_end, event.render_frame + 1);
    }
    end = std::max(end, clip_end);
  }
  return end;
}

// Shared bounce core: validates options, compiles, registers any hosted
// instruments per destination, renders offline, and writes the interleaved
// result. `instruments` may be empty for a silent MIDI bounce. When
// opts.total_frames <= 0 the render length is auto-derived from the compiled
// timeline (plus the longest hosted-instrument release tail) so a caller can
// bounce a MIDI-only arrangement without computing a length by hand. Returns
// through the SONARE_C_TRY/CATCH guard of the caller.
SonareError do_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                              const std::vector<HostedInstrument>& instruments,
                              float** out_interleaved, size_t* out_len) {
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;

  SonareProjectBounceOptions opts{};
  if (options) opts = *options;
  const int block_size = opts.block_size > 0 ? opts.block_size : 128;
  const int num_channels = opts.num_channels > 0 ? opts.num_channels : 2;
  if (block_size <= 0 || num_channels <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  const double project_sr = project->history.project().sample_rate();
  const double sample_rate =
      opts.sample_rate > 0 ? static_cast<double>(opts.sample_rate) : project_sr;
  if (!finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.instrument_latency_samples < 0) return SONARE_ERROR_INVALID_PARAMETER;

  arr::CompileConfig config;
  config.instrument_latency_samples = opts.instrument_latency_samples;
  arr::CompileResult compiled = arr::compile(
      project->history.project(), project->history.midi_content(), project->audio, config);
  if (!compiled.timeline.has_value()) return SONARE_ERROR_INVALID_STATE;

  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(*compiled.timeline, engine);

  // Register the hosted instruments per destination. The engine borrows the
  // pointers; the owning storage lives in the caller for the whole render. Track
  // the longest release tail so an auto-derived length is not truncated.
  int64_t instrument_tail = 0;
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
    if (!engine.set_midi_instrument(hosted.destination_id, hosted.instrument)) {
      return SONARE_ERROR_INVALID_PARAMETER;  // more instruments than the rack holds
    }
    instrument_tail = std::max<int64_t>(instrument_tail, hosted.instrument->tail_samples());
  }

  // Determine the render length: caller-supplied, or auto-derived from the
  // arrangement (musical end + the longest instrument release tail).
  int64_t frames = opts.total_frames;
  if (frames <= 0) {
    frames = arrangement_end_frames(*compiled.timeline);
    if (frames > 0) frames += instrument_tail;
  }
  if (frames < 0 ||
      static_cast<uint64_t>(frames) >
          std::numeric_limits<size_t>::max() / static_cast<uint64_t>(num_channels) ||
      static_cast<uint64_t>(frames) * static_cast<uint64_t>(num_channels) > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  // Plugin-delay compensation: a hosted instrument with internal latency makes
  // the engine delay every source by the project's total latency so clip and
  // instrument audio stay phase-aligned. To return musical time [0, frames)
  // aligned to output sample 0, render `pdc` extra frames and drop the leading
  // `pdc` (the delay-line fill). With no latency-bearing instrument pdc == 0 and
  // the result is identical to a plain bounce.
  const int64_t pdc = static_cast<int64_t>(engine.midi_instrument_latency_samples());
  const int64_t render_frames = frames + pdc;
  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(num_channels);
  if (frames == 0) {
    // Empty arrangement (or zero-length request): a valid empty render.
    *out_interleaved = new float[1];
    *out_len = 0;
    for (const HostedInstrument& hosted : instruments) {
      engine.set_midi_instrument(hosted.destination_id, nullptr);
    }
    return SONARE_OK;
  }
  std::vector<std::vector<float>> channels(
      static_cast<size_t>(num_channels),
      std::vector<float>(static_cast<size_t>(render_frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels.size());
  for (auto& channel : channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, render_frames, block_size);
  // Detach the borrowed instruments before they leave scope.
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }

  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t frame = 0; frame < frames; ++frame) {
    for (int ch = 0; ch < num_channels; ++ch) {
      interleaved[static_cast<size_t>(frame) * num_channels + ch] =
          channels[static_cast<size_t>(ch)][static_cast<size_t>(frame + pdc)];
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
}

// Maps the public built-in waveform ordinal to the core enum (out-of-range
// values fall back to sine via clamp_synth_config).
sonare::midi::BuiltinSynthConfig synth_config_from_c(const SonareBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

}  // namespace
#endif

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  return do_project_bounce(project, options, {}, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, out_interleaved, out_len);
#endif
}

SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].callbacks.render == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<std::unique_ptr<CallbackInstrument>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(std::make_unique<CallbackInstrument>(instruments[i].callbacks));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::unique_ptr<sonare::midi::BuiltinSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(synth_config_from_c(instruments[i].config)));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}
