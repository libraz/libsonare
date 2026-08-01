import { parentPort } from 'node:worker_threads';
import { installOfflineWorkerEndpoint } from '../../dist/worker.js';

if (!parentPort) {
  throw new Error('offline worker thread requires a parent port');
}

installOfflineWorkerEndpoint({
  postMessage(message, transfer) {
    parentPort.postMessage(message, transfer);
  },
  addEventListener(_type, listener) {
    parentPort.on('message', (data) => listener({ data }));
  },
});
