// =============================================================================
//  WaveGrain — PluginProcessor.h
//  A DWT-based granular ROMpler: loads a sample (built-in ROM or user file),
//  plays it chromatically via MIDI through a granular engine whose every grain
//  is processed in the wavelet domain (compression / band shaping / scale-select
//  / octave scale-shift). Thin JUCE wrapper around the JUCE-free
//  wavegrain::GranularEngine.
// =============================================================================
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "dsp/GranularEngine.h"

namespace pid
{
    inline constexpr auto sample    = "sample";
    inline constexpr auto root      = "root";
    inline constexpr auto grainSize = "grainSize";
    inline constexpr auto density   = "density";
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
    inline constexpr auto cutoff    = "cutoff";
    inline constexpr auto reso      = "reso";
    inline constexpr auto output    = "output";
    inline constexpr auto voices    = "voices";
}

class WaveGrainProcessor : public juce::AudioProcessor,
                           private juce::Timer
{
public:
    WaveGrainProcessor();
    ~WaveGrainProcessor() override { stopTimer(); }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "WaveGrain"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState& state() { return apvts; }
    juce::MidiKeyboardState& keyboardState() { return kbState; }
    void panic() { synth.panic(); }

    // Load a user sample (message thread) — returns true on success and switches
    // the Sample parameter to "User".
    bool loadUserSample(const juce::File& file);
    juce::String loadedName() const { return userName; }
    juce::String codecText()  const { return codecText_; }   // "codec 4.2:1 · 190 kbps"

    // UI telemetry
    void copyOverview(std::vector<float>& lo, std::vector<float>& hi, int bins) const { synth.copyOverview(lo, hi, bins); }
    void copyScope(std::vector<float>& d) const { synth.copyScope(d); }
    void copyPlayheads(std::vector<float>& d) const { synth.copyPlayheads(d); }
    int  activeVoices() const { return synth.activeVoices(); }
    int  activeGrains() const { return synth.activeGrains(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    wavegrain::WaveGrainParams paramsFromState() const;
    void timerCallback() override;   // message thread: re-encode the ROM codec on change

    double sr = 48000.0;
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState kbState;
    wavegrain::GranularEngine synth;
    juce::AudioFormatManager formatMgr;
    juce::String userName;

    std::atomic<int> userGen_ { 0 };   // bumped on user load so the poller re-encodes
    bool   lastOn_ = false; int lastSel_ = -1, lastBits_ = -1, lastWt_ = -1, lastUserGen_ = -1;
    double lastRatio_ = -1.0;
    juce::String codecText_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveGrainProcessor)
};
