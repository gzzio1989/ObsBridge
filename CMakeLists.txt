cmake_minimum_required(VERSION 3.22)

project(SyncroomObsBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows配布先PCに VC++ 再頒布可能パッケージが無くても単体動作するよう静的CRT (/MT) を使用
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /MP)
endif()

# macOS では Apple Silicon (arm64) + Intel (x86_64) の Universal Binary を既定にする
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# ローカルJUCEパスが明示指定されていれば利用し、未指定時は公式 JUCE 8.0.12 を自動取得
set(SOBR_JUCE_DIR "" CACHE PATH "Optional path to local JUCE 8.0.12 source directory")
if(SOBR_JUCE_DIR AND EXISTS "${SOBR_JUCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local JUCE source: ${SOBR_JUCE_DIR}")
    add_subdirectory("${SOBR_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.12
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(SOBR_PLUGIN_FORMATS VST3 AU)
else()
    set(SOBR_PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(SyncroomObsBridge
    COMPANY_NAME                "Gzzio"
    BUNDLE_ID                   "com.gzzio.syncroomobsbridge"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_AUTO_MANIFEST          FALSE
    PLUGIN_MANUFACTURER_CODE    Gzzo
    PLUGIN_CODE                 Sobr
    FORMATS                     ${SOBR_PLUGIN_FORMATS}
    PRODUCT_NAME                "Syncroom OBS Bridge"
)

target_sources(SyncroomObsBridge
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/WasapiLoopbackSender.h
)

target_compile_definitions(SyncroomObsBridge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        NOMINMAX=1
        WIN32_LEAN_AND_MEAN=1
)

target_link_libraries(SyncroomObsBridge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_core
        $<$<PLATFORM_ID:Windows>:avrt ole32 mmdevapi>
        $<$<PLATFORM_ID:Darwin>:-framework\ CoreAudio -framework\ AudioToolbox -framework\ AudioUnit -framework\ CoreFoundation>
    PUBLIC
        juce::juce_recommended_config_flags
)

option(SOBR_BUILD_TESTS "Build verification executable" ON)
if(SOBR_BUILD_TESTS)
    add_executable(verify_obs_bridge
        Source/verify_obs_bridge.cpp
        Source/WasapiLoopbackSender.h
    )
    target_include_directories(verify_obs_bridge PRIVATE Source)
    target_compile_definitions(verify_obs_bridge PRIVATE NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
    if(WIN32)
        target_link_libraries(verify_obs_bridge PRIVATE avrt ole32)
    elseif(APPLE)
        target_link_libraries(verify_obs_bridge PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation")
    endif()
endif()
