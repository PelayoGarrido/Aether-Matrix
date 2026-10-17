#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <array>

namespace aether
{
// One worker publishes immutable, decoded clips. The audio thread only reads
// samples; retired clips are destroyed by the worker, never by the callback.
class AudioPreview
{
public:
    ~AudioPreview();
    void prepare(double hostRate);
    struct Clip {juce::AudioBuffer<float> audio;};
    static std::unique_ptr<Clip> decode(const juce::File& completedWav,const std::atomic<bool>* cancel=nullptr);
    void publish(std::unique_ptr<Clip>) noexcept; // Offline worker, after job commit.
    void load(const juce::File& completedWav) {publish(decode(completedWav));}
    void collect(); // Same worker, including while idle.
    void play() noexcept {command.store(1,std::memory_order_release);}
    void stop() noexcept {command.store(2,std::memory_order_release);}
    bool render(juce::AudioBuffer<float>&) noexcept;
    bool isPlaying() const noexcept {return publishedPlaying.load(std::memory_order_relaxed);}
    float progress() const noexcept {return publishedProgress.load(std::memory_order_relaxed);}
    double seconds() const noexcept {return publishedSeconds.load(std::memory_order_relaxed);}
private:
    std::atomic<Clip*> pending{nullptr};
    Clip* active=nullptr;
    juce::AbstractFifo retired{4};std::array<Clip*,4> garbage{};
    std::atomic<int> command{0};std::atomic<bool> publishedPlaying{false};
    std::atomic<float> publishedProgress{0};std::atomic<double> publishedSeconds{0};
    std::array<std::array<float,32>,1024> kernel{};
    double step=1,cursor=0;int fade=240,age=0,stopping=0;bool playing=false,restartRequested=false;
    static_assert(std::atomic<Clip*>::is_always_lock_free && std::atomic<double>::is_always_lock_free);
};
}
