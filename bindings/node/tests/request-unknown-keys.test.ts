import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index.js';
import { NESTED_KEY_HINTS, NESTED_REQUEST_KEYS, REQUEST_KEYS } from '../src/request_keys.js';

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
  'Mixer',
  'Mixer.fromJson',
  'RealtimeEngine',
  'addSend',
  'applyBusInsertParamNow',
  'applyProjectVocalEdit',
  'applyStripInsertParamNow',
  'armCapture',
  'bakeMidiFx',
  'bounceOffline',
  'canSetBusSidechain',
  'canSetStripSidechain',
  'configureCapture',
  'importExternalSeparatedStem',
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
  'setBusInsertBypassed',
  'setBusInsertParam',
  'setBusSidechain',
  'setGraph',
  'setLoop',
  'setMetronome',
  'setPartRig',
  'setPolarityInvert',
  'setStripInsertBypassed',
  'setStripInsertParam',
  'setStripSidechain',
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
  const hinted = [
    ...Object.entries(NESTED_REQUEST_KEYS).flatMap(([name, fields]) =>
      Object.entries(fields).map(([field, keys]) => ({ name, field, keys: keys as object })),
    ),
    ...Object.entries(NESTED_KEY_HINTS).flatMap(([name, fields]) =>
      Object.entries(fields ?? {}).map(([field, keys]) => ({ name, field, keys: keys as object })),
    ),
  ].filter(({ name }) => !methodShapes.includes(name));

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

  it('masteringChain refuses a loudness section left at the top level', () => {
    expect(() =>
      sonare.masteringChain({ samples, sampleRate: 22050, loudness: { targetLufs: -14 } } as never),
    ).toThrow("masteringChain: unknown request key 'loudness' ('loudness' belongs under 'config')");
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
