/**
 * Runtime inputs whose acceptance or refusal class is shared with the Node binding: the same call
 * is accepted, or refused with the same class and text, on both surfaces.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectKey,
  init,
  Mixer,
  mixingScenePreset,
  Project,
  RealtimeEngine,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
} from '../dist/index.js';
import { type ArgumentRefusalClass, expectRefusalOf } from './_helpers';

const SIZE_T_TEXT =
  'must be a finite non-negative integer no greater than Number.MAX_SAFE_INTEGER or the native size_t maximum';

function expectRefusal(run: () => unknown, kind: ArgumentRefusalClass, message: RegExp): void {
  expect(expectRefusalOf(run, kind).message).toMatch(message);
}

function exactly(text: string): RegExp {
  return new RegExp(`^${text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`);
}

describe('runtime refusal parity (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses empty MIDI file bytes as an empty value, not a missing one', () => {
    const project = new Project();
    try {
      expectRefusal(
        () => project.importSmf(new Uint8Array(0)),
        RangeError,
        exactly('Project.importSmf: data must not be empty'),
      );
      expectRefusal(
        () => project.importClipFile(new Uint8Array(0)),
        RangeError,
        exactly('Project.importClipFile: data must not be empty'),
      );
    } finally {
      project.destroy();
    }
  });

  it('refuses an undo depth outside [1, 500000000] and a fractional one with the size_t text', () => {
    const project = new Project();
    try {
      for (const depth of [0, 500_000_001]) {
        expectRefusal(
          () => project.setMaxUndoDepth(depth),
          RangeError,
          exactly('Project.setMaxUndoDepth: depth must be an integer in [1, 500000000]'),
        );
      }
      expect(() => project.setMaxUndoDepth(500_000_000)).not.toThrow();
      expectRefusal(
        () => project.setMaxUndoDepth(1.5),
        RangeError,
        exactly(`Project.setMaxUndoDepth: depth ${SIZE_T_TEXT}`),
      );
    } finally {
      project.destroy();
    }
  });

  it('drains nothing for maxRecords 0 and refuses a negative one with the size_t text', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(engine.drainExternalMidi(0)).toEqual([]);
      expectRefusal(
        () => engine.drainExternalMidi(-1),
        RangeError,
        exactly(`RealtimeEngine.drainExternalMidi: maxRecords ${SIZE_T_TEXT}`),
      );
    } finally {
      engine.destroy();
    }
  });

  it('refuses a fractional UMP word', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expectRefusal(
        () => engine.pushMidiUmp(1, [1.5]),
        RangeError,
        exactly(
          'RealtimeEngine.pushMidiUmp: words[0] must be an integer in [-2147483648, 4294967295]',
        ),
      );
    } finally {
      engine.destroy();
    }
  });

  it('refuses a mixer input plane that is not a Float32Array', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
      sampleRate: 48000,
      maxBlockSize: 128,
    });
    try {
      const plane = [0, 0, 0, 0] as unknown as Float32Array;
      expectRefusal(
        () => mixer.processStereo([plane], [new Float32Array(4)]),
        TypeError,
        exactly('Mixer.processStereo: leftInputs[0] must be a Float32Array'),
      );
    } finally {
      mixer.destroy();
    }
  });
  it('refuses a streaming block past the prepared maximum, and a non-finite chain block, in the C ABI words', () => {
    const tooLong = new Float32Array(129);
    const nonFinite = new Float32Array(64);
    nonFinite[5] = Number.NaN;
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 128 });
    const mono = new StreamingMasteringChain({
      sampleRate: 48000,
      maxBlockSize: 128,
      numChannels: 1,
    });
    const stereo = new StreamingMasteringChain({
      sampleRate: 48000,
      maxBlockSize: 128,
      numChannels: 2,
    });
    const retune = new StreamingRetune();
    try {
      const range = 'must be an integer in [0, 128]';
      expectRefusal(
        () => eq.processMono(tooLong),
        RangeError,
        exactly(`StreamingEqualizer.processMono: samples.length ${range}`),
      );
      expectRefusal(
        () => eq.processStereo(tooLong, tooLong),
        RangeError,
        exactly(`StreamingEqualizer.processStereo: left.length ${range}`),
      );
      expectRefusal(
        () => eq.setSidechainMono(tooLong),
        RangeError,
        exactly(`StreamingEqualizer.setSidechainMono: samples.length ${range}`),
      );
      expectRefusal(
        () => eq.setSidechainStereo(tooLong, tooLong),
        RangeError,
        exactly(`StreamingEqualizer.setSidechainStereo: left.length ${range}`),
      );
      expectRefusal(
        () => mono.processMono(tooLong),
        RangeError,
        exactly(`StreamingMasteringChain.processMono: samples.length ${range}`),
      );
      expectRefusal(
        () => stereo.processStereo(tooLong, tooLong),
        RangeError,
        exactly(`StreamingMasteringChain.processStereo: left.length ${range}`),
      );
      expectRefusal(
        () => mono.processMono(nonFinite),
        RangeError,
        exactly(
          'StreamingMasteringChain.processMono: samples contains a non-finite sample at index 5',
        ),
      );
      expectRefusal(
        () => stereo.processStereo(new Float32Array(64), nonFinite),
        RangeError,
        exactly(
          'StreamingMasteringChain.processStereo: right contains a non-finite sample at index 5',
        ),
      );
      retune.prepare(48000, 128);
      expectRefusal(
        () => retune.processMono(tooLong),
        RangeError,
        exactly(`StreamingRetune.processMono: samples.length ${range}`),
      );
      expectRefusal(
        () => retune.processMono(nonFinite),
        RangeError,
        exactly('StreamingRetune.processMono: samples must contain only finite samples'),
      );
    } finally {
      retune.destroy();
      stereo.destroy();
      mono.destroy();
      eq.destroy();
    }
  });
  it('respells a structured refusal a core class raises directly, without a C-ABI entry in between', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      eq.setBand(0, {
        enabled: true,
        type: 'peak',
        frequencyHz: 1000,
        gainDb: 3,
        q: 1,
        placement: 'mid',
      });
      expectRefusal(
        () => eq.processMono(new Float32Array(64)),
        RangeError,
        exactly(
          'StreamingEqualizer.processMono: numChannels must be 2 when bands[0].placement is mid or side',
        ),
      );
    } finally {
      eq.destroy();
    }
  });
  it('refuses an out-of-range tuning in the same words', () => {
    expectRefusal(
      () => detectKey({ samples: new Float32Array(48000), sampleRate: 48000, tuningCents: 70 }),
      RangeError,
      exactly('detectKey: tuningCents must be finite and in [-50, 50)'),
    );
  });

  it('refuses a repeated send bus in the same words', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setTrackBuses([
        { busId: 1, gainDb: 0 },
        { busId: 2, gainDb: -6 },
      ]);
      engine.setTrackLanes([{ trackId: 10 }]);
      expectRefusal(
        () => engine.setTrackSends(10, [{ busId: 2 }, { busId: 2 }]),
        RangeError,
        exactly('RealtimeEngine.setTrackSends: sends[1].busId must not repeat an earlier send'),
      );
    } finally {
      engine.destroy();
    }
  });
});
