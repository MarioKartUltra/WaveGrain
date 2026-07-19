// =============================================================================
//  WaveGrain — PluginEditor.cpp
// =============================================================================
#include "PluginEditor.h"

void SampleDisplay::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0c0f16)); g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0xff222c3a)); g.drawHorizontalLine((int) b.getCentreY(), b.getX(), b.getRight());

    const int n = (int) lo_.size();
    if (n > 1 && (int) hi_.size() == n)
    {
        const float midY = b.getCentreY(), h = b.getHeight() * 0.46f, w = b.getWidth();
        g.setColour(juce::Colour(0xff3a6f60));
        for (int x = 0; x < (int) w; ++x)
        {
            const int i = (int) ((double) x / w * (n - 1));
            const float y0 = midY - juce::jlimit(-1.0f, 1.0f, hi_[(size_t) i]) * h;
            const float y1 = midY - juce::jlimit(-1.0f, 1.0f, lo_[(size_t) i]) * h;
            g.drawLine(b.getX() + x, y0, b.getX() + x, y1, 1.0f);
        }
    }
    // selected region around Position
    const float W = b.getWidth(), X = b.getX();
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.fillRect(juce::Rectangle<float>(X + pos_ * W, b.getY(), juce::jmax(2.0f, span_ * W), b.getHeight()));
    // live playheads
    g.setColour(juce::Colour(0xff5fd0b6).withAlpha(0.85f));
    for (float hd : heads_) g.fillRect(juce::Rectangle<float>(X + hd * W - 0.5f, b.getY(), 1.5f, b.getHeight()));

    g.setColour(juce::Colour(0xff222c3a)); g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.55f)); g.setFont(11.0f);
    g.drawText("sample (drag audio here) — playheads = live grain clouds",
               getLocalBounds().reduced(5), juce::Justification::topLeft);
}

// -----------------------------------------------------------------------------
WaveGrainEditor::WaveGrainEditor(WaveGrainProcessor& p)
    : juce::AudioProcessorEditor(&p), proc(p),
      keyboard(p.keyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    title.setText("WaveGrain  ·  DWT granular ROMpler", juce::dontSendNotification);
    title.setFont(juce::Font(17.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colour(0xff5fd0b6));
    addAndMakeVisible(title);
    status.setColour(juce::Label::textColourId, juce::Colour(0xffb0c0d0));
    status.setFont(juce::Font(12.0f));
    addAndMakeVisible(status);
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Load a sample", juce::File(),
                                                      "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc){ auto f = fc.getResult(); if (f.existsAsFile()) proc.loadUserSample(f); });
    };
    panicBtn.onClick = [this]{ proc.panic(); };
    addAndMakeVisible(loadBtn); addAndMakeVisible(panicBtn); addAndMakeVisible(display);
    keyboard.setAvailableRange(36, 96); keyboard.setLowestVisibleKey(48);
    addAndMakeVisible(keyboard);

    // Grain
    addChoice(pid::sample, "Sample", { "Pluck","Vowel","Hit","Pad","User" }, 0);
    addSlider(pid::root, "Root Note", 0);
    addChoice(pid::grainSize, "Grain Size", { "256","512","1024","2048","4096" }, 0);
    addSlider(pid::density, "Density", 0);
    addSlider(pid::position, "Position", 0);
    addSlider(pid::scan, "Scan", 0);
    addToggle(pid::freeze, "Freeze", 0);
    addSlider(pid::spray, "Spray", 0);
    addSlider(pid::pitchFine, "Fine Pitch", 0);
    addSlider(pid::spread, "Spread", 0);
    addChoice(pid::window, "Window", { "Hann","Tukey","Gauss","Tri" }, 0);
    // Wavelet
    addChoice(pid::wavelet, "Wavelet", { "Haar","Db2","Db4","Db6" }, 1);
    addSlider(pid::compress, "Compression", 1);
    addSlider(pid::tilt, "Scale Tilt", 1);
    addSlider(pid::bandLow, "Band Low", 1);
    addSlider(pid::bandHigh, "Band High", 1);
    addSlider(pid::octave, "DWT Octave", 1);
    addToggle(pid::codecOn, "ROM Codec", 1);
    addSlider(pid::romRatio, "ROM Ratio", 1);
    addSlider(pid::romBits, "ROM Bits", 1);
    // Amp / Filter
    addSlider(pid::attack, "Attack", 2);
    addSlider(pid::decay, "Decay", 2);
    addSlider(pid::sustain, "Sustain", 2);
    addSlider(pid::release, "Release", 2);
    addSlider(pid::cutoff, "Cutoff", 2);
    addSlider(pid::reso, "Resonance", 2);
    // Output
    addSlider(pid::output, "Output", 3);
    addSlider(pid::voices, "Polyphony", 3);

    setSize(1120, 680);
    startTimerHz(30);
}

void WaveGrainEditor::addSlider(const char* id, const juce::String& name, int group)
{
    Ctrl r; r.group = group;
    auto s = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
    s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
    s->setColour(juce::Slider::trackColourId, juce::Colour(0xff5fd0b6));
    addAndMakeVisible(*s);
    r.sa = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.state(), id, *s);
    r.c = std::move(s);
    r.l = std::make_unique<juce::Label>(); r.l->setText(name, juce::dontSendNotification);
    r.l->setFont(juce::Font(12.0f)); r.l->setColour(juce::Label::textColourId, juce::Colour(0xff90a0b0));
    addAndMakeVisible(*r.l);
    ctrls.push_back(std::move(r));
}
void WaveGrainEditor::addChoice(const char* id, const juce::String& name, juce::StringArray items, int group)
{
    Ctrl r; r.group = group;
    auto cb = std::make_unique<juce::ComboBox>(); cb->addItemList(items, 1);
    addAndMakeVisible(*cb);
    r.ca = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.state(), id, *cb);
    r.c = std::move(cb);
    r.l = std::make_unique<juce::Label>(); r.l->setText(name, juce::dontSendNotification);
    r.l->setFont(juce::Font(12.0f)); r.l->setColour(juce::Label::textColourId, juce::Colour(0xff90a0b0));
    addAndMakeVisible(*r.l);
    ctrls.push_back(std::move(r));
}
void WaveGrainEditor::addToggle(const char* id, const juce::String& name, int group)
{
    Ctrl r; r.group = group;
    auto tb = std::make_unique<juce::ToggleButton>(name);
    tb->setColour(juce::ToggleButton::textColourId, juce::Colour(0xff90a0b0));
    addAndMakeVisible(*tb);
    r.ba = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.state(), id, *tb);
    r.c = std::move(tb);
    r.l = std::make_unique<juce::Label>();
    addAndMakeVisible(*r.l);
    ctrls.push_back(std::move(r));
}

bool WaveGrainEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files) if (f.matchesWildcard("*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a;*.caf", true)) return true;
    return false;
}
void WaveGrainEditor::filesDropped(const juce::StringArray& files, int, int)
{ if (!files.isEmpty()) proc.loadUserSample(juce::File(files[0])); }

void WaveGrainEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0b0e14));
    g.setColour(juce::Colour(0xff5fd0b6)); g.setFont(juce::Font(13.0f, juce::Font::bold));
    for (int gi = 0; gi < kGroups; ++gi)
        g.drawText(groupNames[gi], groupX[gi], groupY, 200, 16, juce::Justification::centredLeft);
}

void WaveGrainEditor::resized()
{
    auto area = getLocalBounds().reduced(8);
    keyboard.setBounds(area.removeFromBottom(64)); area.removeFromBottom(6);

    auto top = area.removeFromTop(28);
    title.setBounds(top.removeFromLeft(260)); top.removeFromLeft(6);
    loadBtn.setBounds(top.removeFromLeft(70));
    panicBtn.setBounds(top.removeFromRight(70)); top.removeFromRight(6);
    status.setBounds(top);

    display.setBounds(area.removeFromTop(150)); area.removeFromTop(8);

    groupY = area.getY();
    const int colW = area.getWidth() / kGroups;
    for (int gi = 0; gi < kGroups; ++gi)
    {
        auto col = area.removeFromLeft(colW).reduced(4, 0);
        groupX[gi] = col.getX(); col.removeFromTop(20);
        auto place = [&](juce::Label& l, juce::Component& ctrl)
        { auto row = col.removeFromTop(28); l.setBounds(row.removeFromLeft(92)); ctrl.setBounds(row.reduced(0, 3)); };
        for (auto& cc : ctrls) if (cc.group == gi) place(*cc.l, *cc.c);
    }
}

void WaveGrainEditor::timerCallback()
{
    std::vector<float> lo, hi, heads;
    proc.copyOverview(lo, hi, 400);
    proc.copyPlayheads(heads);
    const float pos = proc.state().getRawParameterValue(pid::position)->load();
    display.setData(std::move(lo), std::move(hi), std::move(heads), pos, 0.02f);

    juce::String nm = proc.loadedName();
    juce::String codec = proc.codecText();
    status.setText((nm.isEmpty() ? juce::String("built-in ROM") : nm)
                   + "   voices: " + juce::String(proc.activeVoices())
                   + "   grains: " + juce::String(proc.activeGrains())
                   + (codec.isEmpty() ? juce::String() : "   " + codec),
                   juce::dontSendNotification);
}
