cmake_minimum_required(VERSION 3.24)
project(Aether_Matrix VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
if(NOT APPLE AND NOT WIN32)
    message(FATAL_ERROR "Supported plugin targets are macOS and Windows")
endif()
option(AETHER_ENABLE_ONNX "Bundle the optional ONNX CPU decoder" ON)
option(AETHER_BUILD_TESTS "Build processor and export tests" ON)
set(AETHER_ORT_ROOT "" CACHE PATH "Existing ONNX Runtime 1.31.0 SDK (optional)")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.16-security
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} "-DROOT=<SOURCE_DIR>" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyJuce.cmake")
FetchContent_MakeAvailable(JUCE)

if(AETHER_ENABLE_ONNX)
    if(NOT AETHER_ORT_ROOT)
        if(APPLE)
            set(ort_archive onnxruntime-osx-arm64-1.31.0.tgz)
            set(ort_sha 1031b36dd3bdaa8d976a6130d9cf2a05e9be320b93ae804f8427b91dc3432afc)
        else()
            if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
                message(FATAL_ERROR "Windows requires an x64 build")
            endif()
            set(ort_archive onnxruntime-win-x64-1.31.0.zip)
            set(ort_sha 3958d8a44984160692c1b4ffd0de3cb48f8c39f9d34bb31e433db06e30b82195)
        endif()
        FetchContent_Declare(onnxruntime_sdk
            URL https://github.com/microsoft/onnxruntime/releases/download/v1.31.0/${ort_archive}
            URL_HASH SHA256=${ort_sha}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(onnxruntime_sdk)
        set(AETHER_ORT_ROOT "${onnxruntime_sdk_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${AETHER_ORT_ROOT}/include/onnxruntime_cxx_api.h")
        message(FATAL_ERROR "AETHER_ORT_ROOT must point to an extracted C++ SDK")
    endif()
    if(APPLE AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        message(STATUS "Universal plugin: hybrid synthesis on Intel; bundled ONNX runtime is arm64")
    endif()
endif()

set(plugin_formats VST3 Standalone)
if(APPLE)
    list(APPEND plugin_formats AU)
endif()
juce_add_plugin(Aether_Matrix
    COMPANY_NAME "AetherAudioLabs"
    BUNDLE_ID "audio.aetherlabs.matrix"
    PLUGIN_MANUFACTURER_CODE Aeth
    PLUGIN_CODE AeMx
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS ${plugin_formats}
    PRODUCT_NAME "Aether Matrix")

juce_add_binary_data(AetherModel HEADER_NAME DefaultModel.h NAMESPACE AetherModel
    SOURCES models/rave_decoder.onnx)
target_sources(Aether_Matrix PRIVATE
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h
    Source/OnnxEngine.cpp Source/OnnxEngine.h)
target_compile_definitions(Aether_Matrix PUBLIC
    AETHER_ENABLE_ONNX=$<BOOL:${AETHER_ENABLE_ONNX}>
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
if(AETHER_ENABLE_ONNX)
    target_include_directories(Aether_Matrix SYSTEM PRIVATE "${AETHER_ORT_ROOT}/include")
endif()
target_link_libraries(Aether_Matrix PRIVATE
    AetherModel juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

foreach(format IN LISTS plugin_formats)
    set(plugin_target Aether_Matrix_${format})
    if(APPLE)
        set_target_properties(${plugin_target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path/../Frameworks")
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DBUNDLE=$<TARGET_BUNDLE_DIR:${plugin_target}>"
                "-DORT_ROOT=${AETHER_ORT_ROOT}"
                "-DENABLE_ONNX=${AETHER_ENABLE_ONNX}"
                "-DMODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/rave_decoder.onnx"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundleMac.cmake"
            VERBATIM)
    elseif(AETHER_ENABLE_ONNX)
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:${plugin_target}>/onnxruntime.dll"
            VERBATIM)
    endif()
endforeach()

if(AETHER_BUILD_TESTS)
    enable_testing()
    add_executable(AetherMatrixTests tests/ProcessorTests.cpp)
    target_include_directories(AetherMatrixTests PRIVATE
        "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherMatrixTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        AETHER_TEST_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/models/rave_decoder.onnx")
    target_link_libraries(AetherMatrixTests PRIVATE Aether_Matrix)
    if(AETHER_ENABLE_ONNX)
        if(APPLE)
            add_custom_command(TARGET AetherMatrixTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AetherMatrixTests>/Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/libonnxruntime.1.31.0.dylib"
                    "$<TARGET_FILE_DIR:AetherMatrixTests>/Frameworks/libonnxruntime.dylib")
        else()
            add_custom_command(TARGET AetherMatrixTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:AetherMatrixTests>/onnxruntime.dll")
        endif()
    endif()
    add_test(NAME processor COMMAND AetherMatrixTests)
    set_tests_properties(processor PROPERTIES TIMEOUT 90)
endif()
