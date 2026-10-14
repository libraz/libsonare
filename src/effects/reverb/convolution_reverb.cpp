#include "effects/reverb/convolution_reverb.h"

#include <algorithm>
#include <cmath>

#include "rt/scoped_no_denormals.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::effects::reverb {

using constants::kDefaultDawSampleRate;

namespace {
// FFT partition size used by the per-channel convolvers. A power of two keeps
// the underlying real FFT efficient while bounding the block buffering latency.
constexpr int kPartitionSize = 256;
// 60 dB amplitude drop defining the synthesized RT60 tail (matches velvet).
constexpr float kT60Drop = 1000.0f;
// Bound the synthesized IR so prepare() stays cheap and the convolver does not
// allocate a pathologically long partition chain for absurd decaySec values.
constexpr float kMaxDecaySeconds = ConvolutionReverbConfig::kMaxDecaySeconds;
constexpr float kMaxPreDelaySeconds = 1.0f;

// xorshift32: a tiny deterministic PRNG so the synthesized IR is reproducible
// across platforms without pulling in <random> state.
inline std::uint32_t xorshift32(std::uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

std::vector<float> checked_ir(const float* samples, int count) {
  if (count < 0 || (count > 0 && samples == nullptr)) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid impulse response");
  }
  if (count == 0) return {};
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(samples[i])) {
      throw SonareException(ErrorCode::InvalidParameter, "impulse response must be finite");
    }
  }
  return {samples, samples + count};
}

double ir_energy(const std::vector<float>& samples) {
  double energy = 0.0;
  for (float sample : samples) energy += static_cast<double>(sample) * sample;
  return energy;
}

void scale_ir(std::vector<float>& samples, double energy) {
  const double root = std::sqrt(energy);
  const float norm = 1.0f / static_cast<float>(root);
  if (std::isfinite(norm) && norm > 0.0f) {
    for (float& sample : samples) sample *= norm;
  } else {
    for (float& sample : samples) sample = static_cast<float>(sample / root);
  }
}
}  // namespace

void ConvolutionReverb::synthesize_default_ir(double sample_rate) {
  const double sr = sample_rate > 0.0 ? sample_rate : kDefaultDawSampleRate;
  const float decay_sec = std::clamp(config_.decay_sec, 0.0f, kMaxDecaySeconds);
  const float pre_delay_sec = std::clamp(config_.pre_delay_ms / 1000.0f, 0.0f, kMaxPreDelaySeconds);
  const int pre_delay_samples = static_cast<int>(std::lround(pre_delay_sec * sr));
  // Effective T60 tail length; floor it so we always synthesize a usable IR.
  const float rt60 = std::max(0.05f, decay_sec);
  const int tail_samples = std::max(1, static_cast<int>(std::lround(rt60 * sr)));
  const int total = pre_delay_samples + tail_samples;
  ir_.assign(static_cast<size_t>(total), 0.0f);

  // Exponentially decaying white noise: a length-RT60 burst that reaches -60 dB
  // amplitude at the end of the tail, normalized so the IR has unit energy and
  // the wet level is comparable to the algorithmic reverbs.
  const float decay_rate = std::log(kT60Drop) / rt60;
  std::uint32_t state = config_.seed != 0 ? config_.seed : 0x5151ABCDu;
  for (int i = 0; i < tail_samples; ++i) {
    // Uniform white noise in [-1, 1).
    const float noise = static_cast<float>(xorshift32(state)) / 2147483648.0f - 1.0f;
    const float envelope = std::exp(-decay_rate * static_cast<float>(i) / static_cast<float>(sr));
    ir_[static_cast<size_t>(pre_delay_samples + i)] = noise * envelope;
  }
  // The reference level every other IR is matched against; see
  // load_ir_unit_energy for the dry/wet contract this establishes.
  normalize_ir_unit_energy();
}

void ConvolutionReverb::prepare(double sample_rate, int) {
  partition_size_ = kPartitionSize;
  // Synthesize the algorithmic default IR unless the caller supplied one.
  if (!explicit_ir_) {
    synthesize_default_ir(sample_rate);
  }
  convolvers_.resize(static_cast<size_t>(kMaxChannels));
  block_input_.assign(static_cast<size_t>(kMaxChannels),
                      std::vector<float>(static_cast<size_t>(partition_size_), 0.0f));
  block_output_.assign(static_cast<size_t>(kMaxChannels),
                       std::vector<float>(static_cast<size_t>(partition_size_), 0.0f));
  fill_count_.assign(static_cast<size_t>(kMaxChannels), 0);
  rebuild_convolvers();
  reset();
}

void ConvolutionReverb::rebuild_convolvers() {
  if (partition_size_ <= 0) {
    return;
  }
  for (auto& gate : mono_gates_) gate.close();
  for (auto& gate : pair_gates_) gate.close();
  for (auto& convolver : convolvers_) {
    if (!convolver) {
      convolver = std::make_unique<rt::PartitionedConvolver>(
          rt::PartitionedConvolverConfig{partition_size_});
    }
    convolver->set_impulse_response(ir_);
  }
  if (ir_left_.empty()) {
    pair_convolvers_.clear();
    pair_input_.clear();
    pair_output_.clear();
    pair_fill_.clear();
    return;
  }
  const size_t part = static_cast<size_t>(partition_size_);
  pair_convolvers_.resize(2);
  for (size_t i = 0; i < 2; ++i) {
    if (!pair_convolvers_[i]) {
      pair_convolvers_[i] = std::make_unique<rt::PartitionedConvolver>(
          rt::PartitionedConvolverConfig{partition_size_});
    }
    pair_convolvers_[i]->set_impulse_response(i == 0 ? ir_left_ : ir_right_);
  }
  pair_input_.assign(2, std::vector<float>(part, 0.0f));
  pair_output_.assign(2, std::vector<float>(part, 0.0f));
  pair_fill_.assign(2, 0);
}

void ConvolutionReverb::reset_bank_plane(
    std::vector<std::unique_ptr<rt::PartitionedConvolver>>& convolvers,
    std::vector<std::vector<float>>& input_blocks, std::vector<std::vector<float>>& output_blocks,
    std::vector<int>& fills, int plane) {
  if (plane < 0) return;
  const size_t index = static_cast<size_t>(plane);
  if (index < convolvers.size() && convolvers[index]) convolvers[index]->reset();
  if (index < input_blocks.size())
    std::fill(input_blocks[index].begin(), input_blocks[index].end(), 0.0f);
  if (index < output_blocks.size())
    std::fill(output_blocks[index].begin(), output_blocks[index].end(), 0.0f);
  if (index < fills.size()) fills[index] = 0;
}

void ConvolutionReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }
  // An empty IR passes audio through untouched; a reloaded IR resumes from rest.
  if (!stage_.admit(!ir_.empty() && partition_size_ > 0, [this] { reset(); })) {
    return;
  }
  // Convolvers/buffers are preallocated for the maximum supported channel count;
  // clamp here so the audio thread never allocates.
  const int channels_to_process = std::min(num_channels, kMaxChannels);
  // Block-rate dry/wet: smoothed across blocks by the engine parameter slot
  // smoother, not per-sample (see Chorus::process for the rationale).
  const float wet = std::clamp(dry_wet_, 0.0f, 1.0f);
  const float dry = 1.0f - wet;
  // A loaded pair serves stereo; one channel always runs the mono IR.
  const bool use_pair = !pair_convolvers_.empty() && channels_to_process >= 2;

  // Clear FFT and staging history when a skipped bank/plane resumes.
  for (int ch = 0; ch < kMaxChannels; ++ch) {
    const bool base_active = !use_pair && ch < channels_to_process && channels[ch] != nullptr &&
                             static_cast<size_t>(ch) < convolvers_.size() && convolvers_[ch];
    mono_gates_[static_cast<size_t>(ch)].admit(base_active, [this, ch] {
      reset_bank_plane(convolvers_, block_input_, block_output_, fill_count_, ch);
    });

    const bool pair_active = use_pair && ch < channels_to_process && channels[ch] != nullptr &&
                             static_cast<size_t>(ch) < pair_convolvers_.size() &&
                             pair_convolvers_[ch];
    pair_gates_[static_cast<size_t>(ch)].admit(pair_active, [this, ch] {
      reset_bank_plane(pair_convolvers_, pair_input_, pair_output_, pair_fill_, ch);
    });
  }

  for (int ch = 0; ch < channels_to_process; ++ch) {
    const size_t idx = static_cast<size_t>(ch);
    if (use_pair) {
      if (channels[ch] == nullptr || !pair_convolvers_[idx]) continue;
      process_channel(*pair_convolvers_[idx], pair_input_[idx], pair_output_[idx], pair_fill_[idx],
                      channels[ch], num_samples, dry, wet);
    } else {
      if (idx >= convolvers_.size()) break;
      if (channels[ch] == nullptr || !convolvers_[idx]) continue;
      process_channel(*convolvers_[idx], block_input_[idx], block_output_[idx], fill_count_[idx],
                      channels[ch], num_samples, dry, wet);
    }
  }
}

void ConvolutionReverb::process_channel(rt::PartitionedConvolver& convolver,
                                        std::vector<float>& in_block, std::vector<float>& out_block,
                                        int& fill_ref, float* data, int num_samples, float dry,
                                        float wet) {
  int fill = fill_ref;
  for (int i = 0; i < num_samples; ++i) {
    // Emit the output produced one partition ago, then stage the incoming sample; the dry path
    // is delayed by the same partition so the mix stays time-aligned.
    const float input_sample = data[i];
    const float wet_sample = out_block[static_cast<size_t>(fill)];
    const float dry_sample = in_block[static_cast<size_t>(fill)];
    data[i] = dry * dry_sample + wet * wet_sample;
    in_block[static_cast<size_t>(fill)] = input_sample;
    if (++fill == partition_size_) {
      convolver.process_block(in_block.data(), out_block.data());
      fill = 0;
    }
  }
  fill_ref = fill;
}

void ConvolutionReverb::reset() {
  for (int ch = 0; ch < kMaxChannels; ++ch) {
    reset_bank_plane(convolvers_, block_input_, block_output_, fill_count_, ch);
    reset_bank_plane(pair_convolvers_, pair_input_, pair_output_, pair_fill_, ch);
    mono_gates_[static_cast<size_t>(ch)].close();
    pair_gates_[static_cast<size_t>(ch)].close();
  }
}

void ConvolutionReverb::store_ir(const float* impulse_response, int num_samples) {
  auto proposed = checked_ir(impulse_response, num_samples);
  ir_.swap(proposed);
  ir_left_.clear();
  ir_right_.clear();
  // An explicit IR overrides the algorithmic default synthesis in prepare().
  explicit_ir_ = true;
}

bool ConvolutionReverb::normalize_ir_unit_energy() {
  const double energy = ir_energy(ir_);
  if (!(energy > 0.0)) return false;
  scale_ir(ir_, energy);
  return true;
}

void ConvolutionReverb::load_ir(const float* impulse_response, int num_samples) {
  store_ir(impulse_response, num_samples);
  // Feeding the IR into the convolvers (re)allocates FFT partitions; this is a
  // non-RT operation, so it is safe to run here outside the audio thread.
  rebuild_convolvers();
}

void ConvolutionReverb::load_ir(const std::vector<float>& impulse_response) {
  load_ir(impulse_response.data(), static_cast<int>(impulse_response.size()));
}

void ConvolutionReverb::load_ir_unit_energy(const float* impulse_response, int num_samples) {
  auto proposed = checked_ir(impulse_response, num_samples);
  const double energy = ir_energy(proposed);
  if (!(energy > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "impulse response carries no energy; nothing to normalize");
  }
  scale_ir(proposed, energy);
  ir_.swap(proposed);
  ir_left_.clear();
  ir_right_.clear();
  explicit_ir_ = true;
  rebuild_convolvers();
}

void ConvolutionReverb::load_ir_set_unit_energy(const std::vector<float>& mono,
                                                const std::vector<float>& left,
                                                const std::vector<float>& right) {
  auto proposed_mono = checked_ir(mono.data(), static_cast<int>(mono.size()));
  auto proposed_left = checked_ir(left.data(), static_cast<int>(left.size()));
  auto proposed_right = checked_ir(right.data(), static_cast<int>(right.size()));
  const double e_mono = ir_energy(proposed_mono);
  const double e_left = ir_energy(proposed_left);
  const double e_right = ir_energy(proposed_right);
  if (mono.empty() || left.empty() || right.empty() || !(e_mono > 0.0) || !(e_left > 0.0) ||
      !(e_right > 0.0)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "impulse response set needs three non-empty IRs with energy");
  }
  scale_ir(proposed_mono, e_mono);
  scale_ir(proposed_left, 0.5 * (e_left + e_right));
  scale_ir(proposed_right, 0.5 * (e_left + e_right));
  ir_.swap(proposed_mono);
  ir_left_.swap(proposed_left);
  ir_right_.swap(proposed_right);
  explicit_ir_ = true;
  rebuild_convolvers();
}

bool ConvolutionReverb::set_parameter_impl(unsigned int param_id, float value) {
  switch (param_id) {
    case 0:
      // process() clamps dry_wet to [0, 1]; store the raw target.
      dry_wet_ = value;
      return true;
    default:
      return false;
  }
}

std::vector<rt::ParamDescriptor> ConvolutionReverb::parameter_descriptors() const {
  return {{"dryWet", 0}};
}

}  // namespace sonare::effects::reverb
