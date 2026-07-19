// =============================================================================
//  WaveGrain — PluginEditor.h
// =============================================================================
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

// Sample overview (min/max) + live per-voice playheads + selected region.
class SampleDisplay : public juce::Component
{
public:
    void setData(std::vector<float> lo, std::vector<float> hi, std::vector<float> heads,
                 float pos, float scanSpan)
    { lo_ = std::move(lo); hi_ = std::move(hi); heads_ = std::move(heads); pos_ = pos; span_ = scanSpan; repaint(); }
    void paint(juce::Graphics&) override;
private:
    std::vector<float> lo_, hi_, heads_;
    float pos_ = 0.0f, span_ = 0.0f;
};

class WaveGrainEditor : public juce::AudioProcessorEditor,
                        private juce::Timer,
                        public juce::FileDragAndDropTarget
{
public:
    explicit WaveGrainEditor(WaveGrainProcessor&);
    ~WaveGrainEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void addSlider(const char* id, const juce::String& name, int group);
    void addChoice(const char* id, const juce::String& name, juce::StringArray items, int group);
    void addToggle(const char* id, const juce::String& name, int group);

    WaveGrainProcessor& proc;
    juce::MidiKeyboardComponent keyboard;
    juce::Label   title, status;
    juce::TextButton loadBtn { "Load…" }, panicBtn { "Panic" };
    SampleDisplay display;
    std::unique_ptr<juce::FileChooser> chooser;

    struct Ctrl { std::unique_ptr<juce::Component> c; std::unique_ptr<juce::Label> l;
                  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sa;
                  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> ca;
                  std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ba;
                  int group = 0; };
    std::vector<Ctrl> ctrls;
    juce::TooltipWindow tips { this, 500 };

    static constexpr int kGroups = 4;
    const char* groupNames[kGroups] { "Grain", "Wavelet / Codec", "Amp / Filter", "Output" };
    int groupX[kGroups] { 0,0,0,0 }, groupY = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveGrainEditor)
};
