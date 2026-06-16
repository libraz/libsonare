/**
 * Tests for the v1.2 feature additions exposed in WASM:
 * onset envelope, Fourier tempogram, tempogram ratio, NNLS chroma,
 * and EBU R128 LUFS metering.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  analyzeMelody,
  analyzeSections,
  bassChroma,
  chromaCens,
  cqt,
  fourierTempogram,
  hybridCqt,
  init,
  lufs,
  Mixer,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
  mixingScenePresetJson,
  momentaryLufs,
  nnlsChroma,
  noteStretch,
  onsetEnvelope,
  onsetStrengthMulti,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  plp,
  pseudoCqt,
  RealtimeEngine,
  StreamingEqualizer,
  shortTermLufs,
  stft,
  tempogram,
  tempogramRatio,
  voiceChange,
  vqt,
} from '../dist/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number, amp = 0.5): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = amp * Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

function allFinite(arr: Float32Array | number[]): boolean {
  for (const x of arr) {
    if (!Number.isFinite(x)) {
      return false;
    }
  }
  return true;
}

describe('v1.2 feature additions (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const signal = generateSine(220, SR, 3.0);

  describe('onset envelope', () => {
    it('returns a finite per-frame envelope sized to the audio length', () => {
      const env = onsetEnvelope(signal, SR);
      // ~ ceil(signal.length / hop_length=512). Allow ±2 for librosa-style padding/centering.
      const expectedFrames = Math.ceil(signal.length / 512);
      expect(env.length).toBeGreaterThanOrEqual(expectedFrames - 2);
      expect(env.length).toBeLessThanOrEqual(expectedFrames + 2);
      expect(allFinite(env)).toBe(true);
    });

    it('returns finite multi-band onset strength aligned to frames', () => {
      const nBands = 4;
      const result = onsetStrengthMulti(signal, SR, 1024, 256, 48, nBands);
      expect(result.nBands).toBe(nBands);
      expect(result.nFrames).toBeGreaterThan(0);
      expect(result.data.length).toBe(result.nBands * result.nFrames);
      expect(allFinite(result.data)).toBe(true);
      for (const v of result.data) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });
  });

  describe('tempogram family', () => {
    it('Fourier tempogram returns an [nBins x nFrames] matrix of non-negative magnitudes', () => {
      const env = onsetEnvelope(signal, SR);
      const ft = fourierTempogram(env, SR);
      // Default win_length is 384 → n_bins = win_length / 2 + 1 = 193.
      expect(ft.nBins).toBe(193);
      // Fourier tempogram frames align to the onset-envelope length.
      expect(ft.nFrames).toBe(env.length);
      expect(ft.data.length).toBe(ft.nBins * ft.nFrames);
      expect(allFinite(ft.data)).toBe(true);
      // Magnitudes must be non-negative.
      for (const v of ft.data) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });

    it('tempogram ratio returns one finite value per default factor', () => {
      const env = onsetEnvelope(signal, SR);
      const tg = tempogram(env, SR);
      const ratios = tempogramRatio(tg.data, tg.winLength, SR);
      // Defaults to {0.5, 1, 2, 3, 4}.
      expect(ratios.length).toBe(5);
      expect(allFinite(ratios)).toBe(true);
      // Ratio values are normalized magnitudes — non-negative.
      for (const v of ratios) {
        expect(v).toBeGreaterThanOrEqual(0);
      }
    });

    it('plp returns a pulse curve aligned to the envelope', () => {
      const env = onsetEnvelope(signal, SR);
      const pulse = plp(env, SR);
      expect(pulse.length).toBe(env.length);
      expect(allFinite(pulse)).toBe(true);
    });
  });

  describe('NNLS chroma', () => {
    it('returns a 12 x nFrames matrix with normalized non-negative entries', () => {
      const result = nnlsChroma(signal, SR);
      expect(result.nChroma).toBe(12);
      // Aligns to the same hop grid as onset_envelope (~ signal.length / 512).
      const expectedFrames = Math.ceil(signal.length / 512);
      expect(result.nFrames).toBeGreaterThanOrEqual(expectedFrames - 2);
      expect(result.nFrames).toBeLessThanOrEqual(expectedFrames + 2);
      expect(result.data.length).toBe(result.nChroma * result.nFrames);
      expect(allFinite(result.data)).toBe(true);
      // NNLS chroma is non-negative and normalized to [0, 1] per librosa convention.
      for (const v of result.data) {
        expect(v).toBeGreaterThanOrEqual(0);
        expect(v).toBeLessThanOrEqual(1.0 + 1e-5);
      }
    });

    it('computes CENS and bass chroma matrices', () => {
      const cens = chromaCens(signal, SR, 512, 12);
      expect(cens.nChroma).toBe(12);
      expect(cens.nFrames).toBeGreaterThan(0);
      expect(cens.features.length).toBe(cens.nChroma * cens.nFrames);
      expect(cens.sampleRate).toBe(SR);
      expect(cens.hopLength).toBe(512);
      expect(allFinite(cens.features)).toBe(true);
      expect(allFinite(cens.meanEnergy)).toBe(true);

      const bass = bassChroma(signal, SR, 512, 12);
      expect(bass.nChroma).toBe(12);
      expect(bass.nFrames).toBeGreaterThan(0);
      expect(bass.features.length).toBe(bass.nChroma * bass.nFrames);
      expect(bass.sampleRate).toBe(SR);
      expect(bass.hopLength).toBe(512);
      expect(allFinite(bass.features)).toBe(true);
      expect(allFinite(bass.meanEnergy)).toBe(true);
    });
  });

  describe('LUFS metering', () => {
    it('returns finite integrated/momentary/short-term/range values', () => {
      const result = lufs(signal, SR);
      expect(Number.isFinite(result.integratedLufs)).toBe(true);
      expect(Number.isFinite(result.momentaryLufs)).toBe(true);
      expect(Number.isFinite(result.shortTermLufs)).toBe(true);
      expect(Number.isFinite(result.loudnessRange)).toBe(true);
    });

    it('reports a louder signal as higher integrated LUFS', () => {
      const quiet = lufs(generateSine(220, SR, 3.0, 0.1), SR);
      const loud = lufs(generateSine(220, SR, 3.0, 0.8), SR);
      expect(loud.integratedLufs).toBeGreaterThan(quiet.integratedLufs);
    });

    it('momentary/short-term series are non-empty, finite, and within audible LUFS range', () => {
      const mom = momentaryLufs(signal, SR);
      const st = shortTermLufs(signal, SR);
      // 3-second 0.5-amplitude tone -> at least a couple of 400ms momentary windows
      // and at least one 3s short-term window.
      expect(mom.length).toBeGreaterThan(2);
      expect(st.length).toBeGreaterThanOrEqual(1);
      expect(allFinite(mom)).toBe(true);
      expect(allFinite(st)).toBe(true);
      // Every value should land in a sensible loudness range; a 0.5-amp tone is
      // ~-9 LUFS, and -Infinity (a previous silent-input bug we want to catch)
      // would fail the > -100 check.
      for (const v of mom) {
        expect(v).toBeGreaterThan(-100);
        expect(v).toBeLessThan(0);
      }
      for (const v of st) {
        expect(v).toBeGreaterThan(-100);
        expect(v).toBeLessThan(0);
      }
    });
  });

  describe('editing DSP (pitch correct / note stretch / voice change)', () => {
    function peakAmplitude(arr: Float32Array): number {
      let p = 0;
      for (const v of arr) {
        const a = Math.abs(v);
        if (a > p) {
          p = a;
        }
      }
      return p;
    }

    it('pitchCorrectToMidi preserves length and stays within audio range', () => {
      const out = pitchCorrectToMidi(signal, SR, 57, 60);
      expect(out).toBeInstanceOf(Float32Array);
      // Phase-vocoder pitch correction returns the input length verbatim.
      expect(out.length).toBe(signal.length);
      expect(allFinite(out)).toBe(true);
      const peak = peakAmplitude(out);
      // Input peak is 0.5; allow up to ~1.0 for pitch-correction overshoot but
      // catch obvious blow-ups / silence.
      expect(peak).toBeGreaterThan(0.05);
      expect(peak).toBeLessThanOrEqual(1.0);
    });

    it('pitchCorrectToMidiTimevarying follows a caller-supplied F0 contour', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const out = pitchCorrectToMidiTimevarying(signal, f0, 60, SR, hop);
      expect(out).toBeInstanceOf(Float32Array);
      expect(out.length).toBe(signal.length);
      expect(allFinite(out)).toBe(true);

      // Optional voiced / voicedProb arrays are accepted.
      const voiced = new Int32Array(nFrames).fill(1);
      const voicedProb = new Float32Array(nFrames).fill(1);
      const out2 = pitchCorrectToMidiTimevarying(signal, f0, 60, SR, hop, voiced, voicedProb);
      expect(out2.length).toBe(signal.length);
    });

    it('pitchCorrectToMidiTimevarying rejects mismatched companion arrays', () => {
      const hop = 512;
      const nFrames = Math.floor(signal.length / hop) + 1;
      const f0 = new Float32Array(nFrames).fill(220);
      const tooShortVoiced = new Int32Array(nFrames - 1).fill(1);
      const tooShortProb = new Float32Array(nFrames - 1).fill(1);
      expect(() =>
        pitchCorrectToMidiTimevarying(signal, f0, 60, SR, hop, tooShortVoiced),
      ).toThrow();
      expect(() =>
        pitchCorrectToMidiTimevarying(signal, f0, 60, SR, hop, undefined, tooShortProb),
      ).toThrow();
    });

    it('noteStretch lengthens the buffer by the stretch ratio', () => {
      const out = noteStretch(signal, SR, {
        onsetSample: 0,
        offsetSample: signal.length,
        stretchRatio: 1.5,
      });
      expect(out).toBeInstanceOf(Float32Array);
      expect(allFinite(out)).toBe(true);
      // Stretching by 1.5 must lengthen the output (allow ±2% for windowing).
      const expected = Math.round(signal.length * 1.5);
      expect(out.length).toBeGreaterThanOrEqual(Math.floor(expected * 0.98));
      expect(out.length).toBeLessThanOrEqual(Math.ceil(expected * 1.02));
      const peak = peakAmplitude(out);
      expect(peak).toBeGreaterThan(0.05);
      expect(peak).toBeLessThanOrEqual(1.0);
    });

    it('voiceChange preserves length and amplitude range', () => {
      const out = voiceChange(signal, SR, { pitchSemitones: 2, formantFactor: 1.1 });
      expect(out).toBeInstanceOf(Float32Array);
      expect(allFinite(out)).toBe(true);
      // Voice changer is duration-preserving; allow a tiny boundary slack.
      expect(out.length).toBeGreaterThanOrEqual(signal.length - 4);
      expect(out.length).toBeLessThanOrEqual(signal.length + 4);
      const peak = peakAmplitude(out);
      expect(peak).toBeGreaterThan(0.05);
      expect(peak).toBeLessThanOrEqual(1.0);
    });

    it('exposes the editing methods on the Audio class with the same shapes', () => {
      const audio = Audio.fromBuffer(signal, SR);
      const corrected = audio.pitchCorrectToMidi(57, 60);
      const stretched = audio.noteStretch({
        onsetSample: 0,
        offsetSample: signal.length,
        stretchRatio: 1.5,
      });
      const voiced = audio.voiceChange({ pitchSemitones: 2, formantFactor: 1.1 });
      expect(corrected.length).toBe(signal.length);
      expect(stretched.length).toBeGreaterThan(signal.length);
      expect(voiced.length).toBeGreaterThanOrEqual(signal.length - 4);
      expect(allFinite(corrected)).toBe(true);
      expect(allFinite(stretched)).toBe(true);
      expect(allFinite(voiced)).toBe(true);
    });
  });

  describe('Mixer (scene-based routing)', () => {
    const BLOCK = 512;

    function blockEnergy(r: { left: Float32Array; right: Float32Array }): number {
      let sum = 0;
      for (let i = 0; i < r.left.length; i++) {
        sum += r.left[i] * r.left[i] + r.right[i] * r.right[i];
      }
      return sum;
    }

    it('routes a send through reverb and back to master', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      mixer.compile();

      // Strip 0 = vocal, strip 1 = reverb return. Impulse into vocal, silence into return.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);

      const energies: number[] = [];
      for (let block = 0; block < 16; block++) {
        const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
        energies.push(blockEnergy(out));
        vocalL[0] = 0.0;
        vocalR[0] = 0.0;
      }

      // Block 0 carries the dry hit; later blocks carry the reverb tail.
      expect(energies[0]).toBeGreaterThan(1e-6);
      const tail = energies.slice(4).reduce((a, b) => a + b, 0);
      expect(tail).toBeGreaterThan(1e-6);

      mixer.delete();
    });

    it('schedules insert-parameter automation without throwing', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      mixer.compile();

      expect(mixer.stripCount()).toBeGreaterThan(0);

      // Strip 0 (vocal) has pre-fader inserts (insert 0 = eq.parametric).
      // Schedule a linear ramp on param 0 over the first second of audio.
      expect(() => mixer.scheduleInsertAutomation(0, 0, 0, 0, 0.0, 'linear')).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, 0, 48000, 1.0, 'exponential'),
      ).not.toThrow();

      // Out-of-range strip index must throw.
      expect(() => mixer.scheduleInsertAutomation(999, 0, 0, 0, 0.0)).toThrow();

      // Processing after scheduling still produces output.
      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(BLOCK);
      const silentR = new Float32Array(BLOCK);
      const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      mixer.delete();
    });

    it('round-trips the scene topology to JSON', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      const scene = mixer.toSceneJson();
      expect(scene).toContain('"vocal-verb"');
      expect(scene).toContain('"vocal-verb-return"');
      expect(scene).toContain('"destinationBusId":"vocal-verb"');

      const restored = Mixer.fromSceneJson(scene, 48000, BLOCK);
      restored.compile();
      restored.delete();
      mixer.delete();
    });

    it('processes 128-sample render quanta through the single WASM module', () => {
      const quantum = 128;
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, quantum);
      mixer.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const out = mixer.processStereo([vocalL, returnL], [vocalR, returnR]);
      expect(out.left.length).toBe(quantum);
      expect(out.right.length).toBe(quantum);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      mixer.delete();
    });

    it('can render a block into caller-owned output arrays', () => {
      const quantum = 128;
      const scene = mixingScenePresetJson('vocalReverbSend');
      const offline = Mixer.fromSceneJson(scene, 48000, quantum);
      const inplace = Mixer.fromSceneJson(scene, 48000, quantum);
      offline.compile();
      inplace.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const outL = new Float32Array(quantum);
      const outR = new Float32Array(quantum);
      inplace.processStereoInto([vocalL, returnL], [vocalR, returnR], outL, outR);

      expect(Array.from(outL)).toEqual(Array.from(expected.left));
      expect(Array.from(outR)).toEqual(Array.from(expected.right));

      offline.delete();
      inplace.delete();
    });

    it('can render through reusable WASM-heap realtime buffers', () => {
      const quantum = 128;
      const scene = mixingScenePresetJson('vocalReverbSend');
      const offline = Mixer.fromSceneJson(scene, 48000, quantum);
      const realtime = Mixer.fromSceneJson(scene, 48000, quantum);
      offline.compile();
      realtime.compile();

      const vocalL = new Float32Array(quantum);
      const vocalR = new Float32Array(quantum);
      const returnL = new Float32Array(quantum);
      const returnR = new Float32Array(quantum);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const buffer = realtime.createRealtimeBuffer();
      buffer.leftInputs[0].set(vocalL);
      buffer.rightInputs[0].set(vocalR);
      buffer.leftInputs[1].set(returnL);
      buffer.rightInputs[1].set(returnR);
      buffer.process();

      expect(Array.from(buffer.outLeft)).toEqual(Array.from(expected.left));
      expect(Array.from(buffer.outRight)).toEqual(Array.from(expected.right));

      offline.delete();
      realtime.delete();
    });

    it('exposes live mixer controls and meter readers', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, BLOCK);
      const vocalIndex = mixer.stripById('vocal');
      expect(vocalIndex).toBe(0);
      // stripById returns number | null; an unknown id resolves to null.
      expect(mixer.stripById('definitely-not-a-strip')).toBeNull();
      const vocal = vocalIndex as number;

      mixer.setSoloSafe(vocal, true);
      mixer.setSoloed(vocal, false);
      mixer.setPolarityInvert(vocal, false, true);
      mixer.setPanLaw(vocal, 'linear0dB');
      mixer.setChannelDelaySamples(vocal, 0);
      mixer.setVcaOffsetDb(vocal, -1);
      mixer.setDualPan(vocal, -0.2, 0.2);
      const sendIndex = mixer.addSend(vocal, 'wasm-extra-send', 'vocal-verb', -24, 'postFader');
      mixer.setSendDb(vocal, sendIndex, -18);
      mixer.scheduleFaderAutomation(vocal, 0, -6, 'linear');
      mixer.schedulePanAutomation(vocal, 0, 0, 'linear');
      mixer.scheduleWidthAutomation(vocal, 0, 1, 'linear');
      mixer.scheduleSendAutomation(vocal, sendIndex, 0, -12, 'linear');
      mixer.compile();

      const vocalL = new Float32Array(BLOCK);
      const vocalR = new Float32Array(BLOCK);
      const returnL = new Float32Array(BLOCK);
      const returnR = new Float32Array(BLOCK);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const out = mixer.processStereo([vocalL, returnL], [vocalR, returnR]);
      expect(blockEnergy(out)).toBeGreaterThan(0);

      expect(Number.isFinite(mixer.meterTap(vocal, 'preFader').peakDbL)).toBe(true);
      expect(Number.isFinite(mixer.stripMeter(vocal, 'postFader').rmsDbL)).toBe(true);
      expect(mixer.readGoniometerLatest(vocal, 8).length).toBeGreaterThan(0);

      mixer.delete();
    });
  });

  describe('Audio class methods', () => {
    it('exposes onsetEnvelope/nnlsChroma/lufs/momentaryLufs/shortTermLufs with finite values', () => {
      const audio = Audio.fromBuffer(signal, SR);
      const env = audio.onsetEnvelope();
      expect(env.length).toBeGreaterThan(0);
      expect(allFinite(env)).toBe(true);

      const nc = audio.nnlsChroma();
      expect(nc.nChroma).toBe(12);
      expect(nc.data.length).toBe(nc.nChroma * nc.nFrames);
      expect(allFinite(nc.data)).toBe(true);

      const integrated = audio.lufs().integratedLufs;
      expect(Number.isFinite(integrated)).toBe(true);
      expect(integrated).toBeGreaterThan(-100);
      expect(integrated).toBeLessThan(0);

      const mom = audio.momentaryLufs();
      const st = audio.shortTermLufs();
      expect(mom.length).toBeGreaterThan(0);
      expect(st.length).toBeGreaterThan(0);
      expect(allFinite(mom)).toBe(true);
      expect(allFinite(st)).toBe(true);
    });
  });

  describe('RealtimeEngine parameter/transport parity', () => {
    it('honors tunable queue capacities and reads back transport state', () => {
      const engine = new RealtimeEngine(48000, 128, 32, 32);
      engine.setTempo(120);
      engine.addParameter({
        id: 5,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0, // canonical AutomationCurve::Linear
      });

      // Sample-accurate + smoothed parameter changes queue without throwing.
      expect(() => engine.setParameter(5, 6)).not.toThrow();
      expect(() => engine.setParameterSmoothed(5, -3, -1)).not.toThrow();
      expect(engine.midiCcBindingCount()).toBe(0);
      expect(() => engine.bindMidiCc(0, 74, 5, { minValue: -60, maxValue: 12 })).not.toThrow();
      expect(engine.midiCcBindingCount()).toBe(1);
      engine.clearMidiCcBindings();
      expect(engine.midiCcBindingCount()).toBe(0);
      expect(() => engine.setMidiFx(0, '{"transpose_semitones":12}')).not.toThrow();
      expect(() =>
        engine.setMidiFx(
          0,
          '{"arpeggiator_intervals":[0,12],"arpeggiator_step_ppq":0.25,"arpeggiator_gate_ppq":0.125}',
        ),
      ).not.toThrow();
      expect(() => engine.clearMidiFx(0)).not.toThrow();
      expect(() => engine.setMidiFx(0, '{bad json')).toThrow();
      expect(() => engine.setMidiFx(0, '{"quantize_ppq":0}')).toThrow();
      expect(() =>
        engine.setMidiFx(0, '{"arpeggiator_intervals":[],"arpeggiator_step_ppq":0.25}'),
      ).toThrow();
      expect(() =>
        engine.setMidiFx(0, '{"arpeggiator_intervals":[0],"arpeggiator_step_ppq":0}'),
      ).toThrow();
      engine.setMidiInputSource(0);
      expect(engine.midiInputPendingCount()).toBe(0);
      engine.pushMidiInputNoteOn(0, 0, 60, 100, 3);
      expect(engine.midiInputPendingCount()).toBe(1);

      engine.play();
      engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(engine.midiInputPendingCount()).toBe(0);
      engine.clearMidiInputSource();
      expect(() => engine.pushMidiInputNoteOff(0, 0, 60, 0, 0)).toThrow();

      const state = engine.getTransportState();
      expect(state.playing).toBe(true);
      expect(state.sampleRate).toBe(48000);
      expect(state.bpm).toBe(120);
      expect(state.samplePosition).toBe(128);
      engine.destroy();
    });

    it('settles smoothed lane faders before an offline render starts', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(120);
      engine.setTrackLanes([10]);
      const frames = 128 * 8;
      const ones = new Float32Array(frames).fill(1);
      engine.setClips([
        { trackId: 10, channels: [ones, ones], startPpq: 0, lengthSamples: frames },
      ]);
      // Lane 0 fader (reserved mixer namespace) held at -60 dB from ppq 0.
      engine.setAutomationLane(0x4d580001, [{ ppq: 0, value: -60, curveToNext: 2 }]);
      engine.seekSample(0);
      // Priming block with the transport stopped applies the automation target;
      // settleParameters snaps the lane fader smoother to it so the first
      // audible frame renders at -60 dB instead of ramping down from 0 dB.
      engine.process([new Float32Array(128), new Float32Array(128)]);
      engine.settleParameters();
      engine.play();
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.abs(out[0][0])).toBeLessThan(0.01);
      expect(Math.abs(out[0][127])).toBeLessThan(0.01);
      engine.destroy();
    });

    it('routes a lane through its group bus and ducks a lane from a sidechain key', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(120);
      const frames = 128 * 64;
      const pad = new Float32Array(frames).fill(0.5);
      const key = new Float32Array(frames).fill(1.0);
      engine.setTrackBuses([{ busId: 5, gainDb: -6.0206 }]);
      engine.setTrackLanes([{ trackId: 10, outputBusId: 5 }, { trackId: 20 }]);
      engine.setClips([
        { trackId: 10, channels: [pad, pad], startPpq: 0, lengthSamples: frames },
        { trackId: 20, channels: [key, key], startPpq: 0, lengthSamples: frames },
      ]);
      // Ducking insert on the grouped lane keyed from the other lane.
      engine.setTrackStripJson(
        10,
        JSON.stringify({
          version: 1,
          strips: [
            {
              id: 'track-10',
              inserts: [
                {
                  slot: 'post',
                  processor: 'dynamics.duckingProcessor',
                  params: { thresholdDb: -30, ratio: 20, attackMs: 1, releaseMs: 50, rangeDb: 24 },
                },
              ],
            },
          ],
          buses: [],
          connections: [],
        }),
      );
      engine.setLaneSidechain(10, 0, 20);
      // Keep the key lane out of the measured mix; the key snapshot is
      // pre-fader, so the binding still sees it at full level.
      engine.setParameter(0x4d580101, -120, -1);
      engine.seekSample(0);
      engine.process([new Float32Array(128), new Float32Array(128)]);
      engine.settleParameters();
      engine.play();
      let out: Float32Array[] = [];
      for (let block = 0; block < 40; block++) {
        out = engine.process([new Float32Array(128), new Float32Array(128)]);
      }
      const ducked = Math.abs(out[0][127]);
      // Pad 0.5 through the -6 dB group bus would sit at ~0.25 unducked; the
      // hot key must push it far below that.
      expect(ducked).toBeLessThan(0.07);
      engine.setLaneSidechain(10, 0, 0);
      engine.destroy();
    });

    it('seeks markers at a scheduled render frame', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(60);
      engine.setMarkers([{ id: 9, ppq: 4, name: 'verse' }]);
      // Seek to marker 9 (4 quarter notes at 60bpm = 4s = 192000 samples),
      // scheduled at the head of the block.
      expect(() => engine.seekMarker(9, -1)).not.toThrow();
      engine.play();
      engine.process([new Float32Array(128), new Float32Array(128)]);
      const state = engine.getTransportState();
      expect(state.samplePosition).toBe(192000 + 128);
      engine.destroy();
    });

    it('round-trips marker kind and key signature through setMarkers/markerByIndex', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setMarkers([
        { id: 1, ppq: 0, name: 'intro' },
        { id: 2, ppq: 4, name: 'Bb minor', kind: 4, keyFifths: -2, keyMinor: true },
      ]);
      expect(engine.markerCount()).toBe(2);
      const plain = engine.markerByIndex(0);
      expect(plain.kind).toBe(0);
      expect(plain.keyFifths).toBe(0);
      expect(plain.keyMinor).toBe(false);
      const key = engine.markerByIndex(1);
      expect(key.name).toBe('Bb minor');
      expect(key.kind).toBe(4);
      expect(key.keyFifths).toBe(-2);
      expect(key.keyMinor).toBe(true);
      engine.destroy();
    });
  });

  describe('StreamingEqualizer sidechain parity', () => {
    it('accepts mono/stereo sidechain and clears it', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 128 });
      try {
        const key = new Float32Array(128).fill(0.5);
        expect(() => eq.setSidechainMono(key)).not.toThrow();
        expect(() => eq.setSidechainStereo(key, key)).not.toThrow();
        expect(() => eq.clearSidechain()).not.toThrow();
        // Mismatched stereo lengths must throw before reaching WASM.
        expect(() => eq.setSidechainStereo(key, new Float32Array(64))).toThrow();
        // Processing still works after a sidechain round-trip.
        const out = eq.processMono(new Float32Array(128).fill(0.25));
        expect(out.length).toBe(128);
      } finally {
        eq.delete();
      }
    });
  });

  describe('Mixer imperative topology', () => {
    it('adds/removes buses and VCA groups and reports counts', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 128);
      try {
        const busesBefore = mixer.busCount();
        mixer.addBus('parallel-comp', 'aux');
        expect(mixer.busCount()).toBe(busesBefore + 1);
        mixer.removeBus('parallel-comp');
        expect(mixer.busCount()).toBe(busesBefore);

        const vcasBefore = mixer.vcaGroupCount();
        mixer.addVcaGroup('all', -3, ['vocal']);
        expect(mixer.vcaGroupCount()).toBe(vcasBefore + 1);
        mixer.setVcaGroupGainDb('all', -8);
        const scene = JSON.parse(mixer.toSceneJson());
        const group = scene.vcaGroups.find((entry: { id: string }) => entry.id === 'all');
        expect(group.gainDb).toBe(-8);
        mixer.removeVcaGroup('all');
        expect(mixer.vcaGroupCount()).toBe(vcasBefore);

        mixer.compile();
      } finally {
        mixer.delete();
      }
    });
  });

  describe('sections / melody / CQT / VQT parity', () => {
    it('passes fmin/fmax through inverse Mel and MFCC paths', () => {
      const tone = generateSine(440, SR, 0.5);
      const nFft = 1024;
      const hop = 256;
      const nMels = 40;
      const fmin = 80;
      const fmax = 4000;

      const mel = melSpectrogram(tone, SR, nFft, hop, nMels);
      const stft = melToStft(mel.power, nMels, mel.nFrames, SR, nFft, fmin, fmax);
      expect(stft.nBins).toBe(nFft / 2 + 1);
      expect(stft.nFrames).toBe(mel.nFrames);
      expect(allFinite(stft.power)).toBe(true);

      const audio = melToAudio(mel.power, nMels, mel.nFrames, SR, nFft, hop, fmin, fmax, 2);
      expect(audio.length).toBeGreaterThan(0);
      expect(allFinite(audio)).toBe(true);

      const coeffs = mfcc(tone, SR, nFft, hop, nMels, 13);
      const mfccAudio = mfccToAudio(
        coeffs.coefficients,
        13,
        coeffs.nFrames,
        nMels,
        SR,
        nFft,
        hop,
        fmin,
        fmax,
        2,
      );
      expect(mfccAudio.length).toBeGreaterThan(0);
      expect(allFinite(mfccAudio)).toBe(true);
    });

    it('passes htk through inverse Mel paths', () => {
      const tone = generateSine(440, SR, 0.25);
      const nFft = 1024;
      const hop = 256;
      const nMels = 40;
      const mel = melSpectrogram(tone, SR, nFft, hop, nMels, 0, 0, true);

      const slaney = melToStft(mel.power, nMels, mel.nFrames, SR, nFft, 0, 0, false);
      const htk = melToStft(mel.power, nMels, mel.nFrames, SR, nFft, 0, 0, true);
      expect(htk.nBins).toBe(nFft / 2 + 1);
      expect(htk.nFrames).toBe(mel.nFrames);
      expect(allFinite(htk.power)).toBe(true);

      let diff = 0;
      for (let i = 0; i < htk.power.length; i++) {
        diff += (slaney.power[i] - htk.power[i]) ** 2;
      }
      expect(diff).toBeGreaterThan(1e-6);

      const audio = melToAudio(mel.power, nMels, mel.nFrames, SR, nFft, hop, 0, 0, 2, true);
      expect(audio.length).toBeGreaterThan(0);
      expect(allFinite(audio)).toBe(true);
    });

    it('computes CQT and VQT magnitude grids', () => {
      const cqtResult = cqt(signal, SR, 512, 32.7, 24, 12);
      expect(cqtResult.nBins).toBe(24);
      expect(cqtResult.nFrames).toBeGreaterThan(0);
      expect(cqtResult.frequencies.length).toBe(24);
      expect(cqtResult.magnitude.length).toBe(cqtResult.nBins * cqtResult.nFrames);
      expect(cqtResult.sampleRate).toBe(SR);
      expect(allFinite(cqtResult.magnitude)).toBe(true);

      const vqtResult = vqt(signal, SR, 512, 32.7, 24, 12, 10);
      expect(vqtResult.nBins).toBe(24);
      expect(vqtResult.magnitude.length).toBe(vqtResult.nBins * vqtResult.nFrames);
      expect(allFinite(vqtResult.magnitude)).toBe(true);

      const pseudo = pseudoCqt(signal, SR, 512, 32.7, 24, 12);
      expect(pseudo.nBins).toBe(24);
      expect(pseudo.frequencies.length).toBe(24);
      expect(pseudo.magnitude.length).toBe(pseudo.nBins * pseudo.nFrames);
      expect(pseudo.sampleRate).toBe(SR);
      expect(allFinite(pseudo.magnitude)).toBe(true);

      const hybrid = hybridCqt(signal, SR, 512, 32.7, 24, 12);
      expect(hybrid.nBins).toBe(24);
      expect(hybrid.frequencies.length).toBe(24);
      expect(hybrid.magnitude.length).toBe(hybrid.nBins * hybrid.nFrames);
      expect(hybrid.sampleRate).toBe(SR);
      expect(allFinite(hybrid.magnitude)).toBe(true);
    });

    it('guards music feature wrappers before native analysis', () => {
      expect(() => nnlsChroma(new Float32Array([Number.NaN]), SR)).toThrow(/NaN|Inf/);
      expect(() => cqt(signal, 7999)).toThrow(/sampleRate/);
      expect(() => cqt(signal, SR, 0)).toThrow(/hopLength/);
      expect(() => vqt(signal, SR, 512, 32.7, 24, 12, Number.NaN)).toThrow(/gamma/);
      expect(() => analyzeSections(signal, SR, { minSectionSec: 0 })).toThrow(/minSectionSec/);
      expect(() => analyzeMelody(signal, SR, { fmin: 400, fmax: 200 })).toThrow(/fmax/);
      expect(() => onsetEnvelope(new Float32Array(), SR)).toThrow(/empty/);
      expect(() => fourierTempogram(new Float32Array([1, Number.POSITIVE_INFINITY]), SR)).toThrow(
        /NaN|Inf/,
      );
      expect(() => tempogramRatio(new Float32Array([1, 2]), 0, SR)).toThrow(/winLength/);
    });

    it('guards spectrogram and inverse wrapper shape inputs', () => {
      expect(() => stft(new Float32Array([0, Number.NaN]), SR)).toThrow(/NaN|Inf/);
      expect(() => melSpectrogram(signal, 0)).toThrow(/sampleRate/);
      expect(() => melSpectrogram(signal, SR, 1024, 256, 40, 5000, 4000)).toThrow(/fmax/);

      expect(() => melToStft(new Float32Array(5), 2, 3, SR, 1024)).toThrow(/length/);
      expect(() => melToAudio(new Float32Array([1, Number.NaN, 2, 3]), 2, 2, SR, 1024)).toThrow(
        /NaN|Inf/,
      );
      expect(() => mfccToMel(new Float32Array(6), 0, 3, 40)).toThrow(/nMfcc/);
      expect(() => mfccToAudio(new Float32Array(6), 2, 3, 40, 7999)).toThrow(/sampleRate/);
    });

    it('analyzes melody contour with summary stats', () => {
      // 440 Hz tone is squarely within the default melody fmin/fmax range.
      const tone = generateSine(440, SR, 1.0);
      const melody = analyzeMelody(tone, SR);
      expect(Array.isArray(melody.points)).toBe(true);
      expect(melody.points.length).toBeGreaterThan(0);
      expect(Number.isFinite(melody.meanFrequency)).toBe(true);
      expect(Number.isFinite(melody.pitchRangeOctaves)).toBe(true);
      expect(Number.isFinite(melody.pitchStability)).toBe(true);
      expect(Number.isFinite(melody.vibratoRate)).toBe(true);
      const first = melody.points[0];
      expect(Number.isFinite(first.time)).toBe(true);
      expect(Number.isFinite(first.frequency)).toBe(true);
      expect(Number.isFinite(first.confidence)).toBe(true);
    });

    it('analyzes song-structure sections', () => {
      const sections = analyzeSections(signal, SR);
      expect(Array.isArray(sections)).toBe(true);
      for (const section of sections) {
        expect(typeof section.name).toBe('string');
        expect(Number.isFinite(section.start)).toBe(true);
        expect(Number.isFinite(section.end)).toBe(true);
        expect(section.end).toBeGreaterThanOrEqual(section.start);
        expect(Number.isFinite(section.energyLevel)).toBe(true);
        expect(Number.isFinite(section.confidence)).toBe(true);
      }
    });
  });
});
