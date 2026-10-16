// Offline model evaluation, never called from the DAW audio thread.
// V2 event protocol / KV-cache algorithm adapted from SkyTNT/midi-model,
// commit f504d5cb58f769ab0f2909c679238f6621034573 (Apache-2.0).
// See docs/licenses/MIDI-MODEL-APACHE-2.0.txt and THIRD_PARTY_NOTICES.md.
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
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

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Event = std::array<int64_t, 8>;
constexpr int vocabulary = 3406, embedding = 1024, ticksPerBeat = 480;
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }

// Parameter order is part of the trained tokenizer contract, not JSON key order.
enum Param { time1, time2, duration, track, channel, pitch, velocity, patch,
             controller, value, bpm, nn, dd, sf, mi };
constexpr std::array<int, 15> widths {128,16,2048,128,16,128,128,128,128,128,384,16,4,15,2};
constexpr int offset(Param p)
{
    int result = 9;
    for (int i = 0; i < p; ++i) result += widths[static_cast<size_t>(i)];
    return result;
}
static_assert(offset(mi) + widths[mi] == vocabulary);
int token(Param p, int v) { return offset(p) + v; }

size_t residentBytes()
{
#ifdef __APPLE__
    task_vm_info_data_t data {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&data), &count) == KERN_SUCCESS)
        return static_cast<size_t>(data.phys_footprint);
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX data {};
    data.cb=sizeof(data);
    if (GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&data),sizeof(data)))
        return std::max(static_cast<size_t>(data.WorkingSetSize),static_cast<size_t>(data.PrivateUsage));
#endif
    return 0;
}

struct Model
{
    Ort::Session session;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<std::string> inputs, outputs;
    std::vector<std::vector<int64_t>> shapes;
    std::vector<Ort::Value> previous;
    size_t principal = 0;

    Model(Ort::Env& env, const fs::path& file, const Ort::SessionOptions& options,
          const std::string& principalName, size_t cacheCount)
        : session(env, file.c_str(), options)
    {
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < session.GetInputCount(); ++i)
        {
            inputs.emplace_back(session.GetInputNameAllocated(i, allocator).get());
            auto info = session.GetInputTypeInfo(i);
            shapes.push_back(info.GetTensorTypeAndShapeInfo().GetShape());
            if (inputs.back().starts_with("past_key_values"))
            {
                const auto& s = shapes.back();
                if (s.size() != 4 || s[1] <= 0 || s[1] > 16 || s[3] <= 0 || s[3] > 256
                    || info.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                    throw std::runtime_error("Unsupported KV-cache contract");
            }
            else if (inputs.back() != "x" && inputs.back() != "hidden")
                throw std::runtime_error("Unexpected model input");
        }
        for (size_t i = 0; i < session.GetOutputCount(); ++i)
        {
            outputs.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
            if (outputs.back() == principalName) principal = i;
        }
        if (outputs.size() != cacheCount + 1 || outputs.at(principal) != principalName
            || inputs.size() != cacheCount + (principalName == "hidden" ? 1 : 2))
            throw std::runtime_error("Model does not match pinned architecture");
    }

    void clear() { previous.clear(); }

    const Ort::Value& run(Ort::RunOptions& runOptions, const std::vector<int64_t>& x,
                         const std::vector<int64_t>& xShape, std::vector<float>& hidden, bool useHidden)
    {
        std::vector<Ort::Value> tensors;
        std::vector<const char*> inputNames, outputNames;
        tensors.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& name = inputs[i];
            inputNames.push_back(name.c_str());
            if (name == "x")
                tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(x.data()),
                    x.size(), xShape.data(), xShape.size()));
            else if (name == "hidden")
            {
                const std::array<int64_t, 3> shape {1, useHidden ? 1 : 0, embedding};
                tensors.push_back(Ort::Value::CreateTensor<float>(memory, hidden.data(),
                    useHidden ? hidden.size() : 0, shape.data(), shape.size()));
            }
            else if (previous.empty())
            {
                auto shape = shapes[i]; shape[0] = 1; shape[2] = 0;
                Ort::AllocatorWithDefaultOptions allocator;
                tensors.push_back(Ort::Value::CreateTensor<float>(allocator, shape.data(), shape.size()));
            }
            else
            {
                const auto presentName = "present" + name.substr(std::string("past_key_values").size());
                const auto found = std::find(outputs.begin(), outputs.end(), presentName);
                if (found == outputs.end()) throw std::runtime_error("Missing present KV tensor");
                tensors.push_back(std::move(previous[static_cast<size_t>(found - outputs.begin())]));
            }
        }
        for (const auto& name : outputs) outputNames.push_back(name.c_str());
        previous = session.Run(runOptions, inputNames.data(), tensors.data(), tensors.size(),
                               outputNames.data(), outputNames.size());
        return previous.at(principal);
    }
};

int sample(const Ort::Value& output, int low, int high, std::mt19937& random)
{
    const auto info = output.GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
        || info.GetShape() != std::vector<int64_t>{1,1,vocabulary} || low < 0 || high >= vocabulary || low > high)
        throw std::runtime_error("Invalid logit tensor or token mask");
    const auto* logits = output.GetTensorData<float>();
    std::vector<int> ids(static_cast<size_t>(high - low + 1));
    std::iota(ids.begin(), ids.end(), low);
    for (auto id : ids)
        if (!std::isfinite(logits[id])) throw std::runtime_error("Model returned non-finite logits");
    std::sort(ids.begin(), ids.end(), [logits](int a, int b) {return logits[a] > logits[b];});
    std::vector<double> probabilities;
    double total = 0;
    for (auto id : ids) {probabilities.push_back(std::exp(static_cast<double>(logits[id] - logits[ids[0]]))); total += probabilities.back();}
    double cumulative = 0;
    size_t keep = 0;
    // Normalize the allowed-token distribution before top-p, then apply top-k.
    while (keep < ids.size() && keep < 20 && (cumulative < 0.98 * total || keep == 0))
        cumulative += probabilities[keep++];
    probabilities.resize(keep);
    std::discrete_distribution<size_t> distribution(probabilities.begin(), probabilities.end());
    return ids[distribution(random)];
}

struct Note {int start, length, pitch, velocity;};
void variable(std::vector<uint8_t>& out, int value)
{
    if (value < 0 || value > 0x0fffffff) throw std::runtime_error("Invalid MIDI delta");
    uint8_t buffer[4]; int count = 0;
    buffer[count++] = static_cast<uint8_t>(value & 127);
    while ((value >>= 7) != 0) buffer[count++] = static_cast<uint8_t>((value & 127) | 128);
    while (count > 0) out.push_back(buffer[--count]);
}
void bigEndian(std::vector<uint8_t>& out, size_t value, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
std::vector<uint8_t> midi(const std::vector<Note>& notes, int tempo, int program, int key, int minor, int endTick)
{
    struct Message {int tick, order; std::vector<uint8_t> data;};
    const int micros = 60000000 / tempo;
    std::vector<Message> messages {
        {0,0,{0xff,0x51,3,static_cast<uint8_t>(micros >> 16),static_cast<uint8_t>(micros >> 8),static_cast<uint8_t>(micros)}},
        {0,0,{0xff,0x58,4,4,2,24,8}},
        {0,0,{0xff,0x59,2,static_cast<uint8_t>(key),static_cast<uint8_t>(minor)}},
        {0,1,{0xc0,static_cast<uint8_t>(program)}}
    };
    for (const auto& n : notes)
    {
        messages.push_back({n.start,3,{0x90,static_cast<uint8_t>(n.pitch),static_cast<uint8_t>(n.velocity)}});
        messages.push_back({n.start + n.length,2,{0x80,static_cast<uint8_t>(n.pitch),0}});
    }
    std::stable_sort(messages.begin(), messages.end(), [](const auto& a, const auto& b) {
        return a.tick == b.tick ? a.order < b.order : a.tick < b.tick;
    });
    std::vector<uint8_t> body;
    int last = 0;
    for (const auto& m : messages)
    {
        variable(body,m.tick - last); body.insert(body.end(),m.data.begin(),m.data.end()); last = m.tick;
    }
    variable(body,std::max(last,endTick)-last); body.insert(body.end(),{0xff,0x2f,0});
    std::vector<uint8_t> out {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xe0,'M','T','r','k'};
    bigEndian(out,body.size(),4); out.insert(out.end(),body.begin(),body.end()); return out;
}

void writeExclusive(const fs::path& path, const void* data, size_t size)
{
    // C11 exclusive creation prevents overwriting a user's existing MIDI.
#ifdef _WIN32
    auto* file = _wfopen(path.c_str(),L"wbx");
#else
    auto* file = std::fopen(path.c_str(),"wbx");
#endif
    if (!file) throw std::runtime_error("Cannot create output; choose a new output prefix");
    const bool written = std::fwrite(data,1,size,file) == size;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {fs::remove(path); throw std::runtime_error("Output write failed");}
}

int integer(const char* text, int low, int high)
{
    size_t used = 0; const auto s = std::string(text); const int n = std::stoi(s,&used);
    if (used != s.size() || n < low || n > high) throw std::runtime_error("Argument outside supported range");
    return n;
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 3 || argc > 10)
        {
            std::cerr << "Usage: AetherMidiEvaluate MODEL_DIR OUTPUT_PREFIX [events 8..512] [seed 0..2147483647] [bpm 40..300] [bars 1..32] [GM_program 0..127] [sharps -7..7] [minor 0..1]\n";
            return 2;
        }
        const fs::path root = fs::absolute(argv[1]), prefix = fs::absolute(argv[2]);
        const int count = argc > 3 ? integer(argv[3],8,512) : 64;
        const int seed = argc > 4 ? integer(argv[4],0,2147483647) : 42;
        const int tempo = argc > 5 ? integer(argv[5],40,300) : 120;
        const int bars = argc > 6 ? integer(argv[6],1,32) : 8;
        const int program = argc > 7 ? integer(argv[7],0,127) : 0;
        const int key = argc > 8 ? integer(argv[8],-7,7) : 0;
        const int minor = argc > 9 ? integer(argv[9],0,1) : 0;
        for (const auto& [name,size] : std::array<std::pair<const char*,uintmax_t>,2>{{{"model_base.onnx",821713887},{"model_token.onnx",116661381}}})
            if (!fs::is_regular_file(root/name) || fs::file_size(root/name) != size)
                throw std::runtime_error("Run download_candidate.py to obtain the pinned verified model pair");
        if (fs::exists(prefix.string()+".mid") || fs::exists(prefix.string()+".json"))
            throw std::runtime_error("Output already exists");
        if (fs::space(prefix.parent_path()).available < 3ULL*1024*1024*1024)
            throw std::runtime_error("Preserve 3 GiB free before model evaluation");
        std::signal(SIGINT,onSignal); std::signal(SIGTERM,onSignal);
#ifdef __APPLE__
        // An offline generation job must yield CPU time to the DAW.
        if (setpriority(PRIO_PROCESS,0,10)!=0 || pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0)!=0)
            std::cerr << "Could not lower evaluator scheduling priority\n";
#elif defined(_WIN32)
        if (!SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS))
            std::cerr << "Could not lower evaluator scheduling priority\n";
#endif
        const auto began = Clock::now();
        if (std::string(OrtGetApiBase()->GetVersionString()) != "1.31.0")
            throw std::runtime_error("Evaluation requires verified ONNX Runtime 1.31.0");
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR,"AetherMidiEvaluation");
        env.DisableTelemetryEvents();
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2); options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
        // Weight prepacking duplicates a significant portion of this float32 model.
        options.AddConfigEntry("session.disable_prepacking","1");
        Model base(env,root/"model_base.onnx",options,"hidden",24);
        Model decoder(env,root/"model_token.onnx",options,"y",6);
        const double loadSeconds = std::chrono::duration<double>(Clock::now()-began).count();
        std::cerr << "Models loaded in " << loadSeconds << " s\n";
        Ort::RunOptions runOptions;
        std::atomic<bool> done{false}, aborted{false};
        std::atomic<size_t> peak{residentBytes()};
        std::jthread watchdog([&](std::stop_token stop) {
            while (!stop.stop_requested() && !done.load())
            {
                const auto memory = residentBytes(); peak.store(std::max(memory,peak.load()));
                if (interrupted || memory > 4ULL*1024*1024*1024 || Clock::now()-began > std::chrono::seconds(120))
                {aborted.store(true); runOptions.SetTerminate(); return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::vector<Event> events {{1,0,0,0,0,0,0,0},
            {7,token(time1,0),token(time2,0),token(track,0),token(nn,3),token(dd,1),0,0},
            {8,token(time1,0),token(time2,0),token(track,0),token(sf,key+7),token(mi,minor),0,0},
            {6,token(time1,0),token(time2,0),token(track,0),token(bpm,tempo),0,0,0},
            {4,token(time1,0),token(time2,0),token(track,1),token(channel,0),token(patch,program),0,0}};
        std::vector<int64_t> x;
        for (const auto& event : events) x.insert(x.end(),event.begin(),event.end());
        std::vector<float> hidden(embedding);
        std::mt19937 random(static_cast<uint32_t>(seed));
        std::vector<Note> notes;
        int coarse = 0;
        std::string stopReason = "event_limit";
        const int totalUnits = bars*4*16;
        const auto generationStart = Clock::now();
        for (int eventIndex = 0; eventIndex < count; ++eventIndex)
        {
            if (aborted.load()) throw std::runtime_error("Generation cancelled by resource or time limit");
            const auto& output = base.run(runOptions,x,{1,static_cast<int64_t>(x.size()/8),8},hidden,false);
            const auto info = output.GetTensorTypeAndShapeInfo(); const auto shape = info.GetShape();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
                || shape.size()!=3 || shape[0]!=1 || shape[1]<1 || shape[2]!=embedding)
                throw std::runtime_error("Invalid base hidden tensor");
            const auto* last = output.GetTensorData<float>() + (shape[1]-1)*embedding;
            std::copy(last,last+embedding,hidden.begin());
            decoder.clear();
            Event event {}; std::vector<int64_t> tokenInput;
            for (int i=0; i<8; ++i)
            {
                const auto& logits = decoder.run(runOptions,tokenInput,{1,static_cast<int64_t>(tokenInput.size())},hidden,i==0);
                int low = 3, high = 3;
                if (i==0) low = notes.size() < 8 ? 3 : 2; // note or EOS, no uncontrolled metadata changes
                else
                {
                    constexpr std::array<Param,7> params {time1,time2,track,channel,pitch,velocity,duration};
                    const auto p = params[static_cast<size_t>(i-1)];
                    low=offset(p); high=low+widths[p]-1;
                    if (p==track) low=high=token(track,1);
                    if (p==channel) low=high=token(channel,0);
                    if (p==pitch) {low=token(pitch,21); high=token(pitch,108);}
                    if (p==velocity || p==duration) ++low;
                }
                event[static_cast<size_t>(i)] = sample(logits,low,high,random);
                tokenInput = {event[static_cast<size_t>(i)]};
                if (i==0 && event[0]==2) break;
            }
            events.push_back(event);
            if (event[0]==2) {stopReason="eos"; break;}
            coarse+=static_cast<int>(event[1])-offset(time1);
            const int startUnits=coarse*16+static_cast<int>(event[2])-offset(time2);
            // Stop at the requested boundary. Masking the time distribution to
            // the remaining bars traps the model at the final onset and stacks
            // spurious notes there. Preserve natural time sampling instead.
            if (startUnits>=totalUnits) {stopReason="timeline_limit"; break;}
            notes.push_back({startUnits*30,std::min(static_cast<int>(event[7])-offset(duration),totalUnits-startUnits)*30,
                static_cast<int>(event[5])-offset(pitch),static_cast<int>(event[6])-offset(velocity)});
            x.assign(event.begin(),event.end());
            if (eventIndex%16==15) std::cerr << notes.size() << " notes, " << peak.load()/1024/1024 << " MiB footprint\n";
        }
        done.store(true); watchdog.request_stop(); watchdog.join();
        if (aborted.load() || notes.empty()) throw std::runtime_error("No complete usable performance generated");
        // MIDI 1.0 cannot distinguish overlapping voices of the same pitch/channel.
        // Trim those intersections in the shared performance before MIDI/rendering.
        std::stable_sort(notes.begin(),notes.end(),[](const auto& a,const auto& b) {return a.start<b.start;});
        std::map<int,int> next;
        for (auto it=notes.rbegin(); it!=notes.rend(); ++it)
        {
            if (next.contains(it->pitch)) it->length=std::min(it->length,next[it->pitch]-it->start);
            next[it->pitch]=it->start;
        }
        std::erase_if(notes,[](const auto& n) {return n.length<=0;});
        const double seconds=std::chrono::duration<double>(Clock::now()-generationStart).count();
        std::ostringstream report;
        report << "{\n  \"schema\":1,\n  \"candidate\":\"skytnt/midi-model-tv2o-medium\",\n"
            << "  \"revision\":\"0f8f265d4330f4e46527ac2313200254c5757f5f\",\n"
            << "  \"backend\":\"ONNX Runtime CPU, 2 intra-op threads\",\n"
            << "  \"seed\":" << seed << ",\n  \"bpm\":" << tempo << ",\n  \"bars\":" << bars
            << ",\n  \"gm_program\":" << program << ",\n  \"sharps\":" << key << ",\n  \"minor\":" << minor
            << ",\n  \"key_enforcement\":\"conditioning_only\",\n  \"load_seconds\":" << loadSeconds
            << ",\n  \"generation_seconds\":" << seconds << ",\n  \"sampled_peak_footprint_bytes\":" << peak.load()
            << ",\n  \"stop_reason\":\"" << stopReason << "\",\n  \"ticks_per_beat\":480,\n  \"notes\":[";
        for (size_t i=0; i<notes.size(); ++i)
        {
            const auto& n=notes[i]; if (i) report << ',';
            report << "\n    {\"tick\":" << n.start << ",\"duration\":" << n.length
                << ",\"pitch\":" << n.pitch << ",\"velocity\":" << n.velocity << '}';
        }
        report << "\n  ],\n  \"raw_events\":[";
        for (size_t i=0; i<events.size(); ++i)
        {
            if (i) report << ','; report << '[';
            for (size_t j=0; j<8; ++j) {if (j) report << ','; report << events[i][j];} report << ']';
        }
        report << "]\n}\n";
        const auto bytes=midi(notes,tempo,program,key,minor,bars*4*ticksPerBeat);
        const auto json=report.str();
        writeExclusive(prefix.string()+".mid",bytes.data(),bytes.size());
        writeExclusive(prefix.string()+".json",json.data(),json.size());
        std::cout << notes.size() << " notes in " << seconds << " s; peak sampled footprint " << peak.load()/1024/1024 << " MiB\n";
    }
    catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
    return 0;
}
