#include "../../Source/NativeEntry.h"
#include "PianistEngine.h"
#include <juce_cryptography/juce_cryptography.h>
#include "NativeJobSupport.h"
#include <iostream>
namespace
{
using namespace aether::job;
namespace fs=std::filesystem;
std::vector<int64_t> tokens(const juce::var& object,const char* key)
{
    const auto* values=object[juce::Identifier(key)].getArray();
    if (values==nullptr || values->size()<16 || values->size()>4096 || values->size()%8)
        throw std::runtime_error("Expected 2..512 complete token groups");
    std::vector<int64_t> result;
    for (const auto& v:*values)
    {
        if ((!v.isInt() && !v.isInt64()) || static_cast<juce::int64>(v)<0 || static_cast<juce::int64>(v)>=5389)
            throw std::runtime_error("Invalid token integer");
        result.push_back(static_cast<juce::int64>(v));
    }
    return result;
}
void writeFloats(const juce::File& target,const float* data,size_t count)
{
    if (count>512*768) throw std::runtime_error("Parity output exceeds buffer budget");
    juce::FileOutputStream stream(target);
    if (!stream.openedOk()) throw std::runtime_error("Cannot write parity tensor");
    for (size_t i=0;i<count;++i) stream.writeFloat(data[i]); // Explicit little-endian.
    stream.flush();if (stream.getStatus().failed()) throw std::runtime_error("Parity tensor flush failed");
}
}
static int runNative(int argc,char** argv)
{
    try
    {
        if (argc!=4) throw std::runtime_error("Usage: AetherPianistEvaluate VERIFIED_ASSETS TOKENS_JSON NEW_PARITY_JOB");
        const auto root=file(argv[1]),input=file(argv[2]),output=file(argv[3]);
        juce::FileInputStream stream(input);juce::MemoryBlock bytes;
        if (!stream.openedOk()) throw std::runtime_error("Cannot read token fixture");
        stream.readIntoMemoryBlock(bytes,1024*1024+1);
        if (bytes.getSize()>1024*1024) throw std::runtime_error("Token fixture exceeds 1 MiB");
        const auto fixture=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),static_cast<int>(bytes.getSize())));
        const auto score=tokens(fixture,"input"),teacher=tokens(fixture,"output");
        if (score.size()!=teacher.size()) throw std::runtime_error("Teacher/score token counts differ");
        if (fs::space(native(output.getParentDirectory())).available<3ULL*1024*1024*1024+4*1024*1024)
            throw std::runtime_error("Preserve 3 GiB free plus bounded parity outputs");
        if (!fs::create_directory(native(output))) throw std::runtime_error("Choose a new parity job directory");
        fs::permissions(native(output),fs::perms::owner_all,fs::perm_options::replace);
        initialise();Budget budget{output};
        juce::InterProcessLock lock("AetherMatrix-Pianist-native-v1");
        if (!lock.enter(0)) throw std::runtime_error("Another native piano interpretation is running");
        const auto start=Clock::now();
        std::unique_ptr<aether::PianistEngine> model;
        std::atomic<aether::PianistEngine*> target{nullptr};
        std::jthread watchdog([&](std::stop_token stop){
            while (!stop.stop_requested())
            {
                if (!budget.observe())
                    if (auto* engine=target.load(std::memory_order_acquire)) {engine->cancel();return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        model=std::make_unique<aether::PianistEngine>(root);target.store(model.get(),std::memory_order_release);
        auto& engine=*model;const auto loaded=Clock::now();
        auto check=[&] {budget.check();};
        check();engine.encode(score);check();
        const auto encoded=engine.encoderState();writeFloats(output.getChildFile("encoder.f32"),encoded.data(),encoded.size());
        juce::Array<juce::var> snapshots;
        int64_t previous=2;
        for (size_t i=0;i<score.size();++i)
        {
            check();const auto logits=engine.decode(previous);
            if (i==0 || i==1 || i==7 || i==15 || i==63 || i==255 || i==511 || i+1==score.size())
            {
                writeFloats(output.getChildFile("logits-"+juce::String(static_cast<int>(i))+".f32"),logits.data(),logits.size());
                snapshots.add(static_cast<int>(i));
            }
            previous=teacher[i];
        }
        check();juce::var report(new juce::DynamicObject);auto* o=report.getDynamicObject();
        o->setProperty("schema",1);o->setProperty("completed",true);o->setProperty("backend","ONNX Runtime 1.31.0 CPU / FP32 / 2 threads");
        o->setProperty("load_seconds",std::chrono::duration<double>(loaded-start).count());
        o->setProperty("teacher_forcing_seconds",std::chrono::duration<double>(Clock::now()-loaded).count());
        o->setProperty("sampled_peak_footprint_bytes",static_cast<juce::int64>(budget.peak.load(std::memory_order_relaxed)));o->setProperty("snapshots",snapshots);
        o->setProperty("tokens",static_cast<int>(score.size()));o->setProperty("source_fixture_sha256",juce::SHA256(bytes.getData(),bytes.getSize()).toHexString());
        if (!output.getChildFile("native.json").replaceWithText(juce::JSON::toString(report,true))) throw std::runtime_error("Could not complete parity job");
        std::cout<<juce::JSON::toString(report,true).toStdString()<<'\n';
    }
    catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
    return 0;
}

AETHER_NATIVE_ENTRY(runNative)
