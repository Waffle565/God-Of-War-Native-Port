#pragma once

// GOW-Port: the IOP (scheduler, native IRX code, services) on its own host thread, "IopRun" (docs/iop-thread.md).
//
// The EE thread still decides when IOP time passes: every batch of IOP cycles inline mode would run at an EE
// checkpoint (IopSubsystem::takePendingIopCycles) is queued instead, and the thread runs the queued batches in order,
// each with the same runIopCycles call inline mode makes. Every EE call into the IOP (RPC, SIF DMA, IOP memory,
// module load, reset) first takes the IOP exclusively and drains the queue: batches the thread has not started are
// run on the EE thread ("take-back"), so the call sees the IOP exactly where inline mode has it. What the IOP does to
// EE state while it runs on the thread (SIF DMA writes into EE RAM, SIF commands to the EE) is queued and applied by
// the EE thread at its next checkpoint, sync point or before it idles; reads of EE RAM are direct.
// The EE runs at most GOW_IOP_LEAD IOP cycles ahead of the IOP (it waits beyond that); a sleeping thread is woken
// once GOW_IOP_WAKE cycles are queued. GOW_IOP_THREAD=0 runs the IOP inline on the EE thread as before.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ps2x::iop
{
    class IopSubsystem;
}

class PS2IopThread
{
public:
    explicit PS2IopThread(ps2x::iop::IopSubsystem &iop);
    ~PS2IopThread();

    PS2IopThread(const PS2IopThread &) = delete;
    PS2IopThread &operator=(const PS2IopThread &) = delete;

    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    void stop() noexcept;

    // EE thread: a batch of IOP cycles became due (what inline mode would run now).
    void grant(uint64_t iopCycles) noexcept;

    // EE thread: effects on EE state the IOP queued while it ran on the thread.
    [[nodiscard]] bool hasEffects() const noexcept { return m_effectsPending.load(std::memory_order_acquire); }
    void applyEffects() noexcept;
    // True when queued IOP work or effects exist (the EE drains before it idles).
    [[nodiscard]] bool busy() const noexcept;

    // The calling thread runs the IOP right now on the IOP thread (host callbacks defer EE effects).
    [[nodiscard]] static bool onIopThread() noexcept;
    void defer(std::function<void()> effect);

    // Exclusive ownership of the IOP for the scope. drain = true (EE thread only): queued effects are applied and
    // every granted batch is run first, so the IOP state is the one inline mode has here. drain = false: the thread is
    // only paused (other host threads, debug). Nested and IOP-thread scopes are no-ops.
    class Exclusive
    {
    public:
        Exclusive() = default;
        Exclusive(PS2IopThread *owner, bool drain);
        ~Exclusive();
        Exclusive(const Exclusive &) = delete;
        Exclusive &operator=(const Exclusive &) = delete;
        Exclusive(Exclusive &&other) noexcept;
        Exclusive &operator=(Exclusive &&other) = delete;

    private:
        PS2IopThread *m_owner = nullptr;
        bool m_locked = false;
        std::chrono::steady_clock::time_point m_start{};
    };

private:
    static constexpr uint32_t kRingSize = 1024u; // entries are >= 512 cycles: the lead bound keeps the ring short

    void start();
    void threadLoop();
    bool popBatch(uint64_t &cycles) noexcept; // consumer side: the thread, or an owner while the thread is paused
    void takeBack() noexcept;
    void waitForLead(uint64_t resume) noexcept;
    void wakeThread() noexcept;
    void publish() noexcept; // EE thread: hand the batches written since the last publish to the thread
    void report(std::chrono::steady_clock::time_point now);

    ps2x::iop::IopSubsystem &m_iop;
    const bool m_enabled;
    const bool m_stats;
    uint64_t m_leadCycles = 0;
    uint64_t m_wakeCycles = 0;
    uint64_t m_resumeCycles = 0;
    uint64_t m_spinNanos = 0;

    std::thread m_thread;
    bool m_started = false; // EE thread only

    // single-producer (EE) ring of batches; the consumer is whoever holds m_execMutex. The EE writes batches into
    // the ring and publishes them in groups (m_head) of GOW_IOP_WAKE cycles, at sync points and before it idles: one
    // cross-core handoff per group instead of one per batch. Fields written by different threads on own cache lines.
    std::array<uint64_t, kRingSize> m_ring{};
    alignas(64) std::atomic<uint64_t> m_head{0}; // published batches (EE writes)
    alignas(64) std::atomic<uint64_t> m_tail{0}; // next batch to run (consumer writes)
    alignas(64) std::atomic<uint64_t> m_outstanding{0}; // published IOP cycles not finished
    alignas(64) uint64_t m_writeHead = 0; // EE thread only: next slot to write (>= m_head)
    uint64_t m_unpublishedCycles = 0;     // EE thread only
    uint64_t m_cachedTail = 0;            // EE thread only: last tail seen (ring-full check)

    // sleeping / waking (m_waitMutex guards the waits; the flags are also read without it, seq_cst)
    std::mutex m_waitMutex;
    std::condition_variable m_cv;     // IOP thread: work queued or stop
    std::condition_variable m_doneCv; // EE thread: the lead fell back under the limit
    alignas(64) std::atomic<bool> m_sleeping{false};
    std::atomic<bool> m_eeWaiting{false};
    std::atomic<bool> m_stop{false};

    // held while the IOP runs (the thread per run of batches, or an Exclusive owner)
    std::mutex m_execMutex;
    std::atomic<uint32_t> m_wantExclusive{0};

    std::mutex m_effectsMutex;
    std::vector<std::function<void()>> m_effects;
    alignas(64) std::atomic<bool> m_effectsPending{false};

    // statistics (GOW_IOP_THREAD_STATS=1 or PS2X_IOP_NATIVE_STATS=1), [IOP:thread] every 5 s; EE-thread fields
    std::chrono::steady_clock::time_point m_statsLast = std::chrono::steady_clock::now();
    uint64_t m_statGrants = 0, m_statGrantCycles = 0, m_statWakes = 0, m_statLeadWaits = 0, m_statSyncs = 0;
    uint64_t m_statTakeBackCycles = 0, m_statEffects = 0;
    double m_statLeadWaitSeconds = 0.0, m_statLockWaitSeconds = 0.0, m_statTakeBackSeconds = 0.0, m_statHeldSeconds = 0.0;
    uint64_t m_statMaxOutstanding = 0;
    // IOP-thread fields
    alignas(64) std::atomic<uint64_t> m_statThreadNanos{0}, m_statThreadCycles{0}, m_statThreadRuns{0}, m_statSleeps{0};
};
