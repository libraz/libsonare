#pragma once

/// @file voice_changer_insert.h
/// @brief The realtime voice changer as a streaming insert (`voice.changer`).

#include <cstdint>
#include <vector>

#include "editing/voice_changer/realtime.h"
#include "rt/processor_base.h"

namespace sonare::editing::voice_changer {

/// @brief Adapts @ref RealtimeVoiceChanger to @c rt::ProcessorBase for mixer strips and chains.
/// @details Mono and stereo. Construction refuses a configuration with any out-of-domain
///          or non-finite field (InvalidParameter naming the key), so the published parameter
///          bounds are real. Automation refuses an out-of-range value the same way, by returning
///          false and leaving the configuration unchanged.
///
///          The retune grain size, the reverb seed, the formant mode, the inter-sample-peak
///          limiter switch, the pitch engine, the lowest tracked f0 and the vocoder band count are
///          construction-time only: all but the seed change the reported latency, and a delay
///          compensated by a host must not move while it runs. The insert has no MIDI or carrier
///          input, so the vocoder carrier source is construction-time only too and must be
///          internal; the vocoder then plays the voices' own targets. In absolute formant mode
///          (@c formantMode, @c relative or @c absolute) @c formantAmount is ignored; with the
///          grain engine construction refuses a @c formantFactor the warp cannot reach at the
///          configured @c retuneSemitones, and automation refuses a value that would leave the
///          warp out of reach. Every other field is an automatable parameter (see
///          @ref parameter_descriptors); an integer, enum or switch takes its wire value, and a
///          fraction is refused.
///
///          Latency with the psola engine is the longer of the shifter latency and the vocoder's
///          group delay; with the grain engine it is one retune grain plus, in absolute formant
///          mode, one warp frame. Either way, when the limiter switch is on, the limiter's
///          look-ahead is added. The tail is RealtimeVoiceChanger::tail_budget(): the filters,
///          the voice-stage history, the character and space stages and the reverb down to the
///          project's tail floor. Gain reduction covers the compressor, de-esser and sample
///          limiter.
class VoiceChangerInsert : public rt::ProcessorBase {
 public:
  /// @throws SonareException (InvalidParameter) when a field of @p config is non-finite or
  ///         outside the range the voice changer accepts.
  explicit VoiceChangerInsert(RealtimeVoiceChangerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  /// @brief Prepares for at most two channels; @p max_channels above two is refused.
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  /// @brief Processes one or two planes; blocks longer than the prepared size are split.
  /// @throws SonareException (InvalidState) before prepare(), (InvalidParameter) for more planes
  ///         than were prepared.
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  /// @brief The time the reverb needs to fall 60 dB below its loudest echo, in samples; 0 while
  ///        its mix or the wet mix is 0.
  /// @details Control-thread query on the configuration last published.
  int tail_samples() const noexcept override;
  float last_gain_reduction_db() const override;

  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// @brief The configuration last published, as seen from the control thread.
  const RealtimeVoiceChangerConfig& config() const noexcept { return config_; }

 protected:
  bool set_parameter_impl(unsigned int param_id, float value) override;

 private:
  RealtimeVoiceChanger changer_;
  RealtimeVoiceChangerConfig config_;
  double sample_rate_ = 0.0;
  int prepared_channels_ = 0;
  int max_block_size_ = 0;
  std::uint32_t reported_discards_ = 0;
};

}  // namespace sonare::editing::voice_changer
