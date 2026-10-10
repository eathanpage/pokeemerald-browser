// Worker used by play.mjs: pauses the main thread through the inspector after
// a timeout and prints where it was stuck.
const { workerData, parentPort } = require('node:worker_threads');
const inspector = require('node:inspector');
const fs = require('node:fs');

setInterval(() => {}, 1000); // keep the worker alive
const session = new inspector.Session();
session.connectToMainThread();
session.on('Debugger.paused', (msg) => {
  const frames = msg.params.callFrames.map((f) => `  at ${f.functionName || '?'} (${f.url})`);
  fs.writeSync(2, `Watchdog: main thread stuck after ${workerData}s:\n${frames.join('\n')}\n`);
  process.kill(process.pid, 'SIGKILL');
});
session.post('Debugger.enable', () => {
  parentPort.postMessage('ready');
  setTimeout(() => session.post('Debugger.pause'), workerData * 1000);
});
