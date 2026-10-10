// Headless test runner: runs the wasm build in Node and dumps screenshots.
// Usage: node port/tools/headless.mjs <frames> <outdir> [script]
// script: comma separated "frame:keys" entries, e.g. "300:A,310:,400:START"
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const createEmerald = require(path.resolve(process.env.DIST || 'port/dist', 'emerald.js'));

const KEYS = { A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512 };

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
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
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

const frames = parseInt(process.argv[2] || '600');
const outdir = process.argv[3] || 'shots';
const script = (process.argv[4] || '').split(',').filter(Boolean).map(s => {
  const [f, k] = s.split(':');
  return [parseInt(f), (k || '').split('+').filter(Boolean).reduce((m, n) => m | KEYS[n], 0)];
});
const shotEvery = parseInt(process.env.SHOT_EVERY || '60');
fs.mkdirSync(outdir, { recursive: true });

const mod = await createEmerald();
const savePath = process.env.SAVE;
const flashPtr = mod._Port_GetFlash();
if (savePath && fs.existsSync(savePath)) {
  mod.HEAPU8.set(fs.readFileSync(savePath), flashPtr);
  mod._Port_InitFlash(1);
} else {
  mod._Port_InitFlash(0);
}
const t0 = Date.now();
for (let f = 0; f < frames; f++) {
  for (const [sf, keys] of script) if (sf === f) mod._Port_SetKeys(keys);
  mod._Port_Frame();
  if ((f + 1) % shotEvery === 0 || f === frames - 1) {
    const ptr = mod._Port_GetFramebuffer();
    const rgba = Buffer.from(mod.HEAPU8.buffer, ptr, 240 * 160 * 4);
    fs.writeFileSync(path.join(outdir, `f${String(f + 1).padStart(5, '0')}.png`), png(Buffer.from(rgba), 240, 160));
  }
  if (savePath && mod._Port_TakeFlashDirty()) {
    fs.writeFileSync(savePath, Buffer.from(mod.HEAPU8.buffer, flashPtr, mod._Port_GetFlashSize()));
  }
}
console.log(`${frames} frames in ${Date.now() - t0} ms`);
