import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  ErrorCode,
  init,
  Mixer,
  mixingScenePresetJson,
  Project,
  SampleBank,
  SonareError,
  StreamAnalyzer,
} from '../dist/index.js';

const SR = 48000;
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
});
