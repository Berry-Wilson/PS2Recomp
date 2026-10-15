#!/usr/bin/env python3
"""Translate captured IOP modules to C++ (see ps2xIOP/src/emulator/iop_compiled.h).

usage: iop_recomp.py <dump dir> <irx dir> <output dir> MODULE[.IRX] ...

<dump dir> holds <name>_<base>.bin/.info written by setIopModuleDumpDir(); <irx dir> holds the
original IRX files (for the .iopmod text size). For each named module this writes
iop_<module>.cpp with one function that executes the module's text section instruction by
instruction exactly like IopCpuCore::executeInstruction, and iop_modules.cpp with
registerGeneratedIopModules().
"""
import os
import re
import struct
import sys

RAM_SIZE = 2 * 1024 * 1024


def iopmod_text_size(irx_path):
    d = open(irx_path, 'rb').read()
    shoff = struct.unpack_from('<I', d, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 46)
    secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx]
    for s in secs:
        name_off = strtab[4] + s[0]
        name = d[name_off:d.index(b'\0', name_off)]
        if name == b'.iopmod':
            return struct.unpack_from('<I', d, s[4] + 12)[0]
    raise SystemExit('no .iopmod in ' + irx_path)


def fnv_hash(data):
    h = 0xCBF29CE484222325
    for off in range(0, len(data) - 3, 4):
        h ^= struct.unpack_from('<I', data, off)[0]
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Emitter:
    def __init__(self, words, base):
        self.words = words
        self.base = base
        self.end = base + 4 * len(words)

    def word(self, a):
        return self.words[(a - self.base) // 4]

    def is_import_stub(self, a):
        if a + 4 >= self.end:
            return False
        return self.word(a) == 0x03E00008 and (self.word(a + 4) & 0xFFFF0000) == 0x24000000

    def emit(self, a):
        """Returns C++ lines for the instruction at address a."""
        w = self.word(a)
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        fn = w & 63
        imm = w & 0xFFFF
        simm = sx16(imm)
        out = []
        E = 'EXIT(0x%Xu)' % a
        if self.is_import_stub(a):
            return ['    %s; // import stub' % E], False

        body = []          # statements after the exit checks
        written = 0        # register written by write() (interpreter's writtenReg)
        load_value = None  # expression of a scheduled load (load(rt, v)) or None
        branch = None      # (condition expr, target expr) for a new branch
        pre = []           # exit-before checks

        def write(reg, expr):
            nonlocal written
            if reg != 0:
                body.append('gpr[%d] = %s;' % (reg, expr))
                written = reg

        R = lambda r: 'gpr[%d]' % r
        S = lambda r: 'static_cast<int32_t>(gpr[%d])' % r
        simm_u = '0x%Xu' % (simm & 0xFFFFFFFF)
        target_b = '0x%Xu' % ((a + 4 + (simm << 2)) & 0xFFFFFFFF)

        if op == 0x00:
            if fn == 0x00: write(rd, '%s << %d' % (R(rt), sa))
            elif fn == 0x02: write(rd, '%s >> %d' % (R(rt), sa))
            elif fn == 0x03: write(rd, 'static_cast<uint32_t>(%s >> %d)' % (S(rt), sa))
            elif fn == 0x04: write(rd, '%s << (%s & 31u)' % (R(rt), R(rs)))
            elif fn == 0x06: write(rd, '%s >> (%s & 31u)' % (R(rt), R(rs)))
            elif fn == 0x07: write(rd, 'static_cast<uint32_t>(%s >> (%s & 31u))' % (S(rt), R(rs)))
            elif fn == 0x08: branch = ('true', R(rs))
            elif fn == 0x09:
                write(rd if rd else 31, '0x%Xu' % ((a + 8) & 0xFFFFFFFF))
                branch = ('true', R(rs))  # read after the link write, as the interpreter does
            elif fn == 0x10: write(rd, 'cpu.hi')
            elif fn == 0x11: body.append('cpu.hi = %s;' % R(rs))
            elif fn == 0x12: write(rd, 'cpu.lo')
            elif fn == 0x13: body.append('cpu.lo = %s;' % R(rs))
            elif fn == 0x18:
                body.append('{ const int64_t m = static_cast<int64_t>(%s) * static_cast<int64_t>(%s); cpu.lo = static_cast<uint32_t>(m); '
                            'cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(m) >> 32u); }' % (S(rs), S(rt)))
            elif fn == 0x19:
                body.append('{ const uint64_t m = static_cast<uint64_t>(%s) * %s; cpu.lo = static_cast<uint32_t>(m); '
                            'cpu.hi = static_cast<uint32_t>(m >> 32u); }' % (R(rs), R(rt)))
            elif fn == 0x1A:
                body.append('{ const int32_t l = %s; const int32_t r = %s; if (r == 0) { cpu.lo = l >= 0 ? 0xFFFFFFFFu : 1u; cpu.hi = static_cast<uint32_t>(l); } '
                            'else if (l == INT32_MIN && r == -1) { cpu.lo = static_cast<uint32_t>(l); cpu.hi = 0u; } '
                            'else { cpu.lo = static_cast<uint32_t>(l / r); cpu.hi = static_cast<uint32_t>(l %% r); } }' % (S(rs), S(rt)))
            elif fn == 0x1B:
                body.append('if (%s == 0u) { cpu.lo = 0xFFFFFFFFu; cpu.hi = %s; } else { cpu.lo = %s / %s; cpu.hi = %s %% %s; }'
                            % (R(rt), R(rs), R(rs), R(rt), R(rs), R(rt)))
            elif fn == 0x20:
                pre.append('{ const int64_t s = static_cast<int64_t>(%s) + %s; if (s > INT32_MAX || s < INT32_MIN) %s; }' % (S(rs), S(rt), E))
                write(rd, '%s + %s' % (R(rs), R(rt)))
            elif fn == 0x21: write(rd, '%s + %s' % (R(rs), R(rt)))
            elif fn == 0x22:
                pre.append('{ const int64_t s = static_cast<int64_t>(%s) - %s; if (s > INT32_MAX || s < INT32_MIN) %s; }' % (S(rs), S(rt), E))
                write(rd, '%s - %s' % (R(rs), R(rt)))
            elif fn == 0x23: write(rd, '%s - %s' % (R(rs), R(rt)))
            elif fn == 0x24: write(rd, '%s & %s' % (R(rs), R(rt)))
            elif fn == 0x25: write(rd, '%s | %s' % (R(rs), R(rt)))
            elif fn == 0x26: write(rd, '%s ^ %s' % (R(rs), R(rt)))
            elif fn == 0x27: write(rd, '~(%s | %s)' % (R(rs), R(rt)))
            elif fn == 0x2A: write(rd, '(%s < %s) ? 1u : 0u' % (S(rs), S(rt)))
            elif fn == 0x2B: write(rd, '(%s < %s) ? 1u : 0u' % (R(rs), R(rt)))
            else:
                return ['    %s; // syscall/break/reserved' % E], False
        elif op == 0x01:
            if rt == 0x00: branch = ('%s < 0' % S(rs), target_b)
            elif rt == 0x01: branch = ('%s >= 0' % S(rs), target_b)
            elif rt == 0x10:
                write(31, '0x%Xu' % ((a + 8) & 0xFFFFFFFF))
                branch = ('%s < 0' % S(rs), target_b)
            elif rt == 0x11:
                write(31, '0x%Xu' % ((a + 8) & 0xFFFFFFFF))
                branch = ('%s >= 0' % S(rs), target_b)
            else:
                return ['    %s; // reserved' % E], False
        elif op in (0x02, 0x03):
            target = (((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) & 0xFFFFFFFF
            if op == 0x03:
                write(31, '0x%Xu' % ((a + 8) & 0xFFFFFFFF))
            branch = ('true', '0x%Xu' % target)
        elif op == 0x04: branch = ('%s == %s' % (R(rs), R(rt)), target_b)
        elif op == 0x05: branch = ('%s != %s' % (R(rs), R(rt)), target_b)
        elif op == 0x06: branch = ('%s <= 0' % S(rs), target_b)
        elif op == 0x07: branch = ('%s > 0' % S(rs), target_b)
        elif op == 0x08:
            pre.append('{ const int64_t s = static_cast<int64_t>(%s) + %d; if (s > INT32_MAX || s < INT32_MIN) %s; }' % (S(rs), simm, E))
            write(rt, '%s + %s' % (R(rs), simm_u))
        elif op == 0x09: write(rt, '%s + %s' % (R(rs), simm_u))
        elif op == 0x0A: write(rt, '(%s < %d) ? 1u : 0u' % (S(rs), simm))
        elif op == 0x0B: write(rt, '(%s < %s) ? 1u : 0u' % (R(rs), simm_u))
        elif op == 0x0C: write(rt, '%s & 0x%Xu' % (R(rs), imm))
        elif op == 0x0D: write(rt, '%s | 0x%Xu' % (R(rs), imm))
        elif op == 0x0E: write(rt, '%s ^ 0x%Xu' % (R(rs), imm))
        elif op == 0x0F: write(rt, '0x%Xu' % ((imm << 16) & 0xFFFFFFFF))
        elif op in (0x20, 0x24, 0x21, 0x25, 0x23, 0x22, 0x26):
            pre.append('const uint32_t addr = %s + %s; const uint32_t phys = addr & 0x1FFFFFFFu;' % (R(rs), simm_u))
            if op in (0x20, 0x24):
                pre.append('if (phys >= RAM_SIZE) %s;' % E)
                v = 'ram[phys]'
                load_value = ('static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(%s)))' % v) if op == 0x20 else ('static_cast<uint32_t>(%s)' % v)
            elif op in (0x21, 0x25):
                pre.append('if ((addr & 1u) != 0u || phys + 1u >= RAM_SIZE) %s;' % E)
                body.append('uint16_t h; std::memcpy(&h, ram + phys, 2);')
                load_value = 'static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)))' if op == 0x21 else 'static_cast<uint32_t>(h)'
            elif op == 0x23:
                pre.append('if ((addr & 3u) != 0u || phys + 3u >= RAM_SIZE) %s;' % E)
                body.append('uint32_t m; std::memcpy(&m, ram + phys, 4);')
                load_value = 'm'
            else:  # lwl / lwr
                pre.append('const uint32_t aphys = (addr & ~3u) & 0x1FFFFFFFu; if (aphys + 3u >= RAM_SIZE) %s;' % E)
                body.append('uint32_t m; std::memcpy(&m, ram + aphys, 4); const uint32_t old = %s;' % R(rt))
                if op == 0x22:
                    body.append('static constexpr uint32_t masks[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u}; '
                                'static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};')
                    load_value = '(old & masks[addr & 3u]) | (m << shifts[addr & 3u])'
                else:
                    body.append('static constexpr uint32_t masks[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u}; '
                                'static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};')
                    load_value = '(old & masks[addr & 3u]) | (m >> shifts[addr & 3u])'
            if rt == 0:
                load_value = None  # scheduleLoad ignores $zero; the memory read has no side effect in RAM
        elif op in (0x28, 0x29, 0x2B, 0x2A, 0x2E):
            pre.append('const uint32_t addr = %s + %s; const uint32_t phys = addr & 0x1FFFFFFFu;' % (R(rs), simm_u))
            if op == 0x28:
                pre.append('if (phys >= RAM_SIZE) %s;' % E)
                body.append('run.memory.write8(addr, static_cast<uint8_t>(%s));' % R(rt))
            elif op == 0x29:
                pre.append('if ((addr & 1u) != 0u || phys + 1u >= RAM_SIZE) %s;' % E)
                body.append('run.memory.write16(addr, static_cast<uint16_t>(%s));' % R(rt))
            elif op == 0x2B:
                pre.append('if ((addr & 3u) != 0u || phys + 3u >= RAM_SIZE) %s;' % E)
                body.append('run.memory.write32(addr, %s);' % R(rt))
            else:
                pre.append('const uint32_t aligned = addr & ~3u; if ((aligned & 0x1FFFFFFFu) + 3u >= RAM_SIZE) %s;' % E)
                body.append('const uint32_t old = run.memory.read32(aligned); const uint32_t value = %s; uint32_t result = old;' % R(rt))
                if op == 0x2A:  # swl
                    body.append('switch (addr & 3u) { case 0u: result = (old & 0xFFFFFF00u) | (value >> 24u); break; '
                                'case 1u: result = (old & 0xFFFF0000u) | (value >> 16u); break; '
                                'case 2u: result = (old & 0xFF000000u) | (value >> 8u); break; case 3u: result = value; break; }')
                else:  # swr
                    body.append('switch (addr & 3u) { case 0u: result = value; break; case 1u: result = (old & 0x000000FFu) | (value << 8u); break; '
                                'case 2u: result = (old & 0x0000FFFFu) | (value << 16u); break; case 3u: result = (old & 0x00FFFFFFu) | (value << 24u); break; }')
                body.append('run.memory.write32(aligned, result);')
        else:
            return ['    %s; // cop0/cop2/reserved' % E], False

        out.append('    {')
        for p in pre:
            out.append('        ' + p)
        out.append('        const bool ds = cpu.branchPending;')
        for b in body:
            out.append('        ' + b)
        if branch is not None:
            # Evaluated after any link-register write (JALR/BxxZAL read it afterwards in the
            # interpreter too) and before the pending-load commit.
            out.append('        const bool taken = %s; const uint32_t newTarget = %s;' % branch)
        if load_value is not None:
            out.append('        const uint32_t loaded = %s;' % load_value)
        out.append('        if (cpu.pendingLoad) { if (cpu.pendingLoadReg != 0u && cpu.pendingLoadReg != %du) gpr[cpu.pendingLoadReg] = cpu.pendingLoadValue; cpu.pendingLoad = false; }' % written)
        if load_value is not None:
            out.append('        cpu.pendingLoad = true; cpu.pendingLoadReg = %du; cpu.pendingLoadValue = loaded;' % rt)
        out.append('        gpr[0] = 0u; ++run.totalInstructions; ++run.totalCycles;')
        out.append('        if (ds) { cpu.pc = cpu.branchTarget; cpu.branchPending = false; if (STOP) return; goto dispatch; }')
        out.append('        cpu.pc = 0x%Xu;' % ((a + 4) & 0xFFFFFFFF))
        if branch is not None:
            out.append('        cpu.branchPending = taken; if (taken) cpu.branchTarget = newTarget;')
        out.append('        if (STOP) return;')
        out.append('    }')
        return out, True


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1
    dump_dir, irx_dir, out_dir = sys.argv[1:4]
    wanted = [m.upper().replace('.IRX', '') for m in sys.argv[4:]]
    os.makedirs(out_dir, exist_ok=True)
    registrations = []
    for info_name in sorted(os.listdir(dump_dir)):
        if not info_name.endswith('.info'):
            continue
        name, base_hex, size_hex = open(os.path.join(dump_dir, info_name)).read().split()
        module = re.sub(r'\.IRX.*$', '', name.upper())
        if module not in wanted:
            continue
        base = int(base_hex, 16)
        image = open(os.path.join(dump_dir, info_name[:-5] + '.bin'), 'rb').read()
        text_size = iopmod_text_size(os.path.join(irx_dir, module + '.IRX'))
        text = image[:text_size]
        words = list(struct.unpack('<%dI' % (len(text) // 4), text[:len(text) // 4 * 4]))
        em = Emitter(words, base)
        fn_name = 'iopmod_' + module.lower()
        chunk = 128
        nchunks = (len(words) + chunk - 1) // chunk
        lines = ['// Generated by iop_recomp.py from %s (base 0x%X, text 0x%X bytes). Do not edit.' % (name, base, text_size),
                 '#include "emulator/iop_compiled.h"', '', '#include <cstdint>', '#include <cstring>', '',
                 'using namespace ps2x::iop::detail;', '',
                 '// Chunks of %d instructions keep each function small for the C++ optimizer. A chunk returns' % chunk,
                 '// true to continue at cpu.pc (another chunk) and false to hand back to the interpreter.',
                 '#define EXIT(pc_) do { cpu.pc = (pc_); return false; } while (0)',
                 '#define STOP (run.totalCycles >= run.cycleBound || run.totalInstructions >= run.instructionLimit)',
                 'namespace', '{', '    constexpr uint32_t RAM_SIZE = IopMemory::RamSize;']
        for c in range(nchunks):
            first = c * chunk
            last = min(len(words), first + chunk)
            lines.append('')
            lines.append('    bool %s_c%d(IopCompiledRun &run)' % (fn_name, c))
            lines.append('    {')
            lines.append('        IopCpuState &cpu = run.cpu; uint32_t *const gpr = cpu.gpr.data(); const uint8_t *const ram = run.ram; (void)ram;')
            lines.append('    dispatch:')
            lines.append('        switch (cpu.pc)')
            lines.append('        {')
            for i in range(first, last):
                a2 = base + 4 * i
                lines.append('        case 0x%Xu: goto I_%X;' % (a2, a2))
            lines.append('        default: return true;')
            lines.append('        }')
            for i in range(first, last):
                a2 = base + 4 * i
                code, falls = em.emit(a2)
                lines.append('    I_%X:' % a2)
                for l in code:
                    lines.append('    ' + l.replace('if (STOP) return;', 'if (STOP) return false;'))
                if falls:
                    lines.append('        goto I_%X;' % (a2 + 4) if i + 1 < last else '        return true;')
            lines.append('    }')
        lines.append('}')
        lines.append('')
        lines.append('void %s(IopCompiledRun &run)' % fn_name)
        lines.append('{')
        lines.append('    static bool (*const chunks[])(IopCompiledRun &) = {')
        for c in range(nchunks):
            lines.append('        &%s_c%d,' % (fn_name, c))
        lines.append('    };')
        lines.append('    run.cpu.exception = false;')
        lines.append('    for (;;)')
        lines.append('    {')
        lines.append('        const uint32_t pc = run.cpu.pc;')
        lines.append('        if (pc < 0x%Xu || pc >= 0x%Xu || (pc & 3u) != 0u)' % (base, em.end))
        lines.append('            return;')
        lines.append('        if (!chunks[(pc - 0x%Xu) / %du](run))' % (base, 4 * chunk))
        lines.append('            return;')
        lines.append('    }')
        lines.append('}')
        lines.append('#undef EXIT')
        lines.append('#undef STOP')
        with open(os.path.join(out_dir, 'iop_%s.cpp' % module.lower()), 'w', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        h = fnv_hash(text)
        registrations.append((module, base, text_size, h, fn_name))
        print('%s: base 0x%X, %d instructions, hash %016x' % (module, base, len(words), h))

    with open(os.path.join(out_dir, 'iop_modules.cpp'), 'w', newline='\n') as f:
        f.write('// Generated by iop_recomp.py. Do not edit.\n#include "emulator/iop_compiled.h"\n\n')
        for m, base, size, h, fn in registrations:
            f.write('void %s(ps2x::iop::detail::IopCompiledRun &run);\n' % fn)
        f.write('\nvoid registerGeneratedIopModules()\n{\n')
        for m, base, size, h, fn in registrations:
            f.write('    registerIopCompiledModule("%s", 0x%Xu, 0x%Xu, 0x%016XULL, &%s);\n' % (m, base, size, h, fn))
        f.write('}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
