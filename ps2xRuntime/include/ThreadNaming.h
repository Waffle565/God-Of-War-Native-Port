#pragma once

#include <string_view>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINAPI
#define WINAPI __stdcall
#endif

extern "C"
{
    typedef void *HANDLE;
    typedef void *HMODULE;
    typedef const wchar_t *PCWSTR;
    typedef long HRESULT;

    __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const wchar_t *lpModuleName);
    __declspec(dllimport) void *WINAPI GetProcAddress(HMODULE hModule, const char *lpProcName);
    __declspec(dllimport) HANDLE WINAPI GetCurrentThread(void);
}
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#elif defined(__SWITCH__)
#include <cstdlib>
// GOW-Port: ps2_switch_runtime.cpp. libnx starts every thread on the process's default core and Horizon does not
// time-slice threads of one priority, so the runtime's busy threads are spread over the three application cores.
// priorityBoost: steps above the default thread priority (Horizon preempts lower-priority threads on the same core).
extern "C" void ps2xSwitchPinCurrentThread(int core, int priorityBoost);
#endif

namespace ThreadNaming
{
    inline void SetCurrentThreadName(std::string_view name)
    {
#if defined(_WIN32) 
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
        auto setThreadDescription =
            reinterpret_cast<SetThreadDescriptionFn>(::GetProcAddress(kernel32, "SetThreadDescription"));

        if (setThreadDescription)
        {
            std::wstring wname(name.begin(), name.end());
            setThreadDescription(::GetCurrentThread(), wname.c_str());
        }

#elif defined(__APPLE__)
        pthread_setname_np(name.data());

#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name.data());
#elif defined(__SWITCH__)
        // EE + IOP and the GS frontend on core 1, VIF1/VU1 on core 2; the presentation thread and the GL backend on
        // core 0. The EE spends most of a frame spinning in vid::WaitForDMAComplete (profile 2026-10-16: ~85 % of its
        // samples), so the GS frontend shares its core one priority step above it: it runs whenever it has work and
        // the spin only gets what is left. On core 0 it starved behind the GL backend and the presentation.
        // GOW_CORE_<thread>=<core 0-2> in env.txt (GOW_CORE_GSStage, GOW_CORE_GameThread, GOW_CORE_GSWorker, other
        // names) moves a thread for console tests without a rebuild: with deko3d the backend on core 0 got light while
        // core 1 is full (gameplay per 5 s: EE 2.9 s + IOP 1.2 s + GS frontend 0.9 s, docs/switch-deko3d.md).
        int core = 0, boost = 0;
        if (name == "GameThread")
            core = 1;
        else if (name == "GSStage")
            core = 1, boost = 1;
        else if (name == "GSWorker")
            core = 2;
        const std::string variable = "GOW_CORE_" + std::string(name);
        if (const char *value = std::getenv(variable.c_str()); value && value[0] >= '0' && value[0] <= '2' && value[1] == '\0')
            core = value[0] - '0';
        ps2xSwitchPinCurrentThread(core, boost);
#endif
    }
}
