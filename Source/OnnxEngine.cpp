#include "OnnxEngine.h"
#include <algorithm>
#include <cmath>
#include "DefaultModel.h"

#if AETHER_ENABLE_ONNX
 #define ORT_API_MANUAL_INIT
 #include <onnxruntime_cxx_api.h>
 #if JUCE_MAC
  #include <dlfcn.h>
 #elif JUCE_WINDOWS
  #include <windows.h>
 #endif

namespace
{
const OrtApi* loadRuntime()
{
    juce::File module;
 #if JUCE_MAC
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&loadRuntime), &info) == 0)
        return nullptr;
    module = juce::File(juce::String::fromUTF8(info.dli_fname));
    auto library = module.getParentDirectory().getParentDirectory()
                         .getChildFile("Frameworks/libonnxruntime.dylib");
    if (!library.existsAsFile())
        library = module.getParentDirectory().getChildFile("Frameworks/libonnxruntime.dylib");
    auto handle = dlopen(library.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
        return nullptr;
    auto getBase = reinterpret_cast<const OrtApiBase* (*)()>(dlsym(handle, "OrtGetApiBase"));
 #elif JUCE_WINDOWS
    HMODULE currentModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&loadRuntime), &currentModule))
        return nullptr;
    std::array<wchar_t, 32768> path {};
    const auto size = GetModuleFileNameW(currentModule, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size())
        return nullptr;
    module = juce::File(juce::String(path.data()));
    const auto library = module.getSiblingFile("onnxruntime.dll");
    auto handle = LoadLibraryExW(library.getFullPathName().toWideCharPointer(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (handle == nullptr)
        return nullptr;
    auto getBase = reinterpret_cast<const OrtApiBase* (*)()>(GetProcAddress(handle, "OrtGetApiBase"));
 #endif
    // Keep ORT resident: another plugin instance may still own sessions on unload.
    return getBase != nullptr ? getBase()->GetApi(ORT_API_VERSION) : nullptr;
}

bool initialiseRuntime()
{
    static const bool ready = []
    {
        if (const auto* api = loadRuntime())
        {
            Ort::InitApi(api);
            return true;
        }
        return false;
    }();
    return ready;
}
}

struct OnnxEngine::Impl
{
    Ort::Env env { ORT_LOGGING_LEVEL_ERROR, "AetherMatrix" };
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::string inputName, outputName;

    Impl()
    {
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
    }
};
#else
struct OnnxEngine::Impl {};
#endif

OnnxEngine::OnnxEngine() = default;
OnnxEngine::~OnnxEngine() = default;

juce::String OnnxEngine::load(const juce::File& file)
{
#if AETHER_ENABLE_ONNX
    if (!initialiseRuntime())
        return "ONNX runtime unavailable for this architecture; hybrid synthesis active";
    if (file != juce::File{} && (!file.hasFileExtension("onnx") || !file.existsAsFile()
        || file.getSize() <= 0 || file.getSize() > 16 * 1024 * 1024))
        return "Choose a self-contained .onnx file smaller than 16 MiB";
    try
    {
        auto candidate = std::make_unique<Impl>();
        juce::MemoryBlock bytes;
        if (file == juce::File{})
            bytes.append(AetherModel::rave_decoder_onnx, static_cast<size_t>(AetherModel::rave_decoder_onnxSize));
        else if (!file.loadFileAsData(bytes) || bytes.getSize() > 16 * 1024 * 1024)
            return "Cannot read model";
        candidate->session = std::make_unique<Ort::Session>(candidate->env, bytes.getData(),
                                                           bytes.getSize(), candidate->options);
        auto& session = *candidate->session;
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
            return "Model must have one input and one output";
        const auto inputType = session.GetInputTypeInfo(0);
        const auto outputType = session.GetOutputTypeInfo(0);
        const auto input = inputType.GetTensorTypeAndShapeInfo();
        const auto output = outputType.GetTensorTypeAndShapeInfo();
        if (input.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
            || output.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
            || input.GetShape() != std::vector<int64_t> { 1, 16, 1 }
            || output.GetShape() != std::vector<int64_t> { 1, 1, 2048 })
            return "Expected float32 [1,16,1] -> [1,1,2048] at 48 kHz";
        Ort::AllocatorWithDefaultOptions allocator;
        candidate->inputName = session.GetInputNameAllocated(0, allocator).get();
        candidate->outputName = session.GetOutputNameAllocated(0, allocator).get();
        impl = std::move(candidate);
        return {};
    }
    catch (const std::exception&)
    {
        return "Model could not be loaded; hybrid synthesis active";
    }
#else
    juce::ignoreUnused(file);
    return "This build uses hybrid synthesis only";
#endif
}

bool OnnxEngine::render(const std::array<float, 16>& latent, std::array<float, 2048>& audio)
{
#if AETHER_ENABLE_ONNX
    if (impl == nullptr)
        return false;
    try
    {
        const std::array<int64_t, 3> shape { 1, 16, 1 };
        auto input = Ort::Value::CreateTensor<float>(impl->memory, const_cast<float*>(latent.data()),
                                                     latent.size(), shape.data(), shape.size());
        const char* inputs[] { impl->inputName.c_str() };
        const char* outputs[] { impl->outputName.c_str() };
        auto result = impl->session->Run(Ort::RunOptions { nullptr }, inputs, &input, 1, outputs, 1);
        if (result.size() != 1 || !result.front().IsTensor()
            || result.front().GetTensorTypeAndShapeInfo().GetShape() != std::vector<int64_t> { 1, 1, 2048 })
            return false;
        const auto* samples = result.front().GetTensorData<float>();
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0.0f;
        return true;
    }
    catch (const std::exception&) { return false; }
#else
    juce::ignoreUnused(latent, audio);
    return false;
#endif
}
