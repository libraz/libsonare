import { defineConfig } from 'tsup';

export default defineConfig({
  entry: ['src/index.ts', 'src/worklet.ts'],
  format: ['esm'],
  dts: true,
  sourcemap: true,
  clean: false,
  target: 'es2020',
  external: ['./sonare.js'],
});
