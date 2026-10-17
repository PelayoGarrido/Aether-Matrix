#include "PianoStudio.h"
#if defined(AETHER_PIANO_RENDER)
#include "PianoFixture.h"
#endif
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#if JUCE_MAC
#include <fcntl.h>
#include <unistd.h>
#endif
namespace
{
thread_local bool counting=false;thread_local size_t allocations=0,deletions=0;
void require(bool ok,const char* message) {if (!ok) throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message) {bool rejected=false;try {f();} catch (const std::exception&) {rejected=true;}require(rejected,message);}
struct Root
{
    juce::File file=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("aether-studio-tests-"+juce::Uuid().toString());
    Root(){require(file.createDirectory().wasOk(),"Test directory failed");}~Root(){file.deleteRecursively();}
};
void writeMidi(const juce::File& file,juce::MidiMessageSequence sequence)
{
    juce::MidiFile midi;midi.setTicksPerQuarterNote(480);midi.addTrack(sequence);juce::FileOutputStream stream(file);require(midi.writeTo(stream,0),"Test MIDI write failed");
}
void midiTests(const juce::File& root)
{
    aether::Performance original;original.bpm=137;original.bars=2;original.sharps=-3;original.minor=1;
    original.notes={{60,43,0,240,12345,-3456},{64,101,480,360,-2222,1111},{60,75,960,240,0,0}};
    original.pedals={{600,1000,101},{1200,-1000,0}};
    const auto file=root.getChildFile("edited.mid");{juce::FileOutputStream stream(file);require(original.midi().writeTo(stream,0),"Expressive MIDI fixture failed");}
    const auto imported=aether::PianoStudio::readMidi(file,{});
    require(imported.bpm==137 && imported.bars==2 && imported.sharps==-3 && imported.minor==1,"MIDI tempo/key/bars lost");
    require(imported.notes.size()==original.notes.size(),"Editable MIDI note count changed");
    for (size_t i=0;i<original.notes.size();++i)
    {
        const auto& a=original.notes[i];const auto& b=imported.notes[i];
        require(a.pitch==b.pitch && a.velocity==b.velocity,"Edited pitch/velocity lost");
        require(std::abs(original.microsAt(a.tick,a.onsetOffsetUs)-imported.microsAt(b.tick))<50,"Edited onset lost");
        require(std::abs(original.microsAt(a.tick+a.duration,a.releaseOffsetUs)-imported.microsAt(b.tick+b.duration))<50,"Edited release lost");
    }
    require(imported.pedals.size()==original.pedals.size(),"MIDI roundtrip accumulated end-of-take pedal resets");
    aether::Performance shortest;shortest.bpm=40;shortest.bars=1;shortest.ppq=15360;
    shortest.notes={{60,80,0,1,0,1000-shortest.microsAt(1)}};
    const auto tiny=root.getChildFile("short.mid");
    for (int pass=0;pass<3;++pass)
    {
        tiny.deleteFile();{juce::FileOutputStream stream(tiny);require(shortest.midi().writeTo(stream,0),"Short MIDI export failed");}
        shortest=aether::PianoStudio::readMidi(tiny,{});
        require(shortest.microsAt(shortest.notes[0].duration)>0,"Short MIDI roundtrip collapsed the note");
    }
    aether::Performance longTake;longTake.bpm=299;longTake.bars=32;longTake.notes={{60,80,31*4*480,480,0,0}};
    const auto longFile=root.getChildFile("long.mid");juce::MemoryBlock first;
    for (int pass=0;pass<3;++pass)
    {
        longFile.deleteFile();{juce::FileOutputStream stream(longFile);require(longTake.midi().writeTo(stream,0),"32-bar MIDI export failed");}
        juce::MemoryBlock current;require(longFile.loadFileAsData(current),"Cannot inspect long MIDI");
        if (pass==0) first=current;else require(first==current,"MIDI tempo rounding accumulated across edit/render cycles");
        longTake=aether::PianoStudio::readMidi(longFile,{});require(longTake.bars==32,"Own MIDI tempo precision pushed a take beyond 32 bars");
    }
    auto maximum=original;maximum.pedals.clear();
    for (int i=0;i<2048;++i) maximum.pedals.push_back({i,0,i%128});
    const auto maxFile=root.getChildFile("maximum-pedal.mid");{juce::FileOutputStream stream(maxFile);require(maximum.midi().writeTo(stream,0),"Maximum sustain export failed");}
    require(aether::PianoStudio::readMidi(maxFile,{}).pedals.size()==2048,"Own maximum sustain output could not be reimported");
    const auto bad=root.getChildFile("bad.mid");
    juce::MidiMessageSequence sequence;sequence.addEvent(juce::MidiMessage::tempoMetaEvent(500000),0);
    sequence.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(80)),0);sequence.addEvent(juce::MidiMessage::noteOff(1,60),480);
    sequence.addEvent(juce::MidiMessage::tempoMetaEvent(600000),240);writeMidi(bad,sequence);
    rejects([&]{aether::PianoStudio::readMidi(bad,{});},"Tempo changes silently discarded");
    bad.deleteFile();sequence.clear();sequence.addEvent(juce::MidiMessage::noteOn(1,60,static_cast<juce::uint8>(80)),0);writeMidi(bad,sequence);
    rejects([&]{aether::PianoStudio::readMidi(bad,{});},"Hanging MIDI note accepted");
    bad.deleteFile();sequence.addEvent(juce::MidiMessage::noteOff(1,60),480);sequence.addEvent(juce::MidiMessage::pitchWheel(1,9000),120);writeMidi(bad,sequence);
    rejects([&]{aether::PianoStudio::readMidi(bad,{});},"Unsupported pitch automation discarded");
    // Malformed metadata must fail before JUCE's tempo/metre accessors read it.
    for (const std::vector<uint8_t>& track: {std::vector<uint8_t>{0,255,81,0,0,255,47,0},std::vector<uint8_t>{0,255,88,4,4,255,24,8,0,255,47,0}})
    {
        std::vector<uint8_t> bytes{'M','T','h','d',0,0,0,6,0,0,0,1,1,224,'M','T','r','k',0,0,0,static_cast<uint8_t>(track.size())};bytes.insert(bytes.end(),track.begin(),track.end());
        require(bad.replaceWithData(bytes.data(),bytes.size()),"Malformed fixture failed");rejects([&]{aether::PianoStudio::readMidi(bad,{});},"Malformed MIDI metadata reached framework accessors");
    }
}
juce::File makeAudio(const juce::File& root,const char* name,float frequency)
{
    const auto file=root.getChildFile(name);juce::AudioBuffer<float> samples(2,48000);
    for (int i=0;i<48000;++i) {const float value=.2f*std::sin(static_cast<float>(juce::MathConstants<double>::twoPi*frequency*i/48000));samples.setSample(0,i,value);samples.setSample(1,i,-value);}
    juce::WavAudioFormat format;std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer && writer->writeFromAudioSampleBuffer(samples,0,48000),"Preview fixture write failed");return file;
}
void previewTests(const juce::File& root)
{
    const auto audio=makeAudio(root,"preview.wav",440),other=makeAudio(root,"other.wav",880);
    aether::AudioPreview whole,chunked;whole.prepare(44100);chunked.prepare(44100);whole.load(audio);chunked.load(audio);whole.play();chunked.play();
    juce::AudioBuffer<float> a(2,2048),b(2,64),joined(2,2048);whole.render(a);
    for (int block=0;block<32;++block) {chunked.render(b);for(int c=0;c<2;++c)joined.copyFrom(c,block*64,b,c,0,64);}
    for (int c=0;c<2;++c) for (int i=0;i<2048;++i) require(std::abs(a.getSample(c,i)-joined.getSample(c,i))<1e-7,"Preview changes with host block size");
    allocations=deletions=0;counting=true;whole.render(a);counting=false;
    require(!allocations && !deletions,"Preview callback allocates or destroys a clip");
    require(a.getMagnitude(0,2048)>.18f,"Resampled preview is silent or incorrectly scaled");
    whole.load(other);whole.play(); // New take while the previous clip is playing.
    for (int block=0;block<3;++block) {allocations=deletions=0;counting=true;whole.render(a);counting=false;require(!allocations && !deletions,"Clip replacement freed memory in callback");}
    require(whole.isPlaying() && whole.seconds()<.2,"Replacing a playing take restarted the stale clip");
    whole.collect();whole.stop();whole.render(a);whole.render(a);require(!whole.isPlaying(),"Preview stop fade did not finish");
    for (double rate:{22050.0,48000.0,96000.0,192000.0})
    {
        whole.prepare(rate);whole.play();size_t rendered=0;
        while (whole.render(a)) {rendered+=2048;require(rendered<static_cast<size_t>(rate+4096),"Preview duration changed with host rate");}
        require(std::abs(whole.seconds()-1)<.001,"Preview source time lost at host rate");
    }
}
void processTests(const juce::File& root)
{
    const juce::File helper(AETHER_HELPER_PROBE);std::atomic<bool> cancel{false};
    const auto log=root.getChildFile("args.log");const auto unicode=juce::String::fromUTF8("ñ测试");
    aether::NativeProcess::run(helper,{"one space","literal; $(touch never)",unicode},log,3,cancel);
    require(log.loadFileAsString().replace("\r\n","\n")=="one space\nliteral; $(touch never)\n"+unicode+"\n","Helper arguments passed through a shell or changed");
    rejects([&]{aether::NativeProcess::run(helper,{},log,3,cancel);},"Existing process log overwritten");
    rejects([&]{aether::NativeProcess::run(helper,{"fail"},root.getChildFile("failed.log"),3,cancel);},"Nonzero helper exit accepted");
#if JUCE_MAC
    const int fd=open(root.getChildFile("private-token.txt").getFullPathName().toRawUTF8(),O_CREAT|O_RDWR,0600);require(fd>=0,"FD fixture failed");
    try {aether::NativeProcess::run(helper,{"fd",juce::String(fd)},root.getChildFile("fd.log"),3,cancel);} catch (...) {close(fd);throw;}close(fd);
#endif
    const auto sleepLog=root.getChildFile("sleep.log");bool rejected=false;
    std::thread worker([&]{try {aether::NativeProcess::run(helper,{"sleep"},sleepLog,30,cancel);} catch (...) {rejected=true;}});
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (sleepLog.getSize()<5 && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto start=std::chrono::steady_clock::now();cancel.store(true);worker.join();
    require(rejected && std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"Unresponsive helper was not cancelled and reaped promptly");
}
#if defined(AETHER_PIANO_RENDER)
void controllerTests(const juce::File& root)
{
    aether_test::Fixture bank;
    const auto helpers=root.getChildFile("bundle/Helpers"),metadata=root.getChildFile("bundle/Resources/piano"),resources=root.getChildFile("resources");
    require(helpers.createDirectory().wasOk() && metadata.createDirectory().wasOk(),"Controller fixture bundle failed");
#if JUCE_WINDOWS
    const auto executable=helpers.getChildFile("AetherPianoRender.exe");
#else
    const auto executable=helpers.getChildFile("AetherPianoRender");
#endif
    require(juce::File(AETHER_PIANO_RENDER).copyFileTo(executable),"Cannot copy own renderer test helper");
    require(bank.manifest.copyFileTo(metadata.getChildFile("salamander-manifest.json")) && bank.layout.copyFileTo(metadata.getChildFile("salamander-layout.json")),"Cannot copy trusted fixture mapping");
    require(bank.root.getChildFile("Samples").copyDirectoryTo(resources.getChildFile("salamander-v3/Samples")),"Cannot copy synthetic test samples");
    aether::Performance score;score.bars=1;score.notes={{60,64,0,240,0,0}};
    const auto source=root.getChildFile("controller.mid");{juce::FileOutputStream file(source);require(score.midi().writeTo(file,0),"Controller MIDI fixture failed");}
    aether::AudioPreview preview;preview.prepare(48000);aether::PianoStudio studio(preview,root.getChildFile("takes"),helpers);
    auto wait=[&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while (studio.status().busy && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(!studio.status().busy,"Controller failed to finish its bounded test job");return studio.status();
    };
    require(studio.importMidi(resources,source,{}),"Controller rejected valid edited MIDI");const auto ready=wait();
    require(ready.performance && ready.wav.existsAsFile() && ready.midi.existsAsFile() && ready.job.getChildFile("take.json").existsAsFile(),"Controller did not commit a verified take");
    require(ready.performance->notes.size()==1 && ready.performance->notes[0].velocity==64,"Controller changed edited MIDI");
    preview.play();juce::AudioBuffer<float> audio(2,512);require(preview.render(audio) && audio.getMagnitude(0,512)>.01f,"Committed controller take cannot be auditioned");
    require(studio.importMidi(resources,source,{}),"Controller did not accept a second job");studio.cancel();const auto cancelled=wait();
    require(cancelled.wav==ready.wav && cancelled.midi==ready.midi && cancelled.performance==ready.performance,"Cancellation replaced the successful take");
    const auto invalid=root.getChildFile("invalid-controller.mid");require(invalid.replaceWithText("invalid MIDI"),"Invalid fixture failed");
    require(studio.importMidi(resources,invalid,{}),"Could not submit malformed MIDI for boundary validation");const auto failure=wait();
    require(failure.wav==ready.wav && failure.midi==ready.midi && failure.job.getChildFile("failure.json").existsAsFile(),"Failed import lost the prior take or its diagnostics");
}
#endif

}
void* operator new(std::size_t size) {if (counting) ++allocations;if (auto* p=std::malloc(std::max(size,size_t{1})))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {if (counting && p)++deletions;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;Root root;
    try {midiTests(root.file);previewTests(root.file);processTests(root.file);
#if defined(AETHER_PIANO_RENDER)
        controllerTests(root.file);
#endif
        std::cout<<"Edited MIDI, preview resampling/handoff and isolated process recovery passed\n";}
    catch (const std::exception& e) {counting=false;std::cerr<<e.what()<<'\n';return 1;}return 0;
}
