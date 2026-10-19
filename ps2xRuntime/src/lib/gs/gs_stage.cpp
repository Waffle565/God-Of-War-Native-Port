#include "runtime/gs/gs_stage.h"
#include "ThreadNaming.h"

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
    };

    // Bound on queued bytes: enough for several XGKICK packets of a batch, small enough that the worker never
    // runs a whole frame ahead of the GS frontend.
    constexpr size_t kMaxQueuedBytes = 4u << 20;
    // GOW-Port: a sleeping stage is woken once this much is queued, at the end of each worker job (publish) or when
    // someone waits for it (waitIdle), not for every packet: the worker sends a few hundred packets per frame and
    // waking the stage for each one cost more CPU in thread switches (both threads) than the packets' GS work.
    constexpr size_t kWakeBytes = 64u << 10;
    // GOW-Port: the producer (the worker) gathers small packets (XGKICKs are ~1 KB, ~80000 per second in gameplay) in
    // `staged` and queues them together once this much is there, or at publish(): one lock and one queue entry per
    // batch instead of per packet. Larger packets are queued at once (after what is staged, order kept).
    constexpr size_t kStageFlushBytes = 16u << 10;

    struct Stage
    {
        std::mutex mutex;
        std::condition_variable work;   // stage waits for packets
        std::condition_variable space;  // producer waits for room
        std::condition_variable idle;   // waitIdle
        std::vector<Packet> queue; // a vector keeps its capacity across swaps (MSVC deque allocates per Packet)
        std::vector<std::vector<uint8_t>> spare; // recycled buffers (avoid an allocation per packet)
        std::vector<std::vector<std::pair<GifPathId, uint32_t>>> spareSizes;
        size_t queuedBytes = 0;
        bool busy = false;
        bool sleeping = false; // the stage waits on `work` (only then does a producer need to notify it)
        bool stop = false;
        std::thread thread;
        std::thread::id threadId;
        ps2_gs_stage::ProcessFn process;
        std::function<void()> afterBatch;
        Packet staged; // producer side only (one producer at a time: the worker), no lock
        size_t stagedBytes = 0;
    };

    Stage &stage()
    {
        static Stage s;
        return s;
    }

    void loop()
    {
        ThreadNaming::SetCurrentThreadName("GSStage");
        Stage &s = stage();
        std::vector<Packet> batch;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                for (Packet &p : batch)
                {
                    p.data.clear();
                    s.spare.push_back(std::move(p.data));
                    p.sizes.clear();
                    s.spareSizes.push_back(std::move(p.sizes));
                }
                batch.clear();
                s.busy = false;
                if (s.queue.empty())
                    s.idle.notify_all();
                s.sleeping = true;
                s.work.wait(lock, [&s] { return s.stop || !s.queue.empty(); });
                s.sleeping = false;
                if (s.queue.empty())
                    return;
                batch.swap(s.queue);
                s.queuedBytes = 0;
                s.busy = true;
            }
            s.space.notify_all();
            for (const Packet &p : batch)
            {
                size_t offset = 0;
                for (const auto &[path, size] : p.sizes)
                {
                    s.process(path, p.data.data() + offset, size);
                    offset += size;
                    if (s.afterBatch)
                        s.afterBatch(); // per packet: a GS interrupt reaches the EE as soon as its packet is done
                }
            }
        }
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
        s.stop = false;
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
            s.stop = true;
        }
        s.work.notify_all();
        s.thread.join();
        s.threadId = {};
    }

    bool running()
    {
        return stage().thread.joinable();
    }

    bool onStageThread()
    {
        return running() && std::this_thread::get_id() == stage().threadId;
    }

    // queues the staged packets (producer thread)
    void flushStaged(Stage &s)
    {
        if (s.stagedBytes == 0)
            return;
        const size_t size = s.stagedBytes;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.space.wait(lock, [&s, size] { return s.queuedBytes == 0 || s.queuedBytes + size <= kMaxQueuedBytes; });
        s.queuedBytes += size;
        s.queue.push_back(std::move(s.staged));
        s.staged = Packet{};
        if (!s.spare.empty())
        {
            s.staged.data = std::move(s.spare.back());
            s.spare.pop_back();
        }
        if (!s.spareSizes.empty())
        {
            s.staged.sizes = std::move(s.spareSizes.back());
            s.spareSizes.pop_back();
        }
        s.stagedBytes = 0;
        const bool wake = s.sleeping && s.queuedBytes >= kWakeBytes;
        lock.unlock();
        if (wake)
            s.work.notify_one();
    }

    void push(GifPathId path, const uint8_t *data, uint32_t size)
    {
        Stage &s = stage();
        s.staged.data.insert(s.staged.data.end(), data, data + size);
        s.staged.sizes.emplace_back(path, size);
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
        std::unique_lock<std::mutex> lock(s.mutex);
        const bool wake = s.sleeping && !s.queue.empty();
        lock.unlock();
        if (wake)
            s.work.notify_one();
    }

    void waitIdle()
    {
        if (!running() || onStageThread())
            return;
        Stage &s = stage();
        std::unique_lock<std::mutex> lock(s.mutex);
        if (s.sleeping && !s.queue.empty())
            s.work.notify_one();
        s.idle.wait(lock, [&s] { return s.queue.empty() && !s.busy; });
    }
}
