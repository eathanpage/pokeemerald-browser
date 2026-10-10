#!/usr/bin/env python3
"""Makes integer division in a linked wasm module non-trapping.

WebAssembly traps on integer division by zero (and INT_MIN / -1); the GBA's
division routines return 0 instead, and the game relies on that in a few
places. This rewrites every i32/i64 div/rem instruction whose divisor isn't a
safe constant into a call to the matching exported helper from
port/src/divide.c (Port_DivS32 and friends).

Usage: wasm_safe_div.py module.wasm   (rewritten in place)
"""

import struct
import sys

# opcode -> helper export name
DIV_OPS = {
    0x6D: 'Port_DivS32', 0x6E: 'Port_DivU32', 0x6F: 'Port_RemS32', 0x70: 'Port_RemU32',
    0x7F: 'Port_DivS64', 0x80: 'Port_DivU64', 0x81: 'Port_RemS64', 0x82: 'Port_RemU64',
}


class Reader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def byte(self):
        b = self.data[self.pos]
        self.pos += 1
        return b

    def uleb(self):
        result = shift = 0
        while True:
            b = self.byte()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return result

    def sleb(self):
        result = shift = 0
        while True:
            b = self.byte()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    result -= 1 << shift
                return result

    def name(self):
        n = self.uleb()
        s = self.data[self.pos:self.pos + n].decode('utf-8')
        self.pos += n
        return s


def uleb_bytes(value):
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def skip_blocktype(r):
    b = r.data[r.pos]
    if b == 0x40 or b in (0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x70, 0x6F):
        r.pos += 1
    else:
        r.sleb()


def rewrite_body(body, helpers):
    """Returns (new body bytes, number of rewritten instructions)."""
    r = Reader(body)
    out = bytearray()
    # locals
    count = r.uleb()
    for _ in range(count):
        r.uleb()
        r.byte()
    out += body[:r.pos]

    rewritten = 0
    prev_const = None  # value of an immediately preceding i32/i64.const
    while r.pos < len(body):
        start = r.pos
        op = r.byte()
        const = None
        if op in DIV_OPS:
            if prev_const is not None and prev_const not in (0, -1):
                out += body[start:r.pos]
            else:
                out += b'\x10' + uleb_bytes(helpers[DIV_OPS[op]])
                rewritten += 1
            prev_const = None
            continue
        if op in (0x02, 0x03, 0x04):
            skip_blocktype(r)
        elif op in (0x0C, 0x0D, 0x10, 0x12, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0xD2):
            r.uleb()
        elif op == 0x0E:
            for _ in range(r.uleb()):
                r.uleb()
            r.uleb()
        elif op in (0x11, 0x13):
            r.uleb()
            r.uleb()
        elif op == 0x1C:
            for _ in range(r.uleb()):
                r.byte()
        elif 0x28 <= op <= 0x3E:
            r.uleb()
            r.uleb()
        elif op in (0x3F, 0x40):
            r.uleb()
        elif op == 0x41:
            const = r.sleb()
        elif op == 0x42:
            const = r.sleb()
        elif op == 0x43:
            r.pos += 4
        elif op == 0x44:
            r.pos += 8
        elif op == 0xD0:
            r.byte()
        elif op == 0xFC:
            sub = r.uleb()
            if sub <= 7:
                pass
            elif sub == 8:
                r.uleb()
                r.uleb()
            elif sub == 9:
                r.uleb()
            elif sub == 10:
                r.uleb()
                r.uleb()
            elif sub == 11:
                r.uleb()
            elif sub == 12:
                r.uleb()
                r.uleb()
            elif sub == 13:
                r.uleb()
            elif sub == 14:
                r.uleb()
                r.uleb()
            elif sub in (15, 16, 17):
                r.uleb()
            else:
                raise SystemExit(f'wasm_safe_div: unknown 0xFC opcode {sub}')
        elif op == 0xFD:
            raise SystemExit('wasm_safe_div: SIMD instructions are not supported')
        elif op in (0x00, 0x01, 0x05, 0x0B, 0x0F, 0x1A, 0x1B, 0xD1) or 0x45 <= op <= 0xC4:
            pass
        else:
            raise SystemExit(f'wasm_safe_div: unknown opcode 0x{op:02x}')
        out += body[start:r.pos]
        prev_const = const
    return bytes(out), rewritten


def main():
    path = sys.argv[1]
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'\0asm'

    sections = []  # (id, start of payload, end)
    r = Reader(data, 8)
    while r.pos < len(data):
        sec_id = r.byte()
        size = r.uleb()
        sections.append((sec_id, r.pos, r.pos + size))
        r.pos += size

    num_imported_funcs = 0
    exports = {}
    for sec_id, start, end in sections:
        if sec_id == 2:
            r = Reader(data, start)
            for _ in range(r.uleb()):
                r.name()
                r.name()
                kind = r.byte()
                if kind == 0:
                    r.uleb()
                    num_imported_funcs += 1
                elif kind == 1:
                    r.byte()
                    flags = r.uleb()
                    r.uleb()
                    if flags & 1:
                        r.uleb()
                elif kind == 2:
                    flags = r.uleb()
                    r.uleb()
                    if flags & 1:
                        r.uleb()
                elif kind == 3:
                    r.byte()
                    r.byte()
                elif kind == 4:
                    r.byte()
                    r.uleb()
        elif sec_id == 7:
            r = Reader(data, start)
            for _ in range(r.uleb()):
                name = r.name()
                kind = r.byte()
                index = r.uleb()
                if kind == 0:
                    exports[name] = index

    helpers = {}
    for name in DIV_OPS.values():
        if name not in exports:
            raise SystemExit(f'wasm_safe_div: {name} is not exported')
        helpers[name] = exports[name]
    helper_indices = set(helpers.values())

    out = bytearray(data[:8])
    total = 0
    for sec_id, start, end in sections:
        if sec_id != 10:
            out.append(sec_id)
            out += uleb_bytes(end - start)
            out += data[start:end]
            continue
        r = Reader(data, start)
        count = r.uleb()
        code = bytearray(uleb_bytes(count))
        for i in range(count):
            size = r.uleb()
            body = data[r.pos:r.pos + size]
            r.pos += size
            if num_imported_funcs + i in helper_indices:
                new_body = body
            else:
                new_body, n = rewrite_body(body, helpers)
                total += n
            code += uleb_bytes(len(new_body)) + new_body
        out.append(10)
        out += uleb_bytes(len(code))
        out += code

    with open(path, 'wb') as f:
        f.write(out)
    print(f'wasm_safe_div: rewrote {total} division instructions', file=sys.stderr)


if __name__ == '__main__':
    main()
