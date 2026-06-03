import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
export const addon = require('../build/Release/sonare-node.node');
