#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    class IopEmulator
    {
    public:
        explicit IopEmulator(IopHost &host);
        ~IopEmulator();

        IopEmulator(const IopEmulator &) = delete;
        IopEmulator &operator=(const IopEmulator &) = delete;

        void reset();
        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result);
        void runEeCycles(uint64_t eeCycles) noexcept;
        [[nodiscard]] bool accrueEeCycles(uint64_t eeCycles) noexcept;
        [[nodiscard]] uint64_t eeCyclesUntilBatch() const noexcept;
        void runPendingEeCycles() noexcept;
        // inline path, after accrueEeCycles reported a batch: true when the batch ends before the IOP's next event
        // (no ready thread) and was deferred instead (GOW_IOP_IDLE_DEFER); false: runPendingEeCycles as before
        [[nodiscard]] bool deferIdleBatch() noexcept;
        // runs the deferred idle cycles and drops the horizon: every IOP entry from the EE side calls it first
        void syncIdle() noexcept;
        // runPendingEeCycles split for the IOP thread (docs/iop-thread.md): the EE thread takes the batch, the thread runs it
        [[nodiscard]] uint64_t takePendingIopCycles() noexcept;
        void runIopCycles(uint64_t iopCycles) noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        [[nodiscard]] bool hasRpcServer(uint32_t sid) const noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        // GOW-Port: 48 kHz stereo samples produced by the native sound engine (SoundEngine::drainOutput).
        size_t drainAudio(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames);
        [[nodiscard]] uint64_t cycles() const noexcept;
        [[nodiscard]] uint64_t instructions() const noexcept;
        [[nodiscard]] uint32_t loadedModuleCount() const noexcept;
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t rpcServerCount() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
