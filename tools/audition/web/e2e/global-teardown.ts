import { readFileSync, rmSync } from 'node:fs';
import { PID_FILE, ROOT } from './env';

export default async function teardown(): Promise<void> {
  try {
    process.kill(Number(readFileSync(PID_FILE, 'utf8')), 'SIGTERM');
  } catch {
    // already gone
  }
  rmSync(ROOT, { recursive: true, force: true });
}
