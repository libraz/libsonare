#include "effects/acoustic/room_morph.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "acoustic/rir_synthesizer.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::effects::acoustic {

namespace {
// The relative downward expander never fully gates: the strongest suppression
// still leaves 20% of the tail. This keeps the morph musical (a reduction, not
// a noise gate) and avoids the artefacts of aggressive dereverberation.
constexpr float kMaxAttenuation = 0.8f;
// Knee on the tail's level relative to the recent peak: below `kKneeLo` the
// content is treated as pure reverberant tail (full suppression), above
// `kKneeHi` as direct/onset (no suppression).
constexpr float kKneeLo = 0.2f;
constexpr float kKneeHi = 0.7f;
constexpr float kPeakFloor = 1e-6f;

float one_pole_coef(float tau_seconds, int sample_rate) {
  const float tau = std::max(1e-4f, tau_seconds);
  return std::exp(-1.0f / (tau * static_cast<float>(sample_rate)));
}

/// The target-RIR synthesis configuration this processor derives from its own
/// config. Built in one place so validation covers exactly what prepare() submits.
sonare::acoustic::RirSynthConfig rir_config_from(const RoomMorphConfig& config) {
  sonare::acoustic::RirSynthConfig rc;
  rc.ism_order = config.ism_order;
  rc.seed = config.seed;
  rc.max_seconds = config.max_seconds;
  rc.late_model = config.late_model;
  rc.mixing_time_ms = config.mixing_time_ms;  // 0 = auto (~sqrt(V) ms)
  rc.crossfade_ms = config.crossfade_ms;
  rc.air_absorption_enabled = config.air_absorption_enabled;
  rc.air = config.air;
  return rc;
}
}  // namespace

void validate_room_morph_config(const RoomMorphConfig& config) {
  const std::vector<Diagnostic> geometry =
      sonare::acoustic::validate_shoebox(config.target, config.placement);
  SONARE_CHECK_MSG(!has_error(geometry), ErrorCode::InvalidParameter,
                   "room morph target geometry, placement, or material is invalid");
  SONARE_CHECK_MSG(numeric::finite_in_closed_range(config.source_tail_suppression, 0.0f, 1.0f) &&
                       numeric::finite_in_closed_range(config.wet, 0.0f, 1.0f),
                   ErrorCode::InvalidParameter,
                   "room morph wet and source-tail suppression must be within [0,1]");
  // Everything synthesize_rir would refuse, refused here instead: a refused
  // synthesis returns an empty target RIR, which the convolution path renders as
  // dry passthrough rather than as an error.
  const std::vector<Diagnostic> synthesis =
      sonare::acoustic::validate_rir_synth_config(rir_config_from(config));
  SONARE_CHECK_MSG(!has_error(synthesis), ErrorCode::InvalidParameter,
                   "room morph image-source order, RIR timing, or air absorption is invalid");
}

RoomMorphProcessor::RoomMorphProcessor(RoomMorphConfig config) : config_(std::move(config)) {
  validate_room_morph_config(config_);
}

void RoomMorphProcessor::prepare(double sample_rate, int max_block_size) {
  using namespace sonare::acoustic;

  const int sr = sample_rate > 0.0 ? static_cast<int>(std::lround(sample_rate)) : 48000;

  const RirSynthConfig rc = rir_config_from(config_);
  const RirSynthResult res = synthesize_rir(config_.target, config_.placement, sr, rc);

  // The target room, placement and timing were validated at construction; the
  // host sample rate was not, and it is the one synthesis input this processor
  // first sees here. A refused synthesis is an Error diagnostic plus an empty
  // target RIR, which the convolution path would render as an inert insert, so
  // the diagnostics are read rather than dropped.
  SONARE_CHECK_MSG(!has_error(res.diagnostics), ErrorCode::InvalidParameter,
                   "room morph target RIR synthesis failed: " + first_error_text(res.diagnostics));

  // prepare() otherwise synthesizes a default noise IR which the load below
  // immediately discards. This processor always supplies its own target RIR,
  // normalized to the same unit energy as that default so `wet` means the same
  // mix depth here as on the plain convolution reverb; the target RIR's own
  // 1/(4*pi*d) physical scale would otherwise put it around 20 dB down.
  reverb_.suppress_default_ir_synthesis();
  reverb_.prepare(sample_rate, max_block_size);
  reverb_.load_ir_unit_energy(res.rir.data(), static_cast<int>(res.rir.size()));
  reverb_.set_parameter(0, config_.wet);  // dry/wet = target-room mix

  env_attack_ = one_pole_coef(0.005f, sr);
  env_release_ = one_pole_coef(0.050f, sr);
  peak_release_ = one_pole_coef(0.300f, sr);
  gain_smooth_ = one_pole_coef(0.020f, sr);

  // The underlying convolver preallocates mono/stereo engines; match it.
  suppressor_.assign(2, SuppressorState{});
}

void RoomMorphProcessor::process(float* const* channels, int num_channels, int num_samples) {
  // The suppressor envelopes decay multiplicatively toward denormals on silent
  // passages; flush-to-zero for the whole callback (the convolver sets its own).
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  // Step 1: gently suppress the source reverberation tail in place. Skipped
  // entirely (true bypass) when suppression is zero.
  const float supp = std::clamp(config_.source_tail_suppression, 0.0f, 1.0f);
  const float max_cut = supp * kMaxAttenuation;
  if (max_cut > 0.0f) {
    const int n = std::min(num_channels, static_cast<int>(suppressor_.size()));
    for (int ch = 0; ch < n; ++ch) {
      if (channels[ch] == nullptr) continue;
      SuppressorState& st = suppressor_[static_cast<size_t>(ch)];
      float* d = channels[ch];
      for (int i = 0; i < num_samples; ++i) {
        const float a = std::abs(d[i]);
        // Fast envelope (attack on rising level, slower release on decay).
        st.env = (a > st.env) ? env_attack_ * st.env + (1.0f - env_attack_) * a
                              : env_release_ * st.env + (1.0f - env_release_) * a;
        // Slow peak follower tracks the recent local maximum.
        st.peak = std::max(a, st.peak * peak_release_);
        // Level relative to the recent peak: ~1 on onsets, small on the tail.
        const float r = st.env / (st.peak + kPeakFloor);
        float t = std::clamp((r - kKneeLo) / (kKneeHi - kKneeLo), 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);  // smoothstep
        const float target_gain = (1.0f - max_cut) + max_cut * t;
        st.gain = gain_smooth_ * st.gain + (1.0f - gain_smooth_) * target_gain;
        d[i] *= st.gain;
      }
    }
  }

  // Step 2: add the target room. ConvolutionReverb mixes the (suppressed) dry
  // with the target-room convolution, delay-aligned, allocation-free.
  reverb_.process(channels, num_channels, num_samples);
}

void RoomMorphProcessor::reset() {
  reverb_.reset();
  for (SuppressorState& s : suppressor_) s = SuppressorState{};
}

bool RoomMorphProcessor::set_parameter(unsigned int param_id, float value) {
  if (!numeric::finite_in_closed_range(value, 0.0f, 1.0f)) return false;
  switch (param_id) {
    case 0:
      config_.wet = value;
      return reverb_.set_parameter(0, value);
    case 1:
      config_.source_tail_suppression = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> RoomMorphProcessor::parameter_descriptors() const {
  return {{"dryWet", 0}, {"sourceTailSuppression", 1}};
}

Audio room_morph(const Audio& recording, const RoomMorphConfig& config) {
  validate_room_morph_config(config);
  if (recording.empty()) {
    return recording;
  }
  const int sr = recording.sample_rate();

  RoomMorphProcessor processor(config);
  constexpr int kBlock = 256;
  processor.prepare(static_cast<double>(sr), kBlock);

  const int tail = processor.tail_samples();
  // ConvolutionReverb reports a fixed latency, but an empty (invalid-target) IR
  // is a no-op that applies no delay, so only compensate when a RIR was loaded.
  const int latency = processor.target_ir_size() > 0 ? processor.latency_samples() : 0;

  const size_t out_len = recording.size() + static_cast<size_t>(tail);
  const size_t total = out_len + static_cast<size_t>(latency);
  std::vector<float> buf(total, 0.0f);
  for (size_t i = 0; i < recording.size(); ++i) buf[i] = recording[i];

  for (size_t off = 0; off < total; off += static_cast<size_t>(kBlock)) {
    const int nn = static_cast<int>(std::min<size_t>(static_cast<size_t>(kBlock), total - off));
    float* blk = buf.data() + off;
    processor.process(&blk, 1, nn);
  }

  // Drop the leading convolution latency so the offline render is time-aligned.
  std::vector<float> out(out_len, 0.0f);
  for (size_t i = 0; i < out_len && (i + static_cast<size_t>(latency)) < total; ++i) {
    out[i] = buf[i + static_cast<size_t>(latency)];
  }
  return Audio::from_vector(std::move(out), sr);
}

}  // namespace sonare::effects::acoustic
