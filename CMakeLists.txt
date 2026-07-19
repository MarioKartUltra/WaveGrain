cmake_minimum_required(VERSION 3.22)
project(WaveGrain VERSION 2.0.0 LANGUAGES C CXX OBJC OBJCXX)

# foleys_gui_magic requires C++20 (v2 convention).
set(CMAKE_CXX_STANDARD 20)
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

# ---- foleys_gui_magic (WYSIWYG GUI layer, v2) --------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/foleys_gui_magic/modules/foleys_gui_magic")
    juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/external/foleys_gui_magic/modules/foleys_gui_magic")
elseif(EXISTS "$ENV{HOME}/projects/foleys_gui_magic/modules/foleys_gui_magic")
    juce_add_module("$ENV{HOME}/projects/foleys_gui_magic/modules/foleys_gui_magic")
else()
    message(FATAL_ERROR "foleys_gui_magic not found — symlink external/foleys_gui_magic")
endif()

# ---- plugin target: v2 identity so v1 and v2 coexist in a DAW ---------------
juce_add_plugin(WaveGrain
    PLUGIN_MANUFACTURER_CODE Wvgr
    PLUGIN_CODE              Wgr2
    COMPANY_NAME             "WaveGrain"
    BUNDLE_ID                com.wavegrain.v2
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  TRUE
    PLUGIN_NAME              "WaveGrain II"
    PRODUCT_NAME             "WaveGrain II"
    FORMATS                  VST3 Standalone AU)

juce_generate_juce_header(WaveGrain)

# Default GUI layout baked in as BinaryData (Resources/magic.xml).
juce_add_binary_data(WaveGrainAssets SOURCES Resources/magic.xml)

target_sources(WaveGrain PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
if(APPLE)
    target_sources(WaveGrain PRIVATE Source/ui/DropOverlay.mm)
endif()

target_include_directories(WaveGrain PRIVATE Source Source/dsp)
target_compile_definitions(WaveGrain PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
    FOLEYS_SHOW_GUI_EDITOR_PALLETTE=1
    FOLEYS_ENABLE_BINARY_DATA=0)
target_link_libraries(WaveGrain PRIVATE
    WaveGrainAssets
    foleys_gui_magic
    juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)

# ---- JUCE-free offline tests -------------------------------------------------
add_executable(wavegrain_check tools/wavegrain_check.cpp)
set_target_properties(wavegrain_check PROPERTIES CXX_STANDARD 17)
target_include_directories(wavegrain_check PRIVATE Source Source/dsp)

add_executable(acv2_dsp_tests tools/acv2_dsp_tests.cpp)
set_target_properties(acv2_dsp_tests PROPERTIES CXX_STANDARD 17)
target_include_directories(acv2_dsp_tests PRIVATE Source/common)

enable_testing()
add_test(NAME wavegrain_check COMMAND wavegrain_check)
add_test(NAME acv2_tempo_sync COMMAND acv2_dsp_tests tempo)
add_test(NAME acv2_lfo        COMMAND acv2_dsp_tests lfo)
add_test(NAME acv2_seed       COMMAND acv2_dsp_tests seed)
add_test(NAME acv2_units      COMMAND acv2_dsp_tests units)
