cmake_minimum_required(VERSION 3.22)
project(WaveGrain VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE" "${CMAKE_BINARY_DIR}/JUCE")
elseif(EXISTS "$ENV{HOME}/projects/JUCE/CMakeLists.txt")
    add_subdirectory("$ENV{HOME}/projects/JUCE" "${CMAKE_BINARY_DIR}/JUCE")
else()
    message(FATAL_ERROR "No local JUCE found — set -DJUCE_DIR=/path/to/JUCE")
endif()

juce_add_plugin(WaveGrain
    PLUGIN_MANUFACTURER_CODE Wvgr
    PLUGIN_CODE              Wgrn
    COMPANY_NAME             "WaveGrain"
    BUNDLE_ID                com.wavegrain.poc
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  TRUE
    PLUGIN_NAME              "WaveGrain"
    PRODUCT_NAME             "WaveGrain"
    FORMATS                  VST3 Standalone AU)

juce_generate_juce_header(WaveGrain)
target_sources(WaveGrain PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(WaveGrain PRIVATE Source Source/dsp)
target_compile_definitions(WaveGrain PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
target_link_libraries(WaveGrain PRIVATE
    juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)

add_executable(wavegrain_check tools/wavegrain_check.cpp)
enable_testing()
add_test(NAME wavegrain_check COMMAND wavegrain_check)
