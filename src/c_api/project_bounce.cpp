#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
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

// Shared bounce core: validates options, compiles, registers any callback
// instruments per destination, renders offline, and writes the interleaved
// result. `instruments`/`instrument_count` may be null/0 for a silent MIDI
// bounce. Returns through the SONARE_C_TRY/CATCH guard of the caller.
SonareError do_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                              const SonareInstrumentBinding* instruments, size_t instrument_count,
                              float** out_interleaved, size_t* out_len) {
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SonareProjectBounceOptions opts{};
  if (options) opts = *options;
  if (opts.total_frames <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  const int block_size = opts.block_size > 0 ? opts.block_size : 128;
  const int num_channels = opts.num_channels > 0 ? opts.num_channels : 2;
  if (block_size <= 0 || num_channels <= 0 ||
      static_cast<uint64_t>(opts.total_frames) >
          std::numeric_limits<size_t>::max() / static_cast<uint64_t>(num_channels) ||
      static_cast<uint64_t>(opts.total_frames) * static_cast<uint64_t>(num_channels) >
          kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].callbacks.render == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  }
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

  // Register the host's callback instruments per destination. They live on the
  // stack for the whole render; the engine borrows the pointers.
  std::vector<std::unique_ptr<CallbackInstrument>> hosted;
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    hosted.push_back(std::make_unique<CallbackInstrument>(instruments[i].callbacks));
    if (!engine.set_midi_instrument(instruments[i].destination_id, hosted.back().get())) {
      return SONARE_ERROR_INVALID_PARAMETER;  // more instruments than the rack holds
    }
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  const int64_t frames = opts.total_frames;
  // Plugin-delay compensation: a hosted instrument with internal latency makes
  // the engine delay every source by the project's total latency so clip and
  // instrument audio stay phase-aligned. To return musical time [0, frames)
  // aligned to output sample 0, render `pdc` extra frames and drop the leading
  // `pdc` (the delay-line fill). With no latency-bearing instrument pdc == 0 and
  // the result is identical to a plain bounce.
  const int64_t pdc = static_cast<int64_t>(engine.midi_instrument_latency_samples());
  const int64_t render_frames = frames + pdc;
  std::vector<std::vector<float>> channels(
      static_cast<size_t>(num_channels),
      std::vector<float>(static_cast<size_t>(render_frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels.size());
  for (auto& channel : channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, render_frames, block_size);
  // Detach the borrowed instruments before they leave scope.
  for (size_t i = 0; i < instrument_count; ++i) {
    engine.set_midi_instrument(instruments[i].destination_id, nullptr);
  }

  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(num_channels);
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

}  // namespace
#endif

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  return do_project_bounce(project, options, nullptr, 0, out_interleaved, out_len);
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
  return do_project_bounce(project, options, instruments, instrument_count, out_interleaved,
                           out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}
