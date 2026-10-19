#include "ps2_iop_thread.h"

#include "ThreadNaming.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ps2_perf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    thread_local bool t_onIopThread = false;
    thread_local uint32_t t_exclusiveDepth = 0; // Exclusive scopes this thread holds

    bool envSet(const char *name, unsigned long long &value)
    {
        const char *text = std::getenv(name);
        if (!text || !*text)
            return false;
        value = std::strtoull(text, nullptr, 0);
        return true;
    }

    bool threadEnabled()
    {
        const char *value = std::getenv("GOW_IOP_THREAD");
        return !(value && value[0] == '0' && value[1] == '\0');
    }

    bool statsEnabled()
    {
        const char *a = std::getenv("GOW_IOP_THREAD_STATS");
        const char *b = std::getenv("PS2X_IOP_NATIVE_STATS");
        return (a && a[0] == '1') || b != nullptr;
    }

    double secondsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
}

PS2IopThread::PS2IopThread(ps2x::iop::IopSubsystem &iop)
    : m_iop(iop),
      m_enabled(threadEnabled()),
      m_stats(statsEnabled())
{
    // Defaults: the EE may run up to 128 batches (65536 IOP cycles, ~1.8 ms of IOP time) ahead and then waits until
    // the thread is half way back; a sleeping thread is woken once 8 batches (4096 cycles, ~0.11 ms) are queued; an
    // idle thread looks for work for 50 us before it sleeps. Batches are >= 512 cycles, so the ring never fills.
    unsigned long long value = 0;
    m_leadCycles = envSet("GOW_IOP_LEAD", value) && value >= 1024u ? value : 65536u;
    m_leadCycles = std::min<uint64_t>(m_leadCycles, 512ull * (kRingSize - 4u));
    m_wakeCycles = std::min<uint64_t>(envSet("GOW_IOP_WAKE", value) && value != 0u ? value : 4096u, m_leadCycles);
    m_resumeCycles = m_leadCycles / 2u;
    m_spinNanos = (envSet("GOW_IOP_SPIN", value) ? value : 50u) * 1000u;
    if (m_enabled)
        std::fprintf(stderr, "[IOP:thread] IOP runs on its own thread (lead %llu, wake %llu IOP cycles, spin %llu us; GOW_IOP_THREAD=0 runs it inline)\n",
                     static_cast<unsigned long long>(m_leadCycles), static_cast<unsigned long long>(m_wakeCycles),
                     static_cast<unsigned long long>(m_spinNanos / 1000u));
}

PS2IopThread::~PS2IopThread()
{
    stop();
}

bool PS2IopThread::onIopThread() noexcept
{
    return t_onIopThread;
}

void PS2IopThread::stop() noexcept
{
    if (!m_started || m_stop.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
        m_cv.notify_all();
        m_doneCv.notify_all();
    }
    if (m_thread.joinable())
        m_thread.join();
    // shutdown only: batches still queued are dropped (the EE is gone; running them here would post SIF commands
    // to a stopped scheduler from a foreign thread)
    m_head.store(m_writeHead);
    m_tail.store(m_writeHead);
    m_unpublishedCycles = 0;
    m_outstanding.store(0);
}

void PS2IopThread::start()
{
    m_started = true;
    m_thread = std::thread([this] { threadLoop(); });
}

bool PS2IopThread::popBatch(uint64_t &cycles) noexcept
{
    const uint64_t tail = m_tail.load(std::memory_order_relaxed);
    if (tail == m_head.load(std::memory_order_acquire))
        return false;
    cycles = m_ring[tail % kRingSize];
    m_tail.store(tail + 1u, std::memory_order_release);
    return true;
}

void PS2IopThread::threadLoop()
{
    ThreadNaming::SetCurrentThreadName("IopRun"); // Switch: core 0 by default, GOW_CORE_IopRun=<0-2> moves it
    t_onIopThread = true;
    const auto ready = [this]
    {
        return m_tail.load(std::memory_order_seq_cst) != m_head.load(std::memory_order_seq_cst) &&
               m_wantExclusive.load(std::memory_order_seq_cst) == 0u;
    };
    for (;;)
    {
        if (!ready() && m_spinNanos != 0u && !m_stop.load())
        {
            // the EE grants a batch every few microseconds while it runs: look again for a moment before paying for
            // a sleep and a wake (GOW_IOP_SPIN=<microseconds>, 0 = sleep at once)
            const auto spinEnd = std::chrono::steady_clock::now() + std::chrono::nanoseconds(m_spinNanos);
            while (!ready() && !m_stop.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < spinEnd)
                std::this_thread::yield();
        }
        if (!ready())
        {
            // m_sleeping asks the EE for a notify (it clears the flag when it notifies, so a thread slow to wake
            // up costs one notify, not one per batch granted meanwhile); set before the last look, under the lock
            // the EE notifies with, so a batch published meanwhile is either seen here or notified
            std::unique_lock<std::mutex> lock(m_waitMutex);
            for (;;)
            {
                if (m_stop.load())
                    return;
                m_sleeping.store(true, std::memory_order_seq_cst);
                if (ready())
                    break;
                if (m_stats)
                    m_statSleeps.fetch_add(1u, std::memory_order_relaxed);
                m_cv.wait(lock);
            }
            m_sleeping.store(false, std::memory_order_seq_cst);
        }
        if (m_stop.load())
            return;
        std::lock_guard<std::mutex> exec(m_execMutex);
        uint64_t cycles = 0;
        // an owner waiting for the IOP (m_wantExclusive) runs (takes back) what is left
        while (m_wantExclusive.load(std::memory_order_acquire) == 0u && popBatch(cycles))
        {
            const auto t0 = m_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            m_iop.runIopCycles(cycles);
            if (m_stats)
            {
                m_statThreadNanos.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - t0).count()),
                                            std::memory_order_relaxed);
                m_statThreadCycles.fetch_add(cycles, std::memory_order_relaxed);
                m_statThreadRuns.fetch_add(1u, std::memory_order_relaxed);
            }
            const uint64_t left = m_outstanding.fetch_sub(cycles, std::memory_order_seq_cst) - cycles;
            if (left <= m_resumeCycles && m_eeWaiting.load(std::memory_order_seq_cst))
            {
                std::lock_guard<std::mutex> lock(m_waitMutex);
                m_doneCv.notify_one();
            }
        }
    }
}

void PS2IopThread::wakeThread() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
        if (!m_sleeping.load(std::memory_order_seq_cst))
            return;
        m_sleeping.store(false, std::memory_order_seq_cst);
        m_cv.notify_one();
    }
    if (m_stats)
        ++m_statWakes;
}

void PS2IopThread::waitForLead(uint64_t resume) noexcept
{
    // bounded lead: the EE waits for the thread until it is half way back (an idle EE accounts a whole wait at once
    // and then waits for it here; inline mode ran it in one go). EE idle time, not IOP work, for GOW_PERF_DIAG.
    ps2_perf::Scope waiting(ps2_perf::Bucket::GuestWait);
    const auto t0 = m_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    {
        std::unique_lock<std::mutex> lock(m_waitMutex);
        m_eeWaiting.store(true, std::memory_order_seq_cst);
        if (m_sleeping.load(std::memory_order_seq_cst))
        {
            m_sleeping.store(false, std::memory_order_seq_cst);
            m_cv.notify_one();
        }
        m_doneCv.wait(lock, [this, resume]
                      { return m_outstanding.load(std::memory_order_seq_cst) <= resume || m_stop.load(); });
        m_eeWaiting.store(false, std::memory_order_seq_cst);
    }
    if (m_stats)
    {
        ++m_statLeadWaits;
        m_statLeadWaitSeconds += secondsSince(t0);
    }
}

void PS2IopThread::grant(uint64_t iopCycles) noexcept
{
    if (iopCycles == 0u)
        return;
    if (t_exclusiveDepth != 0u || t_onIopThread || m_stop.load(std::memory_order_relaxed))
    {
        // the IOP is owned by this thread right now (a call into the IOP accounted EE time): run it here
        if (t_exclusiveDepth != 0u)
            takeBack(); // the scope published and ran everything when it opened; nothing is left here
        m_iop.runIopCycles(iopCycles);
        return;
    }
    if (!m_started)
    {
        try
        {
            start();
        }
        catch (...)
        {
            m_iop.runIopCycles(iopCycles); // no thread: inline
            return;
        }
    }
    if (m_writeHead - m_cachedTail >= kRingSize)
    {
        m_cachedTail = m_tail.load(std::memory_order_acquire);
        while (m_writeHead - m_cachedTail >= kRingSize)
        {
            publish(); // cannot happen with the clamped lead; kept as a guard
            wakeThread();
            std::this_thread::yield();
            m_cachedTail = m_tail.load(std::memory_order_acquire);
        }
    }
    m_ring[m_writeHead++ % kRingSize] = iopCycles;
    m_unpublishedCycles += iopCycles;
    if (m_stats)
    {
        ++m_statGrants;
        m_statGrantCycles += iopCycles;
    }
    if (m_unpublishedCycles >= m_wakeCycles)
        publish();
    if (m_stats && (m_statGrants & 1023u) == 0u)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_statsLast >= std::chrono::seconds(5))
            report(now);
    }
}

void PS2IopThread::publish() noexcept
{
    if (m_unpublishedCycles == 0u)
        return;
    // outstanding first: it never counts less than the published batches
    const uint64_t outstanding = m_outstanding.fetch_add(m_unpublishedCycles, std::memory_order_seq_cst) + m_unpublishedCycles;
    m_unpublishedCycles = 0;
    m_head.store(m_writeHead, std::memory_order_seq_cst);
    if (m_stats)
        m_statMaxOutstanding = std::max(m_statMaxOutstanding, outstanding);
    if (outstanding > m_leadCycles)
        waitForLead(m_resumeCycles);
    else if (m_sleeping.load(std::memory_order_seq_cst))
        wakeThread();
}

void PS2IopThread::takeBack() noexcept
{
    // the caller owns the IOP (m_execMutex): run every queued batch in order
    uint64_t cycles = 0;
    while (popBatch(cycles))
    {
        const auto t0 = m_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_iop.runIopCycles(cycles);
        if (m_stats)
        {
            m_statTakeBackSeconds += secondsSince(t0);
            m_statTakeBackCycles += cycles;
        }
        m_outstanding.fetch_sub(cycles, std::memory_order_seq_cst);
    }
}

bool PS2IopThread::busy() const noexcept
{
    return hasEffects() || m_unpublishedCycles != 0u || m_outstanding.load(std::memory_order_seq_cst) != 0u;
}

void PS2IopThread::defer(std::function<void()> effect)
{
    std::lock_guard<std::mutex> lock(m_effectsMutex);
    m_effects.push_back(std::move(effect));
    m_effectsPending.store(true, std::memory_order_release);
}

void PS2IopThread::applyEffects() noexcept
{
    if (!m_effectsPending.load(std::memory_order_acquire))
        return;
    std::vector<std::function<void()>> effects;
    {
        std::lock_guard<std::mutex> lock(m_effectsMutex);
        effects.swap(m_effects);
        m_effectsPending.store(false, std::memory_order_release);
    }
    for (auto &effect : effects)
    {
        try
        {
            effect();
        }
        catch (...)
        {
        }
    }
    if (m_stats)
        m_statEffects += effects.size();
}

PS2IopThread::Exclusive::Exclusive(PS2IopThread *owner, bool drain)
{
    if (!owner || !owner->m_enabled || t_onIopThread)
        return;
    m_owner = owner;
    if (t_exclusiveDepth++ != 0u)
        return; // nested: the outer scope owns the IOP
    const bool stats = owner->m_stats && drain;
    if (stats)
        m_start = std::chrono::steady_clock::now();
    owner->m_wantExclusive.fetch_add(1u, std::memory_order_seq_cst);
    owner->m_execMutex.lock();
    owner->m_wantExclusive.fetch_sub(1u, std::memory_order_seq_cst);
    m_locked = true;
    if (stats)
    {
        owner->m_statLockWaitSeconds += secondsSince(m_start);
        ++owner->m_statSyncs;
    }
    if (drain)
    {
        owner->applyEffects(); // what the thread did to EE state comes first, as it did inline
        owner->m_outstanding.fetch_add(owner->m_unpublishedCycles, std::memory_order_seq_cst);
        owner->m_unpublishedCycles = 0;
        owner->m_head.store(owner->m_writeHead, std::memory_order_seq_cst); // the EE's own batches: run here
        owner->takeBack();
    }
}

PS2IopThread::Exclusive::Exclusive(Exclusive &&other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)),
      m_locked(std::exchange(other.m_locked, false)),
      m_start(other.m_start)
{
}

PS2IopThread::Exclusive::~Exclusive()
{
    if (!m_owner)
        return;
    --t_exclusiveDepth;
    if (!m_locked)
        return;
    if (m_owner->m_stats && m_start != std::chrono::steady_clock::time_point{})
        m_owner->m_statHeldSeconds += secondsSince(m_start);
    m_owner->m_execMutex.unlock();
    // a pause (no drain) may leave batches queued for a thread that went to sleep meanwhile
    if (m_owner->m_tail.load() != m_owner->m_head.load())
        m_owner->wakeThread();
}

void PS2IopThread::report(std::chrono::steady_clock::time_point now)
{
    const double window = std::chrono::duration<double>(now - m_statsLast).count();
    m_statsLast = now;
    const uint64_t threadNanos = m_statThreadNanos.exchange(0, std::memory_order_relaxed);
    const uint64_t threadCycles = m_statThreadCycles.exchange(0, std::memory_order_relaxed);
    const uint64_t threadRuns = m_statThreadRuns.exchange(0, std::memory_order_relaxed);
    const uint64_t sleeps = m_statSleeps.exchange(0, std::memory_order_relaxed);
    // ee: what the IOP costs the EE thread now (exclusive scopes incl. the calls they guard, lead waits)
    std::fprintf(stderr,
                 "[IOP:thread] window=%.2f thread_ms=%.1f (%llu batches, %llu cycles, %llu sleeps) | ee: held_ms=%.1f lock_wait_ms=%.1f "
                 "takeback_ms=%.1f (%llu cycles) syncs=%llu lead_waits=%llu (%.1f ms) | grants=%llu (%llu cycles) wakes=%llu "
                 "effects=%llu max_lead=%llu\n",
                 window, threadNanos * 1e-6, static_cast<unsigned long long>(threadRuns),
                 static_cast<unsigned long long>(threadCycles), static_cast<unsigned long long>(sleeps),
                 m_statHeldSeconds * 1000.0, m_statLockWaitSeconds * 1000.0,
                 m_statTakeBackSeconds * 1000.0, static_cast<unsigned long long>(m_statTakeBackCycles),
                 static_cast<unsigned long long>(m_statSyncs), static_cast<unsigned long long>(m_statLeadWaits),
                 m_statLeadWaitSeconds * 1000.0, static_cast<unsigned long long>(m_statGrants),
                 static_cast<unsigned long long>(m_statGrantCycles), static_cast<unsigned long long>(m_statWakes),
                 static_cast<unsigned long long>(m_statEffects), static_cast<unsigned long long>(m_statMaxOutstanding));
    m_statGrants = m_statGrantCycles = m_statWakes = m_statLeadWaits = m_statSyncs = 0;
    m_statTakeBackCycles = m_statEffects = m_statMaxOutstanding = 0;
    m_statLeadWaitSeconds = m_statLockWaitSeconds = m_statTakeBackSeconds = m_statHeldSeconds = 0.0;
}
