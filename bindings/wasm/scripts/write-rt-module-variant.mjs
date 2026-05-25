import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';

const dist = path.resolve(new URL('../dist', import.meta.url).pathname);
const input = path.join(dist, 'sonare-rt.js');
const output = path.join(dist, 'sonare-rt-module.js');

const source = await readFile(input, 'utf8');
let patched = source
  .replace(
    'var ENVIRONMENT_IS_AUDIO_WORKLET=!!globalThis.AudioWorkletGlobalScope;',
    'var ENVIRONMENT_IS_AUDIO_WORKLET=false;',
  )
  .replace(
    'var ENVIRONMENT_IS_WORKER=!!globalThis.WorkerGlobalScope;',
    'var ENVIRONMENT_IS_WORKER=!!globalThis.WorkerGlobalScope||!!globalThis.AudioWorkletGlobalScope;',
  )
  .replace(/isWW&&SonareRt\(\);?\s*$/, 'false&&isWW&&SonareRt();');
if (patched === source || patched.includes('var ENVIRONMENT_IS_AUDIO_WORKLET=!!globalThis')) {
  throw new Error('Could not disable sonare-rt AudioWorklet auto-run footer.');
}
await writeFile(output, patched);
