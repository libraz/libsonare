/**
 * Public type names the Node and WASM packages both export must mean the same
 * shape.
 *
 * `DynamicsResult` used to be the analysis metrics in one package and the
 * processor envelope `{ samples, latencySamples }` in the other, so a shared
 * TypeScript module annotated with it type-checked against one package and
 * failed against the other — the identifier resolved either way and only the
 * member list gave it away.
 *
 * The two packages are separate npm packages and cannot import one module, so
 * what makes a drift red is this: each package builds a sample object with
 * `satisfies`, which is exact (a missing field is a compile error, an extra one
 * is an excess-property error), and then compares that object's keys against
 * `tests/conformance/shared_type_shapes.json`. Adding, removing or renaming a
 * field on either side fails that package's own suite against the shared file,
 * and the sibling suite reads the same file.
 */

import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';
import type {
  CapabilityCatalog,
  CapabilityCatalogChain,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  CapabilityCatalogPresets,
  DynamicsProcessorResult,
  DynamicsResult,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
} from '../src/index';

interface SharedTypeShapes {
  types: Record<string, string[]>;
  aliases: Record<string, string>;
}

const shapes = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/shared_type_shapes.json', import.meta.url),
    'utf8',
  ),
) as SharedTypeShapes;

// Exact by construction: `satisfies` rejects a missing or extra field, so these
// literals cannot drift from the interfaces without a compile error.
const dynamicsResult = {
  dynamicRangeDb: 0,
  peakDb: 0,
  rmsDb: 0,
  crestFactor: 0,
  loudnessRangeLu: 0,
  isCompressed: false,
  loudnessTimesSec: new Float32Array(0),
  loudnessRmsDb: new Float32Array(0),
} satisfies DynamicsResult;

const dynamicsProcessorResult = {
  samples: new Float32Array(0),
  latencySamples: 0,
} satisfies DynamicsProcessorResult;

describe('shared public type shapes', () => {
  it('DynamicsResult is the analysis shape the shared corpus declares', () => {
    expect(Object.keys(dynamicsResult).sort()).toEqual([...shapes.types.DynamicsResult].sort());
  });

  it('DynamicsProcessorResult is the processor envelope the shared corpus declares', () => {
    expect(Object.keys(dynamicsProcessorResult).sort()).toEqual(
      [...shapes.types.DynamicsProcessorResult].sort(),
    );
  });

  it('the two shapes stay disjoint, so a mix-up cannot be silent', () => {
    const analysis = new Set(shapes.types.DynamicsResult);
    const overlap = shapes.types.DynamicsProcessorResult.filter((key) => analysis.has(key));
    expect(overlap).toEqual([]);
  });
});

// The catalog's part types are exported from the entry so a host can name them
// directly; importing them here is what turns a dropped export into a type error.
const catalogParameter = {
  name: 'driveDb',
  id: undefined,
  rtSafe: false,
  type: 'number',
  min: undefined,
  max: undefined,
  minExclusive: false,
  maxExclusive: false,
  maxRelativeTo: undefined,
  default: undefined,
  unit: 'Hz',
  uiMin: undefined,
  uiMax: undefined,
  scale: 'linear',
  choices: undefined,
  slot: 'band1',
  dependsOn: [{ key: 'maxHz', relation: 'le', factor: 1 }],
  coupledWith: ['minHz'],
} satisfies MasteringInsertParamInfo;

const catalogSlot = {
  name: 'band1',
  parent: undefined,
  activation: 'always',
  minCrossoverCutoffs: 1,
} satisfies MasteringInsertSlot;

const catalogProcessor = {
  id: 'saturation.softClipper',
  kind: 'realtime',
  forms: ['realtime'],
  realtimeInsertable: true,
  stereoOnly: false,
  latencySamples: 0,
  tailSamples: 0,
  realtimeCost: undefined,
  channelPolicy: 'perChannel',
  category: 'saturation',
  causal: true,
  params: [catalogParameter],
  offlineParams: undefined,
  slots: [catalogSlot],
} satisfies MasteringProcessorCatalogEntry;

const catalogPresets = {
  mastering: [],
  synth: [],
  mixingScene: [],
  voiceChanger: [],
  playbackRoom: [],
} satisfies CapabilityCatalogPresets;

const catalogMasteringPreset = {
  name: 'vinyl',
  kind: 'restoration',
  targetLufs: undefined,
  truePeakCeilingDbtp: undefined,
  maxLimiterGainReductionDb: undefined,
} satisfies MasteringPresetCatalogEntry;

const catalogChainStage = {
  prefix: 'dynamics.compressor',
  processor: 'dynamics.compressor',
  form: 'offline',
  enabledKey: 'dynamics.compressor.enabled',
} satisfies CapabilityCatalogChainStage;

const catalogChainList = {
  key: 'dynamics.multibandComp.bands',
  processor: 'dynamics.compressor',
  form: 'offline',
  minItems: 2,
  maxItems: 64,
} satisfies CapabilityCatalogChainList;

const catalogChain = {
  stages: [catalogChainStage],
  lists: [catalogChainList],
  params: [catalogParameter],
} satisfies CapabilityCatalogChain;

const capabilityCatalog = {
  version: '',
  abi: null as unknown as CapabilityCatalog['abi'],
  processors: [catalogProcessor],
  chain: catalogChain,
  presets: catalogPresets,
  masteringPresets: [catalogMasteringPreset],
} satisfies CapabilityCatalog;

describe('capability catalog type shapes', () => {
  it.each([
    ['CapabilityCatalog', capabilityCatalog],
    ['CapabilityCatalogChain', catalogChain],
    ['CapabilityCatalogChainList', catalogChainList],
    ['CapabilityCatalogChainStage', catalogChainStage],
    ['CapabilityCatalogPresets', catalogPresets],
    ['MasteringInsertParamInfo', catalogParameter],
    ['MasteringInsertSlot', catalogSlot],
    ['MasteringPresetCatalogEntry', catalogMasteringPreset],
    ['MasteringProcessorCatalogEntry', catalogProcessor],
  ] as const)('%s is the shape the shared corpus declares', (name, sample) => {
    expect(Object.keys(sample).sort()).toEqual([...shapes.types[name]].sort());
  });
});
