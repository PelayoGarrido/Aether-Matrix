#include "AudioPreview.h"
#include <cmath>
#include <stdexcept>
namespace aether
{
AudioPreview::~AudioPreview()
{
    // The processor joins the publishing worker and stops audio first.
    delete pending.exchange(nullptr);delete active;collect();
}
void AudioPreview::prepare(double rate)
{
    if (!std::isfinite(rate) || rate<22050 || rate>192000) throw std::runtime_error("Unsupported preview rate");
    step=48000/rate;fade=std::max(1,static_cast<int>(rate*.005));playing=false;publishedPlaying.store(false);command.store(0);
    const double cutoff=.95*std::min(1.0,rate/48000.0);
    for (size_t phase=0;phase<kernel.size();++phase)
    {
        double total=0;
        for (size_t tap=0;tap<32;++tap)
        {
            const double x=static_cast<double>(tap)-15-static_cast<double>(phase)/1024;
            const double sinc=std::abs(x)<1e-12 ? cutoff : std::sin(juce::MathConstants<double>::pi*x*cutoff)/(juce::MathConstants<double>::pi*x);
            const double window=.42-.5*std::cos(juce::MathConstants<double>::twoPi*static_cast<double>(tap)/31)
                +.08*std::cos(2*juce::MathConstants<double>::twoPi*static_cast<double>(tap)/31);
            kernel[phase][tap]=static_cast<float>(sinc*window);total+=sinc*window;
        }
        for (auto& coefficient:kernel[phase]) coefficient=static_cast<float>(coefficient/total);
    }
}
void AudioPreview::collect()
{
    int a=0,b=0,c=0,d=0;retired.prepareToRead(4,a,b,c,d);
    for (int i=0;i<b;++i) {delete garbage[static_cast<size_t>(a+i)];garbage[static_cast<size_t>(a+i)]=nullptr;}
    for (int i=0;i<d;++i) {delete garbage[static_cast<size_t>(c+i)];garbage[static_cast<size_t>(c+i)]=nullptr;}
    retired.finishedRead(b+d);
}
std::unique_ptr<AudioPreview::Clip> AudioPreview::decode(const juce::File& file,const std::atomic<bool>* cancel)
{
    if (file.isSymbolicLink() || !file.existsAsFile() || file.getSize()>64*1024*1024) throw std::runtime_error("Invalid completed preview WAV");
    juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(),true));
    if (!reader || (reader->sampleRate<48000 || reader->sampleRate>48000) || reader->numChannels!=2 || reader->bitsPerSample!=24
        || reader->lengthInSamples<1 || reader->lengthInSamples>96*1024*1024/8)
        throw std::runtime_error("Preview requires bounded stereo PCM24 at 48 kHz");
    auto clip=std::make_unique<Clip>();clip->audio.setSize(2,static_cast<int>(reader->lengthInSamples));
    for (int start=0;start<clip->audio.getNumSamples();start+=65536)
    {
        if (cancel && cancel->load()) throw std::runtime_error("Piano job cancelled");
        const int count=std::min(65536,clip->audio.getNumSamples()-start);
        if (!reader->read(&clip->audio,start,count,start,true,true)) throw std::runtime_error("Cannot decode completed piano take");
        for (int channel=0;channel<2;++channel)
            for (int i=start;i<start+count;++i)
                if (!std::isfinite(clip->audio.getSample(channel,i))) throw std::runtime_error("Non-finite preview audio");
    }
    return clip;
}
void AudioPreview::publish(std::unique_ptr<Clip> clip) noexcept
{
    jassert(clip);if (!clip) return;
    collect();delete pending.exchange(clip.release(),std::memory_order_acq_rel);
}
bool AudioPreview::render(juce::AudioBuffer<float>& buffer) noexcept
{
    if (!playing && pending.load(std::memory_order_acquire)!=nullptr && (active==nullptr || retired.getFreeSpace()>0))
    {
        auto* next=pending.exchange(nullptr,std::memory_order_acq_rel);
        if (next)
        {
            if (active)
            {
                int a=0,b=0,c=0,d=0;retired.prepareToWrite(1,a,b,c,d);
                garbage[static_cast<size_t>(b ? a : c)]=active;retired.finishedWrite(1);
            }
            active=next;cursor=0;publishedProgress.store(0,std::memory_order_relaxed);publishedSeconds.store(0,std::memory_order_relaxed);
            if (restartRequested) {restartRequested=false;age=0;stopping=0;playing=true;}
        }
    }
    if (!playing && restartRequested && !pending.load(std::memory_order_acquire) && active)
    {restartRequested=false;cursor=0;age=0;stopping=0;playing=true;}
    const int requested=command.exchange(0,std::memory_order_acq_rel);
    if (requested==1 && active)
    {
        if (playing || pending.load(std::memory_order_acquire)) {restartRequested=true;if (playing) stopping=fade;}
        else {cursor=0;age=0;stopping=0;playing=true;}
    }
    else if (requested==2) {restartRequested=false;if (playing) stopping=fade;}
    if (!playing || !active) {publishedPlaying.store(false,std::memory_order_relaxed);return false;}
    buffer.clear();const auto frames=active->audio.getNumSamples();
    for (int i=0;i<buffer.getNumSamples();++i)
    {
        if (cursor>=frames) {playing=false;break;}
        const auto whole=static_cast<int>(cursor);const auto phase=static_cast<size_t>((cursor-whole)*1024);
        const float gain=std::min(1.0f,static_cast<float>(age++)/static_cast<float>(fade))
            * (stopping ? static_cast<float>(stopping)/static_cast<float>(fade) : 1.0f);
        for (int channel=0;channel<std::min(2,buffer.getNumChannels());++channel)
        {
            float sample=step==1.0 ? active->audio.getSample(channel,whole) : 0;
            for (int tap=0;step!=1.0 && tap<32;++tap)
            {
                const int source=whole+tap-15;
                if (source>=0 && source<frames) sample+=active->audio.getSample(channel,source)*kernel[phase][static_cast<size_t>(tap)];
            }
            buffer.setSample(channel,i,sample*gain);
        }
        cursor+=step;if (stopping && --stopping==0) {playing=false;break;}
    }
    publishedProgress.store(static_cast<float>(std::min(1.0,cursor/frames)),std::memory_order_relaxed);
    publishedSeconds.store(cursor/48000.0,std::memory_order_relaxed);
    publishedPlaying.store(playing,std::memory_order_relaxed);return true;
}
}
