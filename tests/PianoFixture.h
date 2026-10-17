#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <stdexcept>
namespace aether_test
{
struct Fixture
{
    static void require(bool ok,const char* message) {if (!ok) throw std::runtime_error(message);}
    juce::File root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("aether-piano-tests","",false);
    juce::File manifest,layout;
    explicit Fixture(bool abrupt=false)
    {
        require(root.getChildFile("Samples").createDirectory().wasOk(),"Fixture directory failed");
        manifest=root.getChildFile("manifest.json");layout=root.getChildFile("layout.json");
        auto map=juce::JSON::parse(juce::File(AETHER_PIANO_LAYOUT).loadFileAsString());
        for (auto& region:*map["regions"].getArray())
        {region.getDynamicObject()->setProperty("tune_cents",0);region.getDynamicObject()->setProperty("release_seconds",0.1);juce::Array<juce::var> zeros;for(int i=0;i<16;++i) zeros.add(0);region.getDynamicObject()->setProperty("offset_frames",zeros);}
        require(layout.replaceWithText(juce::JSON::toString(map)),"Fixture layout failed");
        juce::Array<juce::var> assets;
        for (const char* name:{"C4v8.flac","C4v16.flac","rel40.flac","harmLC4.flac","harmV3C4.flac","pedalD1.flac","pedalD2.flac","pedalU1.flac","pedalU2.flac"})
        {
            const bool main=juce::String(name).startsWith("C4v");
            juce::AudioBuffer<float> samples(2,96000);samples.clear();
            if (main) for (int i=0;i<samples.getNumSamples();++i)
            {const float v=abrupt ? 0.2f : static_cast<float>(0.2*std::sin(i*2*juce::MathConstants<double>::pi*440/48000)*std::min(1.0,i/64.0)*std::min(1.0,(96000-i)/512.0));samples.setSample(0,i,v);samples.setSample(1,i,v*0.9f);}
            const auto file=root.getChildFile("Samples").getChildFile(name);
            std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();juce::FlacAudioFormat format;
            auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
            require(writer!=nullptr && writer->writeFromAudioSampleBuffer(samples,0,samples.getNumSamples()),"Fixture FLAC write failed");writer.reset();
            juce::var entry(new juce::DynamicObject);entry.getDynamicObject()->setProperty("path","Samples/"+juce::String(name));
            entry.getDynamicObject()->setProperty("bytes",file.getSize());juce::FileInputStream data(file);
            entry.getDynamicObject()->setProperty("sha256",juce::SHA256(data).toHexString());assets.add(entry);
        }
        juce::var inventory(new juce::DynamicObject);inventory.getDynamicObject()->setProperty("revision","3382bf9496bba2486f5ab0de55a264d1dfc38404");inventory.getDynamicObject()->setProperty("files",assets);
        require(manifest.replaceWithText(juce::JSON::toString(inventory)),"Fixture manifest failed");
    }
    ~Fixture(){root.deleteRecursively();} // Only this test's uniquely-created fixture.
};
}
