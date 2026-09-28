// The WASM module embeds no HRTF data, so the default set ships as a package
// asset (`@libraz/libsonare/hrtf/default.shrf`) that callers load and hand to
// `HrtfSet.fromBytes`.
import { cp, mkdir, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const scriptDirectory = new URL('.', import.meta.url);
const source = new URL('../../../src/playback/default.shrf', scriptDirectory);
const outputDirectory = new URL('../dist/hrtf/', scriptDirectory);

await rm(outputDirectory, { force: true, recursive: true });
await mkdir(outputDirectory, { recursive: true });
await cp(fileURLToPath(source), fileURLToPath(new URL('default.shrf', outputDirectory)));
