// =============================================================================
//  wavegrain_shot — offscreen UI screenshot tool for WaveGrain II.
//  Links the plugin shared-code target, constructs the processor + editor,
//  drives real audio/MIDI, pumps the macOS run loop so foleys/JUCE timers fire,
//  and writes one 2x PNG per UI state. No bundle is built or installed.
//
//  Usage: wavegrain_shot [output-directory]
// =============================================================================
// NB: deliberately not <JuceHeader.h> — that pulls in the plugin's BinaryData.h,
// whose include dir is not propagated by the shared-code target. PluginProcessor.h
// already brings in juce_audio_utils (and therefore juce_graphics/gui_basics).
#include "PluginProcessor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <initializer_list>
#include <vector>
#include <cmath>

namespace
{

// JUCE 8 defaults JUCE_MODAL_LOOPS_PERMITTED=0, so runDispatchLoopUntil is not
// available. JUCE's macOS message queue is a CFRunLoopSource on the main run
// loop, so CFRunLoopRunInMode dispatches timers / async updaters / attachments.
void pumpMs (int ms)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + (double) ms;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
}

struct BlankStats
{
    double mean = 0.0, stddev = 0.0;
    int distinct = 0;
};

BlankStats measure (const juce::Image& img)
{
    BlankStats s;
    if (! img.isValid()) return s;

    const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
    std::vector<unsigned char> lumaHist (256, 0);
    std::vector<unsigned char> colourSeen (1 << 15, 0);   // 5:5:5 buckets
    double sum = 0.0, sumSq = 0.0;
    juce::int64 n = 0;

    const int stepX = juce::jmax (1, img.getWidth()  / 700);
    const int stepY = juce::jmax (1, img.getHeight() / 700);

    for (int y = 0; y < img.getHeight(); y += stepY)
        for (int x = 0; x < img.getWidth(); x += stepX)
        {
            const auto c = bd.getPixelColour (x, y);
            const double l = 0.2126 * c.getRed() + 0.7152 * c.getGreen() + 0.0722 * c.getBlue();
            sum += l; sumSq += l * l; ++n;
            lumaHist[(size_t) juce::jlimit (0, 255, (int) l)] = 1;
            const int bucket = ((c.getRed() >> 3) << 10) | ((c.getGreen() >> 3) << 5) | (c.getBlue() >> 3);
            colourSeen[(size_t) bucket] = 1;
        }

    if (n > 0)
    {
        s.mean = sum / (double) n;
        s.stddev = std::sqrt (juce::jmax (0.0, sumSq / (double) n - s.mean * s.mean));
    }
    for (auto v : colourSeen) s.distinct += (int) v;
    return s;
}

int failures = 0;

void writeShot (juce::AudioProcessorEditor& ed, const juce::File& dir,
                const juce::String& fileName, const juce::String& stateName)
{
    auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f);
    const auto st = measure (img);

    auto out = dir.getChildFile (fileName);
    out.deleteFile();
    juce::FileOutputStream os (out);
    bool ok = os.openedOk();
    if (ok)
    {
        juce::PNGImageFormat png;
        ok = png.writeImageToStream (img, os);
        os.flush();
    }

    // NB: the WaveGrain fascia is a deliberately narrow amber-on-charcoal palette,
    // so a healthy shot only lands ~300 distinct 5:5:5 colour buckets. Uniform-fill
    // failures collapse to stddev ~0 and a handful of buckets.
    const bool blank = (st.stddev <= 12.0 || st.distinct <= 64 || out.getSize() < 20000);
    juce::String line;
    line << "STATE " << stateName << " " << img.getWidth() << "x" << img.getHeight()
         << " variance=" << juce::String (st.stddev, 2)
         << " distinct=" << st.distinct
         << " mean=" << juce::String (st.mean, 2)
         << " bytes=" << (juce::int64) out.getSize()
         << " -> " << out.getFullPathName();
    std::printf ("%s\n", line.toRawUTF8());

    if (! ok)      { std::printf ("FAIL %s: could not write PNG\n", stateName.toRawUTF8()); ++failures; }
    else if (blank){ std::printf ("FAIL %s: image is effectively uniform (stddev=%.2f distinct=%d)\n",
                                  stateName.toRawUTF8(), st.stddev, st.distinct); ++failures; }
    std::fflush (stdout);
}

// -----------------------------------------------------------------------------
//  Control wiring self-test (--selftest): operate every control that is on
//  screen and check that exactly one parameter follows it. Run once as opened
//  and once with the codec on and every sync chip flipped, so the controls that
//  only appear in those states are covered. Finishes by listing the parameters
//  no control reached.
// -----------------------------------------------------------------------------
// The editor stays offscreen (no peer), so isShowing() is always false: walk
// only through visible components instead.
template <typename T>
void collectShowing (juce::Component& c, std::vector<T*>& out)
{
    if (! c.isVisible()) return;
    if (auto* t = dynamic_cast<T*> (&c))
        if (t->getWidth() >= 12 && t->getHeight() >= 12)
            out.push_back (t);
    for (int i = 0; i < c.getNumChildComponents(); ++i)
        collectShowing (*c.getChildComponent (i), out);
}

struct WiringTest
{
    WaveGrainProcessor& proc;
    juce::StringArray   reached;
    int                 failures = 0;

    std::vector<float> snapshot() const
    {
        std::vector<float> v;
        for (auto* p : proc.getParameters()) v.push_back (p->getValue());
        return v;
    }

    // returns the IDs of the parameters that differ from the snapshot
    juce::StringArray changedSince (const std::vector<float>& before) const
    {
        juce::StringArray ids;
        const auto& params = proc.getParameters();
        for (int i = 0; i < params.size(); ++i)
            if (std::abs (params[i]->getValue() - before[(size_t) i]) > 1.0e-6f)
                if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]))
                    ids.add (withId->paramID);
        return ids;
    }

    void expectOne (const juce::String& what, const juce::StringArray& changed)
    {
        if (changed.size() == 1)
        {
            reached.addIfNotAlreadyThere (changed[0]);
            return;
        }
        ++failures;
        std::printf ("  FAIL %s changed %d parameters [%s]\n", what.toRawUTF8(), changed.size(),
                     changed.joinIntoString (", ").toRawUTF8());
    }

    void run (juce::Component& editor, const char* pass)
    {
        std::vector<juce::Button*>   buttons, toggles;
        std::vector<juce::Slider*>   sliders;
        std::vector<juce::ComboBox*> combos;
        collectShowing (editor, buttons);
        collectShowing (editor, sliders);
        collectShowing (editor, combos);
        for (auto* b : buttons)                      // dice / lock / save only fire an action
            if (b->getClickingTogglesState())
                toggles.push_back (b);
        std::printf ("  [%s] on screen: %d toggles, %d knobs, %d menus\n", pass,
                     (int) toggles.size(), (int) sliders.size(), (int) combos.size());

        for (auto* t : toggles)
        {
            const auto label = "toggle '" + t->getButtonText() + "'";
            const bool wasOn = t->getToggleState();
            auto before = snapshot();
            t->triggerClick();  pumpMs (60);
            expectOne (label, changedSince (before));
            if (t->getToggleState() == wasOn)
            { ++failures; std::printf ("  FAIL %s did not change state when clicked\n", label.toRawUTF8()); }

            t->triggerClick();  pumpMs (60);                  // and back again
            if (! changedSince (before).isEmpty() || t->getToggleState() != wasOn)
            { ++failures; std::printf ("  FAIL %s did not return to its old state\n", label.toRawUTF8()); }
        }

        for (auto* s : sliders)
        {
            const auto   range = s->getRange();
            const double old   = s->getValue();
            const bool   low   = old < range.getStart() + 0.5 * range.getLength();
            auto before = snapshot();
            s->setValue (range.getStart() + (low ? 0.8 : 0.2) * range.getLength(), juce::sendNotificationSync);
            pumpMs (30);
            expectOne ("knob showing '" + s->getTextFromValue (old) + "'", changedSince (before));
            s->setValue (old, juce::sendNotificationSync);
            pumpMs (30);
        }

        for (auto* c : combos)
        {
            if (c->getNumItems() < 2) continue;
            const int old = c->getSelectedItemIndex();
            if (old < 0) continue;                            // the preset picker: nothing selected
            auto before = snapshot();
            c->setSelectedItemIndex ((old + 1) % c->getNumItems(), juce::sendNotificationSync);
            pumpMs (60);
            expectOne ("menu showing '" + c->getItemText (old) + "'", changedSince (before));
            c->setSelectedItemIndex (old, juce::sendNotificationSync);
            pumpMs (60);
        }
    }
};

// Which dragged files the editor says it will take.
int testFileDrop (juce::AudioProcessorEditor& ed)
{
    int bad = 0;
    auto* target = dynamic_cast<juce::FileDragAndDropTarget*> (&ed);
    if (target == nullptr) { std::printf ("  FAIL the editor is not a file drop target\n"); return 1; }

    const auto expect = [&] (const char* path, bool wanted)
    {
        if (target->isInterestedInFileDrag ({ path }) == wanted) return;
        ++bad;
        std::printf ("  FAIL '%s' should be %s\n", path, wanted ? "accepted" : "refused");
    };
    expect ("/Users/someone/Samples/kick.wav",   true);
    expect ("/Users/someone/Samples/LOOP.AIFF",  true);
    expect ("/Users/someone/Samples/pad.flac",   true);
    expect ("/Users/someone/Samples/vox.m4a",    true);
    expect ("/Users/someone/Samples/notes.txt",  false);
    expect ("/Users/someone/Samples/wav",        false);
    std::printf ("  [file drop] %d failures\n", bad);
    return bad;
}

int runSelfTest()
{
    WaveGrainProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    if (ed == nullptr) { std::printf ("FAIL no editor\n"); return 1; }
    ed->setVisible (true);
    pumpMs (900);

    WiringTest test { proc, {}, 0 };
    test.failures += testFileDrop (*ed);
    test.run (*ed, "as opened");

    for (auto* id : { "codecOn", "rateSync", "sizeSync", "lfoARateSync", "lfoBRateSync" })
        proc.setParamRaw (id, 1.0f);
    pumpMs (250);
    test.run (*ed, "codec on, time knobs in beats");

    juce::StringArray unreached;
    for (auto* p : proc.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
            if (! test.reached.contains (withId->paramID))
                unreached.add (withId->paramID);

    std::printf ("\nparameters driven by a control: %d of %d\n", test.reached.size(), proc.getParameters().size());
    std::printf ("parameters with no knob/toggle/menu: %s\n",
                 unreached.isEmpty() ? "none" : unreached.joinIntoString (", ").toRawUTF8());
    std::printf ("=== selftest %s (%d failures) ===\n", test.failures == 0 ? "PASSED" : "FAILED", test.failures);

    ed.reset();
    return test.failures == 0 ? 0 : 1;
}

} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);     // keep the log if the run is killed

    if (argc > 1 && juce::String (argv[1]) == "--selftest")
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        return runSelfTest();
    }

    const juce::String outPath = (argc > 1)
        ? juce::String (juce::CharPointer_UTF8 (argv[1]))
        : juce::String ("/private/tmp/claude-501/-Users-amasaryk-projects/"
                        "fc9277dc-03c0-456c-8d90-535220843ea7/scratchpad/shots/WaveGrain");

    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::File outDir (outPath);
    outDir.createDirectory();
    if (! outDir.isDirectory())
    {
        std::printf ("FAIL: cannot create output directory %s\n", outDir.getFullPathName().toRawUTF8());
        return 2;
    }

    WaveGrainProcessor proc;
    proc.setPlayConfigDetails (0, 2, 48000.0, 512);
    proc.prepareToPlay (48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    if (ed == nullptr) { std::printf ("FAIL: createEditor() returned null\n"); return 2; }
    ed->setVisible (true);
    pumpMs (900);   // foleys parses magic.xml and builds the GUI here

    if (auto* size = std::getenv ("WAVEGRAIN_SHOT_SIZE"))    // e.g. 1040x720 to check the minimum size
    {
        const auto wh = juce::StringArray::fromTokens (juce::String (size), "x", {});
        if (wh.size() == 2) ed->setSize (wh[0].getIntValue(), wh[1].getIntValue());
        pumpMs (300);
    }

    std::printf ("editor size: %d x %d\n", ed->getWidth(), ed->getHeight());
    if (ed->getWidth() <= 0 || ed->getHeight() <= 0)
    {
        ed->setSize (1180, 760);
        pumpMs (300);
        std::printf ("editor size after forced setSize: %d x %d\n", ed->getWidth(), ed->getHeight());
    }
    if (ed->getWidth() <= 0 || ed->getHeight() <= 0)
    {
        std::printf ("FAIL: editor has zero size\n");
        ed.reset();
        return 2;
    }

    juce::AudioBuffer<float> buf (2, 512);

    auto render = [&] (int blocks, std::initializer_list<int> notes = {})
    {
        for (int i = 0; i < blocks; ++i)
        {
            buf.clear();
            juce::MidiBuffer mb;
            if (i == 0)
                for (int n : notes)
                    mb.addEvent (juce::MidiMessage::noteOn (1, n, 0.85f), 0);
            proc.processBlock (buf, mb);
        }
    };

    auto reset = [&]
    {
        proc.panic();
        proc.getMagicState().getKeyboardState().allNotesOff (1);
        pumpMs (80);
    };

    auto set = [&] (const char* id, float v) { proc.setParamRaw (id, v); };

    // Baseline "dense cloud" parameter set shared by several states.
    auto densityBase = [&]
    {
        set (pid::sample,   3.0f);      // Pad
        set (pid::root,     48.0f);
        set (pid::sizeMs,   80.0f);
        set (pid::rateMs,   6.0f);
        set (pid::spray,    0.45f);
        set (pid::spread,   0.9f);
        set (pid::scan,     0.10f);
        set (pid::position, 0.20f);
        set (pid::attack,   0.05f);
        set (pid::sustain,  0.9f);
        set (pid::release,  2.0f);
        set (pid::voices,   8.0f);
        set (pid::compress, 0.25f);
        set (pid::tilt,     0.42f);
    };

    // staggered chord + steady-state settle, leaving voices ACTIVE at snapshot time
    auto driveChord = [&] (std::initializer_list<int> notes, int blocksPerNote)
    {
        for (int n : notes) { render (blocksPerNote, { n }); pumpMs (50); }
        for (int k = 0; k < 8; ++k) { render (12); pumpMs (40); }
        render (8);
        pumpMs (180);
    };

    // ---------------------------------------------------------------- state 1
    {
        reset();
        proc.applyFactoryPreset (0);
        densityBase();
        pumpMs (450);
        driveChord ({ 48, 52, 55, 60 }, 24);
        writeShot (*ed, outDir, "01_default-grain-cloud.png", "1. Default grain cloud (Pad ROM, codec OFF)");
    }

    // ---------------------------------------------------------------- state 2
    {
        reset();
        proc.applyFactoryPreset (4);            // Crushed ROM
        set (pid::sizeMs,   80.0f);
        set (pid::rateMs,   6.0f);
        set (pid::spray,    0.45f);
        set (pid::spread,   0.9f);
        set (pid::scan,     0.08f);
        set (pid::position, 0.15f);
        set (pid::root,     48.0f);
        set (pid::voices,   8.0f);
        set (pid::attack,   0.05f);
        set (pid::sustain,  0.9f);
        set (pid::release,  2.0f);
        pumpMs (700);                           // 15 Hz timer runs waveletEncode -> codecText_
        std::printf ("codec text: '%s'\n", proc.codecText().toRawUTF8());
        driveChord ({ 48, 52, 55, 60 }, 24);
        writeShot (*ed, outDir, "02_rom-codec-engaged.png", "2. ROM codec engaged (codec readout + expanded panel)");
    }

    // ---------------------------------------------------------------- state 3
    {
        reset();
        proc.applyFactoryPreset (0);
        densityBase();
        set (pid::rateSync,     1.0f);
        set (pid::rateDiv,      0.0f);          // 1/32
        set (pid::sizeSync,     1.0f);
        set (pid::sizeDiv,      0.0f);
        set (pid::lfoARateSync, 1.0f);
        set (pid::lfoARateDiv,  2.0f);          // 1/16
        set (pid::lfoBRateSync, 1.0f);
        set (pid::lfoBRateDiv, 13.0f);          // 1/1
        set (pid::voices,      16.0f);
        pumpMs (450);
        driveChord ({ 36, 43, 48, 52, 55, 60, 64, 67 }, 16);
        writeShot (*ed, outDir, "03_tempo-sync-everywhere.png", "3. Tempo-sync everywhere (SyncKnobs on beat divisions)");
    }

    // ---------------------------------------------------------------- state 4
    {
        reset();
        proc.applyFactoryPreset (0);
        densityBase();
        set (pid::rateModA,     0.55f);  set (pid::rateModB,    -0.35f);
        set (pid::sizeModA,    -0.40f);  set (pid::sizeModB,     0.60f);
        set (pid::posModA,      0.70f);  set (pid::posModB,     -0.50f);
        set (pid::pspreadModA,  0.65f);  set (pid::pspreadModB,  0.30f);
        set (pid::compModA,    -0.60f);  set (pid::compModB,     0.45f);
        set (pid::tiltModA,     0.50f);  set (pid::tiltModB,    -0.55f);
        set (pid::spreadModA,   0.80f);  set (pid::spreadModB,  -0.30f);
        set (pid::pitchSpread,  4.0f);
        set (pid::lfoAShape,    0.0f);   set (pid::lfoARateMs, 500.0f);
        set (pid::lfoBShape,    5.0f);   set (pid::lfoBRateMs, 900.0f);
        pumpMs (450);
        driveChord ({ 48, 52, 55, 60 }, 24);
        writeShot (*ed, outDir, "04_full-modulation-matrix.png", "4. Full modulation matrix (LFO depth rings visible)");
    }

    // ---------------------------------------------------------------- state 5
    {
        reset();
        proc.applyFactoryPreset (5);            // Reverse Rain
        set (pid::position, 0.45f);
        set (pid::voices,   8.0f);
        pumpMs (450);
        driveChord ({ 48, 52, 55, 60 }, 24);
        pumpMs (300);                           // 10 Hz seed label timer
        writeShot (*ed, outDir, "05_random-seed-reverse-rain.png", "5. Random seed + Reverse Rain (SeedPanel alt layout, Freeze on)");
    }

    // ---------------------------------------------------------------- state 6
    {
        reset();
        proc.applyFactoryPreset (0);
        densityBase();
        pumpMs (450);
        driveChord ({ 48, 52, 55, 60 }, 24);

        auto* dragTarget = dynamic_cast<juce::FileDragAndDropTarget*> (ed.get());
        if (dragTarget == nullptr)
            std::printf ("WARN: editor is not a FileDragAndDropTarget; overlay state will match state 1\n");
        else
        {
            dragTarget->fileDragEnter (juce::StringArray ("/tmp/example.wav"),
                                       ed->getWidth() / 2, ed->getHeight() / 2);
            ed->repaint();
            pumpMs (200);
        }

        writeShot (*ed, outDir, "06_drag-over-drop-overlay.png", "6. Drag-over drop overlay");

        if (dragTarget != nullptr)
        {
            dragTarget->fileDragExit (juce::StringArray ("/tmp/example.wav"));
            pumpMs (80);
        }
    }

    reset();
    ed.reset();     // editor must die before the processor

    if (failures == 0) std::printf ("ALL STATES OK\n");
    else               std::printf ("FAILURES: %d\n", failures);
    std::fflush (stdout);
    return failures == 0 ? 0 : 1;
}
