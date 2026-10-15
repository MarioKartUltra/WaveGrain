// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Abraham Joseph Okayli Masaryk
// =============================================================================
//  WaveGrain II — PluginProcessor.h
//  DWT granular ROMpler, v2: foleys MagicProcessor GUI layer, tempo-synced
//  grain clock (rate + size in ms OR beat divisions), LFO A/B mod matrix,
//  acv2 seed policy, per-grain pitch spread / reverse, named presets.
//  The DSP core (Source/dsp) stays JUCE-free; the LFO/mod matrix is applied
//  host-side as bounded offsets before the Params struct.
// =============================================================================
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <foleys_gui_magic/foleys_gui_magic.h>

#include "dsp/GranularEngine.h"
#include "common/acv2/dsp/Lfo.h"
#include "common/acv2/dsp/TempoSync.h"
#include "common/acv2/dsp/Units.h"

namespace pid
{
    // ---- kept from v1 (versionHint 1) ----
    inline constexpr auto sample    = "sample";
    inline constexpr auto root      = "root";
    inline constexpr auto position  = "position";
    inline constexpr auto scan      = "scan";
    inline constexpr auto freeze    = "freeze";
    inline constexpr auto spray     = "spray";
    inline constexpr auto pitchFine = "pitchFine";
    inline constexpr auto spread    = "spread";
    inline constexpr auto window    = "window";
    inline constexpr auto wavelet   = "wavelet";
    inline constexpr auto compress  = "compress";
    inline constexpr auto tilt      = "tilt";
    inline constexpr auto bandLow   = "bandLow";
    inline constexpr auto bandHigh  = "bandHigh";
    inline constexpr auto octave    = "octave";
    inline constexpr auto codecOn   = "codecOn";
    inline constexpr auto romRatio  = "romRatio";
    inline constexpr auto romBits   = "romBits";
    inline constexpr auto attack    = "attack";
    inline constexpr auto decay     = "decay";
    inline constexpr auto sustain   = "sustain";
    inline constexpr auto release   = "release";
    inline constexpr auto reso      = "reso";
    inline constexpr auto output    = "output";
    inline constexpr auto voices    = "voices";

    // ---- v2 (versionHint 2) ----
    inline constexpr auto cutoffHz    = "cutoffHz";     // replaces v1 0..1 "cutoff"
    // grain clock: three-param sync convention per control
    inline constexpr auto rateSync    = "rateSync";
    inline constexpr auto rateMs      = "rateMs";
    inline constexpr auto rateDiv     = "rateDiv";
    inline constexpr auto sizeSync    = "sizeSync";
    inline constexpr auto sizeMs      = "sizeMs";
    inline constexpr auto sizeDiv     = "sizeDiv";
    inline constexpr auto pitchSpread = "pitchSpread";
    inline constexpr auto reverseProb = "reverseProb";
    inline constexpr auto seedMode    = "seedMode";
    inline constexpr auto seed        = "seed";
    // LFO A/B
    inline constexpr auto lfoAShape   = "lfoAShape";
    inline constexpr auto lfoARateSync= "lfoARateSync";
    inline constexpr auto lfoARateMs  = "lfoARateMs";
    inline constexpr auto lfoARateDiv = "lfoARateDiv";
    inline constexpr auto lfoAPhase   = "lfoAPhase";
    inline constexpr auto lfoARetrig  = "lfoARetrig";
    inline constexpr auto lfoABipolar = "lfoABipolar";
    inline constexpr auto lfoBShape   = "lfoBShape";
    inline constexpr auto lfoBRateSync= "lfoBRateSync";
    inline constexpr auto lfoBRateMs  = "lfoBRateMs";
    inline constexpr auto lfoBRateDiv = "lfoBRateDiv";
    inline constexpr auto lfoBPhase   = "lfoBPhase";
    inline constexpr auto lfoBRetrig  = "lfoBRetrig";
    inline constexpr auto lfoBBipolar = "lfoBBipolar";
    // mod depths (−1..+1, shown ±%): 7 destinations x 2 sources
    inline constexpr auto rateModA    = "rateModA";
    inline constexpr auto rateModB    = "rateModB";
    inline constexpr auto sizeModA    = "sizeModA";
    inline constexpr auto sizeModB    = "sizeModB";
    inline constexpr auto posModA     = "posModA";
    inline constexpr auto posModB     = "posModB";
    inline constexpr auto pspreadModA = "pspreadModA";
    inline constexpr auto pspreadModB = "pspreadModB";
    inline constexpr auto compModA    = "compModA";
    inline constexpr auto compModB    = "compModB";
    inline constexpr auto tiltModA    = "tiltModA";
    inline constexpr auto tiltModB    = "tiltModB";
    inline constexpr auto spreadModA  = "spreadModA";
    inline constexpr auto spreadModB  = "spreadModB";
}

// ---- factory presets -------------------------------------------------------
struct WaveGrainPreset
{
    const char* name;
    std::vector<std::pair<const char*, float>> values;   // raw (denormalised) values
};

class WaveGrainProcessor : public foleys::MagicProcessor,
                           private juce::Timer
{
public:
    WaveGrainProcessor();
    ~WaveGrainProcessor() override { stopTimer(); }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 4.0; }

    // foleys
    void initialiseBuilder(foleys::MagicGUIBuilder& builder) override;
    juce::ValueTree createGuiValueTree() override;
    juce::AudioProcessorEditor* createEditor() override;

    juce::AudioProcessorValueTreeState& state() { return apvts; }
    foleys::MagicProcessorState& getMagicState() { return magicState; }
    void panic() { synth.panic(); }

    // Load a user sample (message thread) — switches Sample to "User" on success.
    bool loadUserSample(const juce::File& file);
    juce::String loadedName() const { return userName; }
    juce::String codecText()  const { return codecText_; }

    // UI telemetry (message thread)
    void copyOverview(std::vector<float>& lo, std::vector<float>& hi, int bins) const { synth.copyOverview(lo, hi, bins); }
    void copyPlayheads(std::vector<float>& d) const { synth.copyPlayheads(d); }
    void copyGrainMarkers(std::vector<float>& d) const { synth.copyGrainMarkers(d); }
    int  activeVoices() const { return synth.activeVoices(); }
    int  activeGrains() const { return synth.activeGrains(); }
    juce::uint64 lastUsedSeed() const { return (juce::uint64) synth.lastUsedSeed(); }
    std::atomic<float>& lfoAValue() { return lfoAValue_; }
    std::atomic<float>& lfoBValue() { return lfoBValue_; }
    double lastKnownBpm() const { return lastBpm_; }

    // ---- presets -----------------------------------------------------------
    static const std::vector<WaveGrainPreset>& factoryPresets();
    void applyFactoryPreset(int index);
    static juce::File presetDirectory();               // ~/Music/WaveGrain II/Presets
    bool savePresetFile(const juce::String& name);     // message thread
    bool loadPresetFile(const juce::File& f);          // message thread
    void setParamRaw(const char* id, float rawValue);

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    wavegrain::WaveGrainParams paramsFromState(float lfoA, float lfoB);
    void timerCallback() override;   // message thread: re-encode ROM codec on change

    double sr = 48000.0;
    juce::AudioProcessorValueTreeState apvts;
    wavegrain::GranularEngine synth;
    juce::AudioFormatManager formatMgr;
    juce::String userName;

    // LFOs (audio thread only)
    acv2::Lfo lfoA_, lfoB_;
    double lastBpm_ = 120.0;
    std::atomic<float> lfoAValue_ { 0.0f }, lfoBValue_ { 0.0f };

    foleys::MagicLevelSource* outputLevel = nullptr;   // owned by magicState

    std::atomic<int> userGen_ { 0 };
    bool   lastOn_ = false; int lastSel_ = -1, lastBits_ = -1, lastWt_ = -1, lastUserGen_ = -1;
    double lastRatio_ = -1.0;
    juce::String codecText_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveGrainProcessor)
};
