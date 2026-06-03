import { describe, expect, it } from 'vitest';
import {
  Audio,
  noteStretch,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  validateRealtimeVoiceChangerPresetJson,
  voiceChange,
  voiceChangeRealtime,
  voiceCharacterPresetId,
} from '../src/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('editing effects', () => {
  const tone = generateSine(440, SR, 0.5);

  it('pitchCorrectToMidi returns a non-empty Float32Array', () => {
    // 440 Hz is MIDI 69 (A4); correct toward MIDI 71 (B4).
    const result = pitchCorrectToMidi(tone, SR, 69, 71);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('pitchCorrectToMidiTimevarying follows a caller-supplied F0 contour', () => {
    const hop = 512;
    const nFrames = Math.floor(tone.length / hop) + 1;
    const f0 = new Float32Array(nFrames).fill(440);
    const result = pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBe(tone.length);
    expect(result.every((x) => Number.isFinite(x))).toBe(true);

    // Optional voiced / voicedProb arrays are accepted.
    const voiced = new Int32Array(nFrames).fill(1);
    const voicedProb = new Float32Array(nFrames).fill(1);
    const result2 = pitchCorrectToMidiTimevarying(tone, f0, 71, SR, hop, voiced, voicedProb);
    expect(result2.length).toBe(tone.length);
  });

  it('noteStretch returns a non-empty Float32Array', () => {
    const result = noteStretch(tone, SR, 0, tone.length, 1.5);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('voiceChange returns a non-empty Float32Array', () => {
    const result = voiceChange(tone, SR, 2, 1.1);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('RealtimeVoiceChanger processes blocks and exposes presets', () => {
    const changer = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 128,
      channels: 1,
      preset: 'bright-idol',
    });
    const input = tone.subarray(0, 128);
    const output = new Float32Array(input.length);
    changer.processMonoInto(input, output);
    expect(output.some((sample) => Number.isFinite(sample))).toBe(true);
    expect(changer.latencySamples()).toBeGreaterThan(0);
    changer.setConfig('deep-narrator');
    expect(changer.configJson()).toContain('retune');
    changer.destroy();

    const offline = voiceChangeRealtime(tone.subarray(0, 512), SR, 'soft-whisper');
    expect(offline).toBeInstanceOf(Float32Array);
    expect(offline.length).toBe(512);

    // Interleaved stereo path (mirrors the WASM voiceChangeRealtime channels
    // option): a 512-frame stereo buffer is 1024 interleaved samples.
    const stereoIn = new Float32Array(1024);
    for (let i = 0; i < 512; i++) {
      stereoIn[i * 2] = tone[i] ?? 0;
      stereoIn[i * 2 + 1] = tone[i] ?? 0;
    }
    const stereoOut = voiceChangeRealtime(stereoIn, SR, 'soft-whisper', { channels: 2 });
    expect(stereoOut.length).toBe(1024);
    expect(() =>
      voiceChangeRealtime(stereoIn, SR, 'soft-whisper', { channels: 3 as unknown as 2 }),
    ).toThrow(/channels must be 1 or 2/);
    expect(realtimeVoiceChangerPresetNames()).toContain('robot-mascot');
    const presetJson = realtimeVoiceChangerPresetJson('bright-idol');
    expect(presetJson).toContain('bright-idol');
    expect(validateRealtimeVoiceChangerPresetJson(presetJson).ok).toBe(true);
    expect(validateRealtimeVoiceChangerPresetJson('{}').ok).toBe(false);

    // Unknown preset name throws (mirrors WASM/Python) rather than passing an
    // undefined ordinal to the native call.
    expect(() => voiceCharacterPresetId('not-a-preset' as never)).toThrow(
      /Unknown voice character preset/,
    );
  });

  it('exposes editing methods on the Audio class', () => {
    const audio = Audio.fromBuffer(tone, SR);
    expect(audio.pitchCorrectToMidi(69, 71)).toBeInstanceOf(Float32Array);
    expect(audio.noteStretch(0, audio.getLength(), 1.5)).toBeInstanceOf(Float32Array);
    expect(audio.voiceChange(2, 1.1)).toBeInstanceOf(Float32Array);
    audio.destroy();
  });

  it('mono RealtimeVoiceChanger with wetMix=0 returns the dry input (C-1 regression)', () => {
    // Tolerant entry point: pass the full DSP override but only set wetMix=0.
    // The chain must short-circuit to the dry signal.
    const changer = new RealtimeVoiceChanger({
      sampleRate: SR,
      maxBlockSize: 128,
      channels: 1,
      preset: { schemaVersion: 1, id: 'x', dsp: { wetMix: 0 } },
    });
    const input = tone.subarray(0, 128).slice();
    const output = new Float32Array(input.length);
    changer.processMonoInto(input, output);
    for (let i = 0; i < input.length; ++i) {
      expect(output[i]).toBe(input[i]);
    }
    changer.destroy();
  });

  it('WASM-style validate rejects empty objects and required-field omissions (C-2 regression)', () => {
    // The full validator must catch missing dsp / id / name and unknown keys.
    expect(validateRealtimeVoiceChangerPresetJson('{}').ok).toBe(false);
    expect(
      validateRealtimeVoiceChangerPresetJson('{"schemaVersion":1,"id":"x","name":"x"}').ok,
    ).toBe(false);
    expect(
      validateRealtimeVoiceChangerPresetJson(
        '{"schemaVersion":1,"id":"x","name":"x","dsp":{"retune":{"semitones":0,"mix":0,"grainSize":0},"formant":{"factor":1,"amount":0,"body":0,"brightness":0,"nasal":0},"eq":{"highpassHz":80,"bodyDb":0,"presenceDb":0,"airDb":0},"gate":{"thresholdDb":-55,"attackMs":2,"releaseMs":100,"rangeDb":18},"compressor":{"thresholdDb":-22,"ratio":2.5,"attackMs":6,"releaseMs":90,"makeupGainDb":1},"deesser":{"frequencyHz":7200,"thresholdDb":-28,"ratio":3,"rangeDb":8},"reverb":{"mix":0.04,"timeMs":320,"damping":0.55,"seed":1},"limiter":{"ceilingDb":-1,"releaseMs":50}}}',
      ).ok,
    ).toBe(true);
  });

  describe('RealtimeVoiceChanger error paths', () => {
    it.skip('process-before-prepare throws at native layer (skipped: TS constructor always prepares)', () => {
      // The public TypeScript RealtimeVoiceChanger constructor unconditionally
      // calls prepare(), so the unprepared state is unreachable via the public
      // API. The native C++ layer does guard on prepared_ and throws
      // Error("RealtimeVoiceChanger must be prepared before processing"), but
      // that path requires bypassing the TS wrapper to access addon directly.
    });

    it('processMonoInto with oversized block throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        const oversized = new Float32Array(129); // maxBlockSize + 1
        const output = new Float32Array(129);
        expect(() => changer.processMonoInto(oversized, output)).toThrow(/block/);
      } finally {
        changer.destroy();
      }
    });

    it('setConfig with invalid JSON string throws an Error', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        // The native C++ JSON parser rejects malformed JSON and throws.
        expect(() => changer.setConfig('{not valid json}' as unknown as never)).toThrow();
      } finally {
        changer.destroy();
      }
    });

    it('processInterleaved with mismatched channel count throws RangeError', () => {
      // Prepared for 1 channel; passing channels=2 must throw RangeError("invalid channel count").
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: 128,
        channels: 1,
        preset: 'neutral-monitor',
      });
      try {
        // 128 * 2 interleaved samples but only 1 channel was prepared — native
        // binding checks channels <= channels_ and rejects.
        const interleaved = new Float32Array(128 * 2);
        expect(() => changer.processInterleaved(interleaved, 2)).toThrow(/channel/i);
      } finally {
        changer.destroy();
      }
    });
  });

  describe('RealtimeVoiceChanger.processInterleavedInto', () => {
    const FRAMES = 128;
    const CHANNELS = 2;
    const INTERLEAVED = FRAMES * CHANNELS;

    function makeStereoInterleaved(): Float32Array {
      // Distinct sine for L vs R so the test fails if a channel is dropped /
      // mis-deinterleaved. L: 440 Hz, R: 660 Hz.
      const out = new Float32Array(INTERLEAVED);
      for (let i = 0; i < FRAMES; i++) {
        out[i * 2] = Math.sin((2 * Math.PI * 440 * i) / 48000);
        out[i * 2 + 1] = Math.sin((2 * Math.PI * 660 * i) / 48000) * 0.5;
      }
      return out;
    }

    it('writes into a pre-allocated stereo output without allocating', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'bright-idol',
      });
      try {
        // The voice changer reports a non-trivial latency (== retune grain),
        // so the first few blocks may be all-zero look-ahead. Feed enough
        // blocks to flush past latency_samples() before asserting that the
        // chain has produced non-zero output.
        const latencyFrames = changer.latencySamples();
        const blocksToFlush = Math.ceil(latencyFrames / FRAMES) + 1;
        const input = makeStereoInterleaved();
        const output = new Float32Array(INTERLEAVED);
        let anyNonZero = false;
        for (let block = 0; block < blocksToFlush; block++) {
          output.fill(0);
          changer.processInterleavedInto(input, CHANNELS, output);
          for (let i = 0; i < INTERLEAVED; i++) {
            expect(Number.isFinite(output[i])).toBe(true);
            if (output[i] !== 0) {
              anyNonZero = true;
            }
          }
        }
        expect(anyNonZero).toBe(true);
      } finally {
        changer.destroy();
      }
    });

    it('with wetMix=0 is the identity on the interleaved buffer (dry passthrough)', () => {
      // Mirrors the mono C-1 regression: full DSP overridden by wetMix=0 must
      // pass the dry signal through, sample-exact, including for stereo
      // interleaved blocks.
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: { schemaVersion: 1, id: 'x', dsp: { wetMix: 0 } },
      });
      try {
        const input = makeStereoInterleaved();
        const output = new Float32Array(INTERLEAVED);
        changer.processInterleavedInto(input, CHANNELS, output);
        for (let i = 0; i < INTERLEAVED; i++) {
          expect(output[i]).toBe(input[i]);
        }
      } finally {
        changer.destroy();
      }
    });

    it('mismatched input/output lengths throw RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        const input = new Float32Array(INTERLEAVED);
        const tooSmall = new Float32Array(INTERLEAVED - 2);
        expect(() => changer.processInterleavedInto(input, CHANNELS, tooSmall)).toThrow(/length/i);
      } finally {
        changer.destroy();
      }
    });

    it('invalid channel count (0 or > prepared channels) throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        const input = new Float32Array(INTERLEAVED);
        const output = new Float32Array(INTERLEAVED);
        // channels=0 is invalid; cast to bypass the 1|2 TS literal type.
        expect(() => changer.processInterleavedInto(input, 0 as unknown as 1 | 2, output)).toThrow(
          /channel/i,
        );
        // channels=3 exceeds prepared (2) and also doesn't divide 256 evenly.
        expect(() => changer.processInterleavedInto(input, 3 as unknown as 1 | 2, output)).toThrow(
          /channel/i,
        );
      } finally {
        changer.destroy();
      }
    });

    it('block exceeding maxBlockSize throws RangeError', () => {
      const changer = new RealtimeVoiceChanger({
        sampleRate: 48000,
        maxBlockSize: FRAMES,
        channels: CHANNELS,
        preset: 'neutral-monitor',
      });
      try {
        // (FRAMES + 1) frames * 2 channels exceeds maxBlockSize per-frame budget.
        const oversized = new Float32Array((FRAMES + 1) * CHANNELS);
        const output = new Float32Array((FRAMES + 1) * CHANNELS);
        expect(() => changer.processInterleavedInto(oversized, CHANNELS, output)).toThrow(/block/);
      } finally {
        changer.destroy();
      }
    });
  });
});
