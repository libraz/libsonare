/**
 * Request-object versus positional equivalence for every exported function
 * that has both forms. Each row names the request fields once and the harness
 * derives the call from them: `samples, sampleRate, options` for a row marked
 * `sampleOptions`, the listed order for a legacy positional row. A row marked
 * `requestOnly` has no positional form; it keeps the per-field and refusal
 * controls and drops the equivalence. Each row asserts on a non-silent input:
 *
 * - both forms return the same result for non-default values, and for the
 *   required arguments alone (so the two forms share their defaults);
 * - every optional field the row passes moves the positional result when it is
 *   omitted (the control that makes a dropped field visible in the equality);
 * - an invalid value is refused with the same error name and message by both.
 *
 * The inventory is read from the sources, so an overload with no row fails.
 */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { isDeepStrictEqual } from 'node:util';
import { describe, expect, it } from 'vitest';
import type { Matrix2D } from '../../dist/index.js';
import * as sonare from '../../src/index';

await sonare.init();

export const SR = 22050;
/** A sample rate no function defaults to, so passing it is observable. */
export const ALT_SR = 32000;

function lcg(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 2 ** 32 - 0.5;
  };
}

/**
 * Beat-gated harmonic tones with a click per beat and a pitch change halfway,
 * ending in a near-silent tail, so onset, pitch, chroma, silence and loudness
 * readers all have something to measure.
 */
export function signal(seconds: number, root = 220, seed = 1): Float32Array {
  const n = Math.round(SR * seconds);
  const out = new Float32Array(n);
  const noise = lcg(seed);
  const beat = Math.round(SR * 0.25);
  const voiced = Math.round(n * 0.8);
  for (let i = 0; i < n; i++) {
    const t = i / SR;
    const f = i < n / 2 ? root : root * 1.5;
    const env = Math.exp(-((i % beat) / SR) * 6);
    let v = 0;
    if (i < voiced) {
      v =
        0.18 *
        env *
        (Math.sin(2 * Math.PI * f * t) +
          0.5 * Math.sin(2 * Math.PI * f * 1.26 * t) +
          0.4 * Math.sin(2 * Math.PI * f * 1.5 * t) +
          0.2 * Math.sin(2 * Math.PI * f * 2 * t));
      if (i % beat < 40) {
        v += 0.3 * (1 - (i % beat) / 40) * noise();
      }
    }
    out[i] = v + 0.002 * noise();
  }
  return out;
}

export const SIG = signal(1);
export const SIG2 = signal(1, 330, 7);
/** A short excerpt for the constant-Q and inverse rows, whose cost grows fastest with length. */
export const SHORT = SIG.subarray(0, Math.round(SR * 0.3));
export const LONG = signal(3.5);
/** A pure tone with an exactly silent tail, for the pitch trackers' voicing. */
export const TONE = Float32Array.from({ length: SR }, (_, i) =>
  i < SR * 0.8 ? 0.3 * Math.sin((2 * Math.PI * 220 * i) / SR) : 0,
);
/** The same tone over a noise tail, which YIN reads as unvoiced rather than as a period. */
export const TONE_NOISY_TAIL = (() => {
  const noise = lcg(11);
  return TONE.map((v, i) => (i < SR * 0.8 ? v : 0.05 * noise()));
})();
/** A short and a long tone around a gap, for the transcriber's minimum-note setting. */
export const TWO_NOTES = (() => {
  const tone = (hz: number, seconds: number) =>
    Float32Array.from(
      { length: Math.round(seconds * SR) },
      (_, i) => 0.3 * Math.sin((2 * Math.PI * hz * i) / SR),
    );
  return Float32Array.from([
    ...tone(220, 0.15),
    ...new Float32Array(2205),
    ...tone(330, 0.6),
    ...new Float32Array(2205),
  ]);
})();
/** Two contrasting four-second halves, for section segmentation. */
export const LONG8 = (() => {
  const a = signal(4, 220, 1);
  const out = new Float32Array(a.length * 2);
  out.set(a);
  out.set(signal(4, 330, 5), a.length);
  return out;
})();
/** The voiced part of SIG between silent margins, for the trimmers. */
export const PADDED = (() => {
  const out = new Float32Array(SR * 2);
  out.set(SIG.subarray(0, Math.round(SR * 0.8)), SR / 2);
  return out;
})();
export const CLIPPED = SIG.map((v) => Math.max(-0.12, Math.min(0.12, v * 2)));
/** Sparse full-scale impulses over the tones, for the click and crackle repairs. */
export const CLICKY = SIG.map((v, i) => (i % 1500 === 700 ? 0.9 : i % 1500 === 701 ? -0.8 : v));
export const HUMMED = SIG.map((v, i) => v + 0.05 * Math.sin((2 * Math.PI * 60 * i) / SR));
/** An exponentially decaying noise burst, for the impulse-response readers. */
export const IR = (() => {
  const noise = lcg(3);
  return Float32Array.from({ length: SR }, (_, i) => noise() * Math.exp(-i / (0.08 * SR)));
})();

/** A spectrogram whose FFT size leaves room for the default hop, for the phase reconstruction. */
export const ST1024 = sonare.stft(SHORT, SR, { nFft: 1024, hopLength: 256 });
export const MEL = sonare.melSpectrogram(SHORT, SR, { nFft: 512, hopLength: 128, nMels: 32 });
export const MFCC = sonare.mfcc(SHORT, SR, { nFft: 512, hopLength: 128, nMels: 32, nMfcc: 13 });
export const CQT = sonare.cqt(SHORT, SR, {
  hopLength: 128,
  fminHz: 65.4,
  nBins: 36,
  binsPerOctave: 12,
});
export const VQT = sonare.vqt(SHORT, SR, {
  hopLength: 128,
  fminHz: 65.4,
  nBins: 36,
  binsPerOctave: 12,
  gamma: 2,
});
export const ENV = sonare.onsetStrength(SIG, SR, { nFft: 1024, hopLength: 256 });
export const TG = sonare.tempogram({
  onsetEnvelope: ENV,
  sampleRate: SR,
  hopLength: 256,
  winLength: 64,
});
export const CHROMA = sonare.chroma(SIG, SR, { nFft: 1024, hopLength: 256 });
export const F0 = new Float32Array(Math.ceil(SIG.length / 256) + 1).fill(220);
export const VOICED = new Float32Array(F0.length).fill(1);
export const SMALL = Float32Array.from({ length: 80 }, (_, i) => 0.1 + ((i * 37) % 11) / 11);

export type Args = Record<string, unknown>;

/** A progress callback slot; each call gets a fresh recorder in its place. */
export const PROGRESS = Symbol('progress');

export interface Row {
  name: string;
  fn: (...args: never[]) => unknown;
  /**
   * `sampleOptions`: positional form is `(samples, sampleRate, options?)`, the options being
   * every other request field. `requestOnly`: no positional form. Absent: legacy positional order.
   */
  form?: 'sampleOptions' | 'requestOnly';
  /** Request fields (positional order for a legacy row); `?` marks optional, `...` a spread. */
  params: string;
  values: Args;
  /** Overrides both forms must refuse with the same error. */
  invalid: Args;
  /** Request field name for a positional parameter spelled differently. */
  rename?: Record<string, string>;
  /** Fields with no per-field control, and why. */
  inert?: Record<string, string>;
  /**
   * Per-field overrides of the values the control drops the field from, for a
   * field another passed field can mask (a floor that hides a clamp).
   */
  controlBase?: Record<string, Args>;
  /**
   * Optional fields of which at least one must be given; a single-member group is a required
   * field inside the options bag. The minimal call supplies the first member of each group, and
   * that call without the group's members must fail with `error` in every form.
   */
  oneOf?: { fields: string[]; error: string }[];
  /** Documented defaults: the minimal call must equal the minimal call with these values given. */
  defaults?: Args;
  /** Why the minimal call is refused today; the checks on it are expected to fail until that is fixed. */
  knownRed?: string;
}

interface Spec {
  name: string;
  optional: boolean;
  spread: boolean;
}

function specsOf(row: Row): Spec[] {
  return row.params.split(/\s+/).map((token) => ({
    name: token.replace(/^\.\.\./, '').replace(/\?$/, ''),
    optional: token.endsWith('?'),
    spread: token.startsWith('...'),
  }));
}

function positionalArgs(specs: Spec[], values: Args): unknown[] {
  const args = specs.map((spec) => values[spec.name]);
  while (args.length > 0 && args[args.length - 1] === undefined) {
    args.pop();
  }
  return args;
}

function trimTrailing(args: unknown[]): unknown[] {
  const last = args[args.length - 1];
  if (last !== null && typeof last === 'object' && Object.keys(last).length === 0) {
    args.pop();
  }
  return args;
}

function requestOf(row: Row, specs: Spec[], values: Args): Args {
  const request: Args = {};
  for (const spec of specs) {
    const value = values[spec.name];
    if (value === undefined) {
      continue;
    }
    if (spec.spread) {
      Object.assign(request, value);
    } else {
      request[row.rename?.[spec.name] ?? spec.name] = value;
    }
  }
  return request;
}

type Outcome = { value: unknown; progressed: boolean } | { error: string };

/** Replace each PROGRESS slot (top level or one level into an object) with a recorder. */
function materialize(values: Args): { values: Args; progressed: () => boolean } {
  let calls = 0;
  const recorder = () => {
    calls += 1;
  };
  const swap = (value: unknown): unknown => {
    if (value === PROGRESS) {
      return recorder;
    }
    if (
      value !== null &&
      typeof value === 'object' &&
      Object.getPrototypeOf(value) === Object.prototype
    ) {
      return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, swap(v)]));
    }
    return value;
  };
  const swapped = Object.fromEntries(Object.entries(values).map(([k, v]) => [k, swap(v)]));
  return { values: swapped, progressed: () => calls > 0 };
}

async function run(
  row: Row,
  specs: Spec[],
  values: Args,
  form: 'positional' | 'request',
): Promise<Outcome> {
  const { values: live, progressed } = materialize(values);
  const call = row.fn as (...args: unknown[]) => unknown;
  try {
    const request = requestOf(row, specs, live);
    let value: unknown;
    if (form === 'request' || row.form === 'requestOnly') {
      value = await call(request);
    } else if (row.form === 'sampleOptions') {
      const { samples, sampleRate, ...options } = request;
      value = await call(...trimTrailing([samples, sampleRate, options]));
    } else {
      value = await call(...positionalArgs(specs, live));
    }
    return { value, progressed: progressed() };
  } catch (error) {
    return { error: `${(error as Error).name}: ${(error as Error).message}` };
  }
}

interface Field {
  param: string;
  key?: string;
}

function fieldsOf(row: Row, specs: Spec[]): Field[] {
  const fields: Field[] = [];
  for (const spec of specs) {
    const value = row.values[spec.name];
    if (value === undefined) {
      continue;
    }
    if (row.form === 'sampleOptions' && spec.name === 'sampleRate') {
      // Required, so the control swaps it for the rate every other row's default uses.
      fields.push({ param: spec.name });
    } else if (spec.spread) {
      for (const key of Object.keys(value as Args)) {
        fields.push({ param: spec.name, key });
      }
    } else if (spec.optional) {
      fields.push({ param: spec.name });
    }
  }
  return fields.filter((field) => !(row.inert && (field.key ?? field.param) in row.inert));
}

/**
 * `values` with `fields` written over it: a name a positional parameter owns
 * replaces it, any other lands inside the spread options bag.
 */
function withFields(specs: Spec[], values: Args, fields: Args): Args {
  const next: Args = { ...values };
  const spread = specs.find((spec) => spec.spread);
  for (const [key, value] of Object.entries(fields)) {
    if (specs.some((spec) => !spec.spread && spec.name === key) || spread === undefined) {
      next[key] = value;
    } else {
      next[spread.name] = { ...(next[spread.name] as Args), [key]: value };
    }
  }
  return next;
}

/** `values` without `keys`, each removed from the spread options bag when no parameter owns it. */
function withoutFields(specs: Spec[], values: Args, keys: string[]): Args {
  const next: Args = { ...values };
  const spread = specs.find((spec) => spec.spread);
  for (const key of keys) {
    if (specs.some((spec) => !spec.spread && spec.name === key) || spread === undefined) {
      delete next[key];
    } else if (next[spread.name] !== undefined) {
      const { [key]: _, ...rest } = next[spread.name] as Args;
      next[spread.name] = rest;
    }
  }
  return next;
}

/** A field's value in the row, read from the spread options bag when no parameter owns it. */
function fieldValue(specs: Spec[], values: Args, key: string): unknown {
  const spread = specs.find((spec) => spec.spread);
  if (specs.some((spec) => !spec.spread && spec.name === key) || spread === undefined) {
    return values[key];
  }
  return (values[spread.name] as Args | undefined)?.[key];
}

function without(row: Row, values: Args, field: Field): Args {
  const next = { ...values };
  if (field.key === undefined) {
    next[field.param] =
      row.form === 'sampleOptions' && field.param === 'sampleRate' ? SR : undefined;
  } else {
    const { [field.key]: _, ...rest } = values[field.param] as Args;
    next[field.param] = rest;
  }
  return next;
}

/** The row's values with a NaN written into the first audio buffer. */
function poisoned(specs: Spec[], values: Args): Args {
  for (const spec of specs) {
    const value = values[spec.name];
    if (value instanceof Float32Array) {
      const copy = value.slice();
      copy[0] = Number.NaN;
      return { ...values, [spec.name]: copy };
    }
    if (
      value !== null &&
      typeof value === 'object' &&
      (value as Matrix2D).data instanceof Float32Array
    ) {
      const data = (value as Matrix2D).data.slice();
      data[0] = Number.NaN;
      return { ...values, [spec.name]: { ...(value as Matrix2D), data } };
    }
  }
  throw new Error('row has no audio buffer to poison');
}

/** Overload functions whose result is a native handle, compared by their own test below. */
const HANDLE_RESULTS = new Set(['analyzePolyphonic']);

/**
 * Every exported function that declares a request overload next to a
 * positional one, read from the sources the way the parity tool reads them.
 */
export function overloadedFunctions(): string[] {
  const dir = join(import.meta.dirname, '../../src');
  const names = new Set<string>();
  for (const file of readdirSync(dir).filter((f) => f.endsWith('.ts'))) {
    const source = readFileSync(join(dir, file), 'utf8');
    const heads = [
      ...source.matchAll(/^export (?:async )?function (\w+)(?:<[^(]*>)?\(\s*([\w]+)\??:/gm),
    ];
    const byName = new Map<string, string[]>();
    for (const [, name, first] of heads) {
      byName.set(name, [...(byName.get(name) ?? []), first]);
    }
    for (const [name, firsts] of byName) {
      // Request overload + positional overload; the implementation takes `...args` or named parameters.
      if (firsts[0] === 'request' && firsts.length > 1) {
        names.add(name);
      }
    }
  }
  return [...names].filter((name) => !HANDLE_RESULTS.has(name)).sort();
}

/** Registers the equivalence cases for each row. */
export function describeRows(rows: Row[]): void {
  describe.each(rows)('$name', (row) => {
    const specs = specsOf(row);
    const groups = row.oneOf ?? [];
    const required = Object.fromEntries(
      specs.filter((spec) => !spec.optional).map((spec) => [spec.name, row.values[spec.name]]),
    );
    const requiredOnly = withFields(
      specs,
      required,
      Object.fromEntries(
        groups.map((group) => [group.fields[0], fieldValue(specs, row.values, group.fields[0])]),
      ),
    );
    const hasPositional = row.form !== 'requestOnly';

    for (const group of groups) {
      it(`refuses the minimal call without ${group.fields.join(' | ')}`, async () => {
        const forms = hasPositional ? (['request', 'positional'] as const) : (['request'] as const);
        for (const form of forms) {
          expect(
            await run(row, specs, withoutFields(specs, requiredOnly, group.fields), form),
          ).toEqual({
            error: group.error,
          });
        }
      });
    }

    if (row.defaults !== undefined) {
      const defaults = row.defaults;
      it('equals its documented defaults given explicitly', async () => {
        const omitted = await run(row, specs, requiredOnly, 'request');
        expect(omitted).not.toHaveProperty('error');
        const given = await run(row, specs, withFields(specs, requiredOnly, defaults), 'request');
        expect(given).toEqual(omitted);
      });
    }

    if (hasPositional) {
      it('returns the positional result for non-default values', async () => {
        const positional = await run(row, specs, row.values, 'positional');
        expect(positional).not.toHaveProperty('error');
        expect(await run(row, specs, row.values, 'request')).toEqual(positional);
      });

      (row.knownRed === undefined ? it : it.fails)('shares the positional defaults', async () => {
        const request = await run(row, specs, requiredOnly, 'request');
        // Two identical refusals would compare equal and share nothing.
        expect(request).not.toHaveProperty('error');
        expect(request).toEqual(await run(row, specs, requiredOnly, 'positional'));
      });
    } else {
      (row.knownRed === undefined ? it : it.fails)(
        'answers the request form for non-default values and for the required fields alone',
        async () => {
          expect(await run(row, specs, row.values, 'request')).not.toHaveProperty('error');
          expect(await run(row, specs, requiredOnly, 'request')).not.toHaveProperty('error');
        },
      );
    }

    it('moves the result for every field it passes', async () => {
      const full = await run(row, specs, row.values, 'positional');
      const unmoved: string[] = [];
      for (const field of fieldsOf(row, specs)) {
        const label = field.key ?? field.param;
        const masked = { ...row.values, ...row.controlBase?.[label] };
        // `validate` only decides which layer refuses a non-finite sample.
        const base = label === 'validate' ? poisoned(specs, masked) : masked;
        const reference =
          label === 'validate' || row.controlBase?.[label]
            ? await run(row, specs, base, 'positional')
            : full;
        if (
          isDeepStrictEqual(
            await run(row, specs, without(row, base, field), 'positional'),
            reference,
          )
        ) {
          unmoved.push(label);
        }
      }
      expect(unmoved.join(', ')).toBe('');
    });

    it(
      hasPositional
        ? 'refuses an invalid value identically in both forms'
        : 'refuses an invalid value',
      async () => {
        const bad = { ...row.values, ...row.invalid };
        const positional = await run(row, specs, bad, 'positional');
        expect(positional).toHaveProperty('error');
        expect(await run(row, specs, bad, 'request')).toEqual(positional);
      },
    );
  });
}
