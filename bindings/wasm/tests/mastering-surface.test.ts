/**
 * The one-shot mastering entry points the WASM module exposes: the chain and
 * its stereo and progress forms, the named processors, the pair APIs, the
 * assistant, the profiles and the metering beside them.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  lufsInterleaved,
  mastering,
  masteringAssistantSuggest,
  masteringAssistantSuggestStereo,
  masteringAudioProfile,
  masteringAudioProfileStereo,
  masteringChain,
  masteringChainStereo,
  masteringChainStereoWithProgress,
  masteringChainWithProgress,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
  meteringCrestFactorDb,
  meteringCrestFactorDbStereo,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
} from '../dist/index.js';

describe('Sonare WASM Module', () => {
  beforeAll(async () => {
    await init();
  });

  describe('mastering', () => {
    it('should return processed samples and loudness metadata', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }

      const result = mastering(samples, sampleRate, {
        targetLufs: -18.0,
        ceilingDb: -1.0,
        truePeakOversample: 4,
      });
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(Number.isFinite(result.appliedGainDb)).toBe(true);
      expect(typeof result.loudnessTargetLimited).toBe('boolean');
      expect(result.outputLufs).toBeCloseTo(-18.0, 1);

      const ceilingLimited = mastering(samples, sampleRate, {
        targetLufs: -2.0,
        ceilingDb: -1.0,
        truePeakOversample: 4,
      });
      expect(ceilingLimited.loudnessTargetLimited).toBe(true);
    });

    it('treats truePeakOversample: 0 as the library default', () => {
      const sampleRate = 22050;
      const input = new Float32Array(4096);
      for (let i = 0; i < input.length; i++) {
        input[i] = 0.3 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      const implicit = mastering(input, sampleRate, { targetLufs: -18, ceilingDb: -1 });
      const sentinel = mastering(input, sampleRate, {
        targetLufs: -18,
        ceilingDb: -1,
        truePeakOversample: 0,
      });
      expect(sentinel.samples).toEqual(implicit.samples);
    });

    it('should run a configurable mastering chain in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        const tone = Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        const overtone = 0.4 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
        samples[i] = 0.18 * (tone + overtone);
      }

      const result = masteringChain(samples, sampleRate, {
        eq: { tiltDb: 1.5, pivotHz: 1200 },
        dynamics: {
          compressor: {
            thresholdDb: -22,
            ratio: 1.6,
            attackMs: 15,
            releaseMs: 120,
            kneeDb: 3,
          },
        },
        saturation: {
          tape: { driveDb: 1.5, saturation: 0.25, hysteresis: 0.1 },
          exciter: { amount: 0.05, driveDb: 2 },
        },
        spectral: { airBand: { amount: 0.08 } },
        maximizer: {
          truePeakLimiter: {
            ceilingDb: -1,
            oversampleFactor: 4,
            applyGainAtInputRate: true,
          },
        },
        loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
      });

      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(result.stages).toContain('eq.tilt');
      expect(result.stages).toContain('dynamics.compressor');
      expect(result.stages).toContain('saturation.tape');
      expect(result.stages).toContain('maximizer.truePeakLimiter');
      expect(result.stages).toContain('loudness.optimize');
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
      expect(result.report.before.integratedLufs).toBe(result.inputLufs);
      expect(result.report.after.integratedLufs).toBe(result.outputLufs);
      expect(result.report.after.truePeakDbtp).toBe(result.outputTruePeakDbtp);
      expect(result.report.bandEnergyDeltaDb).toBeInstanceOf(Float32Array);
      expect(result.report.bandEnergyDeltaDb).toHaveLength(32);
    });

    describe('color saturation stages engage only when meaningful', () => {
      const sampleRate = 22050;
      const tone = () => {
        const samples = new Float32Array(sampleRate);
        for (let i = 0; i < samples.length; i++) {
          samples[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        }
        return samples;
      };
      const stagesFor = (saturation: Record<string, unknown>): string[] =>
        masteringChain(tone(), sampleRate, { saturation }).stages;

      it('does not engage the exciter when amount is zero', () => {
        expect(stagesFor({ exciter: { amount: 0 } })).not.toContain('saturation.exciter');
      });

      it('does not engage tape when drive and saturation are zero', () => {
        expect(stagesFor({ tape: { driveDb: 0, saturation: 0 } })).not.toContain('saturation.tape');
      });

      it('engages the exciter when amount is positive', () => {
        expect(stagesFor({ exciter: { amount: 0.2 } })).toContain('saturation.exciter');
      });

      it('engages tape when drive is positive', () => {
        expect(stagesFor({ tape: { driveDb: 2 } })).toContain('saturation.tape');
      });

      it('honors an explicit enabled:true even with zero amount', () => {
        expect(stagesFor({ exciter: { amount: 0, enabled: true } })).toContain(
          'saturation.exciter',
        );
      });

      it('honors an explicit enabled:false even with meaningful params', () => {
        expect(stagesFor({ tape: { driveDb: 3, saturation: 0.5, enabled: false } })).not.toContain(
          'saturation.tape',
        );
      });
    });

    it('should run a stereo mastering chain in WASM', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate);
      const right = new Float32Array(sampleRate);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.16 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const result = masteringChainStereo(left, right, sampleRate, {
        eq: { tiltDb: 1.0 },
        dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
        saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
        stereo: {
          imager: { width: 1.15, decorrelationAmount: 0.05 },
          monoMaker: { amount: 0.2 },
        },
        loudness: {
          targetLufs: -18,
          ceilingDb: -1,
          truePeakOversample: 4,
          applyGainAtInputRate: true,
        },
      });

      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(result.sampleRate).toBe(sampleRate);
      expect(result.stages).toContain('eq.tilt');
      expect(result.stages).toContain('stereo.imager');
      expect(result.stages).toContain('stereo.monoMaker');
      expect(result.stages).toContain('loudness.optimize');
      expect(Number.isFinite(result.inputLufs)).toBe(true);
      expect(Number.isFinite(result.outputLufs)).toBe(true);
    });

    it('should invoke progress callback for masteringChainWithProgress', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
      }

      const stages: string[] = [];
      const progresses: number[] = [];
      const result = masteringChainWithProgress(
        samples,
        sampleRate,
        {
          eq: { tiltDb: 1.0 },
          dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
        },
        (progress, stage) => {
          progresses.push(progress);
          stages.push(stage);
        },
      );

      expect(stages).toEqual(['eq.tilt', 'dynamics.compressor']);
      expect(progresses.length).toBe(2);
      expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
      expect(result.stages).toEqual(['eq.tilt', 'dynamics.compressor']);
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(samples.length);
    });

    it('should invoke progress callback for positional masteringChain', () => {
      const samples = new Float32Array(22050).fill(0.1);
      const stages: string[] = [];

      const result = masteringChain(samples, 22050, { eq: { tiltDb: 1 } }, (_, stage) => {
        stages.push(stage);
      });

      expect(result.stages).toEqual(['eq.tilt']);
      expect(stages).toEqual(['eq.tilt']);
    });

    it('should invoke progress callback for masteringChainStereoWithProgress', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate);
      const right = new Float32Array(sampleRate);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.18 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.16 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const stages: string[] = [];
      const progresses: number[] = [];
      const result = masteringChainStereoWithProgress(
        left,
        right,
        sampleRate,
        {
          eq: { tiltDb: 1.0 },
          stereo: { imager: { width: 1.1 } },
        },
        (progress, stage) => {
          progresses.push(progress);
          stages.push(stage);
        },
      );

      expect(stages).toEqual(['eq.tilt', 'stereo.imager']);
      expect(progresses.length).toBe(2);
      expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
      expect(result.stages).toEqual(['eq.tilt', 'stereo.imager']);
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
    });

    it('should invoke progress callback for positional masteringChainStereo', () => {
      const left = new Float32Array(22050).fill(0.1);
      const right = new Float32Array(22050).fill(0.08);
      const stages: string[] = [];

      const result = masteringChainStereo(left, right, 22050, { eq: { tiltDb: 1 } }, (_, stage) => {
        stages.push(stage);
      });

      expect(result.stages).toEqual(['eq.tilt']);
      expect(stages).toEqual(['eq.tilt']);
    });

    it('should expose named mastering processors in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate / 2);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }

      const names = masteringProcessorNames();
      expect(names).toContain('dynamics.compressor');
      expect(names).toContain('eq.equalizer');
      expect(names).toContain('saturation.ampSim');
      expect(names).toContain('saturation.tape');
      expect(names).toContain('stereo.imager');

      const mono = masteringProcess('dynamics.compressor', samples, sampleRate, {
        thresholdDb: -24,
        ratio: 1.5,
      });
      expect(mono.samples).toBeInstanceOf(Float32Array);
      expect(mono.samples.length).toBe(samples.length);
      expect(Number.isFinite(mono.outputLufs)).toBe(true);

      const eq = masteringProcess('eq.equalizer', samples, sampleRate, {
        'band0.enabled': 1,
        'band0.frequencyHz': 440,
        'band0.gainDb': 6,
        'band0.q': 1,
        autoGain: 1,
      });
      expect(eq.samples).toBeInstanceOf(Float32Array);
      expect(eq.samples.length).toBe(samples.length);
      expect(Number.isFinite(eq.outputLufs)).toBe(true);

      const amp = masteringProcess('saturation.ampSim', samples, sampleRate, {
        drive: 0.8,
        bassDb: 2,
        midDb: -3,
        trebleDb: 1.5,
        presenceDb: 3,
        cab: 1,
        levelDb: -6,
      });
      expect(amp.samples).toBeInstanceOf(Float32Array);
      expect(amp.samples.length).toBe(samples.length);
      expect(Number.isFinite(amp.outputLufs)).toBe(true);
    });

    it('should expose named stereo mastering processors in WASM', () => {
      const sampleRate = 22050;
      const left = new Float32Array(sampleRate / 2);
      const right = new Float32Array(sampleRate / 2);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
        right[i] = 0.2 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }

      const result = masteringProcessStereo('stereo.imager', left, right, sampleRate, {
        width: 1.1,
      });
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(left.length);
      expect(result.right.length).toBe(right.length);
      expect(Number.isFinite(result.outputLufs)).toBe(true);

      const leftEq = masteringProcessStereo('eq.equalizer', left, left, sampleRate, {
        'band0.enabled': 1,
        'band0.frequencyHz': 220,
        'band0.gainDb': 12,
        'band0.q': 1,
        'band0.placement': 1,
      });
      const leftPeak = Math.max(...Array.from(leftEq.left, Math.abs));
      const rightPeak = Math.max(...Array.from(leftEq.right, Math.abs));
      expect(leftPeak).toBeGreaterThan(rightPeak * 1.5);

      const linearEq = masteringProcessStereo('eq.equalizer', left, left, sampleRate, {
        phaseMode: 3,
        'band0.enabled': 1,
        'band0.frequencyHz': 220,
        'band0.gainDb': 3,
        'band0.q': 1,
      });
      expect(linearEq.latencySamples).toBeGreaterThan(0);
    });

    it('should report the same loudnessTargetLimited from the mono and stereo named processors', () => {
      // A lone full-scale transient peak-normalizes the source without lifting
      // its program loudness, so the -3 dBTP ceiling blocks the requested
      // -6 LUFS boost on both paths by a margin far wider than the 3 dB BS.1770
      // channel-summing offset between mono and dual-mono stereo.
      const sampleRate = 22050;
      const peaky = new Float32Array(sampleRate);
      for (let i = 0; i < peaky.length; i++) {
        peaky[i] = 0.05 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      peaky[peaky.length >> 1] = 1;
      const params = { targetLufs: -6, ceilingDb: -3, truePeakOversample: 4 };

      const mono = masteringProcess('maximizer.loudnessOptimize', peaky, sampleRate, params);
      const stereo = masteringProcessStereo(
        'maximizer.loudnessOptimize',
        peaky,
        peaky,
        sampleRate,
        params,
      );
      expect(mono.loudnessTargetLimited).toBe(true);
      expect(stereo.loudnessTargetLimited).toBe(mono.loudnessTargetLimited);
      expect(stereo.outputLufs).toBeLessThan(params.targetLufs);
    });

    it('should expose pair and stereo mastering APIs in WASM', () => {
      const sampleRate = 44100;
      const source = new Float32Array(sampleRate / 4);
      const reference = new Float32Array(sampleRate / 4);
      for (let i = 0; i < source.length; i++) {
        source[i] = 0.18 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
        reference[i] = 0.12 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
      }

      expect(masteringPairProcessorNames()).toContain('match.abCrossfade');
      expect(masteringPairAnalysisNames()).toContain('match.referenceLoudness');
      expect(masteringStereoAnalysisNames()).toContain('stereo.monoCompatCheck');

      const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
        mix: 0.25,
      });
      expect(paired.samples).toBeInstanceOf(Float32Array);
      expect(paired.samples.length).toBe(source.length);

      const pairJson = masteringPairAnalyze(
        'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      );
      expect(pairJson).toContain('"sourceLufs"');
      expect(pairJson).toContain('"referenceLufs"');

      const stereoJson = masteringStereoAnalyze(
        'stereo.monoCompatCheck',
        source,
        reference,
        sampleRate,
      );
      expect(stereoJson).toContain('"correlation"');
    });

    it('should accept independent source/reference lengths for pair mastering', () => {
      const sampleRate = 44100;
      const source = new Float32Array(Math.floor(sampleRate * 0.25));
      const reference = new Float32Array(Math.floor(sampleRate * 0.6)); // different duration
      for (let i = 0; i < source.length; i++) {
        source[i] = 0.18 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      for (let i = 0; i < reference.length; i++) {
        reference[i] = 0.12 * Math.sin((2 * Math.PI * 880 * i) / sampleRate);
      }

      const paired = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
        mix: 0.25,
      });
      expect(paired.samples).toBeInstanceOf(Float32Array);
      expect(paired.samples.length).toBe(source.length);

      const pairJson = masteringPairAnalyze(
        'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      );
      expect(pairJson).toContain('"sourceLufs"');
      expect(pairJson).toContain('"referenceLufs"');
    });

    it('should expose mastering assistant suggestions in WASM', () => {
      const sampleRate = 22050;
      const clean = new Float32Array(sampleRate * 3);
      for (let i = 0; i < clean.length; i++) {
        clean[i] = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
      }
      const clicked = Float32Array.from(clean);
      const step = Math.floor(clicked.length / 5);
      for (let k = 1; k <= 4; k++) {
        clicked[k * step] = 0.9;
      }
      const suggest = (samples: Float32Array) =>
        JSON.parse(
          masteringAssistantSuggest(samples, sampleRate, {
            targetLufs: -13,
            ceilingDb: -0.8,
            enableRepair: true,
          }),
        );
      const result = suggest(clean);

      expect(result).toHaveProperty('chainConfig');
      expect(result).toHaveProperty('profile');
      expect(Array.isArray(result.explanation)).toBe(true);
      expect(Array.isArray(result.genreCandidates)).toBe(true);
      expect(result.chainConfig.params['loudness.targetLufs']).toBe(-13);
      expect(result.chainConfig.params['loudness.ceilingDb']).toBeCloseTo(-0.8, 6);

      // enableRepair asks for repair; the measured defect profile picks which stages
      // run. Only the clean side would still pass with enableRepair ignored outright,
      // so both are driven. Booleans serialize as JSON true/false (RFC 8259).
      expect(result.chainConfig.params['repair.declick.enabled']).toBe(false);
      expect(suggest(clicked).chainConfig.params['repair.declick.enabled']).toBe(true);
    });

    it('should expose mastering audio profiles in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate * 2);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 330 * i) / sampleRate);
      }
      const json = masteringAudioProfile(samples, sampleRate, {
        nFft: 1024,
        hopLength: 256,
      });
      const result = JSON.parse(json);

      expect(typeof result.durationSec).toBe('number');
      expect(result.durationSec).toBeGreaterThan(1.9);
      expect(result).toHaveProperty('loudness.integratedLufs');
      expect(result).toHaveProperty('spectral.centroidHz');
      expect(result).toHaveProperty('dynamics.attackDensity');
      expect(Array.isArray(result.genreCandidates)).toBe(true);
    });

    it('should expose streaming platform loudness previews in WASM', () => {
      const sampleRate = 22050;
      const samples = new Float32Array(sampleRate);
      for (let i = 0; i < samples.length; i++) {
        samples[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
      }
      const json = masteringStreamingPreview(samples, sampleRate, [
        { name: 'Unit Test', targetLufs: -12, ceilingDb: -1 },
      ]);
      const result = JSON.parse(json);

      expect(result.platforms).toHaveLength(1);
      expect(result.platforms[0].name).toBe('Unit Test');
      expect(typeof result.platforms[0].integratedLufs).toBe('number');
      expect(typeof result.platforms[0].truePeakDb).toBe('number');
      expect(typeof result.platforms[0].normalizationGainDb).toBe('number');
      expect(typeof result.platforms[0].ceilingRisk).toBe('boolean');
    });

    it('should measure a stereo pair with channel summing in WASM', () => {
      const sampleRate = 48000;
      const n = sampleRate * 4;
      const left = new Float32Array(n);
      const right = new Float32Array(n);
      const downmix = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        left[i] = 0.2 * Math.sin((2 * Math.PI * 1000 * i) / sampleRate);
        right[i] = 0.2 * Math.sin((2 * Math.PI * 1731 * i) / sampleRate);
        downmix[i] = 0.5 * (left[i] + right[i]);
      }

      const interleaved = new Float32Array(n * 2);
      for (let i = 0; i < n; i++) {
        interleaved[2 * i] = left[i];
        interleaved[2 * i + 1] = right[i];
      }

      const stereo = JSON.parse(masteringStreamingPreviewStereo({ left, right, sampleRate }));
      const mono = JSON.parse(masteringStreamingPreview(downmix, sampleRate));
      const stereoLufs = stereo.platforms[0].integratedLufs;

      expect(stereo.platforms[0].name).toBe('Spotify');
      // BS.1770 sums the channel powers while the 0.5*(L+R) downmix quarters
      // them, so a decorrelated pair reads 6.02 dB above what a mono caller can
      // measure, and the normalization gain built on it is off by the same.
      expect(stereoLufs - mono.platforms[0].integratedLufs).toBeCloseTo(6.02, 1);
      expect(stereoLufs).toBeCloseTo(lufsInterleaved(interleaved, 2, sampleRate).integratedLufs, 2);
      expect(stereo.platforms[0].normalizationGainDb).toBeCloseTo(-14 - stereoLufs, 3);

      const profile = JSON.parse(masteringAudioProfileStereo({ left, right, sampleRate }));
      expect(profile.loudness.integratedLufs).toBeCloseTo(stereoLufs, 2);
      expect(profile).toHaveProperty('spectral.centroidHz');

      const suggestion = JSON.parse(masteringAssistantSuggestStereo({ left, right, sampleRate }));
      expect(suggestion).toHaveProperty('chainConfig');
      expect(Array.isArray(suggestion.explanation)).toBe(true);
    });

    it('should keep a crest factor where the downmix cancels in WASM', () => {
      const sampleRate = 48000;
      const left = new Float32Array(sampleRate);
      const right = new Float32Array(sampleRate);
      const downmix = new Float32Array(sampleRate);
      for (let i = 0; i < left.length; i++) {
        left[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
        right[i] = -left[i];
        downmix[i] = 0.5 * (left[i] + right[i]);
      }

      expect(meteringCrestFactorDbStereo({ left, right, sampleRate })).toBeCloseTo(3.01, 1);
      // The anti-phase pair cancels to silence in the downmix, which reports the
      // 0 dB neutral rather than the pair's real 3 dB crest factor.
      expect(meteringCrestFactorDb(downmix, sampleRate)).toBeCloseTo(0, 5);
    });

    it('should reject a stereo pair whose channel lengths differ in WASM', () => {
      const sampleRate = 48000;
      const left = new Float32Array(1024).fill(0.1);
      const right = new Float32Array(512).fill(0.1);
      expect(() => masteringStreamingPreviewStereo({ left, right, sampleRate })).toThrow();
      expect(() => masteringAudioProfileStereo({ left, right, sampleRate })).toThrow();
      expect(() => masteringAssistantSuggestStereo({ left, right, sampleRate })).toThrow();
      expect(() => meteringCrestFactorDbStereo({ left, right, sampleRate })).toThrow();
    });

    it('should expose mixing presets and stereo mix in WASM', () => {
      expect(mixingScenePresetNames()).toContain('vocalReverbSend');
      expect(mixingScenePresetJson('vocalReverbSend')).toContain('"vocal"');

      const left = new Float32Array([1, 1]);
      const right = new Float32Array([0, 0]);
      const result = mixStereo([left], [right], 48000, { inputTrimDb: 6.0206, faderDb: -6.0206 });
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      // +6.02 dB trim and -6.02 dB fader cancel to unity. With the Balance pan
      // law no longer attenuating a centered signal by 3 dB, the output passes
      // through at unity instead of sqrt(0.5).
      expect(result.left[0]).toBeCloseTo(1.0, 2);
      expect(result.left[1]).toBeCloseTo(1.0, 2);
      expect(Array.from(result.right)).toEqual([0, 0]);
      expect(result.meters).toHaveLength(1);
      expect(Number.isFinite(result.meters[0].peakDbL)).toBe(true);
      expect(typeof result.meters[0].likelyMonoCompatible).toBe('boolean');
    });
  });
});
