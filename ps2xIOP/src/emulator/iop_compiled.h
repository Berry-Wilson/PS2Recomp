#pragma once

// Ahead-of-time compiled IOP modules.
//
// A module's text section, as loaded and relocated in IOP RAM, is captured at load time
// (setIopModuleDumpDir), translated offline to C++ by ps2xIOP/tools/iop_recomp.py and
// registered with registerIopCompiledModule. When a module is loaded at the registered base and
// its text hashes to the registered value, the emulator runs the compiled code for PCs inside
// it. Compiled code mirrors IopCpuCore::executeInstruction instruction by instruction and hands
// back to the interpreter (with the CPU state exactly as the interpreter would have it) before
// anything with side effects outside main RAM and the CPU registers: COP0/syscall/break,
// loads/stores outside main RAM, misaligned accesses, overflow traps, import stubs, and PCs
// outside the module. It stops after an instruction once the cycle bound (next DMA completion
// or guest callback) or the instruction limit (scheduler slice) is reached.

#include "core/iop_cpu.h"
#include "core/iop_memory.h"

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCompiledRun
    {
        IopCpuState &cpu;
        IopMemory &memory;
        const uint8_t *ram; // IOP main RAM (IopMemory::RamSize bytes)
        uint64_t &totalCycles;
        uint64_t &totalInstructions;
        uint64_t cycleBound;       // return once totalCycles >= cycleBound
        uint64_t instructionLimit; // return once totalInstructions >= instructionLimit
    };

    using IopCompiledFn = void (*)(IopCompiledRun &run);
}

void registerIopCompiledModule(const char *name, uint32_t base, uint32_t textSize, uint64_t textHash,
                               ps2x::iop::detail::IopCompiledFn fn);
// Writes <name>_<base>.bin (module image as loaded) and .info for every loaded module.
void setIopModuleDumpDir(const char *directory);
void setIopCompiledEnabled(bool enabled);
// Debug: every `period`-th compiled run is replayed with the interpreter from a snapshot and
// CPU registers / main RAM are compared (0 disables).
void setIopCompiledVerify(uint32_t period);
uint64_t hashIopCode(const uint8_t *code, uint32_t size);
