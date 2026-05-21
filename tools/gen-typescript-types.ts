import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

type TypeName =
  | 'MasteringPreset'
  | 'SoloProcessor'
  | 'PairProcessor'
  | 'PairAnalysis'
  | 'StereoAnalysis';

const sources: Record<TypeName, { file: string; marker: string }> = {
  MasteringPreset: {
    file: 'src/mastering/api/presets.cpp',
    marker: 'preset_names',
  },
  SoloProcessor: {
    file: 'src/mastering/api/named_processor.cpp',
    marker: 'processor_names',
  },
  PairProcessor: {
    file: 'src/mastering/api/named_processor.cpp',
    marker: 'pair_processor_names',
  },
  PairAnalysis: {
    file: 'src/mastering/api/named_processor.cpp',
    marker: 'pair_analysis_names',
  },
  StereoAnalysis: {
    file: 'src/mastering/api/named_processor.cpp',
    marker: 'stereo_analysis_names',
  },
};

function extractStringList(file: string, marker: string): string[] {
  const source = readFileSync(resolve(root, file), 'utf8');
  const start = source.indexOf(`${marker}()`);
  if (start < 0) {
    throw new Error(`Could not find ${marker} in ${file}`);
  }
  const body = source.slice(start);
  const match = body.match(/return\s*\{([\s\S]*?)\};/);
  if (!match) {
    throw new Error(`Could not find return list for ${marker} in ${file}`);
  }
  return [...match[1].matchAll(/"([^"]+)"/g)].map((item) => item[1]);
}

function formatUnion(typeName: TypeName, values: string[]): string {
  if (values.length === 0) {
    throw new Error(`${typeName} has no values`);
  }
  if (values.length === 1) {
    return `export type ${typeName} = '${values[0]}';`;
  }
  const lines = values.map((value, index) => {
    const prefix = index === 0 ? '=' : '|';
    const suffix = index === values.length - 1 ? ';' : '';
    return `  ${prefix} '${value}'${suffix}`;
  });
  return [`export type ${typeName}`, ...lines].join('\n');
}

function main() {
  const output = (Object.keys(sources) as TypeName[])
    .map((typeName) => {
      const source = sources[typeName];
      return formatUnion(typeName, extractStringList(source.file, source.marker));
    })
    .join('\n\n');
  process.stdout.write(`${output}\n`);
}

main();
