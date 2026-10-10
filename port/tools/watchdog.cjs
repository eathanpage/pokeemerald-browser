// Worker used by play.mjs: if the main thread stops completing frames for
// `timeout` seconds, pause it through the inspector and print the stack.
const { workerData, parentPort } = require('node:worker_threads');
const inspector = require('node:inspector');
const fs = require('node:fs');

const { timeout, heartbeat } = workerData;
const beat = new Int32Array(heartbeat);
let last = Atomics.load(beat, 0);
let stalled = 0;

const keepAlive = setInterval(() => {}, 1000);
const session = new inspector.Session();
session.connectToMainThread();
session.on('Debugger.paused', (msg) => {
  const frames = msg.params.callFrames.map((f) => `  at ${f.functionName || '?'} (${f.url})`);
  fs.writeSync(2, `Watchdog: no frame completed for ${timeout}s, main thread is in:\n${frames.join('\n')}\n`);
  process.kill(process.pid, 'SIGKILL');
});
session.post('Debugger.enable', () => {
  parentPort.postMessage('ready');
  setInterval(() => {
    const now = Atomics.load(beat, 0);
    stalled = now === last ? stalled + 1 : 0;
    last = now;
    if (stalled >= timeout)
      session.post('Debugger.pause');
  }, 1000);
});
