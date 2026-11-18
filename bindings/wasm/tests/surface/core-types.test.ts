import { describe, expect, expectTypeOf, it } from 'vitest';
import type { Matrix2D as AnalysisMatrix2D } from '../../dist/analysis.js';
import type {
  Capabilities,
  CapabilityCatalog,
  init,
  MasteringInsertParamInfo,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  Matrix2D,
  ModuleHandle,
  nnFilter,
  polyFeatures,
  spectralContrast,
} from '../../dist/index.js';

type ModuleFactory = NonNullable<NonNullable<Parameters<typeof init>[0]>['moduleFactory']>;

describe('core public type names', () => {
  it('exports one Matrix2D from both entries and returns it from the matrix helpers', () => {
    expectTypeOf<Matrix2D>().toEqualTypeOf<{ data: Float32Array; rows: number; cols: number }>();
    expectTypeOf<AnalysisMatrix2D>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<ReturnType<typeof spectralContrast>>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<ReturnType<typeof polyFeatures>>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<ReturnType<typeof nnFilter>>().toEqualTypeOf<Matrix2D>();
    expect(true).toBe(true);
  });

  it('types moduleFactory with the opaque module handle', () => {
    expectTypeOf<ReturnType<ModuleFactory>>().toEqualTypeOf<Promise<ModuleHandle>>();
    expectTypeOf<Capabilities['features']['ffmpeg']>().toEqualTypeOf<boolean>();
    expect(true).toBe(true);
  });

  it('shares one catalog type family between capabilityCatalog and the mastering catalog', () => {
    expectTypeOf<
      CapabilityCatalog['processors'][number]
    >().toEqualTypeOf<MasteringProcessorCatalogEntry>();
    expectTypeOf<
      CapabilityCatalog['chain']['params'][number]
    >().toEqualTypeOf<MasteringInsertParamInfo>();
    expectTypeOf<
      CapabilityCatalog['masteringPresets'][number]
    >().toEqualTypeOf<MasteringPresetCatalogEntry>();
    expectTypeOf<
      MasteringProcessorCatalogEntry['category']
    >().toEqualTypeOf<MasteringProcessorCategory>();
    const entry = {} as MasteringProcessorCatalogEntry;
    // @ts-expect-error catalog entries are readonly
    entry.id = 'x';
    expect(true).toBe(true);
  });

  it('no longer exports the internal or twin type names', () => {
    // @ts-expect-error removed
    type B = import('../../dist/index.js').Matrix2dResult;
    // @ts-expect-error removed
    type C = import('../../dist/index.js').SonareModule;
    // @ts-expect-error removed
    type D = import('../../dist/index.js').SonareCapabilities;
    // @ts-expect-error removed
    type E = import('../../dist/index.js').CapabilityCatalogProcessor;
    // @ts-expect-error removed
    type F = import('../../dist/index.js').CapabilityCatalogParameter;
    // @ts-expect-error removed
    type G = import('../../dist/index.js').CapabilityCatalogMasteringPreset;
    // @ts-expect-error removed
    type H = import('../../dist/index.js').EngineCapabilities;
    // @ts-expect-error removed
    type I = import('../../dist/analysis.js').SonareCapabilities;
    expectTypeOf<[B, C, D, E, F, G, H, I]>().not.toBeNever();
    expect(true).toBe(true);
  });
});
