#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

namespace aether
{
// Offline worker only. No shell, PATH lookup, pipes or audio callback activity.
class NativeProcess
{
public:
    static juce::File helpers();
    static void run(const juce::File& executable,const juce::StringArray& arguments,
                    const juce::File& privateLog,int seconds,const std::atomic<bool>& cancel,
                    const std::function<void()>& poll={});
};
}
