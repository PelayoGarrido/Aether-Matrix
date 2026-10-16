// Listening reference using the user's existing Apple DLS synthesizer.
// No instrument samples are copied or distributed. This is NOT the v1 renderer.
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
void check(OSStatus status) {if (status != noErr) throw std::runtime_error("Apple instrument error: " + std::to_string(status));}
void little(FILE* file, uint32_t value, int count)
{
    for (int i=0; i<count; ++i) if (std::fputc(static_cast<int>((value>>(8*i))&255),file)==EOF)
        throw std::runtime_error("Audio write failed");
}
struct Instrument
{
    AudioUnit unit = nullptr;
    Instrument()
    {
        AudioComponentDescription description {kAudioUnitType_MusicDevice,kAudioUnitSubType_DLSSynth,kAudioUnitManufacturer_Apple,0,0};
        auto component=AudioComponentFindNext(nullptr,&description);
        if (!component) throw std::runtime_error("System DLS instrument unavailable");
        check(AudioComponentInstanceNew(component,&unit));
    }
    ~Instrument() {if (unit) {AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit);}}
};
struct Message {int64_t frame; uint32_t status,pitch,velocity;};
int main(int argc,char** argv)
{
    FILE* file=nullptr;
    try
    {
        if (argc!=2) throw std::runtime_error("Usage: AetherMidiAudition NEW_OUTPUT.wav < validated-performance.txt");
        constexpr double rate=44100;
        constexpr int block=512;
        int bpm,program,endTick,count;
        if (!(std::cin>>bpm>>program>>endTick>>count) || bpm<40 || bpm>300 || program<0 || program>127
            || endTick<480 || endTick>32*4*480 || count<1 || count>512)
            throw std::runtime_error("Invalid bounded performance header");
        const auto frameFor=[bpm](int tick) {return static_cast<int64_t>(std::llround(tick*60.0*rate/(bpm*480.0)));};
        std::vector<Message> messages;
        for (int i=0; i<count; ++i)
        {
            int tick,length,pitch,velocity;
            if (!(std::cin>>tick>>length>>pitch>>velocity) || tick<0 || tick>endTick || length<=0 || length>endTick-tick
                || pitch<0 || pitch>127 || velocity<1 || velocity>127)
                throw std::runtime_error("Invalid note");
            messages.push_back({frameFor(tick),0x90,static_cast<uint32_t>(pitch),static_cast<uint32_t>(velocity)});
            messages.push_back({frameFor(tick+length),0x80,static_cast<uint32_t>(pitch),0});
        }
        std::stable_sort(messages.begin(),messages.end(),[](const auto& a,const auto& b) {
            return a.frame==b.frame ? a.status<b.status : a.frame<b.frame;
        });
        const auto total=frameFor(endTick)+static_cast<int64_t>(2*rate);
        if (fs::space(fs::absolute(argv[1]).parent_path()).available < 3ULL*1024*1024*1024+static_cast<uint64_t>(total)*4)
            throw std::runtime_error("Preserve 3 GiB free before audition rendering");
        Instrument instrument;
        AudioStreamBasicDescription format {};
        format.mSampleRate=rate; format.mFormatID=kAudioFormatLinearPCM;
        format.mFormatFlags=static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked)
                           |static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
        format.mBytesPerPacket=4; format.mFramesPerPacket=1; format.mBytesPerFrame=4;
        format.mChannelsPerFrame=2; format.mBitsPerChannel=32;
        check(AudioUnitSetProperty(instrument.unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format)));
        UInt32 maximum=block;
        check(AudioUnitSetProperty(instrument.unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)));
        check(AudioUnitInitialize(instrument.unit));
        check(MusicDeviceMIDIEvent(instrument.unit,0xc0,static_cast<uint32_t>(program),0,0));
        file=std::fopen(argv[1],"wbx");
        if (!file) throw std::runtime_error("Output exists or cannot be created");
        std::fwrite("RIFF",1,4,file); little(file,static_cast<uint32_t>(36+total*4),4); std::fwrite("WAVEfmt ",1,8,file);
        little(file,16,4); little(file,1,2); little(file,2,2); little(file,44100,4); little(file,44100*4,4);
        little(file,4,2); little(file,16,2); std::fwrite("data",1,4,file); little(file,static_cast<uint32_t>(total*4),4);
        std::array<float,block> left {},right {};
        struct StereoBuffers {UInt32 count; AudioBuffer buffers[2];} buffers {2,{{1,block*4,left.data()},{1,block*4,right.data()}}};
        size_t event=0, clipped=0;
        double peak=0;
        for (int64_t frame=0; frame<total;)
        {
            while (event<messages.size() && messages[event].frame<=frame)
            {
                const auto& message=messages[event++];
                check(MusicDeviceMIDIEvent(instrument.unit,message.status,message.pitch,message.velocity,0));
            }
            int64_t frames=std::min<int64_t>(block,total-frame);
            if (event<messages.size()) frames=std::min(frames,messages[event].frame-frame);
            if (frames<=0) throw std::runtime_error("Non-advancing audio timeline");
            for (auto& buffer:buffers.buffers) buffer.mDataByteSize=static_cast<UInt32>(frames*4);
            AudioTimeStamp stamp {}; stamp.mSampleTime=static_cast<double>(frame); stamp.mFlags=kAudioTimeStampSampleTimeValid;
            AudioUnitRenderActionFlags flags=0;
            check(AudioUnitRender(instrument.unit,&flags,&stamp,0,static_cast<UInt32>(frames),reinterpret_cast<AudioBufferList*>(&buffers)));
            for (int64_t i=0;i<frames;++i)
                for (const float sample:{left[static_cast<size_t>(i)],right[static_cast<size_t>(i)]})
                {
                    if (!std::isfinite(sample)) throw std::runtime_error("Non-finite audition audio");
                    peak=std::max(peak,static_cast<double>(std::abs(sample)));
                    if (std::abs(sample)>1.0f) ++clipped;
                    const auto pcm=static_cast<int16_t>(std::lround(std::clamp(sample,-1.0f,1.0f)*32767.0f));
                    little(file,static_cast<uint16_t>(pcm),2);
                }
            frame+=frames;
        }
        if (std::fclose(file)!=0) {file=nullptr; fs::remove(argv[1]); throw std::runtime_error("Audio flush failed");}
        file=nullptr;
        std::cout<<"{\"renderer\":\"Apple system DLS, listening reference only\",\"sample_rate\":44100,\"frames\":"
            <<total<<",\"sample_peak\":"<<peak<<",\"clipped_samples\":"<<clipped<<"}\n";
    }
    catch (const std::exception& error)
    {
        if (file) {std::fclose(file); fs::remove(argv[1]);}
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
