/* Where the e2e run keeps its synthetic renders and its own scratch root. */

import { tmpdir } from 'node:os';
import { join } from 'node:path';

export const PORT = 8749;
export const ROOT = join(tmpdir(), 'audition-e2e');
export const SCRATCH = join(ROOT, 'scratch');
export const SETS = join(ROOT, 'sets');
export const FEEDBACK = join(SCRATCH, 'feedback');
export const PID_FILE = join(ROOT, 'server.pid');
