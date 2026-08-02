import { cp, mkdir, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const schemas = [
  'realtime-voice-changer-preset.schema.json',
  'realtime-voice-changer-preset-pack.schema.json',
];
const scriptDirectory = new URL('.', import.meta.url);
const sourceDirectory = new URL('../../../schemas/', scriptDirectory);
const outputDirectory = new URL('../dist/schemas/', scriptDirectory);

await rm(outputDirectory, { force: true, recursive: true });
await mkdir(outputDirectory, { recursive: true });
await Promise.all(
  schemas.map((schema) =>
    cp(fileURLToPath(new URL(schema, sourceDirectory)), fileURLToPath(new URL(schema, outputDirectory))),
  ),
);
