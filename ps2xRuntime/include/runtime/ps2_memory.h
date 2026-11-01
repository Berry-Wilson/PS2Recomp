#ifndef PS2_MEMORY_H
#define PS2_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "gs/ps2_gif_arbiter.h"
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif

class GS;

constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u; // 32MB
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;   // Mask for 32MB alignment
constexpr uint32_t PS2_RAM_BASE = 0x00000000;          // Physical base of RDRAM
constexpr uint32_t PS2_SCRATCHPAD_BASE = 0x70000000;
constexpr uint32_t PS2_SCRATCHPAD_ALIAS_BASE = 0xF0000000;
constexpr uint32_t PS2_SCRATCHPAD_SIZE = 16u * 1024u;  // 16KB
constexpr uint32_t PS2_IO_BASE = 0x10000000;           // Base for many I/O regs (Timers, DMAC, INTC)
constexpr uint32_t PS2_IO_SIZE = 0x10000;              // 64KB
constexpr uint32_t PS2_BIOS_BASE = 0x1FC00000;         // Or BFC00000 depending on KSEG
constexpr uint32_t PS2_BIOS_SIZE = 4u * 1024u * 1024u; // 4MB

constexpr uint32_t PS2_VU0_CODE_BASE = 0x11000000; // Base address as seen from EE
constexpr uint32_t PS2_VU0_DATA_BASE = 0x11004000;
constexpr uint32_t PS2_VU0_CODE_SIZE = 4u * 1024u; // 4KB Micro Memory
constexpr uint32_t PS2_VU0_DATA_SIZE = 4u * 1024u; // 4KB Data Memory (VU Mem)

constexpr uint32_t PS2_VU1_CODE_BASE = 0x11008000;
constexpr uint32_t PS2_VU1_DATA_BASE = 0x1100C000;
constexpr uint32_t PS2_VU1_MEM_BASE = PS2_VU1_CODE_BASE; // Alias used by older code paths
constexpr uint32_t PS2_VU1_CODE_SIZE = 16u * 1024u;      // 16KB Micro Memory
constexpr uint32_t PS2_VU1_DATA_SIZE = 16u * 1024u;      // 16KB Data Memory (VU Mem)

constexpr uint32_t PS2_GS_BASE = 0x12000000;
constexpr uint32_t PS2_GS_PRIV_REG_BASE = PS2_GS_BASE; // GS Privileged Registers
constexpr uint32_t PS2_GS_PRIV_REG_SIZE = 0x2000;
constexpr size_t PS2_GS_VRAM_SIZE = 4u * 1024u * 1024u; // 4MB GS VRAM

inline constexpr uint32_t PS2_FIO_O_RDONLY = 0x0001;
inline constexpr uint32_t PS2_FIO_O_WRONLY = 0x0002;
inline constexpr uint32_t PS2_FIO_O_RDWR = 0x0003;
inline constexpr uint32_t PS2_FIO_O_NBLOCK = 0x0010;
inline constexpr uint32_t PS2_FIO_O_APPEND = 0x0100;
inline constexpr uint32_t PS2_FIO_O_CREAT = 0x0200;
inline constexpr uint32_t PS2_FIO_O_TRUNC = 0x0400;
inline constexpr uint32_t PS2_FIO_O_EXCL = 0x0800;
inline constexpr uint32_t PS2_FIO_O_NOWAIT = 0x8000;

inline constexpr uint32_t PS2_FIO_SEEK_SET = 0;
inline constexpr uint32_t PS2_FIO_SEEK_CUR = 1;
inline constexpr uint32_t PS2_FIO_SEEK_END = 2;

inline constexpr uint32_t PS2_FIO_S_IFDIR = 0x1000;
inline constexpr uint32_t PS2_FIO_S_IFREG = 0x2000;

static_assert((PS2_RAM_SIZE & (PS2_RAM_SIZE - 1u)) == 0u, "PS2_RAM_SIZE must be a power of two");
static_assert(PS2_RAM_MASK == (PS2_RAM_SIZE - 1u), "PS2_RAM_MASK must match PS2_RAM_SIZE");

inline std::atomic<uint8_t *> &ps2ScratchpadHostPtrStorage()
{
    static std::atomic<uint8_t *> ptr{nullptr};
    return ptr;
}

inline void ps2SetScratchpadHostPtr(uint8_t *ptr)
{
    ps2ScratchpadHostPtrStorage().store(ptr, std::memory_order_relaxed);
}

inline uint8_t *ps2GetScratchpadHostPtr()
{
    return ps2ScratchpadHostPtrStorage().load(std::memory_order_relaxed);
}

inline bool ps2IsScratchpadAddress(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return true;
    }

    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        return lower >= PS2_SCRATCHPAD_BASE &&
               lower < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);
    }

    return false;
}

inline uint32_t ps2ScratchpadOffset(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return addr - PS2_SCRATCHPAD_BASE;
    }

    const uint32_t lower = addr & 0x7FFFFFFFu;
    return lower - PS2_SCRATCHPAD_BASE;
}

inline bool ps2ResolveGuestPointer(uint32_t addr, uint32_t &offset, bool &scratch)
{
    if (ps2IsScratchpadAddress(addr))
    {
        scratch = true;
        offset = ps2ScratchpadOffset(addr);
        return true;
    }

    uint32_t phys = 0;
    if (addr < 0x20000000u)
    {
        phys = addr;
    }
    else if ((addr >= 0x20000000u && addr < 0x40000000u) ||
             (addr >= 0x80000000u && addr < 0xC0000000u))
    {
        phys = addr & 0x1FFFFFFFu;
    }

    if (phys >= PS2_RAM_SIZE)
    {
        phys &= PS2_RAM_MASK;
    }

    scratch = false;
    offset = phys;
    return true;
}
inline uint8_t *getMemPtr(uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

inline const uint8_t *getConstMemPtr(const uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        const uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

// PS2 GS (Graphics Synthesizer) registers
struct GSRegisters
{
    uint64_t pmode;    // Pixel mode
    uint64_t smode1;   // Sync mode 1
    uint64_t smode2;   // Sync mode 2
    uint64_t srfsh;    // Refresh control
    uint64_t synch1;   // Synchronization control 1
    uint64_t synch2;   // Synchronization control 2
    uint64_t syncv;    // Synchronization control V
    uint64_t dispfb1;  // Display buffer 1
    uint64_t display1; // Display area 1
    uint64_t dispfb2;  // Display buffer 2
    uint64_t display2; // Display area 2
    uint64_t extbuf;   // External buffer
    uint64_t extdata;  // External data
    uint64_t extwrite; // External write
    uint64_t bgcolor;  // Background color
    std::atomic<uint64_t> csr;
    std::atomic<uint64_t> vsyncTick;
    uint64_t imr;      // Interrupt mask
    uint64_t busdir;   // Bus direction
    uint64_t siglblid; // Signal label ID
};
static_assert(sizeof(GSRegisters) == (20u * sizeof(uint64_t)), "GSRegisters layout changed unexpectedly");
static_assert(alignof(GSRegisters) == alignof(uint64_t), "GSRegisters alignment must remain 64-bit");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "GS CSR atomic must be lock-free on all supported targets");

// PS2 VIF (VPU Interface) registers
struct VIFRegisters
{
    uint32_t stat;   // Status
    uint32_t fbrst;  // VIF Force Break
    uint32_t err;    // Error status
    uint32_t mark;   // Interrupt control
    uint32_t cycle;  // Transfer mode
    uint32_t mode;   // Mode control
    uint32_t num;    // Data amount counter
    uint32_t mask;   // Data mask
    uint32_t code;   // VIFcode
    uint32_t itops;  // ITOP save
    uint32_t base;   // Base address
    uint32_t ofst;   // Offset
    uint32_t tops;   // TOPS
    uint32_t itop;   // ITOP
    uint32_t top;    // TOP
    uint32_t row[4]; // Transfer row data
    uint32_t col[4]; // Transfer column data
};
static_assert(sizeof(VIFRegisters) == (23u * sizeof(uint32_t)), "VIFRegisters layout changed unexpectedly");

// PS2 DMA registers
struct DMARegisters
{
    uint32_t chcr; // Channel control
    uint32_t madr; // Memory address
    uint32_t qwc;  // Quadword count
    uint32_t tadr; // Tag address
    uint32_t asr0; // Address stack 0
    uint32_t asr1; // Address stack 1
    uint32_t sadr; // Source address
};
static_assert(sizeof(DMARegisters) == (7u * sizeof(uint32_t)), "DMARegisters layout changed unexpectedly");

struct JumpTable
{
    uint32_t address = 0;          // Base address of the jump table
    uint32_t baseRegister = 0;     // Register used for index
    std::vector<uint32_t> targets; // Jump targets
};

class PS2Memory
{
public:
    PS2Memory();
    ~PS2Memory();

    PS2Memory(const PS2Memory &) = delete;
    PS2Memory &operator=(const PS2Memory &) = delete;
    PS2Memory(PS2Memory &&) = delete;
    PS2Memory &operator=(PS2Memory &&) = delete;

    // Initialize memory
    bool initialize(size_t ramSize = PS2_RAM_SIZE);

    // Memory access methods
    uint8_t *getRDRAM() { return m_rdram; }
    uint8_t *getScratchpad() { return m_scratchpad; }
    uint8_t *getIOPRAM() { return iop_ram; }
    uint64_t dmaStartCount() const { return m_dmaStartCount.load(std::memory_order_relaxed); }
    uint64_t gifCopyCount() const { return m_gifCopyCount.load(std::memory_order_relaxed); }
    uint64_t gsWriteCount() const { return m_gsWriteCount.load(std::memory_order_relaxed); }
    uint64_t vifWriteCount() const { return m_vifWriteCount.load(std::memory_order_relaxed); }
    uint64_t getVU0CodeGeneration() const { return m_vu0CodeGeneration.load(std::memory_order_relaxed); }
    uint64_t getVU1CodeGeneration() const { return m_vu1CodeGeneration.load(std::memory_order_relaxed); }

    // Read/write memory
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    uint64_t read64(uint32_t address);
    __m128i read128(uint32_t address);

    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    void write64(uint32_t address, uint64_t value);
    void write128(uint32_t address, __m128i value);

    // TLB handling
    uint32_t translateAddress(uint32_t virtualAddress);
    bool tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const;
    bool tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid);
    int32_t tlbProbe(uint32_t vpn) const;
    size_t tlbEntryCount() const { return m_tlbEntries.size(); }

    // Hardware register interface
    bool writeIORegister(uint32_t address, uint32_t value);
    uint32_t readIORegister(uint32_t address);

    // EE timers advance from the scheduler's emulated EE-cycle clock. The
    // returned mask uses bits 0..3 for newly raised TIM0..TIM3 interrupts.
    uint32_t advanceEeTimers(uint64_t eeCycles) noexcept;
    [[nodiscard]] uint64_t cyclesUntilNextEeTimerInterrupt() const noexcept;
    void resetEeTimers() noexcept;

    using GifPacketCallback = std::function<void(const uint8_t *, uint32_t)>;
    void setGifPacketCallback(GifPacketCallback cb) { m_gifPacketCallback = std::move(cb); }
    void setGifArbiter(GifArbiter *arbiter) { m_gifArbiter = arbiter; }

    using Vu1MscalCallback = std::function<void(uint32_t startPC, uint32_t top, uint32_t itop)>;
    void setVu1MscalCallback(Vu1MscalCallback cb) { m_vu1MscalCallback = std::move(cb); }

    // VIF1 DMA transfers (VIF commands, the VU1 programs they start and their PATH1/PATH2 output)
    // run on a worker thread while the EE goes on (PS2X_VIF1_THREAD=0 keeps them on the EE thread).
    // Transfers are copied when they start and complete immediately as before; every EE-side access
    // to state the worker owns (VU1 memory, VIF1 and GIF registers/FIFOs, the GIF DMA channel, GS
    // privileged registers other than CSR, GS helpers) calls waitVif1Idle() first.
    void waitVif1Idle(const char *why = "other") const;
    static bool onVif1Worker();
    // Called on the EE thread when a transfer could go to the worker; returning false keeps it on
    // the EE thread (the runtime does so while VU1 D/T breaks are enabled).
    void setVif1AsyncGate(std::function<bool()> gate) { m_vif1AsyncGate = std::move(gate); }
    // Called on the worker when a job set GS FINISH, so the EE thread can raise INTC_GS.
    void setGsFinishNotify(std::function<void()> notify) { m_gsFinishNotify = std::move(notify); }
    // Applies a GIF packet's SIGNAL/FINISH/LABEL on the EE thread; called with nullptr it reports
    // whether that is possible (raw-GIF GS backend). Required for any work to go to the worker.
    void setGifPrivilegedEffects(std::function<bool(const uint8_t *, uint32_t)> fn) { m_gifPrivilegedEffects = std::move(fn); }
    void stopVif1Worker();
    using Vu1MscntCallback = std::function<void(uint32_t top, uint32_t itop)>;
    void setVu1MscntCallback(Vu1MscntCallback cb) { m_vu1MscntCallback = std::move(cb); }
    // VIF0 MSCAL/MSCALF (resume=false, startPC from the VIFcode) and MSCNT (resume=true).
    using Vu0MicroCallback = std::function<void(bool resume, uint32_t startPC, uint32_t itop)>;
    void setVu0MicroCallback(Vu0MicroCallback cb) { m_vu0MicroCallback = std::move(cb); }

    uint8_t *getVU1Code() { return m_vu1Code; }
    const uint8_t *getVU1Code() const { return m_vu1Code; }
    uint8_t *getVU1Data() { return m_vu1Data; }
    const uint8_t *getVU1Data() const { return m_vu1Data; }
    uint8_t *getVU0Code() { return m_vu0Code; }
    const uint8_t *getVU0Code() const { return m_vu0Code; }
    uint8_t *getVU0Data() { return m_vu0Data; }
    const uint8_t *getVU0Data() const { return m_vu0Data; }

    bool isPath3Masked() const { return m_path3Masked; }
    void flushMaskedPath3Packets(bool drainImmediately = true);

    void submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately = true, bool path2DirectHl = false);
    void processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount);
    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr);
    bool tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr);
    void processVIF0Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF0Data(const uint8_t *data, uint32_t sizeBytes);
    void processVIF1Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF1Data(const uint8_t *data, uint32_t sizeBytes);
    void processPendingTransfers();
    std::vector<uint32_t> consumeCompletedDmacCauses();

    int pollDmaRegisters();

    // Track code modifications for self-modifying code
    void registerCodeRegion(uint32_t start, uint32_t end);
    bool isCodeAddress(uint32_t address) const;
    bool isCodeModified(uint32_t address, uint32_t size);
    void clearModifiedFlag(uint32_t address, uint32_t size);

    // GS register accessors
    GSRegisters &gs() { return gs_regs; }
    const GSRegisters &gs() const { return gs_regs; }
    uint8_t *getGSVRAM() { return m_gsVRAM; }
    const uint8_t *getGSVRAM() const { return m_gsVRAM; }
    bool hasSeenGifCopy() const { return m_seenGifCopy; }
    // Main RAM (32MB)
    uint8_t *m_rdram;

    // Scratchpad memory (16KB)
    uint8_t *m_scratchpad;

    // IOP RAM (2MB)
    uint8_t *iop_ram;

    bool m_seenGifCopy;
    std::atomic<uint64_t> m_dmaStartCount{0};
    std::atomic<uint64_t> m_gifCopyCount{0};
    std::atomic<uint64_t> m_gsWriteCount{0};
    std::atomic<uint64_t> m_vifWriteCount{0};
    std::atomic<uint64_t> m_vu0CodeGeneration{0};
    std::atomic<uint64_t> m_vu1CodeGeneration{0};
    // I/O registers
    std::unordered_map<uint32_t, uint32_t> m_ioRegisters;

    // Registers
    GSRegisters gs_regs;
    uint8_t *m_gsVRAM;
    VIFRegisters vif0_regs;
    VIFRegisters vif1_regs;
    DMARegisters dma_regs[10]; // 10 DMA channels

    // TLB entries
    struct TLBEntry
    {
        uint32_t vpn;
        uint32_t pfn;
        uint32_t mask;
        bool valid;
    };

    std::vector<TLBEntry> m_tlbEntries;

    GifPacketCallback m_gifPacketCallback;
    GifArbiter *m_gifArbiter = nullptr;
    Vu1MscalCallback m_vu1MscalCallback;
    Vu1MscntCallback m_vu1MscntCallback;
    Vu0MicroCallback m_vu0MicroCallback;

    uint8_t *m_vu0Code = nullptr;
    uint8_t *m_vu0Data = nullptr;
    uint8_t *m_vu1Code = nullptr;
    uint8_t *m_vu1Data = nullptr;
    bool m_path3Masked = false;
    uint32_t m_vif1PendingPath2ImageQwc = 0u;
    bool m_vif1PendingPath2DirectHl = false;
    std::vector<std::vector<uint8_t>> m_path3MaskedFifo;

    struct PendingTransfer
    {
        bool fromScratchpad = false;
        uint32_t srcAddr = 0;
        uint32_t qwc = 0;
        std::vector<uint8_t> chainData;
    };
    std::vector<PendingTransfer> m_pendingGifTransfers;
    std::vector<PendingTransfer> m_pendingVif0Transfers;
    std::vector<PendingTransfer> m_pendingVif1Transfers;
    // Chain-mode DMA gathers each transfer into a buffer; reusing them avoids regrowing a fresh
    // vector (and copying what it already holds) on every chain DMA.
    std::vector<std::vector<uint8_t>> m_chainBufPool;

    mutable std::mutex m_vif1Mutex;
    mutable std::condition_variable m_vif1Cv;
    // A job is one VIF1 transfer, or a GIF-only DMA batch (PATH3 packets whose sizes are in
    // gifPackets) that must stay in order behind the VIF1 work queued before it.
    struct Vif1Job
    {
        std::vector<uint8_t> data;
        std::vector<uint32_t> gifPackets;
        bool gif = false;
    };
    std::deque<Vif1Job> m_vif1Jobs;                       // guarded by m_vif1Mutex
    std::vector<std::vector<uint8_t>> m_vif1FreeBufs;     // guarded by m_vif1Mutex
    std::atomic<uint32_t> m_vif1Outstanding{0};           // queued or running jobs
    bool m_vif1Stop = false;
    std::thread m_vif1Thread;
    std::function<bool()> m_vif1AsyncGate;
    std::function<void()> m_gsFinishNotify;
    std::function<bool(const uint8_t *, uint32_t)> m_gifPrivilegedEffects;
    // EE-side copy of the VIF1 state the pre-scan needs, advanced in queue order; refreshed from
    // vif1_regs / m_vif1PendingPath2ImageQwc whenever the worker is idle.
    uint32_t m_vif1ScanCycle = 0u;
    uint32_t m_vif1ScanPendingImageQwc = 0u;
    std::atomic<bool> m_path3MaskEverUsed{false};
    void prescanVif1PrivilegedEffects(const uint8_t *data, uint32_t sizeBytes);
    bool vif1AsyncEnabled() const;
    void enqueueVif1(std::vector<uint8_t> data);
    void enqueueVif1Job(Vif1Job job);
    void vif1WorkerLoop();
    std::vector<uint8_t> takeChainBuf();
    void recycleChainBufs(std::vector<PendingTransfer> &transfers);
    std::mutex m_completedDmacMutex;
    std::vector<uint32_t> m_completedDmacCauses;

    struct CodeRegion
    {
        uint32_t start;
        uint32_t end;
        std::vector<bool> modified; // Bitmap of modified 4-byte blocks
    };
    std::vector<CodeRegion> m_codeRegions;

    bool isAddressInRegion(uint32_t address, const CodeRegion &region);
    void markModified(uint32_t address, uint32_t size);
    void markVU0CodeModified() { m_vu0CodeGeneration.fetch_add(1, std::memory_order_relaxed); }
    void markVU1CodeModified() { m_vu1CodeGeneration.fetch_add(1, std::memory_order_relaxed); }
    bool isScratchpad(uint32_t address) const;
    uint8_t *mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit);
    const uint8_t *mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const;
    struct EeTimer
    {
        uint32_t count = 0;
        uint32_t mode = 0;
        uint32_t compare = 0;
        uint32_t hold = 0;
        uint64_t clockRemainder = 0;
    };

    std::array<EeTimer, 4> m_eeTimers{};
    // advanceEeTimers runs for every few EE cycles, so cycles are only accumulated until they
    // reach the next possible timer interrupt (m_eeTimerBudget, exact) or a timer register is
    // accessed; applying them in one step gives the same counts as applying them one by one.
    uint64_t m_eeTimerPendingCycles = 0;
    uint64_t m_eeTimerBudget = 0;
    bool m_gifFqcSet = false; // GIF_STAT.FQC was set by the last DMA batch; cleared on the next advance
    uint32_t applyEeTimerCycles(uint64_t eeCycles) noexcept;
    uint32_t flushEeTimers() noexcept;
    [[nodiscard]] uint64_t computeCyclesUntilNextEeTimerInterrupt() const noexcept;
    bool tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr);
    void completeDmacChannel(uint32_t channelBase, uint32_t cause);
    void queueCompletedDmacCause(uint32_t cause);
};

#endif // PS2_MEMORY_H
