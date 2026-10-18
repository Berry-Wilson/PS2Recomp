#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

#include "iop_compiled.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace
{
    struct RegisteredIopModule
    {
        std::string name;
        uint32_t base = 0;
        uint32_t textSize = 0;
        uint64_t hash = 0;
        ps2x::iop::detail::IopCompiledFn fn = nullptr;
    };

    std::vector<RegisteredIopModule> &registeredIopModules()
    {
        static std::vector<RegisteredIopModule> modules;
        return modules;
    }
    std::string g_iopModuleDumpDir;
    bool g_iopCompiledEnabled = true;
    uint32_t g_iopCompiledVerifyPeriod = 0;
}

void registerIopCompiledModule(const char *name, uint32_t base, uint32_t textSize, uint64_t textHash,
                               ps2x::iop::detail::IopCompiledFn fn)
{
    registeredIopModules().push_back({name ? name : "", base, textSize, textHash, fn});
}

void setIopModuleDumpDir(const char *directory)
{
    g_iopModuleDumpDir = directory ? directory : "";
}

void setIopCompiledEnabled(bool enabled)
{
    g_iopCompiledEnabled = enabled;
}

void setIopCompiledVerify(uint32_t period)
{
    g_iopCompiledVerifyPeriod = period;
}

uint64_t hashIopCode(const uint8_t *code, uint32_t size)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t offset = 0; offset + 4u <= size; offset += 4u)
    {
        uint32_t word = 0;
        std::memcpy(&word, code + offset, sizeof(word));
        hash ^= word;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory, cdvd),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            nextDmaDue = ~0ull;
            cachedThread = nullptr;
            cachedIdle = false;
            activeCompiled.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (!memory.hasDmaStart())
                return;
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                refreshNextDmaDue();
            }
        }

        // Mixes SPU2 output up to the current IOP time and raises its interrupt (IRQ 9) when a
        // voice, DMA or input-buffer access hit a core's IRQA.
        void advanceSpu2()
        {
            IopSpu2 &spu2 = memory.spu2();
            spu2.advanceTo(totalCycles);
            if (spu2.takeIrq())
            {
                pendingDmaInterrupts[IopSpu2::Irq] = totalCycles;
                refreshNextDmaDue();
            }
        }

        void refreshNextDmaDue()
        {
            nextDmaDue = ~0ull;
            for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                nextDmaDue = std::min(nextDmaDue, completionCycle);
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                if (!kernel.dispatchEventImport(call.ordinal, cpu))
                    return ImportDisposition::Missing;
                completeUnthreadedEventWait(call.ordinal, cpu);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // Import stubs are "jr ra" with the ordinal in the delay slot; skip the decoder otherwise.
            if (const auto import = memory.read32(cpu.pc) == 0x03E00008u ? imports.decode(cpu.pc) : std::nullopt)
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            if (memory.hasDmaStart())
                schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        // endAfterService: return as soon as a thread became ready (kernel call, DMA interrupt or
        // guest callback), so the thread scheduler can switch to it (scheduler slices).
        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget, bool endAfterService = false)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                const uint64_t readyBefore = kernel.readyEvents();
                if (activeCompiled.empty() || !runCompiled(cpu, start + instructionBudget))
                {
                    if (!step(cpu))
                        break;
                }
                bool serviced = kernel.readyEvents() != readyBefore;
                if (!servicingDmaInterrupts && totalCycles >= nextDmaDue)
                {
                    servicePendingDmaInterrupts();
                    serviced = true;
                }
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty() &&
                    pendingGuestCallbacks.begin()->first <= totalCycles)
                {
                    servicePendingGuestCallbacks();
                    serviced = true;
                }
                if (serviced && endAfterService)
                    break;
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // WaitEventFlag reached outside a kernel thread (e.g. an RPC handler run through
        // callFunction) cannot yield, so the kernel answers KE_EVF_COND at once. Drivers such as
        // LIBSD's sceSdVoiceTransStatus ignore that and treat the transfer as finished. On hardware
        // the caller would block until a DMA completion interrupt set the flag, so advance time to
        // each pending DMA completion, deliver it, and retry until the wait is satisfied.
        void completeUnthreadedEventWait(uint16_t ordinal, CpuState &cpu)
        {
            constexpr uint16_t kWaitEventFlag = 10u;
            constexpr int32_t kKeEvfCond = -418;
            while (ordinal == kWaitEventFlag && !cpu.yielded && !servicingDmaInterrupts &&
                   static_cast<int32_t>(cpu.gpr[2]) == kKeEvfCond && !pendingDmaInterrupts.empty())
            {
                uint64_t nextCompletion = pendingDmaInterrupts.begin()->second;
                for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                    nextCompletion = std::min(nextCompletion, completionCycle);
                totalCycles = std::max(totalCycles, nextCompletion);
                servicePendingDmaInterrupts();
                if (!kernel.dispatchEventImport(ordinal, cpu))
                    break;
            }
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            refreshNextDmaDue();
            try
            {
                for (const int irq : completed)
                {
                    memory.completeSpuDma(irq);
                    (void)intrman.dispatchInterrupt(irq, *this);
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
            refreshNextDmaDue();
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // Earliest timer / DMA completion / guest callback event, given the kernel's next
        // delayed-thread wake-up.
        uint64_t nextSchedulerEvent(uint64_t kernelWake)
        {
            uint64_t bound = timrman.nextEventCycle(kernelWake);
            bound = std::min(bound, nextDmaDue);
            if (!pendingGuestCallbacks.empty())
                bound = std::min(bound, pendingGuestCallbacks.begin()->first);
            return bound;
        }

        // The EE advances the IOP in very small steps, so most runCycles calls would repeat the
        // previous scheduling decision. It is reused while nothing that feeds the decision
        // changed: no thread became ready or was created/removed, the resumed thread is still
        // ready, and no timer, DMA completion, callback or delayed wake-up is due.
        void cacheScheduleDecision(IopThread *thread)
        {
            cachedThread = thread;
            cachedIdle = thread == nullptr;
            cachedReadyEvents = kernel.readyEvents();
            cachedThreadSetGeneration = kernel.threadSetGeneration();
            cachedKernelWake = kernel.nextWakeCycle(~0ull);
        }

        void invalidateScheduleDecision()
        {
            cachedThread = nullptr;
            cachedIdle = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    if ((cachedThread != nullptr || cachedIdle) && kernel.readyEvents() == cachedReadyEvents &&
                        kernel.threadSetGeneration() == cachedThreadSetGeneration &&
                        (cachedIdle || cachedThread->state == IopThreadState::Ready))
                    {
                        const uint64_t bound = nextSchedulerEvent(cachedKernelWake);
                        if (totalCycles < bound)
                        {
                            if (cachedIdle)
                            {
                                totalCycles = std::max(totalCycles + 1u, std::min(target, bound));
                                continue;
                            }
                            IopThread &thread = *cachedThread;
                            kernel.resumeThread(thread);
                            const uint64_t before = totalCycles;
                            runCpu(thread.cpu,
                                   static_cast<uint32_t>(std::min<uint64_t>({kMaxEventFreeSlice, bound - totalCycles, target - totalCycles})),
                                   true);
                            kernel.endTimeslice(thread, kThreadReturnSentinel);
                            if (totalCycles == before)
                                ++totalCycles;
                            if (thread.state != IopThreadState::Ready || thread.cpu.yielded ||
                                kernel.readyEvents() != cachedReadyEvents ||
                                kernel.threadSetGeneration() != cachedThreadSetGeneration)
                                invalidateScheduleDecision();
                            continue;
                        }
                    }
                    invalidateScheduleDecision();

                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        cacheScheduleDecision(nullptr);
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    // Run until the next scheduled event instead of a fixed short slice: timers,
                    // delayed threads, guest callbacks and DMA completions are only checked here.
                    uint64_t sliceEnd = timrman.nextEventCycle(kernel.nextWakeCycle(target));
                    sliceEnd = std::min(sliceEnd, nextDmaDue);
                    if (!pendingGuestCallbacks.empty())
                        sliceEnd = std::min(sliceEnd, pendingGuestCallbacks.begin()->first);
                    const uint64_t sliceCycles = sliceEnd > totalCycles ? sliceEnd - totalCycles : 1u;
                    runCpu(next->cpu,
                           static_cast<uint32_t>(std::min<uint64_t>({kMaxEventFreeSlice, sliceCycles, target - totalCycles})),
                           true);
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                    if (next->state == IopThreadState::Ready && !next->cpu.yielded)
                        cacheScheduleDecision(next);
                }
                advanceSpu2();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        void onModuleImageLoaded(const std::string &name, uint32_t base, uint32_t size)
        {
            const uint32_t imageSize = std::min<uint32_t>(size, base < IopMemory::RamSize ? IopMemory::RamSize - base : 0u);
            const uint8_t *ram = memory.ram().data();
            if (!g_iopModuleDumpDir.empty() && imageSize != 0u)
            {
                std::error_code ec;
                std::filesystem::create_directories(g_iopModuleDumpDir, ec);
                std::string stem = name;
                for (char &c : stem)
                    if (c == ';' || c == ':' || c == '\\' || c == '/')
                        c = '_';
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "_%05x", base);
                const std::filesystem::path path = std::filesystem::path(g_iopModuleDumpDir) / (stem + suffix);
                std::ofstream bin(path.string() + ".bin", std::ios::binary);
                bin.write(reinterpret_cast<const char *>(ram + base), imageSize);
                std::ofstream info(path.string() + ".info");
                info << name << ' ' << std::hex << base << ' ' << imageSize << '\n';
            }
            if (!g_iopCompiledEnabled)
                return;
            for (const auto &reg : registeredIopModules())
            {
                if (reg.base != base || reg.textSize == 0u || reg.textSize > imageSize)
                    continue;
                if (hashIopCode(ram + base, reg.textSize) != reg.hash)
                {
                    log(LogLevel::Warning, "[IOP] compiled module " + reg.name + " does not match the loaded image; interpreting");
                    continue;
                }
                activeCompiled.push_back({base, base + reg.textSize, reg.fn});
                log(LogLevel::Info, "[IOP] using compiled code for " + reg.name);
            }
        }

        bool interruptWouldBeTaken(const CpuState &cpu) const
        {
            const uint32_t status = cpu.cop0[12];
            return (status & 1u) != 0u && (status & 2u) == 0u && memory.interruptControl() != 0u &&
                   (memory.interruptStatus() & memory.interruptMask()) != 0u;
        }

        // Runs compiled code for cpu.pc if it lies in an active compiled module. Returns true if
        // at least one instruction was executed (otherwise the caller steps the interpreter).
        bool runCompiled(CpuState &cpu, uint64_t instructionLimit)
        {
            const uint32_t pc = cpu.pc;
            for (const ActiveCompiledModule &module : activeCompiled)
            {
                if (pc < module.begin || pc >= module.end)
                    continue;
                if (cpu.stopped || interruptWouldBeTaken(cpu))
                    return false;
                uint64_t cycleBound = nextDmaDue;
                if (!pendingGuestCallbacks.empty())
                    cycleBound = std::min(cycleBound, pendingGuestCallbacks.begin()->first);
                const uint64_t before = totalInstructions;
                const bool verify = g_iopCompiledVerifyPeriod != 0u && (++compiledRuns % g_iopCompiledVerifyPeriod) == 0u;
                std::vector<uint8_t> ramBefore;
                CpuState cpuBefore;
                uint64_t cyclesBefore = totalCycles;
                if (verify)
                {
                    ramBefore.assign(memory.ram().begin(), memory.ram().end());
                    cpuBefore = cpu;
                }
                IopCompiledRun run{cpu, memory, memory.ram().data(), totalCycles, totalInstructions, cycleBound, instructionLimit};
                module.fn(run);
                const uint64_t executed = totalInstructions - before;
                if (verify && executed != 0u)
                    verifyCompiledRun(cpu, ramBefore, cpuBefore, cyclesBefore, before, executed, pc);
                return executed != 0u;
            }
            return false;
        }

        // Replays a compiled run with the interpreter from the saved state and compares.
        void verifyCompiledRun(CpuState &cpu, const std::vector<uint8_t> &ramBefore, const CpuState &cpuBefore,
                               uint64_t cyclesBefore, uint64_t instructionsBefore, uint64_t executed, uint32_t entryPc)
        {
            const std::vector<uint8_t> ramCompiled(memory.ram().begin(), memory.ram().end());
            const CpuState cpuCompiled = cpu;
            const uint64_t cyclesCompiled = totalCycles;
            std::memcpy(memory.ramData(), ramBefore.data(), ramBefore.size());
            cpu = cpuBefore;
            totalCycles = cyclesBefore;
            totalInstructions = instructionsBefore;
            for (uint64_t i = 0; i < executed; ++i)
                (void)step(cpu);
            std::string diff;
            if (cpu.gpr != cpuCompiled.gpr) diff += " gpr";
            if (cpu.hi != cpuCompiled.hi || cpu.lo != cpuCompiled.lo) diff += " hi/lo";
            if (cpu.pc != cpuCompiled.pc) diff += " pc";
            if (cpu.branchPending != cpuCompiled.branchPending || (cpu.branchPending && cpu.branchTarget != cpuCompiled.branchTarget)) diff += " branch";
            if (cpu.pendingLoad != cpuCompiled.pendingLoad ||
                (cpu.pendingLoad && (cpu.pendingLoadReg != cpuCompiled.pendingLoadReg || cpu.pendingLoadValue != cpuCompiled.pendingLoadValue)))
                diff += " pendingLoad";
            if (cpu.cop0 != cpuCompiled.cop0 || cpu.exception != cpuCompiled.exception) diff += " cop0";
            if (totalCycles != cyclesCompiled) diff += " cycles";
            if (std::memcmp(memory.ram().data(), ramCompiled.data(), ramCompiled.size()) != 0) diff += " ram";
            if (!diff.empty() && compiledVerifyMismatches++ < 50)
                std::fprintf(stderr, "[iop-verify] mismatch:%s (entry pc %x, %llu instructions, pc %x/%x)\n", diff.c_str(), entryPc,
                             static_cast<unsigned long long>(executed), cpuCompiled.pc, cpu.pc);
            static uint64_t verified = 0;
            if ((++verified & 0x3FFu) == 0u)
                std::fprintf(stderr, "[iop-verify] %llu compiled runs checked, %llu mismatches\n",
                             static_cast<unsigned long long>(verified), static_cast<unsigned long long>(compiledVerifyMismatches));
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;
            onModuleImageLoaded(module.name, loaded.base, loaded.size);

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            const uint32_t stoppedBase = it->second.base;
            const uint32_t stoppedEnd = it->second.base + it->second.size;
            std::erase_if(activeCompiled, [&](const ActiveCompiledModule &m)
                          { return m.begin < stoppedEnd && m.end > stoppedBase; });
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        // Earliest completion cycle in pendingDmaInterrupts (~0 when empty); lets the
        // per-instruction loop skip the map until something is actually due.
        uint64_t nextDmaDue = ~0ull;
        // Compiled modules whose loaded text matched a registration (see iop_compiled.h).
        struct ActiveCompiledModule
        {
            uint32_t begin = 0;
            uint32_t end = 0;
            IopCompiledFn fn = nullptr;
        };
        std::vector<ActiveCompiledModule> activeCompiled;
        uint64_t compiledRuns = 0;
        uint64_t compiledVerifyMismatches = 0;
        // Reusable scheduling decision (see runCycles).
        IopThread *cachedThread = nullptr;
        bool cachedIdle = false;
        uint64_t cachedReadyEvents = 0;
        uint64_t cachedThreadSetGeneration = 0;
        uint64_t cachedKernelWake = ~0ull;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
