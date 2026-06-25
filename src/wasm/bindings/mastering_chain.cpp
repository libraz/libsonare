/// @file mastering_chain.cpp
/// @brief Embind bindings for mastering chain and loudness facade APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

val js_mastering(val samples, int sample_rate, float target_lufs, float ceiling_db,
                 int true_peak_oversample) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = target_lufs;
  config.ceiling_db = ceiling_db;
  config.true_peak_oversample = true_peak_oversample;

  auto result = mastering::maximizer::loudness_optimize(audio, config);
  std::vector<float> out_vec(result.audio.data(), result.audio.data() + result.audio.size());

  val out = val::object();
  out.set("samples", vectorToFloat32Array(out_vec));
  out.set("sampleRate", result.audio.sample_rate());
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  return out;
}

// ---------------------------------------------------------------------------
// Helpers: build a MasteringChainConfig from the nested JS config object that
// js_mastering_chain / js_mastering_chain_stereo receive.
// ---------------------------------------------------------------------------

mastering::api::MasteringChainConfig masteringChainConfigFromVal(val config) {
  mastering::api::MasteringChainConfig out;

  val repair = objectProperty(config, "repair");
  if (boolProperty(repair, "denoise", false)) {
    out.repair.denoise.enabled = true;
    out.repair.denoise.config.n_fft = intProperty(repair, "nFft", out.repair.denoise.config.n_fft);
    out.repair.denoise.config.hop_length =
        intProperty(repair, "hopLength", out.repair.denoise.config.hop_length);
    out.repair.denoise.config.dd_alpha =
        floatProperty(repair, "ddAlpha", out.repair.denoise.config.dd_alpha);
    out.repair.denoise.config.gain_floor =
        floatProperty(repair, "gainFloor", out.repair.denoise.config.gain_floor);
  }
  if (hasProperty(repair, "declick")) {
    val declick = objectProperty(repair, "declick");
    out.repair.declick.enabled = boolProperty(declick, "enabled", true);
    auto& dc = out.repair.declick.config;
    dc.threshold = floatProperty(declick, "threshold", dc.threshold);
    dc.neighbor_ratio = floatProperty(declick, "neighborRatio", dc.neighbor_ratio);
    dc.max_click_samples = static_cast<size_t>(
        intProperty(declick, "maxClickSamples", static_cast<int>(dc.max_click_samples)));
    dc.lpc_order = intProperty(declick, "lpcOrder", dc.lpc_order);
    dc.residual_ratio = floatProperty(declick, "residualRatio", dc.residual_ratio);
  }
  if (hasProperty(repair, "dereverb")) {
    val dereverb = objectProperty(repair, "dereverb");
    out.repair.dereverb.enabled = boolProperty(dereverb, "enabled", true);
    auto& rc = out.repair.dereverb.config;
    rc.threshold = floatProperty(dereverb, "threshold", rc.threshold);
    rc.attenuation = floatProperty(dereverb, "attenuation", rc.attenuation);
    rc.n_fft = intProperty(dereverb, "nFft", rc.n_fft);
    rc.hop_length = intProperty(dereverb, "hopLength", rc.hop_length);
    rc.t60_sec = floatProperty(dereverb, "t60Sec", rc.t60_sec);
    rc.late_delay_ms = floatProperty(dereverb, "lateDelayMs", rc.late_delay_ms);
    rc.over_subtraction = floatProperty(dereverb, "overSubtraction", rc.over_subtraction);
    rc.spectral_floor = floatProperty(dereverb, "spectralFloor", rc.spectral_floor);
    rc.wpe_enabled = boolProperty(dereverb, "wpeEnabled", rc.wpe_enabled);
    rc.wpe_iterations = intProperty(dereverb, "wpeIterations", rc.wpe_iterations);
    rc.wpe_taps = intProperty(dereverb, "wpeTaps", rc.wpe_taps);
    rc.wpe_strength = floatProperty(dereverb, "wpeStrength", rc.wpe_strength);
  }

  val eq = objectProperty(config, "eq");
  if (hasProperty(eq, "tiltDb") || hasProperty(eq, "pivotHz")) {
    out.eq.tilt.enabled = true;
    out.eq.tilt.tilt_db = floatProperty(eq, "tiltDb", 0.0f);
    out.eq.tilt.pivot_hz = floatProperty(eq, "pivotHz", 1000.0f);
  }

  val dynamics = objectProperty(config, "dynamics");
  if (hasProperty(dynamics, "compressor")) {
    val compressor = objectProperty(dynamics, "compressor");
    out.dynamics.compressor.enabled = boolProperty(compressor, "enabled", true);
    auto& cc = out.dynamics.compressor.config;
    cc.threshold_db = floatProperty(compressor, "thresholdDb", cc.threshold_db);
    cc.ratio = floatProperty(compressor, "ratio", cc.ratio);
    cc.attack_ms = floatProperty(compressor, "attackMs", cc.attack_ms);
    cc.release_ms = floatProperty(compressor, "releaseMs", cc.release_ms);
    cc.knee_db = floatProperty(compressor, "kneeDb", cc.knee_db);
    cc.makeup_gain_db = floatProperty(compressor, "makeupGainDb", cc.makeup_gain_db);
    cc.auto_makeup = boolProperty(compressor, "autoMakeup", cc.auto_makeup);
  }
  if (hasProperty(dynamics, "deesser")) {
    val deesser = objectProperty(dynamics, "deesser");
    out.dynamics.deesser.enabled = boolProperty(deesser, "enabled", true);
    auto& dc = out.dynamics.deesser.config;
    dc.frequency_hz = floatProperty(deesser, "frequencyHz", dc.frequency_hz);
    dc.threshold_db = floatProperty(deesser, "thresholdDb", dc.threshold_db);
    dc.ratio = floatProperty(deesser, "ratio", dc.ratio);
    dc.attack_ms = floatProperty(deesser, "attackMs", dc.attack_ms);
    dc.release_ms = floatProperty(deesser, "releaseMs", dc.release_ms);
    dc.range_db = floatProperty(deesser, "rangeDb", dc.range_db);
    dc.bandpass_q = floatProperty(deesser, "bandpassQ", dc.bandpass_q);
  }
  if (hasProperty(dynamics, "transientShaper")) {
    val ts = objectProperty(dynamics, "transientShaper");
    out.dynamics.transient_shaper.enabled = boolProperty(ts, "enabled", true);
    auto& tc = out.dynamics.transient_shaper.config;
    tc.attack_gain_db = floatProperty(ts, "attackGainDb", tc.attack_gain_db);
    tc.sustain_gain_db = floatProperty(ts, "sustainGainDb", tc.sustain_gain_db);
    tc.fast_attack_ms = floatProperty(ts, "fastAttackMs", tc.fast_attack_ms);
    tc.fast_release_ms = floatProperty(ts, "fastReleaseMs", tc.fast_release_ms);
    tc.slow_attack_ms = floatProperty(ts, "slowAttackMs", tc.slow_attack_ms);
    tc.slow_release_ms = floatProperty(ts, "slowReleaseMs", tc.slow_release_ms);
    tc.sensitivity = floatProperty(ts, "sensitivity", tc.sensitivity);
    tc.max_gain_db = floatProperty(ts, "maxGainDb", tc.max_gain_db);
    tc.gain_smoothing_ms = floatProperty(ts, "gainSmoothingMs", tc.gain_smoothing_ms);
    tc.lookahead_ms = floatProperty(ts, "lookaheadMs", tc.lookahead_ms);
  }
  if (hasProperty(dynamics, "multibandComp")) {
    val mb = objectProperty(dynamics, "multibandComp");
    out.dynamics.multiband_comp.enabled = boolProperty(mb, "enabled", true);
    auto& mc = out.dynamics.multiband_comp.config;
    // Default config has 2 cutoffs ([120,2000]) and 3 bands; we update in place.
    if (hasProperty(mb, "lowCutoffHz")) {
      mc.crossover.cutoffs_hz[0] = floatProperty(mb, "lowCutoffHz", mc.crossover.cutoffs_hz[0]);
    }
    if (hasProperty(mb, "highCutoffHz")) {
      mc.crossover.cutoffs_hz[1] = floatProperty(mb, "highCutoffHz", mc.crossover.cutoffs_hz[1]);
    }
    auto apply_band = [&](int idx, const char* prefix_threshold, const char* prefix_ratio,
                          const char* prefix_attack, const char* prefix_release) {
      auto& band = mc.bands[idx];
      band.threshold_db = floatProperty(mb, prefix_threshold, band.threshold_db);
      band.ratio = floatProperty(mb, prefix_ratio, band.ratio);
      band.attack_ms = floatProperty(mb, prefix_attack, band.attack_ms);
      band.release_ms = floatProperty(mb, prefix_release, band.release_ms);
    };
    apply_band(0, "lowThresholdDb", "lowRatio", "lowAttackMs", "lowReleaseMs");
    apply_band(1, "midThresholdDb", "midRatio", "midAttackMs", "midReleaseMs");
    apply_band(2, "highThresholdDb", "highRatio", "highAttackMs", "highReleaseMs");
  }

  val saturation = objectProperty(config, "saturation");
  if (hasProperty(saturation, "tape")) {
    val tape = objectProperty(saturation, "tape");
    auto& tc = out.saturation.tape.config;
    tc.drive_db = floatProperty(tape, "driveDb", tc.drive_db);
    tc.saturation = floatProperty(tape, "saturation", tc.saturation);
    tc.hysteresis = floatProperty(tape, "hysteresis", tc.hysteresis);
    tc.output_gain_db = floatProperty(tape, "outputGainDb", tc.output_gain_db);
    tc.speed_ips = floatProperty(tape, "speedIps", tc.speed_ips);
    tc.head_bump_db = floatProperty(tape, "headBumpDb", tc.head_bump_db);
    tc.bias = floatProperty(tape, "bias", tc.bias);
    tc.gap_loss = floatProperty(tape, "gapLoss", tc.gap_loss);
    // Tape is a color stage, so the mere presence of the object must not engage
    // it: an explicit `enabled` wins, otherwise defer to the shared core rule
    // (tape_engages_color) so a `{ tape: { driveDb: 0, saturation: 0 } }` config
    // stays bypassed, consistently with the flat-param chain parser.
    out.saturation.tape.enabled = hasProperty(tape, "enabled")
                                      ? boolProperty(tape, "enabled", true)
                                      : mastering::saturation::tape_engages_color(tc);
  }
  if (hasProperty(saturation, "exciter")) {
    val exciter = objectProperty(saturation, "exciter");
    auto& ec = out.saturation.exciter.config;
    ec.frequency_hz = floatProperty(exciter, "frequencyHz", ec.frequency_hz);
    ec.drive_db = floatProperty(exciter, "driveDb", ec.drive_db);
    ec.amount = floatProperty(exciter, "amount", ec.amount);
    ec.q = floatProperty(exciter, "q", ec.q);
    ec.even_odd_mix = floatProperty(exciter, "evenOddMix", ec.even_odd_mix);
    // Same color-stage rule as tape: explicit `enabled` wins, otherwise defer to
    // the shared core rule (exciter_engages_color) so `{ exciter: { amount: 0 } }`
    // stays bypassed.
    out.saturation.exciter.enabled = hasProperty(exciter, "enabled")
                                         ? boolProperty(exciter, "enabled", true)
                                         : mastering::saturation::exciter_engages_color(ec);
  }

  val spectral = objectProperty(config, "spectral");
  if (hasProperty(spectral, "airBand")) {
    val air_band = objectProperty(spectral, "airBand");
    out.spectral.air_band.enabled = boolProperty(air_band, "enabled", true);
    auto& ac = out.spectral.air_band.config;
    ac.amount = floatProperty(air_band, "amount", ac.amount);
    ac.shelf_frequency_hz = floatProperty(air_band, "shelfFrequencyHz", ac.shelf_frequency_hz);
    ac.dynamic_threshold_db =
        floatProperty(air_band, "dynamicThresholdDb", ac.dynamic_threshold_db);
    ac.dynamic_range_db = floatProperty(air_band, "dynamicRangeDb", ac.dynamic_range_db);
  }

  val stereo = objectProperty(config, "stereo");
  if (hasProperty(stereo, "imager")) {
    val imager = objectProperty(stereo, "imager");
    out.stereo.imager.enabled = boolProperty(imager, "enabled", true);
    auto& ic = out.stereo.imager.config;
    ic.width = floatProperty(imager, "width", ic.width);
    ic.output_gain_db = floatProperty(imager, "outputGainDb", ic.output_gain_db);
    ic.decorrelation_amount = floatProperty(imager, "decorrelationAmount", ic.decorrelation_amount);
    ic.preserve_energy = boolProperty(imager, "preserveEnergy", ic.preserve_energy);
  }
  if (hasProperty(stereo, "monoMaker")) {
    val mono_maker = objectProperty(stereo, "monoMaker");
    out.stereo.mono_maker.enabled = boolProperty(mono_maker, "enabled", true);
    out.stereo.mono_maker.config.amount =
        floatProperty(mono_maker, "amount", out.stereo.mono_maker.config.amount);
  }

  val maximizer = objectProperty(config, "maximizer");
  if (hasProperty(maximizer, "truePeakLimiter")) {
    val limiter = objectProperty(maximizer, "truePeakLimiter");
    out.maximizer.true_peak_limiter.enabled = boolProperty(limiter, "enabled", true);
    auto& lc = out.maximizer.true_peak_limiter.config;
    lc.ceiling_db = floatProperty(limiter, "ceilingDb", lc.ceiling_db);
    lc.lookahead_ms = floatProperty(limiter, "lookaheadMs", lc.lookahead_ms);
    lc.release_ms = floatProperty(limiter, "releaseMs", lc.release_ms);
    lc.oversample_factor = intProperty(limiter, "oversampleFactor", lc.oversample_factor);
    lc.apply_gain_at_input_rate =
        boolProperty(limiter, "applyGainAtInputRate", lc.apply_gain_at_input_rate);
  }

  val loudness = objectProperty(config, "loudness");
  if (!loudness.isUndefined() && !loudness.isNull()) {
    out.loudness.enabled = boolProperty(loudness, "enabled", true);
    out.loudness.target_lufs = floatProperty(loudness, "targetLufs", out.loudness.target_lufs);
    out.loudness.ceiling_db = floatProperty(loudness, "ceilingDb", out.loudness.ceiling_db);
    out.loudness.true_peak_oversample =
        intProperty(loudness, "truePeakOversample", out.loudness.true_peak_oversample);
    out.loudness.release_ms = floatProperty(loudness, "releaseMs", out.loudness.release_ms);
    out.loudness.apply_gain_at_input_rate =
        boolProperty(loudness, "applyGainAtInputRate", out.loudness.apply_gain_at_input_rate);
  }

  return out;
}

val js_mastering_chain(val samples, int sample_rate, val config) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_mono(data.data(), data.size(), sample_rate);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

val js_mastering_chain_stereo(val left_samples, val right_samples, int sample_rate, val config) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

// Mastering chain (mono) with progress callback
val js_mastering_chain_with_progress(val samples, int sample_rate, val config,
                                     val progress_callback) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  }
  auto result = chain.process_mono(data.data(), data.size(), sample_rate);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

// Mastering chain (stereo) with progress callback
val js_mastering_chain_stereo_with_progress(val left_samples, val right_samples, int sample_rate,
                                            val config, val progress_callback) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  }
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

void registerMasteringChainBindings() {
  function("mastering", &js_mastering);
  function("masteringChain", &js_mastering_chain);
  function("masteringChainStereo", &js_mastering_chain_stereo);
  function("masteringChainWithProgress", &js_mastering_chain_with_progress);
  function("masteringChainStereoWithProgress", &js_mastering_chain_stereo_with_progress);
}

#endif  // __EMSCRIPTEN__
