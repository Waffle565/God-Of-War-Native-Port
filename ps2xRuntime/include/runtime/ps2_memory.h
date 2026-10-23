#ifndef PS2_MEMORY_H
#define PS2_MEMORY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <deque>
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
    // The pairs [lo, hi) of the micro memory that can differ between generation `from` and generation `to` (the union of
    // the changes recorded in between); the whole memory when one of them is no longer (or not yet) recorded. Lets the
    // native VU1 layer compare and copy only an MPG's range after a code change instead of all 16 KB.
    void vu1CodeChangedRange(uint64_t from, uint64_t to, uint32_t &lo, uint32_t &hi) const
    {
        constexpr uint32_t kPairs = PS2_VU1_CODE_SIZE / 8u;
        lo = 0u, hi = kPairs;
        if (to <= from || to - from > kVu1ChangeLog)
            return;
        uint32_t l = kPairs, h = 0u;
        for (uint64_t g = from + 1u; g <= to; ++g)
        {
            const uint64_t e = m_vu1CodeChanges[g & (kVu1ChangeLog - 1u)].load(std::memory_order_acquire);
            if ((e >> 32) != (g & 0xFFFFFFFFu))
                return; // overwritten, or the writer has not stored it yet: everything
            l = (std::min<uint32_t>)(l, static_cast<uint32_t>(e >> 12) & 0xFFFu);
            h = (std::max<uint32_t>)(h, static_cast<uint32_t>(e) & 0xFFFu);
        }
        lo = l, hi = h;
    }

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
    // EE cycles advanceEeTimers can still take without stepping the timers (0: the next call steps them).
    uint64_t eeTimerQuietCycles() const noexcept
    {
        return m_eeTimerPendingCycles < m_eeTimerBudget ? m_eeTimerBudget - m_eeTimerPendingCycles : 0u;
    }
    // Called before every EE timer register access (EeScheduler::syncDeferredCycles): the scheduler's checkpoint
    // fast path defers the timer cycles; limitsChanged = a write that can move the next timer event.
    void setEeCycleSync(void (*sync)(void *, bool), void *context) noexcept
    {
        m_eeCycleSync = sync;
        m_eeCycleSyncContext = context;
    }
    // GOW-Port: native EE clock. A timer without compare/overflow events (no CMPE/OVFE/ZRET, e.g. the game's
    // T0 = BUSCLK/256 and T1 = H-BLNK counters) is not stepped: its count is computed when read, from the guest
    // EE cycle count since its epoch (the last COUNT write or clock change). Timers with events keep the stepping
    // model below. The recompiler emits constant-address timer loads/stores as direct calls to these (no
    // hardware-register path); readIORegister/writeIORegister use the same functions.
    // `address` may be physical or a KSEG0/KSEG1 alias; isEeTimerRegister tells whether it names a timer register.
    static bool isEeTimerRegister(uint32_t address) noexcept;
    uint32_t eeTimerLoad(uint32_t address) noexcept;
    bool eeTimerStore(uint32_t address, uint32_t value) noexcept;
    // GOW-Port: native DMA request service. The game's own stores to DMA channel registers (0x10008000-0x1000EFFF:
    // its scratchpad copies on channels 8/9, the boot VIF0 chain, D_STAT/D_PCR) are requests for a copy: a CHCR store
    // with STR runs the transfer at once (scratchpad copy or VIF/GIF packet) and queues its DMA-end handler; no DMA
    // controller state is kept; the IPU channels have nothing to move (the native MPEG decoder takes the movie data
    // from the demuxed PSS). D_ENABLEW/D_ENABLER (0x1000F590/0x1000F520) belong to it too: DMA is never suspended.
    // The recompiler emits constant-address 32-bit accesses to these registers as direct calls (no hardware-register
    // path; run-time checked where the address is computed); readIORegister/writeIORegister use the same functions.
    static bool isEeDmaRegister(uint32_t address) noexcept;
    uint32_t eeDmaLoad(uint32_t address) noexcept;
    bool eeDmaStore(uint32_t address, uint32_t value);
    // GOW-Port: native display service entry for game code (runtime/gs/gs_display.h). The recompiler emits 64-bit
    // stores to the GS display registers (PMODE, SMODE2, DISPFB1/2, DISPLAY1/2, BGCOLOR) as gsDisplayStore and 64-bit
    // CSR loads as gsStatusLoad (no hardware-register path). `address` may be physical or a KSEG0/KSEG1 alias.
    static bool isGsDisplayRegister(uint32_t address) noexcept;
    static bool isGsStatusRegister(uint32_t address) noexcept;
    void gsDisplayStore(uint32_t address, uint64_t value);
    uint64_t gsStatusLoad();
    [[nodiscard]] uint64_t cyclesUntilNextEeTimerInterrupt() const noexcept;
    void resetEeTimers() noexcept;

    using GifPacketCallback = std::function<void(const uint8_t *, uint32_t)>;
    void setGifPacketCallback(GifPacketCallback cb) { m_gifPacketCallback = std::move(cb); }
    void setGifArbiter(GifArbiter *arbiter) { m_gifArbiter = arbiter; }

    using Vu1MscalCallback = std::function<void(uint32_t startPC, uint32_t top, uint32_t itop)>;
    void setVu1MscalCallback(Vu1MscalCallback cb) { m_vu1MscalCallback = std::move(cb); }
    using Vu1MscntCallback = std::function<void(uint32_t top, uint32_t itop)>;
    void setVu1MscntCallback(Vu1MscntCallback cb) { m_vu1MscntCallback = std::move(cb); }

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
    // PS2X_GS_WORKER_TRACE=<s>: one "[gsw-trace] <host us> <what> <value>" line (kick/job-start/job-done/gs-irq)
    static void gsWorkerTrace(const char *what, size_t value);
    void processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount);
    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void processVIF0Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF0Data(const uint8_t *data, uint32_t sizeBytes);
    void processVIF1Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF1Data(const uint8_t *data, uint32_t sizeBytes);
    // GOW-Port: native DMA data path (docs/native-port.md, step 2). There is no DMA controller model: a send hands the
    // game's packets straight to their consumer (VIF0 / VIF1 packets to the VIF decoder -> VU0 / VU1, GIF packets to
    // the GIF paths -> GS), either a flat buffer or the game's DMA tag list walked in place, and is complete when it
    // returns (with the GS worker the bytes are copied now and consumed on the worker, in order). The caller signals
    // completion (DMA-end handlers): sceDmaSend dispatches them itself, game code that starts a channel through its
    // CHCR gets them after the store (consumeCompletedDmacCauses).
    enum class DmaPath : uint8_t { Vif0, Vif1, Gif };
    void sendDma(DmaPath path, uint32_t address, uint32_t qwc);
    void sendDmaChain(DmaPath path, uint32_t tagAddress);
    std::vector<uint32_t> consumeCompletedDmacCauses();

    // GOW-Port: PS2X_GS_WORKER=1 runs VIF1/GIF DMA processing (VIF1 -> VU1 -> PATH1/2/3 -> GS) on a worker
    // thread, like PCSX2's MTVU. DMA completion is still signalled at the kick, as without the worker.
    // Invariant: VIF1/VU1/GIF/GS-frontend state is touched by the worker while it has jobs, and by any other
    // thread only after syncGsWorker(); every such entry point below calls it.
    void syncGsWorker(const char *site = "stub");
    // GOW-Port: like syncGsWorker, but returns as soon as fewer than `maxQueued` jobs wait in the worker queue (the
    // running job and the GS stage may still be busy); 0 = syncGsWorker. For sync points whose only purpose is that
    // the game may reuse its DMA buffers (the worker copied them at the kick): PS2X_GS_SYNCPATH_DEPTH.
    void syncGsWorkerPending(const char *site, size_t maxQueued);
    void stopGsWorker();
    bool onGsWorkerThread() const
    {
        // s_gsWorkerOf: set by gsWorkerLoop on its own thread (a thread_local read instead of std::this_thread::get_id,
        // which on the Switch is pthread_self -> __syscall_thread_self -> the TLS read; asked per XGKICK packet)
        return m_gsWorkerEnabled &&
               (s_gsWorkerOf == this || s_vu1RunOf == this);
    }
    inline static thread_local const PS2Memory *s_gsWorkerOf = nullptr;
    inline static thread_local const PS2Memory *s_vu1RunOf = nullptr; // set by vu1AsyncLoop (PS2X_VU1_ASYNC=1)
    // GOW-Port: PS2X_VU1_ASYNC=1 (with the GS worker): VU1 microprograms run on their own thread ("VU1Run") while the
    // worker goes on decoding the VIF1 stream (UNPACKs into the other input buffers), as the hardware overlaps them.
    // The worker waits for the running program before anything the hardware also orders behind it (the next
    // MSCAL/MSCNT, MPG, FLUSH*, DIRECT, MSKPATH3, PATH3 GIF data, GIF arbiter drains) and before it reports idle.
    // Only the VU run itself (VU1 registers, VU1 data/code, XGKICK -> GIF arbiter -> GS stage) is on that thread.
    // PS2X_VU1_ASYNC=2 (test): no thread, each program runs at the next of those points, i.e. after every UNPACK the
    // overlap could let through first; equal output there means the game never unpacks into a running program's data.
    struct Vu1AsyncJob { uint32_t startPC, top, itop; bool mscnt; };
    using Vu1AsyncExec = std::function<void(const Vu1AsyncJob &)>;
    void setVu1AsyncExec(Vu1AsyncExec exec) { m_vu1AsyncExec = std::move(exec); }
    // true: the job was handed to the VU1 thread (caller on the worker thread only)
    bool vu1AsyncSubmit(const Vu1AsyncJob &job);
    void vu1AsyncWait();
    bool vu1AsyncEnabled() const { return m_vu1AsyncOn; }
    // VU0 FBRST as the EE had it when the running worker job was queued (MSCAL D/T-bit enables).
    uint32_t gsWorkerJobFbrst() const { return m_gsWorkerJobFbrst; }
    void setFbrstProvider(std::function<uint32_t()> provider) { m_fbrstProvider = std::move(provider); }
    // Called on the worker after a job that left GS CSR SIGNAL/FINISH set, so an idle EE scheduler wakes
    // up and delivers the GS interrupt now instead of at its next timer/VBlank deadline.
    void setGsWorkerWake(std::function<void()> wake) { m_gsWorkerWake = std::move(wake); }

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
    // per generation g (slot g % kVu1ChangeLog): (g low 32 bits) << 32 | first changed pair << 12 | end pair
    static constexpr uint32_t kVu1ChangeLog = 64u;
    std::atomic<uint64_t> m_vu1CodeChanges[kVu1ChangeLog] = {};
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

    uint8_t *m_vu0Code = nullptr;
    uint8_t *m_vu0Data = nullptr;
    uint8_t *m_vu1Code = nullptr;
    uint8_t *m_vu1Data = nullptr;
    bool m_path3Masked = false;
    // GOW-Port: DIRECT fragmentado es distinto de una IMAGE entre comandos DIRECT completos.
    uint32_t m_vif1DirectBytesRemaining = 0u;
    bool m_vif1DirectHl = false;
    std::vector<uint8_t> m_vif1DirectPayload; // como máximo 65536 QW (1 MiB)
    void submitVif1DirectPayload(const uint8_t *data, uint32_t sizeBytes, bool directHl);
    std::vector<std::vector<uint8_t>> m_path3MaskedFifo;

    struct GsWorkerItem
    {
        bool vif1 = false; // false: GIF PATH3
        std::vector<uint8_t> data;
    };
    struct GsWorkerJob
    {
        std::vector<GsWorkerItem> items;
        uint32_t fbrst = 0;
    };
    bool needsGsWorkerSync(uint32_t address) const;
    void syncGsWorkerFor(uint32_t address);
    void syncGsWorkerIfOtherThread(const char *site);
    void waitGsWorkerIdle(const char *site, uint32_t address);
    void appendGuestBytes(uint32_t address, uint32_t bytes, std::vector<uint8_t> &out);
    void deliverDma(DmaPath path, std::vector<uint8_t> &&bytes);
    void runGsWorkerJob(GsWorkerJob &job);
    void gsWorkerLoop();
    bool m_gsWorkerEnabled = false;
    std::thread m_gsWorker;
    std::thread::id m_gsWorkerId;
    std::mutex m_gsWorkerMutex;
    std::condition_variable m_gsWorkerCv;
    std::condition_variable m_gsWorkerIdleCv;
    std::deque<GsWorkerJob> m_gsWorkerJobs;
    bool m_gsWorkerBusy = false;
    bool m_gsWorkerStop = false;
    unsigned m_gsWorkerDepthWaiters = 0; // threads in syncGsWorkerPending (under m_gsWorkerMutex)
    uint32_t m_gsWorkerJobFbrst = 0;
    std::function<uint32_t()> m_fbrstProvider;
    std::function<void()> m_gsWorkerWake;
    // PS2X_VU1_ASYNC (vu1AsyncSubmit): one job slot, 0 idle / 1 queued / 2 running; the VU1 thread spins for
    // m_vu1AsyncSpinNs after a job before it sleeps on the condition variable.
    void vu1AsyncLoop();
    bool m_vu1AsyncOn = false;
    bool m_vu1AsyncDefer = false; // PS2X_VU1_ASYNC=2: no thread, the program runs at the next sync point (worst-case overlap test)
    std::thread m_vu1Async;
    std::thread::id m_vu1AsyncId;
    std::mutex m_vu1AsyncMutex;
    std::condition_variable m_vu1AsyncCv;     // VU1 thread waits for a job
    std::condition_variable m_vu1AsyncDoneCv; // worker waits for the job's end
    std::atomic<int> m_vu1AsyncState{0};
    std::atomic<bool> m_vu1AsyncSleeping{false};
    std::atomic<bool> m_vu1AsyncWaiting{false};
    bool m_vu1AsyncStop = false;
    bool m_vu1AsyncDrainAfter = false; // a job end found the VU1 thread busy: drain the GIF arbiter after the wait
    uint64_t m_vu1AsyncSpinNs = 0;
    uint64_t m_vu1AsyncWaitSpinNs = 0; // PS2X_VU1_ASYNC_WAIT_SPIN_US: the worker spins this long for the program's end (0: 4096 pauses)
    Vu1AsyncJob m_vu1AsyncJob{};
    Vu1AsyncExec m_vu1AsyncExec;

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
    // bytes [offset, offset + size) of the micro memory changed (default: all of it); after the write itself
    void markVU1CodeModified(uint32_t offset = 0u, uint32_t size = PS2_VU1_CODE_SIZE)
    {
        const uint64_t g = m_vu1CodeGeneration.fetch_add(1, std::memory_order_relaxed) + 1u;
        const uint64_t lo = (std::min<uint32_t>)(offset, PS2_VU1_CODE_SIZE) / 8u;
        const uint64_t hi = ((std::min<uint32_t>)(offset + size, PS2_VU1_CODE_SIZE) + 7u) / 8u;
        m_vu1CodeChanges[g & (kVu1ChangeLog - 1u)].store(((g & 0xFFFFFFFFu) << 32) | (lo << 12) | hi, std::memory_order_release);
    }
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
        uint64_t epoch = 0; // clock-only timers: guest EE cycle at which `count` was exact (zero remainder)
    };

    std::array<EeTimer, 4> m_eeTimers{};
    // EE cycles accounted since the timers were last advanced. advanceEeTimers runs at every EE checkpoint;
    // it only does the per-timer math once the pending cycles reach m_eeTimerBudget (the cycles until the
    // next timer interrupt), and timer register accesses flush first, so the guest sees exactly the same
    // counts and interrupts as with per-call advancing (the tick math carries its remainder exactly).
    uint64_t m_eeTimerPendingCycles = 0;
    uint64_t m_eeTimerBudget = 0;
    void (*m_eeCycleSync)(void *, bool) = nullptr;
    void *m_eeCycleSyncContext = nullptr;
    uint64_t m_eeCycleTotal = 0; // guest EE cycles seen by advanceEeTimers (the scheduler's EE cycle count)
    uint32_t eeTimerCount(const EeTimer &timer) const noexcept;
    uint32_t stepEeTimers(uint64_t eeCycles) noexcept;
    void flushEeTimers() noexcept;
    // GOW-Port: GIF_STAT en m_ioRegisters (los elementos de unordered_map no se mueven; solo clear() lo
    // invalida). advanceEeTimers lo usa ~1,5 millones de veces por segundo.
    uint32_t *m_gifStatSlot = nullptr;
    bool tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr);
    bool processScratchpadChain(uint32_t channelBase, uint32_t chcr); // GOW-Port
    void queueCompletedDmacCause(uint32_t cause);
};

#endif // PS2_MEMORY_H
