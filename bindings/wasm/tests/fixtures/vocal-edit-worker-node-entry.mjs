import { parentPort } from 'node:worker_threads';
import { installVocalEditWorkerEndpoint } from '../../dist/vocal_edit_worker.js';

if (!parentPort) {
  throw new Error('vocal edit worker thread requires a parent port');
}

installVocalEditWorkerEndpoint({
  postMessage(message, transfer) {
    parentPort.postMessage(message, transfer);
  },
  addEventListener(_type, listener) {
    parentPort.on('message', (data) => listener({ data }));
  },
});
