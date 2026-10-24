#include "runtime/gs/gs_stage.h"
#include "ThreadNaming.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // A queue entry: one or more GIF packets back to back in `data`, `sizes` gives each one's path and length.
    struct Packet
    {
        std::vector<uint8_t> data;
        std::vector<std::pair<GifPathId, uint32_t>> sizes;
        size_t bytes = 0;
    };

    // Bound on queued bytes: enough for several XGKICK packets of a batch, small enough that the worker never
    // runs a whole frame ahead of the GS frontend.
    constexpr size_t kMaxQueuedBytes = 4u << 20;
    // GOW-Port: a sleeping stage is woken once this much is queued, at the end of each worker job (publish) or when
    // someone waits for it (waitIdle), not for every packet: the worker sends a few hundred packets per frame and
    // waking the stage for each one cost more CPU in thread switches (both threads) than the packets' GS work.
    constexpr size_t kWakeBytes = 64u << 10;
    // GOW-Port: the producer (the worker) gathers small packets (XGKICKs are ~1 KB, ~80000 per second in gameplay) in
    // its slot and publishes the slot once this much is there, or at publish(): one hand-off per batch, not per packet.
    constexpr size_t kStageFlushBytes = 16u << 10;
    // GOW-Port (2026-10-24, sw-k7): the queue is a single-producer / single-consumer ring of packet pointers. The
    // producer fills a packet, stores it at `head` and publishes it by moving `head`; the stage processes the packet
    // at `tail` and frees it by moving `tail`; the producer takes freed packets back (up to `tail`) onto its own LIFO
    // free list, so the next packet it fills is the one the stage finished last (its buffer still in the L2: a plain
    // ring of 256 x 16 KB buffers cycled 4 MB through the 2 MB L2 and made every hand-off write miss). The mutex is
    // taken only to sleep or to wake a sleeper: with a mutex around every hand-off the worker spent 2-2.5 % of the VU1
    // thread in svcArbitrateLock (pthread_mutex_lock <- flushStaged, console fight profile, update_la6).
    constexpr uint32_t kSlots = 256;
    // a packet whose buffer grew past this (a large image packet) gives the memory back after use
    constexpr size_t kShrinkBytes = 256u << 10;

    struct Stage
    {
        std::mutex mutex;               // sleeping / waking only
        std::condition_variable work;   // stage waits for packets
        std::condition_variable space;  // producer waits for room
        std::condition_variable idle;   // waitIdle
        Packet packets[kSlots];
        Packet *ring[kSlots] = {};
        alignas(64) std::atomic<uint32_t> head{0};      // published packets (producer writes)
        alignas(64) std::atomic<uint32_t> tail{0};      // finished packets (stage writes)
        alignas(64) std::atomic<bool> sleeping{false};  // the stage waits on `work` (only then does a producer notify)
        std::atomic<bool> producerWaiting{false};
        std::atomic<uint32_t> idleWaiters{0};
        std::atomic<bool> stop{false};
        uint64_t spinNanos = 0;
        std::thread thread;
        std::thread::id threadId;
        ps2_gs_stage::ProcessFn process;
        std::function<void()> afterBatch;
        // producer side only (one producer at a time: the worker)
        alignas(64) uint32_t fillHead = 0;  // == head, producer's copy
        uint32_t reclaimed = 0;             // packets in [reclaimed, tail) are finished but not yet on `freeList`
        size_t inFlightBytes = 0;           // bytes of the packets in [reclaimed, head)
        Packet *current = nullptr;          // the packet being filled
        size_t stagedBytes = 0;
        std::vector<Packet *> freeList;

        Stage()
        {
            freeList.reserve(kSlots);
            for (uint32_t i = kSlots; i-- > 1;)
                freeList.push_back(&packets[i]);
            current = &packets[0];
        }
    };

    Stage &stage()
    {
        static Stage s;
        return s;
    }

    thread_local bool t_onStage = false; // true on the stage thread (loop)

    // Sleep / wake protocol: the sleeper stores `sleeping` (seq_cst) and then re-reads `head` under the mutex; a
    // producer stores `head` (seq_cst) and then reads `sleeping`: one of the two sees the other, and a notify is sent
    // with the mutex held, so it cannot fall between the sleeper's check and its wait. Same for producerWaiting /
    // tail and idleWaiters / tail.
    void loop()
    {
        ThreadNaming::SetCurrentThreadName("GSStage");
        t_onStage = true; // onStageThread
        Stage &s = stage();
        uint32_t tail = s.tail.load(std::memory_order_relaxed);
        for (;;)
        {
            if (s.head.load(std::memory_order_acquire) == tail && s.spinNanos != 0)
            {
                // GOW-Port: look for the next packet a while before sleeping: during a worker job the producer hands
                // one over every few tens of microseconds, and each sleep cost a wake (mutex + svc on both threads)
                const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(s.spinNanos);
                for (uint32_t i = 1; s.head.load(std::memory_order_acquire) == tail; ++i)
                {
#if defined(__aarch64__)
                    asm volatile("yield" ::: "memory");
#endif
                    if ((i & 63u) == 0u && std::chrono::steady_clock::now() >= until)
                        break;
                }
            }
            if (s.head.load(std::memory_order_acquire) == tail)
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                s.sleeping.store(true, std::memory_order_seq_cst);
                if (s.idleWaiters.load(std::memory_order_seq_cst) != 0)
                    s.idle.notify_all();
                s.work.wait(lock, [&s, tail] {
                    return s.stop.load(std::memory_order_acquire) || s.head.load(std::memory_order_seq_cst) != tail;
                });
                s.sleeping.store(false, std::memory_order_relaxed);
                if (s.head.load(std::memory_order_acquire) == tail)
                    return; // stop requested and nothing left
            }
            Packet &p = *s.ring[tail % kSlots];
            size_t offset = 0;
            for (const auto &[path, size] : p.sizes)
            {
                s.process(path, p.data.data() + offset, size);
                offset += size;
                if (s.afterBatch)
                    s.afterBatch(); // per packet: a GS interrupt reaches the EE as soon as its packet is done
            }
            p.data.clear();
            p.sizes.clear();
            if (p.data.capacity() > kShrinkBytes)
                std::vector<uint8_t>().swap(p.data);
            ++tail;
            s.tail.store(tail, std::memory_order_seq_cst);
            if (s.producerWaiting.load(std::memory_order_seq_cst))
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.space.notify_all();
            }
            if (s.idleWaiters.load(std::memory_order_seq_cst) != 0 && s.head.load(std::memory_order_seq_cst) == tail)
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.idle.notify_all();
            }
        }
    }

    void wakeStage(Stage &s)
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.work.notify_one();
    }
}

namespace ps2_gs_stage
{
    void start(ProcessFn process, std::function<void()> afterBatch)
    {
        Stage &s = stage();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.thread.joinable())
            return;
        s.process = std::move(process);
        s.afterBatch = std::move(afterBatch);
        s.stop.store(false);
        // GOW_GS_STAGE_SPIN_US: how long the idle stage polls for packets before it sleeps (default 100 us on the
        // Switch, where the stage has core 3 to itself; 0 elsewhere)
#if defined(__SWITCH__)
        s.spinNanos = 100000u;
#endif
        if (const char *v = std::getenv("GOW_GS_STAGE_SPIN_US"))
            s.spinNanos = std::strtoull(v, nullptr, 10) * 1000u;
        s.thread = std::thread(loop);
        s.threadId = s.thread.get_id();
        std::fprintf(stderr, "[gs-stage] GS frontend on its own thread, pipelined behind VU1 (PS2X_GS_PIPELINE=0 disables)\n");
    }

    void stop()
    {
        Stage &s = stage();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.thread.joinable())
                return;
            s.stop.store(true);
            s.work.notify_all();
        }
        s.thread.join();
        s.threadId = {};
    }

    bool running()
    {
        return stage().thread.joinable();
    }

    bool onStageThread()
    {
        return t_onStage; // set by the stage thread itself (no std::this_thread::get_id per call)
    }

    // takes the packets the stage finished back onto the free list (producer thread)
    void reclaim(Stage &s)
    {
        const uint32_t tail = s.tail.load(std::memory_order_seq_cst);
        while (s.reclaimed != tail)
        {
            Packet *p = s.ring[s.reclaimed % kSlots];
            s.inFlightBytes -= p->bytes;
            s.freeList.push_back(p);
            ++s.reclaimed;
        }
    }

    // publishes the packet being filled (producer thread); waits while the byte bound is reached or no packet is free
    void flushStaged(Stage &s)
    {
        if (s.stagedBytes == 0)
            return;
        const size_t size = s.stagedBytes;
        auto fits = [&s, size] {
            reclaim(s);
            return (s.inFlightBytes == 0 || s.inFlightBytes + size <= kMaxQueuedBytes) && !s.freeList.empty();
        };
        if (!fits())
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.producerWaiting.store(true, std::memory_order_seq_cst);
            s.space.wait(lock, fits);
            s.producerWaiting.store(false, std::memory_order_relaxed);
        }
        s.current->bytes = size;
        s.ring[s.fillHead % kSlots] = s.current;
        s.inFlightBytes += size;
        ++s.fillHead;
        s.head.store(s.fillHead, std::memory_order_seq_cst);
        s.current = s.freeList.back(); // the packet the stage finished last
        s.freeList.pop_back();
        s.stagedBytes = 0;
        if (s.inFlightBytes >= kWakeBytes && s.sleeping.load(std::memory_order_seq_cst))
            wakeStage(s);
    }

    void push(GifPathId path, const uint8_t *data, uint32_t size)
    {
        Stage &s = stage();
        Packet &p = *s.current;
        p.data.insert(p.data.end(), data, data + size);
        p.sizes.emplace_back(path, size);
        s.stagedBytes += size;
        if (s.stagedBytes >= kStageFlushBytes)
            flushStaged(s);
    }

    void publish()
    {
        if (!running())
            return;
        Stage &s = stage();
        flushStaged(s);
        if (s.sleeping.load(std::memory_order_seq_cst) &&
            s.head.load(std::memory_order_relaxed) != s.tail.load(std::memory_order_seq_cst))
            wakeStage(s);
    }

    void waitIdle()
    {
        if (!running() || onStageThread())
            return;
        Stage &s = stage();
        if (s.head.load(std::memory_order_seq_cst) == s.tail.load(std::memory_order_seq_cst))
            return;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.idleWaiters.fetch_add(1, std::memory_order_seq_cst);
        if (s.sleeping.load(std::memory_order_seq_cst))
            s.work.notify_one();
        s.idle.wait(lock, [&s] { return s.head.load(std::memory_order_seq_cst) == s.tail.load(std::memory_order_seq_cst); });
        s.idleWaiters.fetch_sub(1, std::memory_order_relaxed);
    }
}
