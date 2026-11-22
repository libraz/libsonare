/**
 * Insert params the library emits name an enum value by its catalog token, and a
 * document carrying tokens survives a round trip through the mixer.
 *
 * Each check reads the declared type of every key it meets — from the insert's
 * parameter descriptors or the chain catalog — so a key emitted as an ordinal
 * fails whichever emitter carries it, and each one requires that enum keys were
 * met at all, so an emitter that stopped writing them cannot pass by omission.
 */

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  type MasteringInsertName,
  type MasteringInsertParamInfo,
  Mixer,
  type MixSceneDocument,
  masteringInsertParamInfo,
  masteringPresetNames,
  masteringPresetParams,
  mixingAssistantSuggest,
} from '../src/index.js';

const SR = 22_050;

interface CatalogParam {
  readonly name: string;
  readonly type: string;
  readonly choices?: readonly { name: string }[];
}
const catalog = JSON.parse(
  readFileSync(new URL('../../../tools/capability-catalog.json', import.meta.url), 'utf8'),
) as {
  chain: {
    params: readonly CatalogParam[];
    lists: readonly { key: string; processor?: MasteringInsertName | null }[];
  };
};

/** Keys of `params` whose value disagrees with the descriptor's type, and how many enum keys were met. */
function misfits(
  where: string,
  params: Record<string, unknown>,
  declared: ReadonlyMap<string, Pick<MasteringInsertParamInfo, 'type' | 'choices'>>,
): { misfit: string[]; enums: number } {
  const misfit: string[] = [];
  let enums = 0;
  for (const [key, value] of Object.entries(params)) {
    const param = declared.get(key);
    if (param?.type !== 'enum') {
      continue;
    }
    enums += 1;
    if (!(param.choices ?? []).some((choice) => choice.name === value)) {
      misfit.push(`${where}.${key}: ${JSON.stringify(value)}`);
    }
  }
  return { misfit, enums };
}

const insertParams = new Map<string, ReadonlyMap<string, MasteringInsertParamInfo>>();
function declaredFor(insert: MasteringInsertName): ReadonlyMap<string, MasteringInsertParamInfo> {
  let declared = insertParams.get(insert);
  if (!declared) {
    declared = new Map(masteringInsertParamInfo({ insert }).map((p) => [p.name, p]));
    insertParams.set(insert, declared);
  }
  return declared;
}

/** Enum misfits over every insert of a scene document. */
function sceneMisfits(scene: MixSceneDocument): { misfit: string[]; enums: number } {
  const out = { misfit: [] as string[], enums: 0 };
  for (const node of [...scene.strips, ...scene.buses]) {
    for (const insert of node.inserts) {
      const found = misfits(
        `${node.id}.${insert.processor}`,
        (insert.params ?? {}) as Record<string, unknown>,
        declaredFor(insert.processor),
      );
      out.misfit.push(...found.misfit);
      out.enums += found.enums;
    }
  }
  return out;
}

const VOICE_PARTIALS = [0.3, 0.7, 0.9, 0.85, 0.8, 0.7, 0.6, 0.55, 0.5, 0.45, 0.4, 0.35];

/** A voice over 180 Hz with about 2% of its energy in a 40 Hz rumble, enough for a high-pass. */
function voiceWithRumble(): Float32Array {
  const out = new Float32Array(Math.floor(SR * 0.6));
  for (let i = 0; i < out.length; i++) {
    const t = i / SR;
    let voice = 0;
    for (const [partial, level] of VOICE_PARTIALS.entries()) {
      voice += level * Math.sin(2 * Math.PI * 180 * (partial + 1) * t);
    }
    out[i] = 0.045 * voice + 0.014 * Math.sin(2 * Math.PI * 40 * t);
  }
  return out;
}

describe('insert params the library emits', () => {
  it('name each enum value of an assistant suggestion by its token', () => {
    const { scene } = mixingAssistantSuggest({
      tracks: [{ id: 'vox', name: 'Lead Vox', channels: [voiceWithRumble()] }],
      sampleRate: SR,
      enableHighPass: true,
    });
    const found = sceneMisfits(scene);
    expect(found.enums).toBeGreaterThan(0);
    expect(found.misfit).toEqual([]);
  });

  it('name each enum value of every preset chain config by its token', () => {
    const chainParams = new Map(catalog.chain.params.map((p) => [p.name, p]));
    const lists = new Map(catalog.chain.lists.map((l) => [l.key, l.processor ?? null]));
    const misfit: string[] = [];
    let enums = 0;
    const walk = (prefix: string, value: unknown): void => {
      if (Array.isArray(value)) {
        const processor = lists.get(prefix);
        if (!processor) {
          return;
        }
        for (const [index, item] of value.entries()) {
          const found = misfits(`${prefix}[${index}]`, item, declaredFor(processor));
          misfit.push(...found.misfit);
          enums += found.enums;
        }
        return;
      }
      if (typeof value === 'object' && value !== null) {
        for (const [key, inner] of Object.entries(value)) {
          walk(prefix ? `${prefix}.${key}` : key, inner);
        }
        return;
      }
      const param = chainParams.get(prefix);
      if (param?.type !== 'enum') {
        return;
      }
      enums += 1;
      if (!(param.choices ?? []).some((choice) => choice.name === value)) {
        misfit.push(`${prefix}: ${JSON.stringify(value)}`);
      }
    };
    for (const preset of masteringPresetNames()) {
      walk('', masteringPresetParams(preset));
    }
    expect(enums).toBeGreaterThan(0);
    expect(misfit).toEqual([]);
  });

  it('keep their tokens through a mixer round trip', () => {
    const inserts = [
      {
        processor: 'eq.cutFilter',
        params: { highPassEnabled: true, highPassFrequencyHz: 90, highPassSlope: 'db24PerOct' },
      },
      { processor: 'saturation.ampSim', params: { ampModel: 'modernHiGain', topology: 'circuit' } },
      { processor: 'effects.gsEfx', params: { realization: 'classic' } },
    ];
    const scene = { strips: [{ id: 'gtr', inserts }] };
    const first = Mixer.fromJson(JSON.stringify(scene), { sampleRate: 48_000 });
    const json = first.toJson();
    first[Symbol.dispose]();
    const second = Mixer.fromJson(json, { sampleRate: 48_000 });
    const reread = JSON.parse(second.toJson()) as MixSceneDocument;
    second[Symbol.dispose]();
    const strip = reread.strips.find((s) => s.id === 'gtr');
    for (const { processor, params } of inserts) {
      const written = strip?.inserts.find((insert) => insert.processor === processor)?.params;
      expect(written).toMatchObject(params);
    }
    const found = sceneMisfits(reread);
    expect(found.enums).toBeGreaterThanOrEqual(4);
    expect(found.misfit).toEqual([]);
  });
});
