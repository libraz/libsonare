#!/usr/bin/env node
// Generate a type-level comparison for every Node type also published by WASM.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const here = __dirname;
const root = path.resolve(here, '../../../..');
const target = path.join(here, 'shared-types.generated.mts');
const exceptionFile = path.join(here, 'shared-types.exceptions.json');
const typeKinds = new Set(['enum', 'interface', 'type']);
const nodePackage = path.join(root, 'bindings/node');
const wasmPackage = path.join(root, 'bindings/wasm');
const ts = require(path.join(nodePackage, 'node_modules/typescript'));
const nodeEntries = ['index.ts'];
const wasmManifest = JSON.parse(fs.readFileSync(path.join(wasmPackage, 'package.json'), 'utf8'));
const wasmEntries = Object.values(wasmManifest.exports)
  .filter((value) => value && typeof value === 'object' && value.types)
  .map((value) => path.basename(value.types).replace(/\.d\.ts$/, '.ts'));

function extract(packageDir, entries) {
  const tool = path.join(root, 'tools/api/ts_exports.cjs');
  const output = execFileSync(process.execPath, [tool, packageDir, ...entries], {
    cwd: root,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
  });
  return JSON.parse(output);
}

const nodeRows = extract(nodePackage, nodeEntries);
const wasmRows = extract(wasmPackage, wasmEntries);
const nodeTypes = nodeRows.filter((row) => typeKinds.has(row.kind));
const wasmTypes = wasmRows.filter((row) => typeKinds.has(row.kind));
const nodeByName = new Map(nodeTypes.map((row) => [row.name, row]));
const wasmByName = new Map();
for (const row of wasmTypes) {
  const list = wasmByName.get(row.name) ?? [];
  if (!list.some((item) => item.entry === row.entry)) list.push(row);
  wasmByName.set(row.name, list);
}
const shared = [...nodeByName.keys()]
  .filter((name) => wasmByName.has(name))
  .sort((a, b) => a.localeCompare(b, 'en'));
const wasmClasses = new Set(wasmRows.filter((row) => row.kind === 'class').map((row) => row.name));
const sharedClasses = new Set(
  nodeRows.filter((row) => row.kind === 'class' && wasmClasses.has(row.name)).map((row) => row.name),
);
if (!nodeTypes.length || !wasmTypes.length || !shared.length) {
  throw new Error(
    `type export scan was empty (node=${nodeTypes.length}, wasm=${wasmTypes.length}, shared=${shared.length})`,
  );
}

const pairs = [];
for (const name of shared) {
  for (const wasm of wasmByName.get(name)) pairs.push({ name, node: nodeByName.get(name), wasm });
}
const keyOf = ({ name, wasm }) => `${name}@${wasm.entry}`;
const pairKeys = new Set(pairs.map(keyOf));
let exceptions = {};
if (fs.existsSync(exceptionFile)) {
  exceptions = JSON.parse(fs.readFileSync(exceptionFile, 'utf8'));
  if (!exceptions || Array.isArray(exceptions) || typeof exceptions !== 'object') {
    throw new Error('shared-types.exceptions.json must be a JSON object');
  }
}
for (const [key, reason] of Object.entries(exceptions)) {
  if (!pairKeys.has(key)) throw new Error(`unused shared-type exception: ${key}`);
  if (typeof reason !== 'string' || !reason.trim() || reason.includes('\n')) {
    throw new Error(`exception ${key} needs a one-line reason`);
  }
}

// Instantiate generic exports with defaults or witnesses that satisfy their constraints.
const declarationCache = new Map();
function declarationParams(row) {
  const absolute = path.resolve(root, row.file);
  let source = declarationCache.get(absolute);
  if (!source) {
    source = ts.createSourceFile(
      absolute,
      fs.readFileSync(absolute, 'utf8'),
      ts.ScriptTarget.Latest,
      true,
    );
    declarationCache.set(absolute, source);
  }
  const declarations = source.statements.filter((statement) =>
    (ts.isTypeAliasDeclaration(statement) || ts.isInterfaceDeclaration(statement) || ts.isEnumDeclaration(statement)) &&
    statement.name.text === row.name,
  );
  let found = declarations[0];
  if (!found) {
    found = source.statements.find((statement) => {
      if (!ts.isTypeAliasDeclaration(statement) && !ts.isInterfaceDeclaration(statement) && !ts.isEnumDeclaration(statement)) return false;
      const line = source.getLineAndCharacterOfPosition(statement.getStart(source)).line + 1;
      return line === row.line;
    });
  }
  if (!found) throw new Error(`could not find declaration for ${row.name} at ${row.file}:${row.line}`);
  return found.typeParameters ?? [];
}
function typeRef(namespace, row) {
  const params = declarationParams(row);
  if (!params.length || params.every((param) => param.default)) return `${namespace}.${row.name}`;
  const args = params.map(({ constraint }) => {
    if (!constraint) return '{ samples: Float32Array; sampleRate: number; value: number }';
    if (
      ts.isTypeOperatorNode(constraint) &&
      constraint.operator === ts.SyntaxKind.KeyOfKeyword &&
      ts.isTypeReferenceNode(constraint.type) &&
      ts.isIdentifier(constraint.type.typeName)
    ) {
      return `keyof ${namespace}.${constraint.type.typeName.text}`;
    }
    throw new Error(`no shared witness for ${row.name} constraint ${constraint.getText()}`);
  });
  return `${namespace}.${row.name}<${args.join(', ')}>`;
}

const entryAliases = new Map();
for (const row of wasmTypes) {
  if (!entryAliases.has(row.entry)) entryAliases.set(row.entry, `W${entryAliases.size}`);
}
const lines = [
  '// Generated by generate-shared-types.cjs. Do not edit.',
  '// Shared request/result records are compared at every WASM package entry that exports them.',
  '// Runtime handle classes are intentionally outside this record/type parity check.',
  'import type * as N from "../../src/index.js";',
  ...[...entryAliases.entries()].map(([entry, alias]) => {
    const specifier = `../../../wasm/src/${entry.replace(/\.ts$/, '.js')}`;
    return `import type * as ${alias} from ${JSON.stringify(specifier)};`;
  }),
  '',
  'type IsAssignable<A, B> = [A] extends [B] ? true : false;',
  'type Equal<A, B> =',
  '  (<T>() => T extends A ? 1 : 2) extends (<T>() => T extends B ? 1 : 2)',
  '    ? true',
  '    : false;',
  'type ReadonlyKeys<T> = T extends unknown',
  '  ? {',
  '      [K in keyof T]-?: Equal<Pick<T, K>, Readonly<Pick<T, K>>> extends true ? K : never;',
  '    }[keyof T]',
  '  : never;',
  'type Matches<A, B> = IsAssignable<A, B> extends true',
  '  ? IsAssignable<B, A> extends true',
  '    ? Equal<ReadonlyKeys<A>, ReadonlyKeys<B>>',
  '    : false',
  '  : false;',
  'type Assert<T extends true> = T;',
  '',
];
for (let index = 0; index < pairs.length; index += 1) {
  const pair = pairs[index];
  const key = keyOf(pair);
  const node = typeRef('N', pair.node);
  const wasm = typeRef(entryAliases.get(pair.wasm.entry), pair.wasm);
  const name = `_SharedType${String(index).padStart(4, '0')}`;
  if (exceptions[key]) {
    lines.push(`// EXCEPTION ${key}: ${exceptions[key]}`);
    lines.push(`type ${name} = Assert<Matches<${node}, ${wasm}> extends false ? true : false>;`);
  } else {
    lines.push(`// ${key}`);
    lines.push(`type ${name} = Assert<Matches<${node}, ${wasm}>>;`);
  }
}
lines.push('', `// ${shared.length} shared names; ${pairs.length} entry-level checks; ${sharedClasses.size} shared class(es) excluded.`, '');
const expected = lines.join('\n');
if (process.argv.includes('--check')) {
  if (!fs.existsSync(target) || fs.readFileSync(target, 'utf8') !== expected) {
    process.stderr.write('shared type test is stale; run node bindings/node/tests/typing/generate-shared-types.cjs\n');
    process.exit(1);
  }
} else {
  fs.writeFileSync(target, expected);
  process.stdout.write(
    `generated ${target}: ${shared.length} shared names, ${pairs.length} entry-level checks, ${sharedClasses.size} shared class(es) excluded\n`,
  );
}
