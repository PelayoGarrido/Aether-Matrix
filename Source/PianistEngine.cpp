#include "PianistEngine.h"
#include "OnnxEngine.h"
#include "DefaultPianistModel.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

#if AETHER_ENABLE_ONNX
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
namespace
{
juce::MemoryBlock verified(const juce::File& root,const char* name,size_t size,const char* sha)
{
    const auto file=root.getChildFile(name);
    if (root.isSymbolicLink() || file.isSymbolicLink() || !file.existsAsFile() || file.getSize()!=static_cast<juce::int64>(size))
        throw std::runtime_error("Pinned Pianist asset missing or changed");
    juce::FileInputStream stream(file);juce::MemoryBlock bytes;
    if (!stream.openedOk()) throw std::runtime_error("Cannot read Pianist asset");
    stream.readIntoMemoryBlock(bytes,static_cast<juce::int64>(size+1));
    if (bytes.getSize()!=size || juce::SHA256(bytes.getData(),bytes.getSize()).toHexString()!=sha)
        throw std::runtime_error("Pianist asset failed SHA256 verification");
    return bytes;
}
void tensorContract(const Ort::Value& tensor,const std::vector<int64_t>& shape)
{
    if (!tensor.IsTensor()) throw std::runtime_error("Pianist output is not a tensor");
    const auto info=tensor.GetTensorTypeAndShapeInfo();
    if (info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape()!=shape)
        throw std::runtime_error("Pianist tensor contract mismatch");
    const auto* data=tensor.GetTensorData<float>();
    for (size_t i=0;i<info.GetElementCount();++i)
        if (!std::isfinite(data[i])) throw std::runtime_error("Pianist returned non-finite values");
}
void interfaceContract(const Ort::Session& session,const std::vector<std::string>& inputs,const std::vector<std::string>& outputs)
{
    if (session.GetInputCount()!=inputs.size() || session.GetOutputCount()!=outputs.size())
        throw std::runtime_error("Pianist interface size mismatch");
    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i=0;i<inputs.size();++i)
        if (session.GetInputNameAllocated(i,allocator).get()!=inputs[i]) throw std::runtime_error("Unexpected Pianist input");
    for (size_t i=0;i<outputs.size();++i)
        if (session.GetOutputNameAllocated(i,allocator).get()!=outputs[i]) throw std::runtime_error("Unexpected Pianist output");
}
}
#endif

namespace aether
{
#if AETHER_ENABLE_ONNX
struct PianistEngine::Impl
{
    // Sessions reference these immutable, verified external-weight bytes.
    juce::MemoryBlock weights;
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR,"AetherPianist"};
    Ort::SessionOptions options;
    Ort::RunOptions run;
    Ort::MemoryInfo memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> encoder,decoder;
    std::vector<std::vector<float>> encoded;
    std::vector<Ort::Value> state;
    std::atomic<bool> cancelled{false};
    size_t position=0,notes=0;
    const std::vector<std::string> encoderInputs{"score"},encoderOutputs{"hidden","cross_k_0","cross_v_0","cross_k_1","cross_v_1"};
    const std::vector<std::string> decoderInputs{"token","position","cross_k_0","cross_v_0","past_key_0","past_value_0","cross_k_1","cross_v_1","past_key_1","past_value_1"};
    const std::vector<std::string> decoderOutputs{"logits","present_key_0","present_value_0","present_key_1","present_value_1"};

    explicit Impl(const juce::File& root)
    {
        env.DisableTelemetryEvents();options.SetIntraOpNumThreads(2);options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        options.AddConfigEntry("session.intra_op.allow_spinning","0");
        options.AddConfigEntry("session.inter_op.allow_spinning","0");
        options.AddConfigEntry("session.use_external_initializer_file_buffers_directly","1");
        weights=verified(root,"model.safetensors",271506656,"8fb9111efc147adcc61a4885bd42a6e6b625626d9fb8576811916927aa8281d0");
        options.AddExternalInitializersFromFilesInMemory({ORT_TSTR("model.safetensors")},
            {static_cast<char*>(weights.getData())},{weights.getSize()});
        // A score is encoded once per offline job. Retaining another packed
        // copy of every encoder matrix wastes memory for this workload.
        // Decoder matrices remain packed because they run at every token.
        options.AddConfigEntry("session.disable_prepacking","1");
        // Architecture graphs are part of the compiled application, not user
        // supplied ONNX. External weights are supplied from verified bytes.
        encoder=std::make_unique<Ort::Session>(env,AetherPianistModel::pianist_encoder_onnx,
            static_cast<size_t>(AetherPianistModel::pianist_encoder_onnxSize),options);
        interfaceContract(*encoder,encoderInputs,encoderOutputs);
    }
    std::vector<Ort::Value> execute(Ort::Session& session,const std::vector<std::string>& inputs,
        const std::vector<std::string>& outputs,std::vector<Ort::Value>& tensors)
    {
        std::vector<const char*> in,out;
        for (const auto& name:inputs) in.push_back(name.c_str());
        for (const auto& name:outputs) out.push_back(name.c_str());
        return session.Run(run,in.data(),tensors.data(),tensors.size(),out.data(),out.size());
    }
    Ort::Value alias(Ort::Value& source)
    {
        const auto info=source.GetTensorTypeAndShapeInfo();const auto shape=info.GetShape();
        return Ort::Value::CreateTensor<float>(memory,source.GetTensorMutableData<float>(),info.GetElementCount(),shape.data(),shape.size());
    }
};
#else
struct PianistEngine::Impl {};
#endif
PianistEngine::PianistEngine(const juce::File& root)
{
#if AETHER_ENABLE_ONNX
    if (!OnnxEngine::initialiseBundledRuntime()) throw std::runtime_error("Bundled native runtime is unavailable for this architecture");
    impl=std::make_unique<Impl>(root);
#else
    juce::ignoreUnused(root);throw std::runtime_error("Native trained inference is disabled in this build");
#endif
}
PianistEngine::~PianistEngine()=default;
void PianistEngine::encode(const std::vector<int64_t>& tokens)
{
#if AETHER_ENABLE_ONNX
    if (tokens.size()<16 || tokens.size()>4096 || tokens.size()%8) throw std::runtime_error("Pianist requires 2..512 complete score notes");
    const std::array<std::array<int64_t,2>,8> ranges{{{5,133},{261,5252},{134,261},{262,5261},{5261,5389},{5261,5389},{5261,5389},{5261,5389}}};
    for (size_t i=0;i<tokens.size();++i)
        if (tokens[i]<ranges[i%8][0] || tokens[i]>=ranges[i%8][1]) throw std::runtime_error("Invalid score token");
    if (impl->cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Pianist job is cancelled");
    impl->state.clear();impl->decoder.reset();impl->encoded.clear();impl->position=0;impl->notes=tokens.size()/8;
    if (!impl->encoder)
    {
        impl->options.AddConfigEntry("session.disable_prepacking","1");
        impl->encoder=std::make_unique<Ort::Session>(impl->env,AetherPianistModel::pianist_encoder_onnx,
            static_cast<size_t>(AetherPianistModel::pianist_encoder_onnxSize),impl->options);
    }
    const std::array<int64_t,2> shape{1,static_cast<int64_t>(tokens.size())};
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(impl->memory,const_cast<int64_t*>(tokens.data()),tokens.size(),shape.data(),shape.size()));
    {
        auto output=impl->execute(*impl->encoder,impl->encoderInputs,impl->encoderOutputs,inputs);
        tensorContract(output[0],{1,static_cast<int64_t>(impl->notes),768});
        for (size_t i=1;i<5;++i) tensorContract(output[i],{1,4,static_cast<int64_t>(impl->notes),128});
        // Own the small cross-attention state independently of the encoder's
        // allocator. Destroy all encoder outputs before releasing its session.
        for (const auto& tensor:output)
        {
            const auto* begin=tensor.GetTensorData<float>();
            impl->encoded.emplace_back(begin,begin+tensor.GetTensorTypeAndShapeInfo().GetElementCount());
        }
    }
    impl->encoder.reset();
    if (impl->cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Pianist job is cancelled");
    impl->options.AddConfigEntry("session.disable_prepacking","0");
    impl->decoder=std::make_unique<Ort::Session>(impl->env,AetherPianistModel::pianist_decoder_onnx,
        static_cast<size_t>(AetherPianistModel::pianist_decoder_onnxSize),impl->options);
    interfaceContract(*impl->decoder,impl->decoderInputs,impl->decoderOutputs);
#else
    juce::ignoreUnused(tokens);throw std::runtime_error("Native inference disabled");
#endif
}
std::array<float,PianistEngine::vocabulary> PianistEngine::decode(int64_t token)
{
#if AETHER_ENABLE_ONNX
    if (impl->cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Pianist job is cancelled");
    if (!impl->decoder || impl->encoded.empty() || impl->position>=4096 || token<0 || token>=static_cast<int64_t>(vocabulary))
        throw std::runtime_error("Invalid or excessive Pianist decoder sequence");
    const std::array<int64_t,2> tokenShape{1,1};const std::array<int64_t,1> positionShape{1};
    int64_t positionValue=static_cast<int64_t>(impl->position);
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(impl->memory,&token,1,tokenShape.data(),tokenShape.size()));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(impl->memory,&positionValue,1,positionShape.data(),positionShape.size()));
    for (size_t i=0;i<2;++i)
    {
        const std::array<int64_t,4> crossShape{1,4,static_cast<int64_t>(impl->notes),128};
        for (size_t j=1;j<=2;++j)
        {
            auto& cross=impl->encoded[j+i*2];
            inputs.push_back(Ort::Value::CreateTensor<float>(impl->memory,cross.data(),cross.size(),crossShape.data(),crossShape.size()));
        }
        for (size_t j=0;j<2;++j)
            if (impl->state.empty())
            {
                const std::array<int64_t,4> empty{1,4,0,128};Ort::AllocatorWithDefaultOptions allocator;
                inputs.push_back(Ort::Value::CreateTensor<float>(allocator,empty.data(),empty.size()));
            }
            else inputs.push_back(impl->alias(impl->state[1+i*2+j]));
    }
    auto next=impl->execute(*impl->decoder,impl->decoderInputs,impl->decoderOutputs,inputs);
    tensorContract(next[0],{1,static_cast<int64_t>(vocabulary)});
    for (size_t i=1;i<5;++i)
    {
        const auto info=next[i].GetTensorTypeAndShapeInfo();
        if (info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape()!=std::vector<int64_t>{1,4,static_cast<int64_t>(impl->position+1),128})
            throw std::runtime_error("Pianist cache contract mismatch");
    }
    std::array<float,vocabulary> logits;
    std::copy_n(next[0].GetTensorData<float>(),vocabulary,logits.begin());
    impl->state=std::move(next);++impl->position;return logits;
#else
    juce::ignoreUnused(token);throw std::runtime_error("Native inference disabled");
#endif
}
std::vector<float> PianistEngine::encoderState() const
{
#if AETHER_ENABLE_ONNX
    if (impl->encoded.empty()) throw std::runtime_error("Encode a score first");
    return impl->encoded[0];
#else
    throw std::runtime_error("Native inference disabled");
#endif
}
void PianistEngine::cancel() noexcept
{
#if AETHER_ENABLE_ONNX
    impl->cancelled.store(true,std::memory_order_relaxed);
    try {impl->run.SetTerminate();} catch (...) {}
#endif
}
size_t PianistEngine::position() const noexcept
{
#if AETHER_ENABLE_ONNX
    return impl->position;
#else
    return 0;
#endif
}
}
