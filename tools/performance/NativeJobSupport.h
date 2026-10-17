#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <thread>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace aether::job
{
using Clock=std::chrono::steady_clock;
inline std::atomic<bool> cancelled{false};
static_assert(std::atomic<bool>::is_always_lock_free);
inline void onSignal(int) {cancelled.store(true,std::memory_order_relaxed);}
inline juce::File file(const char* path) {return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path));}
inline std::filesystem::path native(const juce::File& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(path.getFullPathName().toStdString());
#endif
}
inline size_t footprint()
{
#ifdef __APPLE__
    task_vm_info_data_t data{};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&data),&count)==KERN_SUCCESS) return static_cast<size_t>(data.phys_footprint);
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX data{};data.cb=sizeof(data);
    if (GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&data),sizeof(data)))
        return std::max(static_cast<size_t>(data.WorkingSetSize),static_cast<size_t>(data.PrivateUsage));
#endif
    return 0;
}
inline void initialise()
{
    cancelled.store(false,std::memory_order_relaxed);std::signal(SIGINT,onSignal);std::signal(SIGTERM,onSignal);
#ifdef __APPLE__
    setpriority(PRIO_PROCESS,0,10);pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
#elif defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
#endif
}
inline void json(const juce::File& file,const juce::var& value)
{
    if (!file.replaceWithText(juce::JSON::toString(value,true))) throw std::runtime_error("Could not save interpretation metadata");
}
struct Budget
{
    juce::File directory;
    Clock::time_point started=Clock::now();
    std::atomic<size_t> peak{0};
    // Sample in the watchdog during loading/encoding as well as token decoding.
    // A sampled bound is not an OS-enforced memory allocation ceiling.
    std::atomic<int> failure{0};
    bool observe() noexcept
    {
        const auto measured=footprint();auto previous=peak.load(std::memory_order_relaxed);
        while (measured>previous && !peak.compare_exchange_weak(previous,measured,std::memory_order_relaxed)) {}
        int reason=0;
        if (cancelled.load(std::memory_order_relaxed) || Clock::now()-started>std::chrono::seconds(150)) reason=1;
        else if (peak.load(std::memory_order_relaxed)>2ULL*1024*1024*1024) reason=2;
        else
        {
            try
            {
                std::error_code error;const auto space=std::filesystem::space(native(directory),error);
                if (error || space.available<3ULL*1024*1024*1024) reason=3;
            }
            catch (...) {reason=3;}
        }
        if (reason) {int none=0;failure.compare_exchange_strong(none,reason,std::memory_order_relaxed);}
        return failure.load(std::memory_order_relaxed)==0;
    }
    void check()
    {
        observe();
        switch (failure.load(std::memory_order_relaxed))
        {
            case 1: throw std::runtime_error("Interpretation cancelled or timed out");
            case 2: throw std::runtime_error("Interpretation exceeds 2 GiB sampled footprint");
            case 3: throw std::runtime_error("Interpretation disk reserve reached or unavailable");
            default: break;
        }
    }
};
}
