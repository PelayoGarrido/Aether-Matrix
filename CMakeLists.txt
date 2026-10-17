cmake_minimum_required(VERSION 3.24)
project(Aether_Matrix VERSION 1.0.0 LANGUAGES C CXX)

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
option(AETHER_BUILD_GENERATION_TOOLS "Build Piano Studio and its bundled offline helpers" ON)
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

if(AETHER_BUILD_GENERATION_TOOLS)
    if(NOT AETHER_ENABLE_ONNX)
        message(FATAL_ERROR "Generation evaluation requires the verified ONNX SDK")
    endif()
    add_subdirectory(tools/midi)
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
juce_add_binary_data(AetherPianistModel HEADER_NAME DefaultPianistModel.h NAMESPACE AetherPianistModel
    SOURCES models/pianist_encoder.onnx models/pianist_decoder.onnx)
target_sources(Aether_Matrix PRIVATE
    Source/PluginProcessor.cpp Source/PluginProcessor.h
    Source/PluginEditor.cpp Source/PluginEditor.h Source/AudioQuality.h
    Source/Performance.cpp Source/Performance.h Source/PianoSampler.cpp Source/PianoSampler.h
    Source/AudioPreview.cpp Source/AudioPreview.h Source/PianoStudio.cpp Source/PianoStudio.h
    Source/NativeProcess.cpp Source/NativeProcess.h Source/PianoStudioView.cpp Source/PianoStudioView.h
    Source/PianistEngine.cpp Source/PianistEngine.h
    Source/PianoInterpretation.cpp Source/PianoInterpretation.h
    Source/OnnxEngine.cpp Source/OnnxEngine.h)
target_compile_definitions(Aether_Matrix PUBLIC
    AETHER_ENABLE_ONNX=$<BOOL:${AETHER_ENABLE_ONNX}>
    AETHER_PIANO_STUDIO=$<BOOL:${AETHER_BUILD_GENERATION_TOOLS}>
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
if(AETHER_ENABLE_ONNX)
    target_include_directories(Aether_Matrix SYSTEM PRIVATE "${AETHER_ORT_ROOT}/include")
endif()
target_link_libraries(Aether_Matrix PRIVATE
    AetherModel AetherPianistModel juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

if(WIN32)
    target_link_libraries(Aether_Matrix PRIVATE psapi)
endif()
if(AETHER_BUILD_GENERATION_TOOLS)
    add_executable(AetherPianoRender tools/piano/render.cpp)
    target_include_directories(AetherPianoRender PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherPianoRender PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(AetherPianoRender PRIVATE Aether_Matrix)
    add_executable(AetherStudioQualify tools/performance/studio_qualify.cpp)
    target_include_directories(AetherStudioQualify PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherStudioQualify PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(AetherStudioQualify PRIVATE Aether_Matrix)
    add_executable(AetherPianistEvaluate tools/performance/native_main.cpp)
    add_executable(AetherPianistPerform tools/performance/interpret_main.cpp)
    target_include_directories(AetherPianistPerform PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherPianistPerform PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(AetherPianistPerform PRIVATE Aether_Matrix)
    target_include_directories(AetherPianistEvaluate PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherPianistEvaluate PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(AetherPianistEvaluate PRIVATE Aether_Matrix)
    if(APPLE)
        add_custom_command(TARGET AetherPianistEvaluate POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AetherPianistEvaluate>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/libonnxruntime.1.31.0.dylib"
                "$<TARGET_FILE_DIR:AetherPianistEvaluate>/Frameworks/libonnxruntime.dylib")
        add_custom_command(TARGET AetherPianistPerform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:AetherPianistPerform>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/libonnxruntime.1.31.0.dylib"
                "$<TARGET_FILE_DIR:AetherPianistPerform>/Frameworks/libonnxruntime.dylib")
    else()
        target_link_libraries(AetherPianistEvaluate PRIVATE psapi)
        target_link_libraries(AetherPianistPerform PRIVATE psapi)
        add_custom_command(TARGET AetherPianistPerform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:AetherPianistPerform>/onnxruntime.dll")
        add_custom_command(TARGET AetherPianistEvaluate POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AETHER_ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:AetherPianistEvaluate>/onnxruntime.dll")
    endif()
endif()

foreach(format IN LISTS plugin_formats)
    set(plugin_target Aether_Matrix_${format})
    if(AETHER_BUILD_GENERATION_TOOLS)
        add_dependencies(${plugin_target} AetherPianistPerform AetherPianoRender AetherMidiEvaluate)
    endif()
    set(piano_helper_args)
    if(AETHER_BUILD_GENERATION_TOOLS)
        list(APPEND piano_helper_args "-DPIANO_PERFORM=$<TARGET_FILE:AetherPianistPerform>" "-DPIANO_RENDER=$<TARGET_FILE:AetherPianoRender>" "-DMIDI_COMPOSE=$<TARGET_FILE:AetherMidiEvaluate>")
    endif()
    if(APPLE)
        set_target_properties(${plugin_target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path/../Frameworks")
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DBUNDLE=$<TARGET_BUNDLE_DIR:${plugin_target}>"
                "-DORT_ROOT=${AETHER_ORT_ROOT}"
                "-DENABLE_ONNX=${AETHER_ENABLE_ONNX}"
                "-DMODEL=${CMAKE_CURRENT_SOURCE_DIR}/models/rave_decoder.onnx"
                "-DPIANO_STUDIO=${AETHER_BUILD_GENERATION_TOOLS}"
                ${piano_helper_args}
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundleMac.cmake"
            VERBATIM)
    elseif(AETHER_ENABLE_ONNX)
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AETHER_ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:${plugin_target}>/onnxruntime.dll"
            VERBATIM)
    endif()
    if(WIN32 AND AETHER_BUILD_GENERATION_TOOLS)
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${plugin_target}>/Helpers" "$<TARGET_FILE_DIR:${plugin_target}>/Resources/piano"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:AetherPianistPerform>" "$<TARGET_FILE:AetherPianoRender>" "$<TARGET_FILE:AetherMidiEvaluate>" "${AETHER_ORT_ROOT}/lib/onnxruntime.dll" "$<TARGET_FILE_DIR:${plugin_target}>/Helpers/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/piano/salamander-manifest.json" "${CMAKE_CURRENT_SOURCE_DIR}/tools/piano/salamander-layout.json" "$<TARGET_FILE_DIR:${plugin_target}>/Resources/piano/"
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
    add_executable(AetherAudioQualityTests tests/AudioQualityTests.cpp)
    target_include_directories(AetherAudioQualityTests PRIVATE
        "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherAudioQualityTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
    target_link_libraries(AetherAudioQualityTests PRIVATE Aether_Matrix)
    add_test(NAME audio_quality COMMAND AetherAudioQualityTests)
    add_executable(AetherPianoTests tests/PianoTests.cpp)
    target_include_directories(AetherPianoTests PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherPianoTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        AETHER_PIANO_LAYOUT="${CMAKE_CURRENT_SOURCE_DIR}/tools/piano/salamander-layout.json")
    target_link_libraries(AetherPianoTests PRIVATE Aether_Matrix)
    add_executable(AetherHelperProbe tests/HelperProbe.cpp)
    add_executable(AetherPianoStudioTests tests/PianoStudioTests.cpp)
    target_include_directories(AetherPianoStudioTests PRIVATE "$<TARGET_PROPERTY:Aether_Matrix,INCLUDE_DIRECTORIES>" Source)
    target_compile_definitions(AetherPianoStudioTests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        AETHER_HELPER_PROBE="$<TARGET_FILE:AetherHelperProbe>")
    if(AETHER_BUILD_GENERATION_TOOLS)
        target_compile_definitions(AetherPianoStudioTests PRIVATE AETHER_PIANO_RENDER="$<TARGET_FILE:AetherPianoRender>"
            AETHER_PIANO_LAYOUT="${CMAKE_CURRENT_SOURCE_DIR}/tools/piano/salamander-layout.json")
        add_dependencies(AetherPianoStudioTests AetherPianoRender)
    endif()
    target_link_libraries(AetherPianoStudioTests PRIVATE Aether_Matrix)
    add_dependencies(AetherPianoStudioTests AetherHelperProbe)
    add_test(NAME piano_studio COMMAND AetherPianoStudioTests)
    set_tests_properties(piano_studio PROPERTIES TIMEOUT 30)
    add_test(NAME piano COMMAND AetherPianoTests)
    set_tests_properties(piano PROPERTIES TIMEOUT 30)
    set_tests_properties(audio_quality PROPERTIES TIMEOUT 30)
    add_test(NAME processor COMMAND AetherMatrixTests)
    set_tests_properties(processor PROPERTIES TIMEOUT 90)
endif()
