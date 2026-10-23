#pragma once

// GOW-Port: native sound engine (replaces the register-level SPU2 model). The game's sound driver (989snd) reaches the
// sound hardware only through the LIBSD API; those calls are native (src/native/iop_native_overrides.cpp) and land here
// as typed voice / core state. No SPU2 registers and no IOP DMA controller exist: transfers are copies into the sample
// RAM. Mixing: 2 cores x 24 ADPCM voices (pitch with linear interpolation, ADSR envelope), dry mix L/R per voice,
// master volume, plus the core input stream (LIBSD block transfer, "ADMA") at the stream volume. 48 kHz stereo output.
// DSP algorithms as in PCSX2 (pcsx2/SPU2) and psx-spx (ADPCM / ADSR are the same as on the PS1). Reverb: parameters
// stored, not mixed yet. docs/native-port.md (step 3), docs/sound.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ps2x::iop::detail
{
    class SoundEngine
    {
    public:
        static constexpr uint32_t RamHalfwords = 1u << 20u; // 2 MB sample RAM
        static constexpr uint32_t VoicesPerCore = 24u;
        static constexpr uint32_t CyclesPerSample = 768u;  // IOP 36.864 MHz / 48 kHz

        SoundEngine();
        ~SoundEngine();
        SoundEngine(const SoundEngine &) = delete;
        SoundEngine &operator=(const SoundEngine &) = delete;
        void reset();

        // ---- LIBSD API (entry codes as libsd: (id << 8) | (voice << 1) | core; master volumes have bit 7 set) ----
        void setParam(uint32_t entry, uint16_t value);   // sceSdSetParam
        [[nodiscard]] uint16_t getParam(uint32_t entry) const; // sceSdGetParam
        void setSwitch(uint32_t entry, uint32_t mask);   // sceSdSetSwitch (KON, KOFF, VMIX*, NON, PMON)
        [[nodiscard]] uint32_t getSwitch(uint32_t entry) const; // sceSdGetSwitch
        void setAddr(uint32_t entry, uint32_t byteAddress); // sceSdSetAddr (SSA, LSA, NAX, ESA, EEA, ...)
        [[nodiscard]] uint32_t getAddr(uint32_t entry) const; // sceSdGetAddr (byte address)
        void setCoreAttr(uint32_t entry, uint32_t value);  // sceSdSetCoreAttr
        [[nodiscard]] uint32_t getCoreAttr(uint32_t entry) const;
        void initCore(uint32_t core);                      // sceSdInit state of one core

        // sample RAM transfers (sceSdVoiceTrans): byte address in sample RAM
        void writeSampleRam(uint32_t byteAddress, const uint8_t *source, size_t bytes);
        void readSampleRam(uint32_t byteAddress, uint8_t *destination, size_t bytes) const;
        void clearSampleRam(uint32_t byteAddress, size_t bytes);

        // core input stream (sceSdBlockTrans with SD_BLOCK_LOOP): the engine reads 512-byte L / R blocks from the ring in
        // IOP RAM at 48 kHz; onHalf is called (from advanceTo) each time a half of the ring has been consumed.
        struct StreamRing
        {
            const uint8_t *base = nullptr; // IOP RAM of the ring
            uint32_t size = 0;             // bytes
            uint32_t position = 0;         // next byte to read
            bool active = false;
            bool loop = false;             // false: the buffer plays once
        };
        void startStream(uint32_t core, const uint8_t *ring, uint32_t size, bool loop);
        void stopStream(uint32_t core);
        [[nodiscard]] uint32_t streamPosition(uint32_t core) const { sync(); m_lastPollCycle = m_lastCycle; return m_stream[core & 1u].position; }
        [[nodiscard]] bool takeStreamHalf(uint32_t core) noexcept; // a half was consumed since the last call

        // time: one stereo sample every CyclesPerSample IOP cycles
        void advanceTo(uint64_t iopCycle);

        // stereo samples produced so far (producer: IOP thread, consumer: host audio thread)
        [[nodiscard]] size_t pendingFrames() const;
        size_t drainOutput(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames = SIZE_MAX);
        // seconds the mixer thread spent mixing since the last call (0 when mixing inline)
        [[nodiscard]] double takeMixerSeconds() noexcept;
        // caller thread: time spent waiting for the mixer thread in sync() since the last call, and how many waits
        [[nodiscard]] double takeSyncWaitSeconds(uint64_t &waits) noexcept;
        // samples the mixer thread mixed since the last call
        [[nodiscard]] uint64_t takeWorkerSamples() noexcept { return m_workerSamples.exchange(0, std::memory_order_relaxed); }

    private:
        struct Voice
        {
            // parameters (as the driver sets them)
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0;                // start, halfwords
            // playback state
            bool active = false;
            uint32_t nax = 0;                // next ADPCM block to decode (halfwords)
            uint32_t playBlock = 0;          // block being played (NAX reads from it, like PCSX2's NextA)
            bool playStarted = false;
            uint32_t lsa = 0;                // loop start
            bool lsaFromDriver = false;      // the driver set LSA: block loop-start flags do not override it
            uint32_t pitchCounter = 0;       // 4.12
            int16_t decoded[28]{};
            uint32_t decodedIndex = 28;      // 28 = decode the next block first
            int32_t hist1 = 0, hist2 = 0;
            int16_t prev[2]{};
            uint8_t blockFlags = 0;
            enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release } phase = Phase::Off;
            int32_t envelope = 0;
            uint32_t envelopeCounter = 0;
        };
        struct Core
        {
            std::array<Voice, VoicesPerCore> voices{};
            uint32_t vmixL = 0, vmixR = 0, vmixEL = 0, vmixER = 0, non = 0, pmon = 0, endx = 0;
            uint16_t mmix = 0, attr = 0;
            uint16_t mvolL = 0, mvolR = 0, evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            uint32_t esa = 0, eea = 0, irqa = 0, tsa = 0; // halfwords
        };

        void keyOn(Core &core, uint32_t mask);
        void keyOff(Core &core, uint32_t mask);
        void decodeBlock(Core &core, uint32_t index);
        int32_t stepVoice(Core &core, uint32_t index);
        static void stepEnvelope(Voice &v);
        static uint32_t naxOf(const Voice &v) noexcept;
        void streamSample(uint32_t core, int32_t &left, int32_t &right);
        void step();
        uint64_t mixTo(uint64_t iopCycle); // returns the samples mixed

        // GOW-Port: mixing on its own thread ("SoundMix", docs/sound.md "Mixer thread"). advanceTo() only records the
        // cycle to mix up to; the worker mixes there. Every other call first waits until the worker has caught up
        // (sync), so each register write or read lands after exactly the samples it did when mixing ran inline on the
        // IOP thread: the output is sample-identical, and the mixing overlaps with the EE between the driver's calls.
        // While a core input stream (a movie) plays, the IOP fills its ring outside these calls, so advanceTo() mixes
        // inline then. GOW_SOUND_THREAD=0: always inline.
        void sync() const;
        void workerLoop();
        // The hand-off is a state word, so the caller (the EE thread, which runs the IOP) never takes a lock the worker
        // may hold: on the Switch a condition-variable signal makes the woken thread the mutex owner at once, and the
        // worker (core 0, behind the deko3d and GS stage threads) then held it until it got the core: the EE stood in
        // svcArbitrateLock for ~15 % of its time in fights (GOW_PC_SAMPLE, 2026-10-23). The mutex / condition variable
        // only put the worker to sleep (it re-checks every 2 ms, so a signal sent between its check and its wait costs
        // at most that) and let sync() sleep while a batch is being mixed.
        enum : uint32_t { JobIdle = 0, JobPending = 1, JobRunning = 2, JobRunningPending = 3 };
        mutable std::atomic<uint32_t> m_jobState{JobIdle};
        mutable std::mutex m_jobMutex;
        mutable std::condition_variable m_jobCv;  // worker: a job may be pending
        mutable std::condition_variable m_doneCv; // sync(): the running batch is done
        uint64_t m_batchCycles = 128u * CyclesPerSample; // GOW_SOUND_BATCH=<samples> (default 128, ~2.7 ms) per wake of the worker
        std::atomic<uint64_t> m_workerSamples{0}; // samples mixed by the worker (the rest: inline in sync())
        std::atomic<uint64_t> m_jobTarget{0}; // the latest advanceTo() cycle handed to the worker
        mutable uint64_t m_callerTarget = 0; // caller thread: the latest advanceTo() cycle
        mutable uint64_t m_handedTarget = 0; // caller thread: the cycle mixed up to or handed to the worker
        std::atomic<uint64_t> m_mixerNanos{0};
        mutable uint64_t m_syncWaitNanos = 0, m_syncWaits = 0;
        std::atomic<bool> m_stop{false};
        bool m_async = false;
        std::thread m_worker;

        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        std::array<StreamRing, 2> m_stream{};
        std::array<bool, 2> m_streamHalf{};
        std::vector<int16_t> m_output;
        mutable std::mutex m_outputMutex;
        // samples step() mixed during one advanceTo(), appended to m_output under one lock (EE thread only)
        std::vector<int16_t> m_mixed;
        double m_resamplePhase = 0.0; // drainOutput: fractional read position when playing back faster
        uint64_t m_lastCycle = 0;
        // GOW_SND_STALE (streamSample): when the stream position was last polled, the largest advanceTo step, and a hash of
        // each 1 KB block of the ring as last played
        mutable uint64_t m_lastPollCycle = 0;
        uint64_t m_maxAdvance = 0;
        std::array<uint32_t, 64> m_blockHash{};
    };
}
