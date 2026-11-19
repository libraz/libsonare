import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index.js';
import { NESTED_REQUEST_KEYS, REQUEST_KEYS } from '../src/request_keys.js';

type Entry = (request: unknown) => unknown;

const entries = (name: string): Entry => {
  const fn = (sonare as unknown as Record<string, unknown>)[name];
  if (typeof fn !== 'function') {
    throw new Error(`${name} is not exported from the package index`);
  }
  return fn as Entry;
};

/** What calling the entry point with `request` throws or rejects with, if anything. */
async function outcome(name: string, request: unknown): Promise<Error | undefined> {
  try {
    await entries(name)(request);
  } catch (error) {
    return error as Error;
  }
  return undefined;
}

const declared = (keys: object): Record<string, undefined> =>
  Object.fromEntries(Object.keys(keys).map((key) => [key, undefined]));

const refusals = /unknown (request key|option)/;

/** Shapes of class-method requests; they are not index functions and carry their own unknown-key cases. */
const methodShapes = [
  'ClipPageProvider',
  'ExternalSeparatedStem',
  'FileClipPageProvider',
  'Mixer',
  'Mixer.fromJson',
  'PlaybackLoudnessMeter',
  'PlaybackRenderer',
  'ProjectInstrumentBinding',
  'RealtimeEngine',
  'addClip',
  'addSend',
  'analyzeTempo',
  'applyBusInsertParamNow',
  'applyProjectVocalEdit',
  'applyStripInsertParamNow',
  'armCapture',
  'autoTempo',
  'bakeMidiFx',
  'bounce',
  'bounceOffline',
  'bounceWithBuiltinInstruments',
  'bounceWithInstruments',
  'bounceWithSf2Instruments',
  'bounceWithSynthInstruments',
  'canSetBusSidechain',
  'canSetStripSidechain',
  'clearPartRig',
  'configureCapture',
  'configureMeter',
  'freezeOffline',
  'importExternalSeparatedStem',
  'importExternalStems',
  'moveClip',
  'partRig',
  'prepare',
  'previewMidiFx',
  'pushMidiInputProgram',
  'pushMidiProgram',
  'renderOffline',
  'restoreBusInsertParam',
  'restoreStripInsertParam',
  'scheduleFaderAutomation',
  'scheduleInsertAutomation',
  'schedulePanAutomation',
  'scheduleSendAutomation',
  'scheduleWidthAutomation',
  'setAssistSidecar',
  'setAudioSourceMetadata',
  'setBusInsertBypassed',
  'setBusInsertParam',
  'setBusSidechain',
  'setClipFade',
  'setClipLoop',
  'setClipTakes',
  'setGraph',
  'setLoop',
  'setMarker',
  'setMetronome',
  'setPartRig',
  'setPolarityInvert',
  'setProgramOnChannel',
  'setSourceAudio',
  'setStripInsertBypassed',
  'setStripInsertParam',
  'setStripSidechain',
  'setStripSurroundPan',
  'setSurroundPan',
  'setTrackBuses',
  'setTrackLanes',
  'setTrackSoloMute',
  'transcribeToClip',
  'vocalApply',
  'vocalRender',
];

describe('request objects refuse keys outside their declared set', () => {
  const names = Object.keys(REQUEST_KEYS).filter((name) => !methodShapes.includes(name));

  it('declares every other shape for a function the package index exports', () => {
    expect(
      names.filter(
        (name) => typeof (sonare as unknown as Record<string, unknown>)[name] !== 'function',
      ),
    ).toEqual([]);
    expect(methodShapes.filter((name) => !(name in REQUEST_KEYS))).toEqual([]);
  });

  it.each(names)('%s refuses an unknown key, naming it', async (name) => {
    // A bare estimate is also a valid first argument there, so the request form is the one with `estimate`.
    const request =
      name === 'roomGeometryFromEstimate' || name === 'masteringRepairDereverbConfigForRoom'
        ? { estimate: {}, notAField: 1 }
        : { notAField: 1 };
    const error = await outcome(name, request);
    expect(error).toBeInstanceOf(TypeError);
    expect(error?.message).toBe(`${name}: unknown request key 'notAField'`);
  });

  it.each(names)('%s accepts every declared key', async (name) => {
    const error = await outcome(name, declared(REQUEST_KEYS[name as keyof typeof REQUEST_KEYS]));
    expect(error?.message ?? '').not.toMatch(refusals);
  });

  it('refuses the first unknown key in the order the caller wrote them', async () => {
    const error = await outcome('detectBpm', { samples: new Float32Array(8), second: 1, first: 2 });
    expect(error?.message).toBe("detectBpm: unknown request key 'second'");
  });

  it('refuses before any other validation of the request', async () => {
    const error = await outcome('detectBpm', {
      samples: new Float32Array(0),
      sampleRate: -1,
      x: 1,
    });
    expect(error?.message).toBe("detectBpm: unknown request key 'x'");
  });
});

describe('nested options objects refuse keys outside their declared set', () => {
  const nested = Object.entries(NESTED_REQUEST_KEYS)
    .flatMap(([name, fields]) =>
      Object.entries(fields).map(([field, keys]) => ({ name, field, keys })),
    )
    .filter(({ name }) => !methodShapes.includes(name));

  it.each(nested)('$name.$field refuses an unknown key, naming it', async ({ name, field }) => {
    const error = await outcome(name, { [field]: { notAnOption: 1 } });
    expect(error).toBeInstanceOf(TypeError);
    expect(error?.message).toBe(`${name}: unknown option 'notAnOption' in '${field}'`);
  });

  it.each(nested)('$name.$field accepts every declared key', async ({ name, field, keys }) => {
    const error = await outcome(name, { [field]: declared(keys) });
    expect(error?.message ?? '').not.toMatch(refusals);
  });
});

describe('a misplaced key points at the object it belongs under', () => {
  const hinted = Object.entries(NESTED_REQUEST_KEYS)
    .flatMap(([name, fields]) =>
      Object.entries(fields).map(([field, keys]) => ({ name, field, keys: keys as object })),
    )
    .filter(({ name }) => !methodShapes.includes(name));

  it.each(hinted)('$name suggests $field', async ({ name, field, keys }) => {
    const own = REQUEST_KEYS[name as keyof typeof REQUEST_KEYS];
    const misplaced = Object.keys(keys).find((key) => !Object.hasOwn(own, key));
    if (misplaced === undefined) {
      // Every field of the nested object is also accepted at the top level.
      return;
    }
    const error = await outcome(name, { [misplaced as string]: {} });
    expect(error?.message).toBe(
      `${name}: unknown request key '${misplaced}' ('${misplaced}' belongs under '${field}')`,
    );
  });
});

describe('a class-method request refuses nested keys through an engine instance', () => {
  it('configureCapture refuses an unknown punch key and suggests where a misplaced one belongs', () => {
    const engine = new sonare.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, punch: { notAnOption: 1 } } as never),
      ).toThrow("configureCapture: unknown option 'notAnOption' in 'punch'");
      expect(() =>
        engine.configureCapture({ capacityFrames: 128, inputMonitor: { notAnOption: 1 } } as never),
      ).toThrow("configureCapture: unknown option 'notAnOption' in 'inputMonitor'");
    } finally {
      engine.destroy();
    }
  });
});

describe('reported repros', () => {
  const samples = new Float32Array(2048);

  it('masterAudio refuses a loudness section left at the top level', () => {
    expect(() =>
      sonare.masterAudio({ samples, sampleRate: 22050, loudness: { targetLufs: -14 } } as never),
    ).toThrow(/^masterAudio: unknown request key 'loudness'$/);
  });

  it('analyze with onProgress refuses an unknown request key and a nested options object', () => {
    expect(() =>
      sonare.analyze({
        samples,
        sampleRate: 22050,
        onProgress: () => {},
        tunning: 'auto',
      } as never),
    ).toThrow("analyze: unknown request key 'tunning'");
    expect(() =>
      sonare.analyze({
        samples,
        sampleRate: 22050,
        onProgress: () => {},
        options: { bogus: 1 },
      } as never),
    ).toThrow("analyze: unknown request key 'options'");
  });

  it('detectBpm refuses a key it does not take', () => {
    expect(() => sonare.detectBpm({ samples, sampleRate: 22050, bpmMin: 60 })).not.toThrow();
    expect(() => sonare.detectBpm({ samples, sampleRate: 22050, hopLength: 512 } as never)).toThrow(
      "detectBpm: unknown request key 'hopLength'",
    );
    expect(() => sonare.detectBpm({ samples, sampleRate: 22050, bogus: 1 } as never)).toThrow(
      "detectBpm: unknown request key 'bogus'",
    );
  });

  it('a positional options bag is held to the same declared fields', async () => {
    const refused = async (name: string, bag: unknown): Promise<string | undefined> => {
      try {
        await (entries(name) as (...args: unknown[]) => unknown)(samples, 22050, bag);
      } catch (error) {
        return (error as Error).message;
      }
      return undefined;
    };
    for (const name of [
      'analyze',
      'analyzeAsync',
      'detectKey',
      'detectOnsets',
      'masteringRepairDeclick',
    ]) {
      expect(await refused(name, { bogus: 1 }), name).toBe(`${name}: unknown option 'bogus'`);
    }
    expect(() => sonare.analyze(samples, 22050, { nFft: 1024 })).not.toThrow();
  });

  it('positional calls are not request objects and keep their forms', () => {
    expect(typeof sonare.detectBpm(samples, 22050)).toBe('number');
  });
});

describe('a Project request method refuses an unknown key', () => {
  const extra = { notAField: 1 };
  const calls: Array<[string, (project: sonare.Project) => unknown]> = [
    ['addClip', (p) => p.addClip({ trackId: 1, startPpq: 0, lengthPpq: 1, ...extra } as never)],
    [
      'analyzeTempo',
      (p) => p.analyzeTempo({ samples: new Float32Array(8), sampleRate: 48000, ...extra } as never),
    ],
    [
      'autoTempo',
      (p) => p.autoTempo({ samples: new Float32Array(8), sampleRate: 48000, ...extra } as never),
    ],
    ['bounce', (p) => p.bounce(extra as never)],
    ['bounceWithBuiltinInstruments', (p) => p.bounceWithBuiltinInstruments([], extra as never)],
    ['bounceWithInstruments', (p) => p.bounceWithInstruments([], extra as never)],
    ['bounceWithSf2Instruments', (p) => p.bounceWithSf2Instruments([], extra as never)],
    ['bounceWithSynthInstruments', (p) => p.bounceWithSynthInstruments([], extra as never)],
    [
      'ProjectInstrumentBinding',
      (p) => p.bounceWithInstruments([{ destinationId: 1, ...extra } as never]),
    ],
    ['clearPartRig', (p) => p.clearPartRig({ destinationId: 1, part: 0, ...extra } as never)],
    [
      'importExternalStems',
      (p) => p.importExternalStems({ sampleRate: 48000, stems: [], ...extra } as never),
    ],
    [
      'ExternalSeparatedStem',
      (p) => p.importExternalStems({ sampleRate: 48000, stems: [extra] } as never),
    ],
    ['moveClip', (p) => p.moveClip(1, 0, extra as never)],
    ['partRig', (p) => p.partRig({ destinationId: 1, part: 0, ...extra } as never)],
    ['setAssistSidecar', (p) => p.setAssistSidecar(extra as never)],
    ['setAudioSourceMetadata', (p) => p.setAudioSourceMetadata({ sourceId: 1, ...extra } as never)],
    ['setClipFade', (p) => p.setClipFade(1, extra as never)],
    ['setClipLoop', (p) => p.setClipLoop(1, extra as never)],
    ['setClipTakes', (p) => p.setClipTakes(1, [], extra as never)],
    ['setMarker', (p) => p.setMarker({ ppq: 0, ...extra } as never)],
    ['setProgramOnChannel', (p) => p.setProgramOnChannel(1, { program: 0, ...extra } as never)],
    ['setSourceAudio', (p) => p.setSourceAudio({ sourceId: 1, ...extra } as never)],
  ];

  // Nested shapes are refused under the member that took them.
  const entryOf: Record<string, string> = {
    ProjectInstrumentBinding: 'bounceWithInstruments',
    ExternalSeparatedStem: 'importExternalStems',
  };

  it.each(calls)('%s refuses an unknown key, naming it', (name, call) => {
    const project = new sonare.Project();
    try {
      expect(() => call(project)).toThrow(
        new TypeError(`Project.${entryOf[name] ?? name}: unknown request key 'notAField'`),
      );
    } finally {
      project.destroy();
    }
  });

  it('setProgramOnChannel refuses an unknown bank key and points a misplaced one at bank', () => {
    const project = new sonare.Project();
    try {
      expect(() =>
        project.setProgramOnChannel(1, { program: 0, bank: { notAnOption: 1 } } as never),
      ).toThrow("setProgramOnChannel: unknown option 'notAnOption' in 'bank'");
      expect(() => project.setProgramOnChannel(1, { program: 0, msb: 0 } as never)).toThrow(
        "setProgramOnChannel: unknown request key 'msb' ('msb' belongs under 'bank')",
      );
    } finally {
      project.destroy();
    }
  });
});
