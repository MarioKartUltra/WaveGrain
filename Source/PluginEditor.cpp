// =============================================================================
//  WaveGrain II — PluginEditor.cpp
// =============================================================================
#include "PluginEditor.h"
#include "common/acv2/ui/Theme.h"
#include "common/acv2/ui/LnfV2.h"
#include "common/acv2/ui/ModRing.h"
#include "common/acv2/dsp/TempoSync.h"

namespace th = acv2::theme;
using acv2::ui::toColour;

namespace
{
    const th::Palette& pal() { return th::kWaveGrain; }
    constexpr const char* kAudioWildcards = "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf";

    // String::matchesWildcard takes one pattern, not a list: the filter splits it.
    bool isAudioFile(const juce::String& path)
    {
        static const juce::WildcardFileFilter filter(kAudioWildcards, {}, "audio");
        return filter.isFileSuitable(juce::File::createFileWithoutCheckingPath(path));
    }

    WaveGrainProcessor* processorOf(foleys::MagicGUIBuilder& b)
    {
        return dynamic_cast<WaveGrainProcessor*>(b.getMagicState().getProcessor());
    }
}

// =============================================================================
//  Hero: waveform overview + position window + live playheads + grain cloud.
//  Click/drag sets the Position parameter. Shows sample identity + codec text.
// =============================================================================
class WgSampleView : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    explicit WgSampleView(WaveGrainProcessor* p) : proc(p) {}

    void refresh()
    {
        if (proc == nullptr) return;
        proc->copyOverview(lo_, hi_, 420);
        proc->copyPlayheads(heads_);
        proc->copyGrainMarkers(marks_);
        pos_    = proc->state().getRawParameterValue(pid::position)->load();
        sampleSel_ = (int) proc->state().getRawParameterValue(pid::sample)->load();
        name_   = proc->loadedName();
        codec_  = proc->codecText();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(toColour(pal().hero)); g.fillRoundedRectangle(b, th::kSpacing.cornerM);

        // centre rule
        g.setColour(toColour(pal().outline).withAlpha(0.6f));
        g.drawHorizontalLine((int) b.getCentreY(), b.getX() + 6, b.getRight() - 6);

        // waveform min/max fill
        const int n = (int) lo_.size();
        if (n > 1 && (int) hi_.size() == n)
        {
            const float midY = b.getCentreY(), h = b.getHeight() * 0.44f, w = b.getWidth() - 12.0f, x0 = b.getX() + 6.0f;
            g.setColour(toColour(pal().accentDim).withAlpha(0.85f));
            for (int x = 0; x < (int) w; ++x)
            {
                const int i = (int) ((double) x / w * (n - 1));
                const float y0 = midY - juce::jlimit(-1.0f, 1.0f, hi_[(size_t) i]) * h;
                const float y1 = midY - juce::jlimit(-1.0f, 1.0f, lo_[(size_t) i]) * h;
                g.drawLine(x0 + (float) x, y0, x0 + (float) x, juce::jmax(y1, y0 + 1.0f), 1.0f);
            }
        }

        // position window
        const float W = b.getWidth() - 12.0f, X = b.getX() + 6.0f;
        g.setColour(toColour(pal().accent).withAlpha(0.12f));
        g.fillRect(juce::Rectangle<float>(X + pos_ * W, b.getY() + 4, juce::jmax(3.0f, 0.02f * W), b.getHeight() - 8));
        g.setColour(toColour(pal().accent).withAlpha(0.55f));
        g.drawVerticalLine((int) (X + pos_ * W), b.getY() + 4, b.getBottom() - 4);

        // live voice playheads
        g.setColour(toColour(pal().accent).withAlpha(0.35f));
        for (float hd : heads_)
            g.fillRect(juce::Rectangle<float>(X + hd * W - 0.5f, b.getY() + 4, 1.5f, b.getHeight() - 8));

        // grain cloud: x = source position, y = stereo pan, fade with progress
        for (size_t i = 0; i + 2 < marks_.size() + 1 && i + 2 < marks_.size() + 3; i += 3)
        {
            if (i + 2 >= marks_.size()) break;
            const float sx = marks_[i], prog = marks_[i + 1], pan = marks_[i + 2];
            const float gx = X + sx * W;
            const float gy = b.getCentreY() + (pan - 0.5f) * b.getHeight() * 0.62f;
            const float r  = 1.8f + 3.2f * (1.0f - prog);
            g.setColour(toColour(pal().accent).withAlpha(0.15f + 0.75f * (1.0f - prog)));
            g.fillEllipse(gx - r, gy - r, r * 2.0f, r * 2.0f);
        }

        // identity chip (ROM name or user file — never the plugin title)
        {
            static const char* romNames[5] = { "PLUCK", "VOWEL", "HIT", "PAD", "USER" };
            juce::String chip = (sampleSel_ >= 0 && sampleSel_ < 4)
                ? juce::String(romNames[sampleSel_])
                : (name_.isNotEmpty() ? name_.toUpperCase() : juce::String("USER"));
            if (codec_.isNotEmpty()) chip << "   " << codec_;
            g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
            const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), chip) + 14.0f;
            auto chipR = juce::Rectangle<float>(b.getX() + 8, b.getY() + 7, tw, 17.0f);
            g.setColour(toColour(pal().panel).withAlpha(0.85f));
            g.fillRoundedRectangle(chipR, th::kSpacing.cornerS);
            g.setColour(toColour(pal().textPrimary));
            g.drawText(chip, chipR, juce::Justification::centred, false);
        }

        // drop hint bottom-right
        g.setColour(toColour(pal().textSecondary).withAlpha(0.75f));
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawText("drag audio here", getLocalBounds().reduced(10, 7),
                   juce::Justification::bottomRight, false);

        g.setColour(toColour(pal().outline));
        g.drawRoundedRectangle(b.reduced(0.5f), th::kSpacing.cornerM, 1.0f);
    }

    void mouseDown(juce::MouseEvent const& e) override
    {
        if (proc == nullptr) return;
        param()->beginChangeGesture();
        dragToPos(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (proc) dragToPos(e); }
    void mouseUp(const juce::MouseEvent&) override     { if (proc) param()->endChangeGesture(); }

private:
    juce::RangedAudioParameter* param() const { return proc->state().getParameter(pid::position); }
    void dragToPos(const juce::MouseEvent& e)
    {
        const float v = juce::jlimit(0.0f, 1.0f, ((float) e.x - 6.0f) / juce::jmax(1.0f, (float) getWidth() - 12.0f));
        param()->setValueNotifyingHost(v);
    }

    WaveGrainProcessor* proc = nullptr;
    std::vector<float> lo_, hi_, heads_, marks_;
    float pos_ = 0.0f;
    int sampleSel_ = 0;
    juce::String name_, codec_;
};

class SampleViewItem : public foleys::GuiItem, private juce::Timer
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(SampleViewItem)
    SampleViewItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node)
        : foleys::GuiItem(b, node), view(processorOf(b))
    {
        addAndMakeVisible(view);
        startTimerHz(30);
    }
    ~SampleViewItem() override { stopTimer(); }
    juce::Component* getWrappedComponent() override { return &view; }
    void update() override {}
private:
    void timerCallback() override { view.refresh(); }
    WgSampleView view;
};

// =============================================================================
//  Sigil: abstract grain-cluster identity mark + BETA tag. Stealth-beta: no
//  title text anywhere — the accent + sigil carry the identity.
// =============================================================================
class WgSigil : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float side = juce::jmin(b.getHeight(), 34.0f);
        auto s = juce::Rectangle<float>(b.getX(), b.getCentreY() - side * 0.5f, side, side);

        // deterministic grain-cluster glyph: dots on a golden-angle spiral
        const float cx = s.getCentreX(), cy = s.getCentreY();
        for (int i = 0; i < 13; ++i)
        {
            const float t  = (float) i / 12.0f;
            const float ang = 2.39996f * (float) i;               // golden angle
            const float rad = 2.0f + t * side * 0.42f;
            const float r   = 1.1f + 2.2f * (1.0f - t);
            g.setColour(toColour(pal().accent).withAlpha(0.35f + 0.65f * (1.0f - t)));
            g.fillEllipse(cx + std::cos(ang) * rad - r, cy + std::sin(ang) * rad - r, r * 2, r * 2);
        }

        // BETA tag
        auto tag = juce::Rectangle<float>(s.getRight() + 8.0f, cy - 8.0f, 36.0f, 16.0f);
        g.setColour(toColour(pal().accent));
        g.fillRoundedRectangle(tag, th::kSpacing.cornerS);
        g.setColour(toColour(pal().background));
        g.setFont(juce::Font(juce::FontOptions(th::kType.beta).withStyle("Bold")));
        g.drawText("BETA", tag, juce::Justification::centred, false);
    }
};

class SigilItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(SigilItem)
    SigilItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node) : foleys::GuiItem(b, node)
    { addAndMakeVisible(sigil); }
    juce::Component* getWrappedComponent() override { return &sigil; }
    void update() override {}
private:
    WgSigil sigil;
};

// =============================================================================
//  SyncKnob: the frozen three-param convention rendered as ONE knob that swaps
//  its attachment between <base>Ms and <base>Div + a ms/beat chip. Optional
//  mod-a / mod-b attributes add animated LFO depth rings (acv2::ui::ModRing).
// =============================================================================
class WgSyncKnob : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::AudioProcessorValueTreeState::Listener,
                   private juce::Timer
{
public:
    WgSyncKnob(WaveGrainProcessor* p, const juce::String& base,
               const juce::String& modA, const juce::String& modB)
        : proc(p), base_(base), modAId_(modA), modBId_(modB)
    {
        msSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        msSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 15);
        divSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        divSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 15);
        divSlider.textFromValueFunction = [](double v)
            { return juce::String(acv2::tempo::label((int) std::lround(v))); };
        divSlider.valueFromTextFunction = [](const juce::String& t)
            { const int i = acv2::tempo::indexOfLabel(t.trim().toStdString()); return (double) (i >= 0 ? i : 0); };
        addAndMakeVisible(msSlider);
        addChildComponent(divSlider);

        chip.setClickingTogglesState(true);
        addAndMakeVisible(chip);

        if (proc != nullptr)
        {
            auto& st = proc->state();
            msAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, base_ + "Ms", msSlider);
            divAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, base_ + "Div", divSlider);
            chipAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(st, base_ + "Sync", chip);
            st.addParameterListener(base_ + "Sync", this);

            if (modAId_.isNotEmpty())
            {
                ringA = std::make_unique<acv2::ui::ModRing>(proc->lfoAValue(), msSlider, pal());
                addAndMakeVisible(*ringA);
            }
            if (modBId_.isNotEmpty())
            {
                ringB = std::make_unique<acv2::ui::ModRing>(proc->lfoBValue(), msSlider, pal());
                addAndMakeVisible(*ringB);
            }
            startTimerHz(20);
            syncChanged(st.getRawParameterValue(base_ + "Sync")->load() > 0.5f);
        }
    }

    ~WgSyncKnob() override
    {
        stopTimer();
        if (proc != nullptr) proc->state().removeParameterListener(base_ + "Sync", this);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto chipR = r.removeFromBottom(16).reduced(juce::jmax(0, r.getWidth() / 2 - 24), 0);
        chip.setBounds(chipR);
        msSlider.setBounds(r);
        divSlider.setBounds(r);
        auto knobArea = r.withHeight(juce::jmax(10, r.getHeight() - 15));   // above textbox
        if (ringA) ringA->setBounds(knobArea);
        if (ringB) ringB->setBounds(knobArea);
    }

private:
    void parameterChanged(const juce::String&, float v) override
    {
        juce::MessageManager::callAsync([sp = juce::Component::SafePointer<WgSyncKnob>(this), v]
            { if (sp != nullptr) sp->syncChanged(v > 0.5f); });
    }
    void syncChanged(bool beats)
    {
        msSlider.setVisible(!beats);
        divSlider.setVisible(beats);
        chip.setButtonText(beats ? "beat" : "ms");
    }
    void timerCallback() override
    {
        if (proc == nullptr) return;
        if (ringA && modAId_.isNotEmpty())
            ringA->setDepth(proc->state().getRawParameterValue(modAId_)->load());
        if (ringB && modBId_.isNotEmpty())
            ringB->setDepth(proc->state().getRawParameterValue(modBId_)->load());
    }

    WaveGrainProcessor* proc;
    juce::String base_, modAId_, modBId_;
    juce::Slider msSlider, divSlider;
    juce::ToggleButton chip { "ms" };   // ToggleButton so LnfV2 draws it as a chip
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> msAtt, divAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chipAtt;
    std::unique_ptr<acv2::ui::ModRing> ringA, ringB;
};

class SyncKnobItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(SyncKnobItem)
    SyncKnobItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node) : foleys::GuiItem(b, node)
    { update(); }

    juce::Component* getWrappedComponent() override { return knob.get(); }

    void update() override
    {
        const auto base = configNode.getProperty("base", "rate").toString();
        const auto modA = configNode.getProperty("mod-a", "").toString();
        const auto modB = configNode.getProperty("mod-b", "").toString();
        if (knob == nullptr || base != curBase || modA != curA || modB != curB)
        {
            curBase = base; curA = modA; curB = modB;
            knob = std::make_unique<WgSyncKnob>(processorOf(magicBuilder), base, modA, modB);
            addAndMakeVisible(*knob);
            resized();
        }
    }

    std::vector<foleys::SettableProperty> getSettableProperties() const override
    {
        return {
            { configNode, "base",  foleys::SettableProperty::Text, "rate", {} },
            { configNode, "mod-a", foleys::SettableProperty::Text, "", {} },
            { configNode, "mod-b", foleys::SettableProperty::Text, "", {} },
        };
    }

private:
    std::unique_ptr<WgSyncKnob> knob;
    juce::String curBase, curA, curB;
};

// =============================================================================
//  ModKnob: plain destination knob + animated LFO A/B depth rings.
// =============================================================================
class WgModKnob : public juce::Component,
                  public juce::SettableTooltipClient,
                  private juce::Timer
{
public:
    WgModKnob(WaveGrainProcessor* p, const juce::String& paramId,
              const juce::String& modA, const juce::String& modB)
        : proc(p), modAId_(modA), modBId_(modB)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 15);
        addAndMakeVisible(slider);
        if (proc != nullptr && paramId.isNotEmpty() && proc->state().getParameter(paramId) != nullptr)
        {
            att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc->state(), paramId, slider);
            if (modAId_.isNotEmpty())
            { ringA = std::make_unique<acv2::ui::ModRing>(proc->lfoAValue(), slider, pal()); addAndMakeVisible(*ringA); }
            if (modBId_.isNotEmpty())
            { ringB = std::make_unique<acv2::ui::ModRing>(proc->lfoBValue(), slider, pal()); addAndMakeVisible(*ringB); }
            startTimerHz(20);
        }
    }
    ~WgModKnob() override { stopTimer(); }

    void resized() override
    {
        slider.setBounds(getLocalBounds());
        auto knobArea = getLocalBounds().withHeight(juce::jmax(10, getHeight() - 15));
        if (ringA) ringA->setBounds(knobArea);
        if (ringB) ringB->setBounds(knobArea);
    }

private:
    void timerCallback() override
    {
        if (proc == nullptr) return;
        if (ringA && modAId_.isNotEmpty()) ringA->setDepth(proc->state().getRawParameterValue(modAId_)->load());
        if (ringB && modBId_.isNotEmpty()) ringB->setDepth(proc->state().getRawParameterValue(modBId_)->load());
    }
    WaveGrainProcessor* proc;
    juce::String modAId_, modBId_;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    std::unique_ptr<acv2::ui::ModRing> ringA, ringB;
};

class ModKnobItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(ModKnobItem)
    ModKnobItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node) : foleys::GuiItem(b, node)
    { update(); }

    juce::Component* getWrappedComponent() override { return knob.get(); }

    void update() override
    {
        const auto id   = configNode.getProperty("parameter", "").toString();
        const auto modA = configNode.getProperty("mod-a", "").toString();
        const auto modB = configNode.getProperty("mod-b", "").toString();
        if (knob == nullptr || id != curId || modA != curA || modB != curB)
        {
            curId = id; curA = modA; curB = modB;
            knob = std::make_unique<WgModKnob>(processorOf(magicBuilder), id, modA, modB);
            addAndMakeVisible(*knob);
            resized();
        }
    }

    std::vector<foleys::SettableProperty> getSettableProperties() const override
    {
        return {
            { configNode, "parameter", foleys::SettableProperty::Choice, "", magicBuilder.createParameterMenuLambda() },
            { configNode, "mod-a", foleys::SettableProperty::Text, "", {} },
            { configNode, "mod-b", foleys::SettableProperty::Text, "", {} },
        };
    }

private:
    std::unique_ptr<WgModKnob> knob;
    juce::String curId, curA, curB;
};

// =============================================================================
//  SeedPanel: seed mode + (Fixed/Per-Key) seed knob OR (Random) last-rolled
//  seed readout, plus dice (reroll) + lock (freeze the last seed) buttons.
//  Mode-dependent visibility with internal reflow.
// =============================================================================
class WgSeedPanel : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::AudioProcessorValueTreeState::Listener,
                    private juce::Timer
{
public:
    explicit WgSeedPanel(WaveGrainProcessor* p) : proc(p)
    {
        mode.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(mode);
        seedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        seedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 15);
        addAndMakeVisible(seedSlider);

        lastLabel.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
        lastLabel.setColour(juce::Label::textColourId, toColour(pal().textPrimary));
        lastLabel.setJustificationType(juce::Justification::centredLeft);
        addChildComponent(lastLabel);

        dice.setTooltip("Reroll the global seed");
        lock.setTooltip("Lock the last-rolled seed (copies it into Fixed mode)");
        addAndMakeVisible(dice);
        addChildComponent(lock);

        if (proc != nullptr)
        {
            auto& st = proc->state();
            // the attachment only selects an item: the box has to be filled first
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(st.getParameter(pid::seedMode)))
                mode.addItemList(choice->choices, 1);
            modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(st, pid::seedMode, mode);
            seedAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, pid::seed, seedSlider);
            st.addParameterListener(pid::seedMode, this);

            dice.onClick = [this]
            {
                auto v = (float) (1 + (juce::Random::getSystemRandom().nextInt(999999)));
                proc->setParamRaw(pid::seed, v);
            };
            lock.onClick = [this]
            {
                const auto last = (int) (proc->lastUsedSeed() % 999999ull);
                proc->setParamRaw(pid::seed, (float) juce::jmax(1, last));
                proc->setParamRaw(pid::seedMode, 0.0f);   // Fixed
            };
            startTimerHz(10);
            modeChanged((int) st.getRawParameterValue(pid::seedMode)->load());
        }
    }
    ~WgSeedPanel() override
    {
        stopTimer();
        if (proc != nullptr) proc->state().removeParameterListener(pid::seedMode, this);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop(juce::jmin(22, r.getHeight() / 2));
        mode.setBounds(top.removeFromLeft(juce::jmax(80, getWidth() / 2 - 40)));
        top.removeFromLeft(4);
        dice.setBounds(top.removeFromLeft(30));
        top.removeFromLeft(4);
        lock.setBounds(top.removeFromLeft(30));
        r.removeFromTop(2);
        seedSlider.setBounds(r);
        lastLabel.setBounds(r);
    }

private:
    void parameterChanged(const juce::String&, float v) override
    {
        juce::MessageManager::callAsync([sp = juce::Component::SafePointer<WgSeedPanel>(this), v]
            { if (sp != nullptr) sp->modeChanged((int) v); });
    }
    void modeChanged(int m)
    {
        const bool random = (m == 2);
        seedSlider.setVisible(!random);
        lastLabel.setVisible(random);
        lock.setVisible(random);
        resized();
    }
    void timerCallback() override
    {
        if (proc == nullptr || !lastLabel.isVisible()) return;
        lastLabel.setText("rolled " + juce::String((juce::int64) (proc->lastUsedSeed() % 999999ull)),
                          juce::dontSendNotification);
    }

    WaveGrainProcessor* proc;
    juce::ComboBox mode;
    juce::Slider seedSlider;
    juce::Label lastLabel { {}, "rolled -" };
    juce::TextButton dice { juce::String::fromUTF8("\xE2\x9A\x84") },   // die glyph
                     lock { juce::String::fromUTF8("\xF0\x9F\x94\x92") }; // lock glyph
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>  seedAtt;
};

class SeedPanelItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(SeedPanelItem)
    SeedPanelItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node)
        : foleys::GuiItem(b, node), panel(processorOf(b))
    { addAndMakeVisible(panel); }
    juce::Component* getWrappedComponent() override { return &panel; }
    void update() override {}
private:
    WgSeedPanel panel;
};

// =============================================================================
//  CodecPanel: ROM-codec toggle + (when on) ratio/bits knobs. Off = knobs hide
//  and the panel collapses to the chip (mode-dependent visibility, UI-only).
// =============================================================================
class WgCodecPanel : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit WgCodecPanel(WaveGrainProcessor* p) : proc(p)
    {
        on.setClickingTogglesState(true);
        addAndMakeVisible(on);
        ratio.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        ratio.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 15);
        bits.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        bits.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 15);
        addChildComponent(ratio);
        addChildComponent(bits);

        if (proc != nullptr)
        {
            auto& st = proc->state();
            onAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(st, pid::codecOn, on);
            ratioAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, pid::romRatio, ratio);
            bitsAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(st, pid::romBits, bits);
            st.addParameterListener(pid::codecOn, this);
            onChanged(st.getRawParameterValue(pid::codecOn)->load() > 0.5f);
        }
    }
    ~WgCodecPanel() override
    { if (proc != nullptr) proc->state().removeParameterListener(pid::codecOn, this); }

    void resized() override
    {
        auto r = getLocalBounds();
        on.setBounds(r.removeFromTop(18).reduced(juce::jmax(0, getWidth() / 2 - 36), 0));
        r.removeFromTop(2);
        ratio.setBounds(r.removeFromLeft(r.getWidth() / 2));
        bits.setBounds(r);
    }

private:
    void parameterChanged(const juce::String&, float v) override
    {
        juce::MessageManager::callAsync([sp = juce::Component::SafePointer<WgCodecPanel>(this), v]
            { if (sp != nullptr) sp->onChanged(v > 0.5f); });
    }
    void onChanged(bool b)
    {
        ratio.setVisible(b);
        bits.setVisible(b);
        on.setButtonText(b ? "codec on" : "codec off");
    }

    WaveGrainProcessor* proc;
    juce::ToggleButton on { "codec off" };   // ToggleButton so LnfV2 draws it as a chip
    juce::Slider ratio, bits;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ratioAtt, bitsAtt;
};

class CodecPanelItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(CodecPanelItem)
    CodecPanelItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node)
        : foleys::GuiItem(b, node), panel(processorOf(b))
    { addAndMakeVisible(panel); }
    juce::Component* getWrappedComponent() override { return &panel; }
    void update() override {}
private:
    WgCodecPanel panel;
};

// =============================================================================
//  PresetBar: factory presets + user presets under ~/Music/<product>/Presets.
// =============================================================================
class WgPresetBar : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    explicit WgPresetBar(WaveGrainProcessor* p) : proc(p)
    {
        box.setTextWhenNothingSelected("presets");
        box.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(box);
        save.setTooltip("Save the current state as a named preset");
        addAndMakeVisible(save);
        rebuild();

        box.onChange = [this]
        {
            if (proc == nullptr) return;
            const int id = box.getSelectedId();
            if (id >= 1000)
            {
                const int i = id - 1000;
                if (i >= 0 && i < userFiles.size()) proc->loadPresetFile(userFiles[i]);
            }
            else if (id >= 1)
                proc->applyFactoryPreset(id - 1);
        };
        save.onClick = [this] { promptSave(); };
    }

    void resized() override
    {
        auto r = getLocalBounds();
        save.setBounds(r.removeFromRight(52));
        r.removeFromRight(4);
        box.setBounds(r);
    }

private:
    void rebuild()
    {
        box.clear(juce::dontSendNotification);
        const auto& fp = WaveGrainProcessor::factoryPresets();
        for (int i = 0; i < (int) fp.size(); ++i)
            box.addItem(fp[(size_t) i].name, i + 1);
        userFiles.clear();
        auto dir = WaveGrainProcessor::presetDirectory();
        if (dir.isDirectory())
        {
            auto files = dir.findChildFiles(juce::File::findFiles, false, "*.xml");
            files.sort();
            if (!files.isEmpty()) box.addSeparator();
            for (int i = 0; i < files.size(); ++i)
            {
                userFiles.add(files[i]);
                box.addItem(files[i].getFileNameWithoutExtension(), 1000 + i);
            }
        }
    }

    void promptSave()
    {
        auto* win = new juce::AlertWindow("Save preset", "Preset name:", juce::MessageBoxIconType::NoIcon);
        win->addTextEditor("name", "", {});
        win->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        win->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        win->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, win](int result)
            {
                if (result == 1 && proc != nullptr)
                {
                    const auto name = win->getTextEditorContents("name").trim();
                    if (name.isNotEmpty() && proc->savePresetFile(name)) rebuild();
                }
                delete win;
            }), false);
    }

    WaveGrainProcessor* proc;
    juce::ComboBox box;
    juce::TextButton save { "save" };
    juce::Array<juce::File> userFiles;
};

class PresetBarItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(PresetBarItem)
    PresetBarItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node)
        : foleys::GuiItem(b, node), bar(processorOf(b))
    { addAndMakeVisible(bar); }
    juce::Component* getWrappedComponent() override { return &bar; }
    void update() override {}
private:
    WgPresetBar bar;
};

// =============================================================================
//  Status: live voices/grains/bpm readout (mono figures, quiet).
// =============================================================================
class WgStatus : public juce::Component, private juce::Timer
{
public:
    explicit WgStatus(WaveGrainProcessor* p) : proc(p)
    {
        label.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain)));
        label.setColour(juce::Label::textColourId, toColour(pal().textSecondary));
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
        startTimerHz(8);
    }
    ~WgStatus() override { stopTimer(); }
    void resized() override { label.setBounds(getLocalBounds()); }
private:
    void timerCallback() override
    {
        if (proc == nullptr) return;
        label.setText("v " + juce::String(proc->activeVoices())
                    + "  g " + juce::String(proc->activeGrains())
                    + "  " + juce::String(proc->lastKnownBpm(), 1) + " bpm",
                      juce::dontSendNotification);
    }
    WaveGrainProcessor* proc;
    juce::Label label;
};

class StatusItem : public foleys::GuiItem
{
public:
    FOLEYS_DECLARE_GUI_FACTORY(StatusItem)
    StatusItem(foleys::MagicGUIBuilder& b, const juce::ValueTree& node)
        : foleys::GuiItem(b, node), status(processorOf(b))
    { addAndMakeVisible(status); }
    juce::Component* getWrappedComponent() override { return &status; }
    void update() override {}
private:
    WgStatus status;
};

// =============================================================================
void registerWaveGrainItems(foleys::MagicGUIBuilder& builder)
{
    builder.registerFactory("SampleView", &SampleViewItem::factory);
    builder.registerFactory("Sigil",      &SigilItem::factory);
    builder.registerFactory("SyncKnob",   &SyncKnobItem::factory);
    builder.registerFactory("ModKnob",    &ModKnobItem::factory);
    builder.registerFactory("SeedPanel",  &SeedPanelItem::factory);
    builder.registerFactory("CodecPanel", &CodecPanelItem::factory);
    builder.registerFactory("PresetBar",  &PresetBarItem::factory);
    builder.registerFactory("Status",     &StatusItem::factory);
    builder.registerLookAndFeel("LnfV2", std::make_unique<acv2::ui::LnfV2>(th::kWaveGrain));
}

// =============================================================================
//  Editor
// =============================================================================
WaveGrainEditor::WaveGrainEditor(WaveGrainProcessor& p, std::unique_ptr<foleys::MagicGUIBuilder> builder)
    : foleys::MagicPluginEditor(p.getMagicState(), std::move(builder)), proc(p)
{
}

WaveGrainEditor::~WaveGrainEditor()
{
#if JUCE_MAC
    overlay_.reset();
    overlayPeer_ = nullptr;
#endif
}

void WaveGrainEditor::parentHierarchyChanged()
{
#if JUCE_MAC
    auto* peer = getPeer();
    if (peer == overlayPeer_) return;
    overlay_.reset();
    overlayPeer_ = peer;
    if (peer != nullptr)
    {
        wgui::DropOverlay::Callbacks cbs;
        cbs.onFile = [sp = juce::Component::SafePointer<WaveGrainEditor>(this)](const juce::String& path)
        {
            if (sp == nullptr) return;
            juce::File f(path);
            if (f.existsAsFile() && isAudioFile(path))
                sp->proc.loadUserSample(f);
        };
        cbs.onHighlight = [sp = juce::Component::SafePointer<WaveGrainEditor>(this)](bool over)
        {
            if (sp != nullptr) sp->setDragOver(over);
        };
        overlay_ = std::make_unique<wgui::DropOverlay>(*peer, std::move(cbs));
    }
#endif
}

bool WaveGrainEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile(f)) return true;
    return false;
}

void WaveGrainEditor::filesDropped(const juce::StringArray& files, int, int)
{
    setDragOver(false);
    if (!files.isEmpty()) proc.loadUserSample(juce::File(files[0]));
}

void WaveGrainEditor::fileDragEnter(const juce::StringArray&, int, int) { setDragOver(true); }
void WaveGrainEditor::fileDragExit(const juce::StringArray&)            { setDragOver(false); }

void WaveGrainEditor::setDragOver(bool b)
{
    if (dragOver_ == b) return;
    dragOver_ = b;
    repaint();
}

void WaveGrainEditor::paintOverChildren(juce::Graphics& g)
{
    if (!dragOver_) return;
    auto b = getLocalBounds().toFloat();
    g.setColour(toColour(pal().accent).withAlpha(0.10f));
    g.fillAll();
    g.setColour(toColour(pal().accent));
    g.drawRoundedRectangle(b.reduced(3.0f), th::kSpacing.cornerM, 2.5f);

    auto chip = juce::Rectangle<float>(0, 0, 190.0f, 34.0f).withCentre(b.getCentre());
    g.setColour(toColour(pal().panel));
    g.fillRoundedRectangle(chip, th::kSpacing.cornerM);
    g.setColour(toColour(pal().accent));
    g.drawRoundedRectangle(chip, th::kSpacing.cornerM, 1.5f);
    g.setColour(toColour(pal().textPrimary));
    g.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
    g.drawText("drop audio to load", chip, juce::Justification::centred, false);
}
