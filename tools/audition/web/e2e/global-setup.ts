/* Write two synthetic sets, build the page, and start serve.py over them.
 *
 * The scratch root is a fresh temp directory, so the run reads and writes its
 * own feedback log and never a listener's. Names are invented. */

import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FEEDBACK, PID_FILE, PORT, ROOT, SCRATCH, SETS } from './env';

const WEB = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SERVE = resolve(WEB, '..', 'serve.py');

/** A mono 16-bit sine, `seconds` long. */
function sine(freq: number, seconds = 1.5, rate = 48000): Buffer {
  const n = Math.round(seconds * rate);
  const data = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const fade = Math.min(1, i / 480, (n - i) / 4800);
    data.writeInt16LE(
      Math.round(Math.sin((2 * Math.PI * freq * i) / rate) * 0.3 * fade * 32767),
      i * 2,
    );
  }
  const head = Buffer.alloc(44);
  head.write('RIFF', 0);
  head.writeUInt32LE(36 + data.length, 4);
  head.write('WAVE', 8);
  head.write('fmt ', 12);
  head.writeUInt32LE(16, 16);
  head.writeUInt16LE(1, 20);
  head.writeUInt16LE(1, 22);
  head.writeUInt32LE(rate, 24);
  head.writeUInt32LE(rate * 2, 28);
  head.writeUInt16LE(2, 32);
  head.writeUInt16LE(16, 34);
  head.write('data', 36);
  head.writeUInt32LE(data.length, 40);
  return Buffer.concat([head, data]);
}

function writeSet(
  id: string,
  manifest: Record<string, unknown>,
  files: Record<string, number>,
): void {
  const root = join(SETS, id);
  for (const [rel, freq] of Object.entries(files)) {
    mkdirSync(dirname(join(root, rel)), { recursive: true });
    writeFileSync(join(root, rel), sine(freq));
  }
  writeFileSync(join(root, 'manifest.json'), JSON.stringify(manifest, null, 2));
}

async function waitForServer(): Promise<void> {
  for (let i = 0; i < 100; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/sets.json`);
      if (res.ok) {
        return;
      }
    } catch {
      // not up yet
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('serve.py did not come up');
}

export default async function setup(): Promise<void> {
  rmSync(ROOT, { recursive: true, force: true });
  mkdirSync(FEEDBACK, { recursive: true });
  writeSet(
    'e2e-grand',
    {
      title: 'Test Grand',
      group: 'pianos',
      notes: 'synthetic',
      schema_version: 2,
      set_generation: 'e'.repeat(64),
      voice: { program: 0, bank: 0, name: 'Test Grand', rig: 'amp' },
      sources: {
        model: { label: 'library render', role: 'model', scope: 'product' },
        ref: { label: 'Invented Recording', role: 'reference' },
      },
      comparisons: [
        {
          id: 'gm_gs_product',
          scope: 'product',
          model_sources: ['model'],
          oracle_sources: ['ref'],
          status: 'matched',
          reasons: [],
        },
      ],
      items: [
        {
          id: 'music',
          label: 'Music',
          tracks: { model: 'music/model.wav', ref: 'music/ref.wav' },
          meta: { notes: [{ start: 0, note: 60, velocity: 100 }] },
        },
        { id: 'single-c4', label: 'C4', tracks: { model: 'c4/model.wav', ref: 'c4/ref.wav' } },
      ],
    },
    { 'music/model.wav': 261.6, 'music/ref.wav': 262, 'c4/model.wav': 261.6, 'c4/ref.wav': 262 },
  );
  writeSet(
    'e2e-organ',
    {
      title: 'Test Organ',
      voice: { program: 19, name: 'Test Organ' },
      sources: { model: { label: 'library render', role: 'model' } },
      items: [{ id: 'music', label: 'Music', tracks: { model: 'music/model.wav' } }],
    },
    { 'music/model.wav': 440 },
  );

  const build = spawnSync('yarn', ['build'], { cwd: WEB, stdio: 'inherit' });
  if (build.status !== 0) {
    throw new Error('the page did not build');
  }
  const server = spawn(
    'python3',
    [SERVE, '--no-open', '--port', String(PORT), join(SETS, 'e2e-grand'), join(SETS, 'e2e-organ')],
    { env: { ...process.env, SONARE_VOICEMATCH_ROOT: SCRATCH }, stdio: 'ignore', detached: true },
  );
  server.unref();
  writeFileSync(PID_FILE, String(server.pid));
  await waitForServer();
}
