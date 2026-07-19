// =============================================================================
//  WaveGrain II — PluginEditor.h
//  foleys MagicPluginEditor subclass adding:
//   - the JUCE FileDragAndDropTarget path (Finder + plain Live browser drags)
//   - the native promise-aware DropOverlay (Ableton browser promise drags)
//   - a visible whole-editor drop-highlight state
//  Custom GuiItems (registered via registerWaveGrainItems, all palette-visible):
//   SampleView (hero waveform + live grain cloud), Sigil (stealth identity +
//   BETA), SyncKnob (ONE knob swapping ms/beat attachment + chip), ModKnob
//   (knob + LFO A/B depth rings), SeedPanel, CodecPanel, PresetBar, Status.
// =============================================================================
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <foleys_gui_magic/foleys_gui_magic.h>
#include "PluginProcessor.h"

#if JUCE_MAC
 #include "ui/DropOverlay.h"
#endif

// Register all WaveGrain II custom GuiItems with the builder.
void registerWaveGrainItems(foleys::MagicGUIBuilder& builder);

class WaveGrainEditor : public foleys::MagicPluginEditor,
                        public juce::FileDragAndDropTarget
{
public:
    WaveGrainEditor(WaveGrainProcessor&, std::unique_ptr<foleys::MagicGUIBuilder> builder);
    ~WaveGrainEditor() override;

    // ---- JUCE drag-and-drop path ----
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;

    void paintOverChildren(juce::Graphics&) override;
    void parentHierarchyChanged() override;

private:
    void setDragOver(bool b);

    WaveGrainProcessor& proc;
    juce::TooltipWindow tips { this, 400 };
    bool dragOver_ = false;

#if JUCE_MAC
    std::unique_ptr<wgui::DropOverlay> overlay_;
    juce::ComponentPeer* overlayPeer_ = nullptr;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveGrainEditor)
};
