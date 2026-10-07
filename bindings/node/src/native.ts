import { createRequire } from 'node:module';
import { checkAbiVersion } from './abi.js';

const require = createRequire(import.meta.url);
export const addon = require('../build/Release/sonare-node.node');
checkAbiVersion(addon.abiVersion?.());
