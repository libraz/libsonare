/**
 * Every key the chain config type declares is one the chain reader accepts, with
 * the type the reader gives it.
 *
 * The flat stages take their processor's parameters, so the check reaches them
 * through the catalog stage table and the generated parameter map; the nested
 * multiband stage and the processor-less loudness stage are compared against the
 * catalog's own chain parameters, which come from the same field tables the
 * reader walks.
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import ts from 'typescript';
import { describe, expect, it } from 'vitest';

const TYPES = fileURLToPath(new URL('../src/public_types_mastering.ts', import.meta.url));
const CATALOG = fileURLToPath(new URL('../../../tools/capability-catalog.json', import.meta.url));

interface CatalogParam {
  readonly name: string;
  readonly type: string;
  readonly choices?: readonly { name: string }[];
}
interface Catalog {
  readonly chain: {
    readonly lists: readonly { key: string; processor?: string | null }[];
    readonly params: readonly CatalogParam[];
    readonly stages: readonly { prefix: string; processor?: string }[];
  };
  readonly processors: readonly { id: string; params: readonly CatalogParam[] }[];
}

const catalog = JSON.parse(readFileSync(CATALOG, 'utf8')) as Catalog;
const program = ts.createProgram([TYPES], {
  module: ts.ModuleKind.NodeNext,
  moduleResolution: ts.ModuleResolutionKind.NodeNext,
  noEmit: true,
  skipLibCheck: true,
  strict: true,
  target: ts.ScriptTarget.ES2022,
});
const checker = program.getTypeChecker();

function exportedType(name: string): ts.Type {
  const file = program.getSourceFile(TYPES);
  if (!file) {
    throw new Error(`no source file ${TYPES}`);
  }
  const moduleSymbol = checker.getSymbolAtLocation(file);
  const symbol = moduleSymbol
    ? checker.getExportsOfModule(moduleSymbol).find((s) => s.name === name)
    : undefined;
  if (!symbol) {
    throw new Error(`${name} not found`);
  }
  return checker.getDeclaredTypeOfSymbol(symbol);
}

/** The non-nullable type of member @p key of @p type, array elements unwrapped. */
function memberType(type: ts.Type, key: string): ts.Type {
  const member = type.getProperty(key);
  if (!member) {
    throw new Error(`no member ${key} on ${checker.typeToString(type)}`);
  }
  const resolved = checker.getNonNullableType(checker.getTypeOfSymbol(member));
  return checker.isArrayType(resolved)
    ? checker.getTypeArguments(resolved as ts.TypeReference)[0]
    : resolved;
}

/** The object type at @p path under the chain config. */
function typeAt(path: readonly string[]): ts.Type {
  return path.reduce(memberType, exportedType('MasteringChainConfig'));
}

/** The keys of the object type at @p path under the chain config. */
function keysAt(path: readonly string[]): string[] {
  return checker
    .getPropertiesOfType(typeAt(path))
    .map((p) => p.name)
    .sort();
}

/** Keys of @p reference whose member type on @p type is not mutually assignable with it. */
function typeMismatches(type: ts.Type, reference: ts.Type): string[] {
  return checker
    .getPropertiesOfType(reference)
    .map((p) => p.name)
    .filter((key) => {
      const ours = memberType(type, key);
      const theirs = memberType(reference, key);
      return !(
        checker.isTypeAssignableTo(ours, theirs) && checker.isTypeAssignableTo(theirs, ours)
      );
    })
    .map((key) => `${key}: ${checker.typeToString(memberType(type, key))}`);
}

/**
 * Chain members whose token set is wider than the processor catalog's: the chain
 * reader accepts these tokens and the catalog entry for the processor leaves them out.
 */
const WIDER_THAN_PROCESSOR: Readonly<Record<string, string>> = {
  'repair.decrackle.mode':
    'the chain reader accepts waveletShrinkage; the catalog lists median only',
  'repair.denoise.noiseEstimator': 'the chain reader accepts quantile; the catalog omits it',
};
const widerUsed = new Set<string>();

/** The processor's generated parameter type. */
const generatedParams = (id: string): ts.Type =>
  memberType(exportedType('MasteringProcessorParamMap'), id);

/** Chain list keys embedding `{i}`: a flat spelling of list elements, never a nested member. */
const patternListKeys = new Set(
  catalog.chain.lists.filter((l) => l.key.includes('{i}')).map((l) => l.key),
);

/** Direct members of the object at @p path whose type disagrees with its catalog chain parameter. */
function catalogMismatches(path: readonly string[]): string[] {
  const prefix = path.join('.');
  const type = typeAt(path);
  return catalog.chain.params
    .filter(
      (p) =>
        p.name.startsWith(`${prefix}.`) &&
        !p.name.slice(prefix.length + 1).includes('.') &&
        !patternListKeys.has(p.name),
    )
    .filter((p) => {
      const ours = memberType(type, p.name.slice(prefix.length + 1));
      if (p.type === 'enum') {
        const tokens = ours.isUnion() ? ours.types : [ours];
        const names = tokens.map((t) => (t.isStringLiteral() ? t.value : '?')).sort();
        return JSON.stringify(names) !== JSON.stringify(p.choices?.map((c) => c.name).sort());
      }
      const flag = p.type === 'boolean' ? ts.TypeFlags.BooleanLike : ts.TypeFlags.NumberLike;
      const parts = ours.isUnion() ? ours.types : [ours];
      return !parts.every((t) => (t.flags & flag) !== 0);
    })
    .map((p) => p.name);
}

const processorParams = (id: string): string[] => {
  const processor = catalog.processors.find((p) => p.id === id);
  if (!processor) {
    throw new Error(`no catalog processor ${id}`);
  }
  return processor.params.map((p) => p.name);
};
const chainParamsUnder = (prefix: string): string[] =>
  catalog.chain.params
    .filter((p) => p.name.startsWith(`${prefix}.`))
    .map((p) => p.name.slice(prefix.length + 1));
const nested = (prefix: string): boolean =>
  chainParamsUnder(prefix).some((rest) => rest.includes('.'));

describe('the chain config type', () => {
  it('declares exactly the modules and stages the catalog lists', () => {
    const declared = keysAt([]).flatMap((module) =>
      module === 'loudness' ? ['loudness'] : keysAt([module]).map((s) => `${module}.${s}`),
    );
    expect(declared.sort()).toEqual(catalog.chain.stages.map((s) => s.prefix).sort());
  });

  it.each(catalog.chain.stages.filter((s) => s.processor && !nested(s.prefix)))(
    'stage $prefix takes its processor parameters, typed alike, and enabled',
    ({ prefix, processor }) => {
      const path = prefix.split('.');
      const expected = [...processorParams(processor as string), 'enabled'].sort();
      expect(keysAt(path)).toEqual(expected);
      const mismatched = typeMismatches(typeAt(path), generatedParams(processor as string))
        .map((entry) => `${prefix}.${entry}`)
        .filter((entry) => {
          const key = entry.slice(0, entry.indexOf(':'));
          if (!(key in WIDER_THAN_PROCESSOR)) {
            return true;
          }
          widerUsed.add(key);
          return false;
        });
      expect(mismatched).toEqual([]);
      expect(catalogMismatches(path)).toEqual([]);
    },
  );

  it('the nested multiband stage matches the chain parameters and its band list', () => {
    const path = 'dynamics.multibandComp'.split('.');
    expect(nested(path.join('.'))).toBe(true);
    const bands = catalog.chain.lists.find((l) => l.key === `${path.join('.')}.bands`);
    const bandProcessor = bands?.processor;
    if (!bandProcessor) {
      throw new Error('the band list names no processor');
    }
    expect(keysAt(path)).toEqual(['bands', 'crossover', 'enabled']);
    expect(keysAt([...path, 'crossover'])).toEqual(
      chainParamsUnder(`${path.join('.')}.crossover`).sort(),
    );
    expect(keysAt([...path, 'bands'])).toEqual(processorParams(bandProcessor).sort());
    expect(typeMismatches(typeAt([...path, 'bands']), generatedParams(bandProcessor))).toEqual([]);
    expect(catalogMismatches(path)).toEqual([]);
    expect(catalogMismatches([...path, 'crossover'])).toEqual([]);
  });

  it('the loudness stage matches its chain parameters', () => {
    expect(keysAt(['loudness'])).toEqual(chainParamsUnder('loudness').sort());
    expect(catalogMismatches(['loudness'])).toEqual([]);
  });

  it('excuses no member that agrees with its processor', () => {
    expect(Object.keys(WIDER_THAN_PROCESSOR).filter((key) => !widerUsed.has(key))).toEqual([]);
  });

  it('tells a token union from one narrowed by a token', () => {
    const mode = memberType(typeAt(['repair', 'denoise']), 'mode');
    const narrowed = mode.isUnion() ? mode.types[0] : mode;
    expect(checker.isTypeAssignableTo(mode, narrowed)).toBe(false);
  });
});
