import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  ErrorCode,
  HrtfSet,
  init,
  Mixer,
  mixingScenePresetJson,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  Project,
  RealtimeEngine,
  RealtimeVoiceChanger,
  SampleBank,
  SonareError,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
} from '../dist/index.js';

const SR = 48000;

function hrtfBytes(): Uint8Array {
  return new Uint8Array(readFileSync(new URL('../dist/hrtf/default.shrf', import.meta.url)));
}
const BLOCK = 128;

function expectInvalidState(call: () => unknown): void {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(SonareError);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
  expect((caught as SonareError).codeName).toBe('InvalidState');
}

describe('Symbol.dispose / using', () => {
  beforeAll(async () => {
    await init();
  });

  it('exposes a Symbol.dispose key', () => {
    expect(typeof Symbol.dispose).toBe('symbol');
  });

  it('Project releases its handle at the end of a `using` block', () => {
    let captured: Project | undefined;
    {
      using project = new Project();
      captured = project;
      expect(project.toJson().length).toBeGreaterThan(0);
    }
    expectInvalidState(() => captured?.toJson());
  });

  it('Mixer, SampleBank and StreamAnalyzer release through Symbol.dispose', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
    const bank = new SampleBank();
    const analyzer = new StreamAnalyzer();
    for (const handle of [mixer, bank, analyzer]) {
      handle[Symbol.dispose]();
      expect(() => handle.delete()).not.toThrow();
    }
    expectInvalidState(() => mixer.drainTailStereo(BLOCK));
    expectInvalidState(() => bank.sampleCount());
    expectInvalidState(() => analyzer.frameCount());
  });

  it('delete() is idempotent', () => {
    const project = new Project();
    project.delete();
    expect(() => project.delete()).not.toThrow();
    expect(() => project.destroy()).not.toThrow();
    expect(() => project[Symbol.dispose]()).not.toThrow();
  });

  it('use after release throws SonareError InvalidState', () => {
    const project = new Project();
    project.delete();
    expectInvalidState(() => project.toJson());
  });

  it('Audio accepts a no-op dispose', () => {
    const audio = Audio.fromBuffer(new Float32Array([0.5, -0.25]), SR);
    expect(() => audio[Symbol.dispose]()).not.toThrow();
    expect(audio.length).toBe(2);
  });

  const releasedHandleCases: Array<{
    name: string;
    make: () => { delete(): void };
    call: (handle: never) => unknown;
  }> = [
    {
      name: 'PlaybackRenderer',
      make: () => {
        const hrtf = HrtfSet.fromBytes(hrtfBytes());
        const renderer = new PlaybackRenderer({ config: {}, hrtf, sampleRate: SR });
        hrtf.delete();
        return renderer;
      },
      call: (h: PlaybackRenderer) => h.reset(),
    },
    {
      name: 'PlaybackLoudnessMeter',
      make: () => new PlaybackLoudnessMeter(2, SR),
      call: (h: PlaybackLoudnessMeter) => h.integratedLufs(),
    },
    {
      name: 'RealtimeEngine',
      make: () => new RealtimeEngine(SR, BLOCK),
      call: (h: RealtimeEngine) => h.midiInstrumentCount(),
    },
    {
      name: 'RealtimeVoiceChanger',
      make: () => new RealtimeVoiceChanger(),
      call: (h: RealtimeVoiceChanger) => h.reset(),
    },
    {
      name: 'StreamingMasteringChain',
      make: () => new StreamingMasteringChain({}),
      call: (h: StreamingMasteringChain) => h.reset(),
    },
    {
      name: 'StreamingEqualizer',
      make: () => new StreamingEqualizer(),
      call: (h: StreamingEqualizer) => h.clear(),
    },
    {
      name: 'StreamingRetune',
      make: () => new StreamingRetune(),
      call: (h: StreamingRetune) => h.reset(),
    },
    {
      name: 'Mixer',
      make: () => Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK),
      call: (h: Mixer) => h.drainTailStereo(BLOCK),
    },
    { name: 'SampleBank', make: () => new SampleBank(), call: (h: SampleBank) => h.sampleCount() },
    {
      name: 'StreamAnalyzer',
      make: () => new StreamAnalyzer(),
      call: (h: StreamAnalyzer) => h.frameCount(),
    },
    { name: 'Project', make: () => new Project(), call: (h: Project) => h.toJson() },
  ];

  it.each(releasedHandleCases)('$name refuses use after delete() with InvalidState', (testCase) => {
    const handle = testCase.make();
    handle.delete();
    expectInvalidState(() => testCase.call(handle as never));
  });
});
