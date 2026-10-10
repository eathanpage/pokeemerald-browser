// Scriptable headless player for testing the port.
//
// Usage: node port/tools/play.mjs <script-file | -e "commands"> [outdir]
//
// Commands (one per line or separated by ';'):
//   wait N            run N frames with no input
//   press KEY[+KEY] [N]  hold the keys for N frames (default 6), then release
//                     and wait 10 frames
//   hold KEY[+KEY] N  hold the keys for N frames, then release
//   repeat N CMD...   run a command N times
//   shot NAME         save a screenshot as NAME.png
//   snap FILE         save the whole emulator state to FILE
//   load FILE         restore the emulator state from FILE
//   savefile FILE     write the 128K flash save to FILE
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const createEmerald = require(path.resolve(process.env.DIST || 'port/dist', 'emerald.js'));

const KEYS = { A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512 };

function crc32(buf) {
  let crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    let c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// WATCHDOG=<seconds>: if the game hangs, print the stuck call stack and exit.
if (process.env.WATCHDOG) {
  const { Worker } = await import('node:worker_threads');
  const worker = new Worker(new URL('./watchdog.cjs', import.meta.url), { workerData: Number(process.env.WATCHDOG) });
  await new Promise((resolve) => worker.once('message', resolve));
  worker.unref();
}

const arg = process.argv[2];
const script = arg === '-e' ? process.argv[3] : fs.readFileSync(arg, 'utf8');
const outdir = (arg === '-e' ? process.argv[4] : process.argv[3]) || 'shots';
fs.mkdirSync(outdir, { recursive: true });

let mod = await createEmerald();
if (process.env.FLASH) {
  mod.HEAPU8.set(fs.readFileSync(process.env.FLASH), mod._Port_GetFlash());
  mod._Port_InitFlash(1);
} else {
  mod._Port_InitFlash(0);
}
let frame = 0;

function run(n, keys = 0) {
  mod._Port_SetKeys(keys);
  for (let i = 0; i < n; i++) {
    mod._Port_Frame();
    frame++;
  }
  mod._Port_SetKeys(0);
}

function parseKeys(s) {
  return s.split('+').reduce((m, k) => {
    if (!(k in KEYS)) throw new Error(`unknown key ${k}`);
    return m | KEYS[k];
  }, 0);
}

function exec(line) {
  const parts = line.trim().split(/\s+/);
  const cmd = parts[0];
  switch (cmd) {
    case '':
      break;
    case 'wait':
      run(parseInt(parts[1]));
      break;
    case 'press':
      run(parseInt(parts[2] || '6'), parseKeys(parts[1]));
      run(10);
      break;
    case 'hold':
      run(parseInt(parts[2]), parseKeys(parts[1]));
      break;
    case 'repeat': {
      const n = parseInt(parts[1]);
      for (let i = 0; i < n; i++) exec(parts.slice(2).join(' '));
      break;
    }
    case 'shot': {
      const ptr = mod._Port_GetFramebuffer();
      fs.writeFileSync(path.join(outdir, `${parts[1]}.png`), png(Buffer.from(mod.HEAPU8.slice(ptr, ptr + 240 * 160 * 4)), 240, 160));
      break;
    }
    case 'snap':
      fs.writeFileSync(parts[1], zlib.gzipSync(mod.HEAPU8, { level: 1 }));
      break;
    case 'load': {
      const mem = zlib.gunzipSync(fs.readFileSync(parts[1]));
      mod.HEAPU8.set(mem);
      break;
    }
    case 'battle':
      mod._Port_TestWildBattle(parseInt(parts[1]), parseInt(parts[2] || '5'));
      break;
    case 'dump':
      mod._Port_DebugDump();
      break;
    case 'savefile': {
      const ptr = mod._Port_GetFlash();
      fs.writeFileSync(parts[1], mod.HEAPU8.slice(ptr, ptr + mod._Port_GetFlashSize()));
      break;
    }
    default:
      throw new Error(`unknown command ${cmd}`);
  }
}

const t0 = Date.now();
for (const line of script.split(/[\n;]/)) {
  if (line.trim().startsWith('#')) continue;
  exec(line);
}
console.log(`${frame} frames in ${Date.now() - t0} ms`);
