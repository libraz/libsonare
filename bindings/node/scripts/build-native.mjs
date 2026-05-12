#!/usr/bin/env node
/**
 * Thin wrapper around `cmake-js compile` that forwards the SONARE_FFMPEG
 * environment variable as a CMake cache definition. Keeps the package.json
 * script portable across shells (npm/yarn on Windows do not run bash).
 *
 * Usage:
 *   yarn build:native                   # AUTO detect via pkg-config (WAV/MP3 if absent, +M4A/AAC/FLAC/OGG if present)
 *   SONARE_FFMPEG=1 yarn build:native   # require FFmpeg (fails if dev libs missing)
 *   SONARE_FFMPEG=0 yarn build:native   # force OFF, never link FFmpeg
 */
import { spawn } from 'node:child_process';

const args = ['compile'];
const flag = process.env.SONARE_FFMPEG;
if (flag === '1' || flag === 'ON' || flag === 'on') {
  args.push('--CDSONARE_WITH_FFMPEG=ON');
  console.log('[build-native] FFmpeg support required (SONARE_WITH_FFMPEG=ON)');
} else if (flag === '0' || flag === 'OFF' || flag === 'off') {
  args.push('--CDSONARE_WITH_FFMPEG=OFF');
  console.log('[build-native] FFmpeg support disabled (SONARE_WITH_FFMPEG=OFF)');
} else {
  console.log(
    '[build-native] FFmpeg support: AUTO (detect via pkg-config; set SONARE_FFMPEG=1 to require, =0 to disable)',
  );
}

const child = spawn('cmake-js', args, {
  stdio: 'inherit',
  shell: process.platform === 'win32',
});
child.on('exit', (code) => process.exit(code ?? 1));
