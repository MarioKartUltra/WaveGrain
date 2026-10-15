# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Abraham Joseph Okayli Masaryk
cmake_minimum_required(VERSION 3.22)
project(WaveGrain VERSION 2.0.0 LANGUAGES C CXX OBJC OBJCXX)

# foleys_gui_magic requires C++20 (v2 convention).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# JUCE and foleys_gui_magic: a local checkout, else fetched (cmake/ThirdParty.cmake)
include(cmake/ThirdParty.cmake)


# ---- build switches ---------------------------------------------------------
# GUI_EDITOR OFF = no foleys WYSIWYG window next to the plugin (builds to hand out).
# INSTALL_AFTER_BUILD OFF = leave the bundles in the build dir, touch no plugin folder.
option(WAVEGRAIN_GUI_EDITOR "Open the foleys WYSIWYG editor window with the plugin" OFF)
option(WAVEGRAIN_INSTALL_AFTER_BUILD "Copy the built plugins into ~/Library/Audio/Plug-Ins" ON)

# ---- plugin target: v2 identity so v1 and v2 coexist in a DAW ---------------
juce_add_plugin(WaveGrain
    PLUGIN_MANUFACTURER_CODE Wvgr
    PLUGIN_CODE              Wgr2
    COMPANY_NAME             "WaveGrain"
    BUNDLE_ID                com.wavegrain.v2
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  ${WAVEGRAIN_INSTALL_AFTER_BUILD}
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
    FOLEYS_SHOW_GUI_EDITOR_PALLETTE=$<BOOL:${WAVEGRAIN_GUI_EDITOR}>
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

# ---- offscreen UI screenshot tool (not a test; links shared code only) -------
# Links the juce_add_plugin shared-code static library so no VST3/AU/Standalone
# bundle target is built and COPY_PLUGIN_AFTER_BUILD is never triggered.
add_executable(wavegrain_shot tools/wavegrain_shot.cpp)
set_target_properties(wavegrain_shot PROPERTIES
    CXX_STANDARD 20
    EXCLUDE_FROM_ALL TRUE
    OSX_ARCHITECTURES arm64)
target_link_libraries(wavegrain_shot PRIVATE WaveGrain)
target_include_directories(wavegrain_shot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/dsp
    $<TARGET_PROPERTY:WaveGrain,JUCE_GENERATED_SOURCES_DIRECTORY>
    $<TARGET_PROPERTY:juce_audio_utils,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:foleys_gui_magic,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(wavegrain_shot PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JucePlugin_Build_VST=0 JucePlugin_Build_VST3=0 JucePlugin_Build_AU=0
    JucePlugin_Build_AUv3=0 JucePlugin_Build_AAX=0 JucePlugin_Build_Standalone=0
    JucePlugin_Build_Unity=0 JucePlugin_Build_LV2=0)

enable_testing()
add_test(NAME wavegrain_check COMMAND wavegrain_check)
add_test(NAME acv2_tempo_sync COMMAND acv2_dsp_tests tempo)
add_test(NAME acv2_lfo        COMMAND acv2_dsp_tests lfo)
add_test(NAME acv2_seed       COMMAND acv2_dsp_tests seed)
add_test(NAME acv2_units      COMMAND acv2_dsp_tests units)
