#!/usr/bin/env python3
"""Converts the assembled GBA data objects (ARM ELF) into WebAssembly assembly.

The game keeps a lot of its data (event scripts, battle scripts, map headers,
songs, voicegroups...) in GNU assembler files.  Rather than rewriting those,
the browser port assembles them with arm-none-eabi-as exactly like the GBA
build does, and this script turns the resulting relocatable objects into an
LLVM WebAssembly assembly file:

  * every allocated ELF section becomes one wasm data segment, so labels keep
    their relative layout (scripts fall through from one label to the next);
  * every label becomes a wasm data symbol at the same offset;
  * every R_ARM_ABS32 relocation becomes a `.int32 symbol+addend`, which the
    wasm linker resolves to a memory address, or to a table index when the
    target is a C function (callnative, specials, anim tasks...).

The raw bytes are written to a side file and pulled in with `.incbin`.

The signatures of the C functions referenced from data are read from the
compiled wasm objects, since the wasm assembler needs them. WebAssembly traps
when a function is called through a pointer of a different type, which ARM
doesn't care about (e.g. `special` calls every special as `u16 (*)(void)`,
including the many `void` ones). The call signature each table is used with
can be given with --call-sig; references to functions with another signature
then point to generated adapter functions.
"""

import argparse
import os
import struct
import sys

# --------------------------------------------------------------------------
# Minimal ELF32 little-endian reader

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
STT_SECTION = 3
STT_FILE = 4
STB_LOCAL = 0
R_ARM_ABS32 = 2
R_ARM_ABS16 = 5


class ElfSection:
    def __init__(self, idx, name, type_, flags, offset, size, link, info, align, entsize, data):
        self.idx = idx
        self.name = name
        self.type = type_
        self.flags = flags
        self.size = size
        self.link = link
        self.info = info
        self.align = align
        self.entsize = entsize
        self.data = data


class ElfSymbol:
    def __init__(self, idx, name, value, size, info, shndx):
        self.idx = idx
        self.name = name
        self.value = value
        self.size = size
        self.bind = info >> 4
        self.type = info & 0xF
        self.shndx = shndx


class ElfObject:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[:4] != b'\x7fELF' or raw[4] != 1 or raw[5] != 1:
            raise ValueError(f'{path}: not a 32-bit little-endian ELF file')
        (e_shoff,) = struct.unpack_from('<I', raw, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', raw, 0x2E)
        hdrs = []
        for i in range(e_shnum):
            hdrs.append(struct.unpack_from('<IIIIIIIIII', raw, e_shoff + i * e_shentsize))
        shstr = hdrs[e_shstrndx]
        shstr_data = raw[shstr[4]:shstr[4] + shstr[5]]

        def cstr(buf, off):
            return buf[off:buf.index(b'\0', off)].decode('latin-1')

        self.sections = []
        for i, h in enumerate(hdrs):
            name, type_, flags, addr, offset, size, link, info, align, entsize = h
            data = b'' if type_ == SHT_NOBITS else raw[offset:offset + size]
            self.sections.append(ElfSection(i, cstr(shstr_data, name), type_, flags, offset,
                                            size, link, info, align, entsize, data))

        self.symbols = []
        self.relocs = {}  # section index -> list of (offset, type, symidx)
        for sec in self.sections:
            if sec.type == SHT_SYMTAB:
                strtab = self.sections[sec.link].data
                for i in range(sec.size // 16):
                    name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', sec.data, i * 16)
                    self.symbols.append(ElfSymbol(i, cstr(strtab, name), value, size, info, shndx))
            elif sec.type == SHT_REL:
                rels = []
                for i in range(sec.size // 8):
                    r_offset, r_info = struct.unpack_from('<II', sec.data, i * 8)
                    rels.append((r_offset, r_info & 0xFF, r_info >> 8))
                self.relocs[sec.info] = rels


# --------------------------------------------------------------------------
# Minimal wasm object reader (just enough to find defined symbols and the
# signatures of defined functions)

VALTYPES = {0x7F: 'i32', 0x7E: 'i64', 0x7D: 'f32', 0x7C: 'f64', 0x7B: 'v128',
            0x70: 'funcref', 0x6F: 'externref'}


class WasmReader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def u8(self):
        v = self.data[self.pos]
        self.pos += 1
        return v

    def leb(self):
        result = shift = 0
        while True:
            b = self.u8()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return result

    def sleb(self):
        result = shift = 0
        while True:
            b = self.u8()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    result -= 1 << shift
                return result

    def name(self):
        n = self.leb()
        s = self.data[self.pos:self.pos + n].decode('utf-8')
        self.pos += n
        return s

    def limits(self):
        flags = self.u8()
        self.leb()
        if flags & 1:
            self.leb()


def read_wasm_symbols(path, functions, data_symbols):
    """Adds the defined global functions (name -> signature string) and data
    symbols of a wasm object file to the given containers."""
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:4] != b'\0asm':
        raise ValueError(f'{path}: not a wasm object')
    r = WasmReader(raw, 8)
    types = []
    func_types = []  # function index -> type index (imports first)
    linking = None
    while r.pos < len(raw):
        sec_id = r.u8()
        size = r.leb()
        end = r.pos + size
        if sec_id == 1:  # type
            for _ in range(r.leb()):
                form = r.u8()
                assert form == 0x60, 'unexpected type form'
                params = [VALTYPES[r.u8()] for _ in range(r.leb())]
                results = [VALTYPES[r.u8()] for _ in range(r.leb())]
                types.append((params, results))
        elif sec_id == 2:  # import
            for _ in range(r.leb()):
                r.name()
                r.name()
                kind = r.u8()
                if kind == 0:
                    func_types.append(r.leb())
                elif kind == 1:
                    r.u8()
                    r.limits()
                elif kind == 2:
                    r.limits()
                elif kind == 3:
                    r.u8()
                    r.u8()
                elif kind == 4:
                    r.u8()
                    r.leb()
        elif sec_id == 3:  # function
            for _ in range(r.leb()):
                func_types.append(r.leb())
        elif sec_id == 0:  # custom
            name = r.name()
            if name == 'linking':
                linking = (r.pos, end)
        r.pos = end

    if linking is None:
        return
    r.pos, end = linking
    r.leb()  # version
    while r.pos < end:
        sub_type = r.u8()
        sub_len = r.leb()
        sub_end = r.pos + sub_len
        if sub_type == 8:  # WASM_SYMBOL_TABLE
            for _ in range(r.leb()):
                kind = r.u8()
                flags = r.leb()
                undefined = flags & 0x10
                local = flags & 0x02
                if kind in (0, 2, 4, 5):
                    index = r.leb()
                    name = None
                    if not undefined or flags & 0x40:
                        name = r.name()
                    if kind == 0 and not undefined and not local:
                        params, results = types[func_types[index]]
                        functions[name] = '(%s) -> (%s)' % (', '.join(params), ', '.join(results))
                elif kind == 1:
                    name = r.name()
                    if not undefined:
                        r.leb()
                        r.leb()
                        r.leb()
                        if not local:
                            data_symbols.add(name)
                elif kind == 3:
                    r.leb()
        r.pos = sub_end


# --------------------------------------------------------------------------
# Conversion

def parse_sig(sig):
    params, results = sig.split('->')
    def types(part):
        part = part.strip().strip('()').strip()
        return [t.strip() for t in part.split(',')] if part else []
    return types(params), types(results)


def normalize_sig(sig):
    params, results = parse_sig(sig)
    return '(%s) -> (%s)' % (', '.join(params), ', '.join(results))


def thunk_asm(thunk, target, call_sig, target_sig):
    """Assembly for a function with call_sig that calls target (target_sig),
    passing zero for missing arguments and adapting the return value."""
    call_params, call_results = parse_sig(call_sig)
    target_params, target_results = parse_sig(target_sig)
    for t in call_params + call_results + target_params + target_results:
        if t != 'i32':
            raise SystemExit(f'elf2wasm: cannot adapt {target} {target_sig} to {call_sig}')
    lines = [
        f'\t.section\t.text.{thunk},"",@',
        f'\t.type\t{thunk},@function',
        f'{thunk}:',
        f'\t.functype\t{thunk} {call_sig}',
    ]
    for i in range(len(target_params)):
        lines.append(f'\tlocal.get\t{i}' if i < len(call_params) else '\ti32.const\t0')
    lines.append(f'\tcall\t{target}')
    if target_results and not call_results:
        lines.append('\tdrop')
    elif call_results and not target_results:
        lines.append('\ti32.const\t0')
    lines.append('\tend_function')
    lines.append('')
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--wasm-objs', nargs='*', default=[])
    ap.add_argument('--elf-objs', nargs='*', default=[])
    ap.add_argument('--call-sig', action='append', default=[], metavar='LOCATION=SIGNATURE',
                    help='functions referenced from LOCATION (a data label, which covers '
                         'everything up to the next label, or an object file name) are called '
                         'with SIGNATURE, e.g. "gSpecials=() -> (i32)"')
    args = ap.parse_args()

    call_sigs = {}
    for spec in args.call_sig:
        location, sig = spec.split('=', 1)
        call_sigs[location.strip()] = normalize_sig(sig)

    functions = {}
    wasm_data = set()
    for path in args.wasm_objs:
        read_wasm_symbols(path, functions, wasm_data)

    objs = [ElfObject(p) for p in args.elf_objs]

    # Global symbols defined by the ELF objects themselves.
    elf_globals = {}  # name -> (obj index, symbol)
    for oi, obj in enumerate(objs):
        for sym in obj.symbols:
            if sym.bind != STB_LOCAL and sym.shndx != SHN_UNDEF and sym.name:
                if sym.name in elf_globals:
                    raise SystemExit(f'error: {sym.name} defined twice ({obj.path})')
                elf_globals[sym.name] = (oi, sym)

    bin_path = os.path.splitext(args.output)[0] + '.bin'
    blob = bytearray()
    out = []
    used_functions = {}
    undefined_data = set()
    errors = []
    emit = out.append

    def local_name(oi, name):
        return f'__gba{oi}_{name}'

    def section_label(oi, sec):
        return f'__gbasec{oi}_{sec.idx}'

    def section_ident(oi, obj, sec):
        base = os.path.splitext(os.path.basename(obj.path))[0]
        name = sec.name.strip('.').replace('.', '_')
        return f'.data.gba.{base}.{name}.{oi}'

    thunks = {}  # (target, call_sig) -> thunk name

    def resolve(oi, obj, sym, addend, where, call_sig=None):
        """Returns the assembly expression for symbol+addend, or an int for
        absolute values."""
        if sym.type == STT_SECTION:
            return f'{section_label(oi, obj.sections[sym.shndx])}+{addend}'
        if sym.shndx == SHN_ABS:
            return sym.value + addend
        if sym.shndx != SHN_UNDEF:
            name = sym.name if sym.bind != STB_LOCAL else local_name(oi, sym.name)
            return f'{name}+{addend}' if addend else name
        name = sym.name
        if name in elf_globals:
            doi, dsym = elf_globals[name]
            if dsym.shndx == SHN_ABS:
                return dsym.value + addend
            return f'{name}+{addend}' if addend else name
        if name in functions:
            if addend:
                errors.append(f'{where}: function {name} referenced with addend {addend}')
            used_functions[name] = functions[name]
            if call_sig is not None and call_sig != functions[name]:
                key = (name, call_sig)
                if key not in thunks:
                    thunks[key] = f'__port_thunk{len(thunks)}_{name}'
                return thunks[key]
            return name
        if name in wasm_data:
            undefined_data.add(name)
            return f'{name}+{addend}' if addend else name
        errors.append(f'{where}: undefined symbol {name}')
        return 0

    for oi, obj in enumerate(objs):
        for sec in obj.sections:
            if not (sec.flags & SHF_ALLOC) or sec.size == 0:
                continue
            if sec.type not in (SHT_PROGBITS, SHT_NOBITS):
                continue
            data = sec.data if sec.type == SHT_PROGBITS else bytes(sec.size)
            relocs = sorted(obj.relocs.get(sec.idx, []))
            syms = sorted((s for s in obj.symbols
                           if s.shndx == sec.idx and s.type != STT_SECTION and s.type != STT_FILE
                           and s.name and not s.name.startswith('$')),
                          key=lambda s: s.value)
            align = max(sec.align, 1)
            p2 = align.bit_length() - 1

            emit(f'\t.section\t{section_ident(oi, obj, sec)},"",@')
            emit(f'\t.p2align\t{p2}')
            sec_label = section_label(oi, sec)
            emit(f'\t.type\t{sec_label},@object')
            emit(f'{sec_label}:')
            emit(f'\t.size\t{sec_label}, {sec.size}')

            # Event points: symbol definitions and relocations, in offset order.
            offsets = sorted({s.value for s in syms} | {sec.size})
            sym_at = {}
            for s in syms:
                sym_at.setdefault(s.value, []).append(s)
            next_off = {}
            for a, b in zip(offsets, offsets[1:]):
                next_off[a] = b

            obj_base = os.path.splitext(os.path.basename(obj.path))[0]
            # Call signature regions: [start, end) -> signature
            sig_regions = []
            for s_ in syms:
                if s_.name in call_sigs:
                    sig_regions.append((s_.value, next_off.get(s_.value, sec.size), call_sigs[s_.name]))

            def call_sig_at(off):
                for start, end, sig in sig_regions:
                    if start <= off < end:
                        return sig
                return call_sigs.get(obj_base)

            pos = 0
            blob_base = len(blob)
            blob.extend(data)

            def flush(upto):
                nonlocal pos
                if upto > pos:
                    emit(f'\t.incbin\t"{bin_path}", {blob_base + pos}, {upto - pos}')
                    pos = upto

            events = sorted(set(sym_at) | {r[0] for r in relocs})
            rel_at = {}
            for r in relocs:
                rel_at[r[0]] = r
            for off in events:
                if off > sec.size:
                    break
                flush(off)
                for s in sym_at.get(off, []):
                    name = s.name if s.bind != STB_LOCAL else local_name(oi, s.name)
                    size = next_off.get(off, sec.size) - off
                    if s.bind != STB_LOCAL:
                        emit(f'\t.globl\t{name}')
                    emit(f'\t.type\t{name},@object')
                    emit(f'{name}:')
                    emit(f'\t.size\t{name}, {size}')
                if off in rel_at:
                    r_off, r_type, r_sym = rel_at[off]
                    sym = obj.symbols[r_sym]
                    where = f'{obj.path}:{sec.name}+0x{off:x}'
                    if r_type == R_ARM_ABS32:
                        (addend,) = struct.unpack_from('<i', data, off)
                        val = resolve(oi, obj, sym, addend, where, call_sig_at(off))
                        if isinstance(val, int):
                            emit(f'\t.int32\t{val & 0xFFFFFFFF}')
                        else:
                            emit(f'\t.int32\t{val}')
                        pos = off + 4
                    elif r_type == R_ARM_ABS16:
                        (addend,) = struct.unpack_from('<h', data, off)
                        val = resolve(oi, obj, sym, addend, where)
                        if not isinstance(val, int):
                            errors.append(f'{where}: 16-bit relocation against non-absolute {sym.name}')
                            val = 0
                        emit(f'\t.int16\t{val & 0xFFFF}')
                        pos = off + 2
                    else:
                        errors.append(f'{where}: unsupported relocation type {r_type}')
            flush(sec.size)
            emit('')

    if errors:
        for e in errors[:50]:
            print('elf2wasm: error:', e, file=sys.stderr)
        if len(errors) > 50:
            print(f'elf2wasm: ... and {len(errors) - 50} more errors', file=sys.stderr)
        sys.exit(1)

    header = ['# Generated by port/tools/elf2wasm.py. Do not edit.']
    for name, sig in sorted(used_functions.items()):
        header.append(f'\t.functype\t{name} {sig}')
    if used_functions:
        header.append('\t.no_dead_strip\t__indirect_function_table')
    for name in sorted(undefined_data):
        header.append(f'\t.size\t{name}, 0')
    header.append('')
    for (target, call_sig), thunk in sorted(thunks.items(), key=lambda kv: kv[1]):
        header.extend(thunk_asm(thunk, target, call_sig, functions[target]))
    if thunks:
        print(f'elf2wasm: generated {len(thunks)} call adapters', file=sys.stderr)

    with open(bin_path, 'wb') as f:
        f.write(blob)
    with open(args.output, 'w') as f:
        f.write('\n'.join(header + out))
        f.write('\n')


if __name__ == '__main__':
    main()
