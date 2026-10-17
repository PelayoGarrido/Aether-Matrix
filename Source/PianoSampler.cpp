#include "PianoSampler.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace aether
{
namespace
{
constexpr int taps=32,phases=1024;
constexpr size_t cacheLimit=768ULL*1024*1024,voiceLimit=256;
constexpr double pi=3.14159265358979323846;
double bounded(const juce::var& v,const char* field,double low,double high)
{
    const auto x=v[juce::Identifier(field)];
    if (!x.isInt() && !x.isInt64() && !x.isDouble()) throw std::runtime_error("Missing instrument number");
    const auto n=static_cast<double>(x);
    if (!std::isfinite(n) || n<low || n>high) throw std::runtime_error("Invalid instrument number");
    return n;
}
juce::var readJson(const juce::File& path)
{
    if (!path.existsAsFile() || path.getSize()>1024*1024) throw std::runtime_error("Invalid instrument metadata file");
    juce::FileInputStream stream(path);juce::MemoryBlock bytes;
    if (!stream.openedOk()) throw std::runtime_error("Cannot read instrument metadata");
    stream.readIntoMemoryBlock(bytes,1024*1024+1);
    if (bytes.getSize()>1024*1024) throw std::runtime_error("Instrument metadata exceeds 1 MiB");
    juce::var result;
    if (juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),static_cast<int>(bytes.getSize())),result).failed() || !result.isObject()) throw std::runtime_error("Invalid instrument metadata JSON");
    return result;
}
bool filename(const juce::String& text)
{
    if (text.isEmpty() || text.length()>64 || text.startsWithChar('.') || !text.endsWith(".flac")) return false;
    for (const auto c:text)
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='#' || c=='_' || c=='-' || c=='.')) return false;
    return true;
}
struct Kernel
{
    explicit Kernel(double speed)
    {
        const double cutoff=std::min(1.0,1.0/speed)*0.96;
        table.resize((phases+1)*taps);
        for (int phase=0;phase<=phases;++phase)
        {
            const double fraction=static_cast<double>(phase)/phases;
            double sum=0;
            for (int k=0;k<taps;++k)
            {
                const double x=k-(taps/2-1)-fraction;
                const double sinc=std::abs(x)<1.0e-12 ? cutoff : std::sin(pi*cutoff*x)/(pi*x);
                const double window=0.42+0.5*std::cos(pi*x/(taps/2))+0.08*std::cos(2*pi*x/(taps/2));
                const auto sample=static_cast<float>(sinc*window);
                table[static_cast<size_t>(phase*taps+k)]=sample;sum+=sample;
            }
            for (int k=0;k<taps;++k) table[static_cast<size_t>(phase*taps+k)]/=static_cast<float>(sum);
        }
    }
    std::array<float,2> at(const juce::AudioBuffer<float>& source,double cursor) const
    {
        const auto integer=static_cast<int>(cursor);
        const double position=(cursor-integer)*phases;
        const auto phase=std::clamp(static_cast<int>(position),0,phases-1);
        const float alpha=static_cast<float>(position-phase);
        const auto* a=table.data()+phase*taps;
        const auto* b=a+taps;
        std::array<float,2> out {};
        const auto* left=source.getReadPointer(0);const auto* right=source.getReadPointer(1);
        for (int k=0;k<taps;++k)
        {
            const int index=integer+k-(taps/2-1);
            if (index>=0 && index<source.getNumSamples())
            {const float coefficient=a[k]+(b[k]-a[k])*alpha;out[0]+=left[index]*coefficient;out[1]+=right[index]*coefficient;}
        }
        return out;
    }
    std::vector<float> table;
};
struct Sample {juce::AudioBuffer<float> audio;double rate=48000;size_t used=0;};
struct Asset {int64_t bytes=0;juce::String sha;};
struct Region {int lo,hi,root;juce::String name;double tune,release;std::array<int,16> offset {};};
struct Release {int lo,hi,root,minVelocity,maxVelocity;juce::String name;double volume,veltrack,decay;};
struct Voice
{
    size_t id=0;int pitch=60,velocity=80;
    bool main=false,keyDown=true,released=false,retriggerFade=false;
    double cursor=0,speed=1,releaseSeconds=1,age=0,retriggerFrames=0;
    float gain=1,envelope=1;
    std::shared_ptr<Sample> sample;
    std::shared_ptr<Kernel> kernel;
};
}
struct PianoSampler::Impl
{
    juce::File root;
    double rate=48000;
    std::map<juce::String,Asset> assets;
    std::map<juce::String,std::shared_ptr<Sample>> cache;
    std::map<double,std::shared_ptr<Kernel>> kernels;
    std::vector<Region> regions;
    std::vector<Release> releases;
    std::array<int,16> velocityHigh {};
    std::vector<Voice> voices;
    juce::AudioFormatManager formats;
    int pedalValue=0,pedalAlternation=0;
    size_t bytes=0,serial=0,peak=0;

    std::shared_ptr<Sample> load(const juce::String& name)
    {
        if (!filename(name)) throw std::runtime_error("Invalid piano sample name");
        if (const auto found=cache.find(name);found!=cache.end()) {found->second->used=++serial;return found->second;}
        const auto expected=assets.find(name);
        if (expected==assets.end()) throw std::runtime_error("Piano sample is not in the approved manifest");
        const auto folder=root.getChildFile("Samples"),file=folder.getChildFile(name);
        if (root.isSymbolicLink() || folder.isSymbolicLink() || file.isSymbolicLink() || !file.existsAsFile()
            || file.getSize()!=expected->second.bytes) throw std::runtime_error("Piano sample missing or changed");
        juce::FileInputStream stream(file);juce::MemoryBlock encoded;
        if (!stream.openedOk()) throw std::runtime_error("Cannot read piano sample");
        stream.readIntoMemoryBlock(encoded,static_cast<juce::int64>(expected->second.bytes+1));
        if (encoded.getSize()!=static_cast<size_t>(expected->second.bytes)
            || juce::SHA256(encoded.getData(),encoded.getSize()).toHexString()!=expected->second.sha)
            throw std::runtime_error("Piano sample failed SHA256 verification");
        // Decode precisely the verified bytes, rather than reopening a path
        // that could have been replaced between verification and decoding.
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(
            std::make_unique<juce::MemoryInputStream>(encoded.getData(),encoded.getSize(),false)));
        if (reader==nullptr || reader->numChannels!=2 || reader->sampleRate<44000 || reader->sampleRate>96000
            || reader->lengthInSamples<32 || reader->lengthInSamples>reader->sampleRate*120)
            throw std::runtime_error("Unexpected piano audio contract");
        const auto needed=static_cast<size_t>(reader->lengthInSamples)*2*sizeof(float);
        if (needed>cacheLimit) throw std::runtime_error("Sample exceeds piano memory budget");
        while (bytes+needed>cacheLimit)
        {
            auto victim=cache.end();
            for (auto it=cache.begin();it!=cache.end();++it)
                if (it->second.use_count()==1 && (victim==cache.end() || it->second->used<victim->second->used)) victim=it;
            if (victim==cache.end()) throw std::runtime_error("Active samples exceed piano memory budget");
            bytes-=static_cast<size_t>(victim->second->audio.getNumSamples())*2*sizeof(float);cache.erase(victim);
        }
        auto sample=std::make_shared<Sample>();sample->rate=reader->sampleRate;sample->used=++serial;
        sample->audio.setSize(2,static_cast<int>(reader->lengthInSamples));
        if (!reader->read(&sample->audio,0,sample->audio.getNumSamples(),0,true,true)) throw std::runtime_error("Piano decode failed");
        for (int c=0;c<2;++c)
            for (int i=0;i<sample->audio.getNumSamples();++i)
                if (!std::isfinite(sample->audio.getSample(c,i))) throw std::runtime_error("Non-finite piano sample");
        bytes+=needed;cache.emplace(name,sample);return sample;
    }
    void add(Voice voice,const juce::String& name,int rootNote,double tune=0,int offset=0)
    {
        if (voices.size()>=voiceLimit) throw std::runtime_error("Performance exceeds 256 active piano voices; no silent voice stealing");
        voice.sample=load(name);
        voice.speed=voice.sample->rate/rate*std::exp2((voice.pitch-rootNote+tune/100.0)/12.0);
        if (!std::isfinite(voice.speed) || voice.speed<0.25 || voice.speed>4) throw std::runtime_error("Invalid resampling ratio");
        auto found=kernels.find(voice.speed);
        if (found==kernels.end()) found=kernels.emplace(voice.speed,std::make_shared<Kernel>(voice.speed)).first;
        voice.kernel=found->second;voice.cursor=offset;
        if (voice.cursor<0 || voice.cursor>=voice.sample->audio.getNumSamples()) throw std::runtime_error("Invalid sample offset");
        voices.push_back(std::move(voice));peak=std::max(peak,voices.size());
    }
    void releaseNoises(const Voice& source,bool hammer)
    {
        if (hammer)
        {
            Voice voice;voice.pitch=source.pitch;
            voice.gain=static_cast<float>(std::pow(source.velocity/127.0,1.64)*std::pow(10.0,(-37-2*source.age)/20));
            add(voice,"rel"+juce::String(source.pitch-20)+".flac",source.pitch);
            return;
        }
        for (const auto& region:releases)
            if (source.pitch>=region.lo && source.pitch<=region.hi && source.velocity>=region.minVelocity && source.velocity<=region.maxVelocity)
            {
                Voice voice;voice.pitch=source.pitch;
                voice.gain=static_cast<float>(std::pow(source.velocity/127.0,2*region.veltrack/100)*std::pow(10.0,(region.volume-region.decay*source.age)/20));
                if (voice.gain>1.0e-5f) add(voice,region.name,region.root);
            }
    }
};
PianoSampler::PianoSampler(const juce::File& bank,const juce::File& manifest,const juce::File& layout,double sampleRate)
    :impl(std::make_unique<Impl>())
{
    if (!juce::approximatelyEqual(sampleRate,44100.0) && !juce::approximatelyEqual(sampleRate,48000.0)
        && !juce::approximatelyEqual(sampleRate,96000.0)) throw std::runtime_error("Unsupported piano sample rate");
    impl->root=bank;impl->rate=sampleRate;impl->voices.reserve(voiceLimit);
    impl->formats.registerFormat(new juce::FlacAudioFormat,true);
    const auto inventory=readJson(manifest),map=readJson(layout);
    const juce::String revision="3382bf9496bba2486f5ab0de55a264d1dfc38404";
    if (inventory["revision"].toString()!=revision || map["source_revision"].toString()!=revision)
        throw std::runtime_error("Piano mapping/manifest revision mismatch");
    const auto* files=inventory["files"].getArray();
    if (files==nullptr || files->size()>1000) throw std::runtime_error("Invalid piano inventory");
    for (const auto& file:*files)
    {
        const auto path=file["path"].toString();if (!path.startsWith("Samples/")) continue;
        const auto name=path.substring(8),sha=file["sha256"].toString();
        if (!filename(name) || sha.length()!=64 || !sha.containsOnly("0123456789abcdef") || impl->assets.contains(name))
            throw std::runtime_error("Invalid piano asset entry");
        impl->assets.emplace(name,Asset{static_cast<int64_t>(bounded(file,"bytes",1,32*1024*1024)),sha});
    }
    const auto* velocities=map["velocity_high"].getArray();const auto* regions=map["regions"].getArray();const auto* releases=map["release_regions"].getArray();
    if (velocities==nullptr || velocities->size()!=16 || regions==nullptr || regions->size()!=30 || releases==nullptr || releases->size()!=69)
        throw std::runtime_error("Unexpected piano layout");
    for (int i=0;i<16;++i) {const int n=static_cast<int>((*velocities)[i]);if (n<1 || n>127 || (i && n<=impl->velocityHigh[static_cast<size_t>(i-1)])) throw std::runtime_error("Invalid velocity layers");impl->velocityHigh[static_cast<size_t>(i)]=n;}
    if (impl->velocityHigh.back()!=127) throw std::runtime_error("Incomplete velocity layers");
    for (const auto& r:*regions)
    {
        Region region{static_cast<int>(bounded(r,"lo",21,108)),static_cast<int>(bounded(r,"hi",21,108)),static_cast<int>(bounded(r,"root",21,108)),
            r["name"].toString(),bounded(r,"tune_cents",-100,100),bounded(r,"release_seconds",0.02,8),{}};
        const auto* offsets=r["offset_frames"].getArray();if (offsets==nullptr || offsets->size()!=16) throw std::runtime_error("Missing piano sample offsets");
        for (int i=0;i<16;++i) {const int n=static_cast<int>((*offsets)[i]);if (n<0 || n>48000) throw std::runtime_error("Invalid piano offset");region.offset[static_cast<size_t>(i)]=n;}
        if (region.lo>region.hi || region.root<region.lo-1 || region.root>region.hi+1) throw std::runtime_error("Invalid piano key map");
        impl->regions.push_back(region);
    }
    for (int key=21;key<=108;++key)
        if (std::count_if(impl->regions.begin(),impl->regions.end(),[key](const auto& r) {return key>=r.lo && key<=r.hi;})!=1)
            throw std::runtime_error("Piano key map has a hole or overlap");
    for (const auto& r:*releases)
        impl->releases.push_back({static_cast<int>(bounded(r,"lo",21,108)),static_cast<int>(bounded(r,"hi",21,108)),static_cast<int>(bounded(r,"root",21,108)),
            static_cast<int>(bounded(r,"min_velocity",1,127)),static_cast<int>(bounded(r,"max_velocity",1,127)),r["sample"].toString(),
            bounded(r,"volume_db",-60,12),bounded(r,"velocity_track",0,100),bounded(r,"decay_db_per_second",0,30)});
}
PianoSampler::~PianoSampler()=default;
void PianoSampler::noteOn(size_t id,const PerformanceNote& note)
{
    const auto region=std::find_if(impl->regions.begin(),impl->regions.end(),[&](const auto& r) {return note.pitch>=r.lo && note.pitch<=r.hi;});
    if (region==impl->regions.end() || note.velocity<1 || note.velocity>127) throw std::runtime_error("Piano note outside range");
    const auto layer=static_cast<size_t>(std::lower_bound(impl->velocityHigh.begin(),impl->velocityHigh.end(),note.velocity)-impl->velocityHigh.begin());
    Voice voice;voice.id=id;voice.pitch=note.pitch;voice.velocity=note.velocity;voice.main=true;voice.releaseSeconds=region->release;
    voice.gain=static_cast<float>(std::pow(note.velocity/127.0,1.46));
    // The source instrument specifies note_polyphony=1 / off_time=0.5.
    // Retain a continuous, bounded fade rather than stacking this string's
    // entire recorded decay at every repeated attack under the sustain pedal.
    for (auto& previous:impl->voices)
        if (previous.main && previous.pitch==note.pitch && !previous.retriggerFade)
        {previous.retriggerFade=true;previous.retriggerFrames=0.5*impl->rate;}
    impl->add(voice,region->name+"v"+juce::String(static_cast<int>(layer+1))+".flac",region->root,region->tune,region->offset[layer]);
}
void PianoSampler::noteOff(size_t id)
{
    std::vector<Voice> released;
    for (auto& voice:impl->voices)
        if (voice.main && voice.id==id && voice.keyDown) {voice.keyDown=false;released.push_back(voice);}
    for (const auto& voice:released) impl->releaseNoises(voice,true);
}
void PianoSampler::pedal(int value)
{
    if (value<0 || value>127) throw std::runtime_error("Invalid pedal value");
    const bool wasDown=impl->pedalValue>=64,isDown=value>=64;
    impl->pedalValue=value;
    if (wasDown!=isDown)
    {
        Voice voice;voice.pitch=60;voice.gain=static_cast<float>(std::pow(10.0,(isDown ? -20 : -19)/20.0));
        const auto name=juce::String(isDown ? "pedalD" : "pedalU")+juce::String((impl->pedalAlternation++%2)+1)+".flac";
        impl->add(voice,name,60);
    }
}
void PianoSampler::render(juce::AudioBuffer<float>& out,int frames)
{
    if (out.getNumChannels()!=2 || frames<0 || frames>out.getNumSamples()) throw std::runtime_error("Invalid piano render buffer");
    out.clear();
    std::vector<Voice> noises;
    for (auto& voice:impl->voices)
    {
        const bool damp=voice.main && !voice.keyDown && (impl->pedalValue<64 || voice.pitch>=89);
        if (damp && !voice.released && !voice.retriggerFade) {noises.push_back(voice);voice.released=true;}
        // Recapture may pause damping, but must not retrigger this key's
        // string-release layers on every subsequent pedal-up transition.
    }
    for (const auto& voice:noises) impl->releaseNoises(voice,false);
    for (auto& voice:impl->voices)
    {
        const bool damp=voice.main && !voice.keyDown && (impl->pedalValue<64 || voice.pitch>=89);
        double release=voice.releaseSeconds;
        if (voice.pitch<89 && impl->pedalValue>0 && impl->pedalValue<64) release*=1+3*std::pow(impl->pedalValue/63.0,2);
        const float decay=static_cast<float>(std::exp(std::log(0.0001)/(release*impl->rate)));
        auto* left=out.getWritePointer(0);auto* right=out.getWritePointer(1);
        for (int i=0;i<frames;++i)
        {
            if (voice.cursor>=voice.sample->audio.getNumSamples()+taps/2 || voice.envelope<1.0e-7f
                || (voice.retriggerFade && voice.retriggerFrames<=0)) break;
            const auto sample=voice.kernel->at(voice.sample->audio,voice.cursor);
            // Sample-start alignment may land on a nonzero waveform value.
            // Smooth each voice boundary, never the whole polyphonic mix:
            // existing sustained notes must not dip when another key starts.
            auto smooth=[](double t) {t=std::clamp(t,0.0,1.0);return t*t*(3.0-2.0*t);};
            constexpr double boundarySeconds=0.001;
            const double attack=smooth(voice.age/boundarySeconds);
            const double remaining=(voice.sample->audio.getNumSamples()-1-voice.cursor)/voice.speed;
            const double tail=smooth(remaining/(impl->rate*boundarySeconds));
            const float gain=voice.gain*voice.envelope*static_cast<float>(attack*tail*(voice.retriggerFade ? voice.retriggerFrames/(0.5*impl->rate) : 1.0));
            left[i]+=sample[0]*gain;right[i]+=sample[1]*gain;
            voice.cursor+=voice.speed;voice.age+=1.0/impl->rate;
            if (damp) voice.envelope*=decay;
            if (voice.retriggerFade) voice.retriggerFrames-=1;
        }
    }
    std::erase_if(impl->voices,[](const auto& voice) {return voice.cursor>=voice.sample->audio.getNumSamples()+taps/2 || voice.envelope<1.0e-7f
        || (voice.retriggerFade && voice.retriggerFrames<=0);});
}
size_t PianoSampler::cacheBytes() const {return impl->bytes;}
size_t PianoSampler::peakVoices() const {return impl->peak;}
}
