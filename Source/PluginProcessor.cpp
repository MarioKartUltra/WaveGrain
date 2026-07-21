// =============================================================================
//  WaveGrain II — PluginProcessor.cpp
// =============================================================================
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Codec.h"
#include "BinaryData.h"

using APVTS = juce::AudioProcessorValueTreeState;
using namespace wavegrain;
namespace units = acv2::units;
namespace tempo = acv2::tempo;

// -----------------------------------------------------------------------------
//  Parameter layout. v1 ids keep versionHint 1; everything new uses hint 2.
//  Every parameter displays REAL units (acv2::units) — never bare 0..1.
// -----------------------------------------------------------------------------
namespace
{
    juce::NormalisableRange<float> unit() { return { 0.0f, 1.0f, 0.0001f }; }

    auto msAttr()   { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::msToText((double) v)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) units::textToMs(t.toStdString()); }); }
    auto secAttr()  { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::msToText((double) v * 1000.0)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) (units::textToMs(t.toStdString()) / 1000.0); }); }
    auto pctAttr()  { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::percentToText((double) v, false)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) units::textToPercent(t.toStdString()); }); }
    auto spctAttr() { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::percentToText((double) v, true)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) units::textToPercent(t.toStdString()); }); }
    auto hzAttr()   { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::hzToText((double) v)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) units::textToHz(t.toStdString()); }); }
    auto stAttr()   { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::semitonesToText((double) v)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) units::textToSemitones(t.toStdString()); }); }
    auto gainDbAttr() { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int)
            { return juce::String(units::dbToText(v > 0.0001f ? 20.0 * std::log10((double) v) : -200.0)); })
        .withValueFromStringFunction([](const juce::String& t)
            { return (float) std::pow(10.0, units::textToDb(t.toStdString()) / 20.0); }); }
    auto octFineAttr() { return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float v, int){ return juce::String(units::semitonesToText((double) v * 12.0)); })
        .withValueFromStringFunction([](const juce::String& t){ return (float) (units::textToSemitones(t.toStdString()) / 12.0); }); }

    juce::StringArray divisionLabels() { return tempo::labels<juce::StringArray>(); }

    // Mod-matrix ranges (bounded offsets; see Lfo.h header sketch)
    constexpr float kPosModRange     = 0.5f;   // ± half the sample
    constexpr float kPSpreadModRange = 6.0f;   // ± 6 st
    constexpr float kCompModRange    = 0.5f;
    constexpr float kTiltModRange    = 0.5f;
    constexpr float kSpreadModRange  = 0.5f;
    // rate & size are modulated multiplicatively: base * 2^(depth * lfo * 2 oct)
    constexpr float kRateSizeModOct  = 2.0f;
}

APVTS::ParameterLayout WaveGrainProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto f1 = [&](const char* id, const char* nm, juce::NormalisableRange<float> r, float d, auto attr)
        { p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id,1}, nm, r, d, attr)); };
    auto f2 = [&](const char* id, const char* nm, juce::NormalisableRange<float> r, float d, auto attr)
        { p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id,2}, nm, r, d, attr)); };
    auto c1 = [&](const char* id, const char* nm, juce::StringArray ch, int d)
        { p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id,1}, nm, ch, d)); };
    auto c2 = [&](const char* id, const char* nm, juce::StringArray ch, int d)
        { p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id,2}, nm, ch, d)); };
    auto in1 = [&](const char* id, const char* nm, int lo, int hi, int d)
        { p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{id,1}, nm, lo, hi, d)); };
    auto in2 = [&](const char* id, const char* nm, int lo, int hi, int d)
        { p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{id,2}, nm, lo, hi, d)); };
    auto b1 = [&](const char* id, const char* nm, bool d)
        { p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id,1}, nm, d)); };
    auto b2 = [&](const char* id, const char* nm, bool d)
        { p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id,2}, nm, d)); };

    const auto noteAttr = juce::AudioParameterIntAttributes()
        .withStringFromValueFunction([](int v, int){ return juce::String(units::noteName(v)); })
        .withValueFromStringFunction([](const juce::String& t)
            { const int n = units::noteNameToMidi(t.toStdString()); return n >= 0 ? n : t.getIntValue(); });

    // ---- sample / grain ----
    c1(pid::sample, "Sample", { "Pluck","Vowel","Hit","Pad","User" }, 0);
    p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{pid::root,1}, "Root Note", 24, 84, 60, noteAttr));
    b2(pid::rateSync, "Grain Rate Sync", false);
    f2(pid::rateMs,  "Grain Rate",  { 2.0f, 500.0f, 0.0f, 0.35f }, 8.0f, msAttr());   // fuller default overlap (was 12)
    c2(pid::rateDiv, "Grain Rate Div", divisionLabels(), 2);            // 1/16
    b2(pid::sizeSync, "Grain Size Sync", false);
    f2(pid::sizeMs,  "Grain Size",  { 4.0f, 120.0f, 0.0f, 0.5f }, 21.0f, msAttr());
    c2(pid::sizeDiv, "Grain Size Div", divisionLabels(), 0);            // 1/32
    f1(pid::position, "Position", unit(), 0.0f, pctAttr());
    f1(pid::scan, "Scan", { -1.0f, 1.0f, 0.001f }, 0.0f, spctAttr());
    b1(pid::freeze, "Freeze", false);
    f1(pid::spray, "Spray", unit(), 0.1f, pctAttr());
    f1(pid::pitchFine, "Fine Pitch", { -1.0f, 1.0f, 0.001f }, 0.0f, octFineAttr());
    f2(pid::pitchSpread, "Pitch Spread", { 0.0f, 12.0f, 0.01f }, 0.0f, stAttr());
    f2(pid::reverseProb, "Reverse", unit(), 0.0f, pctAttr());
    f1(pid::spread, "Stereo Spread", unit(), 0.4f, pctAttr());
    c1(pid::window, "Window", { "Hann","Tukey","Gauss","Tri" }, 0);

    // ---- wavelet ops / codec ----
    c1(pid::wavelet, "Wavelet", { "Haar","Db2","Db4","Db6" }, 2);
    f1(pid::compress, "Compression", unit(), 0.0f, pctAttr());
    f1(pid::tilt, "Scale Tilt", unit(), 0.5f, pctAttr());
    f1(pid::bandLow, "Band Low", unit(), 0.0f, pctAttr());
    f1(pid::bandHigh, "Band High", unit(), 1.0f, pctAttr());
    in1(pid::octave, "DWT Octave", -2, 2, 0);
    b1(pid::codecOn, "ROM Codec", false);
    f1(pid::romRatio, "ROM Ratio", unit(), 0.5f, pctAttr());
    in1(pid::romBits, "ROM Bits", 2, 16, 12);

    // ---- seed policy ----
    c2(pid::seedMode, "Seed Mode", { "Fixed","Per-Key","Random" }, 1);   // Per-Key: chord voices decorrelate (was Fixed)
    in2(pid::seed, "Seed", 1, 999999, 1234);

    // ---- LFO A/B ----
    const juce::StringArray shapes { "Sine","Triangle","Saw Up","Saw Down","Square","S&H","Smooth Rnd","Drift" };
    const juce::StringArray retrigs { "Free","Key","Transport" };
    c2(pid::lfoAShape, "LFO A Shape", shapes, 0);
    b2(pid::lfoARateSync, "LFO A Sync", false);
    f2(pid::lfoARateMs, "LFO A Rate", { 10.0f, 10000.0f, 0.0f, 0.3f }, 500.0f, msAttr());
    c2(pid::lfoARateDiv, "LFO A Div", divisionLabels(), tempo::kDefaultDivision);
    f2(pid::lfoAPhase, "LFO A Phase", unit(), 0.0f, pctAttr());
    c2(pid::lfoARetrig, "LFO A Retrig", retrigs, 0);
    b2(pid::lfoABipolar, "LFO A Bipolar", true);
    c2(pid::lfoBShape, "LFO B Shape", shapes, 1);
    b2(pid::lfoBRateSync, "LFO B Sync", false);
    f2(pid::lfoBRateMs, "LFO B Rate", { 10.0f, 10000.0f, 0.0f, 0.3f }, 2000.0f, msAttr());
    c2(pid::lfoBRateDiv, "LFO B Div", divisionLabels(), 13);            // 1/1
    f2(pid::lfoBPhase, "LFO B Phase", unit(), 0.0f, pctAttr());
    c2(pid::lfoBRetrig, "LFO B Retrig", retrigs, 0);
    b2(pid::lfoBBipolar, "LFO B Bipolar", true);

    // ---- mod depths ----
    auto depth = [&](const char* id, const char* nm)
        { f2(id, nm, { -1.0f, 1.0f, 0.001f }, 0.0f, spctAttr()); };
    depth(pid::rateModA, "Rate < A");     depth(pid::rateModB, "Rate < B");
    depth(pid::sizeModA, "Size < A");     depth(pid::sizeModB, "Size < B");
    depth(pid::posModA, "Pos < A");       depth(pid::posModB, "Pos < B");
    depth(pid::pspreadModA, "PSprd < A"); depth(pid::pspreadModB, "PSprd < B");
    depth(pid::compModA, "Comp < A");     depth(pid::compModB, "Comp < B");
    depth(pid::tiltModA, "Tilt < A");     depth(pid::tiltModB, "Tilt < B");
    depth(pid::spreadModA, "Sprd < A");   depth(pid::spreadModB, "Sprd < B");

    // ---- env / tone / out ----
    f1(pid::attack,  "Attack",  { 0.001f, 2.0f, 0.0f, 0.4f }, 0.01f, secAttr());
    f1(pid::decay,   "Decay",   { 0.005f, 3.0f, 0.0f, 0.4f }, 0.2f,  secAttr());
    f1(pid::sustain, "Sustain", unit(), 0.8f, pctAttr());
    f1(pid::release, "Release", { 0.005f, 4.0f, 0.0f, 0.4f }, 0.4f,  secAttr());
    f2(pid::cutoffHz, "Cutoff", { 20.0f, 20000.0f, 0.0f, 0.25f }, 19000.0f, hzAttr());
    f1(pid::reso, "Resonance", unit(), 0.15f, pctAttr());
    f1(pid::output, "Output", { 0.0f, 2.0f, 0.001f }, 0.7f, gainDbAttr());
    in1(pid::voices, "Polyphony", 1, 16, 8);
    return { p.begin(), p.end() };
}

// -----------------------------------------------------------------------------
WaveGrainProcessor::WaveGrainProcessor()
    : foleys::MagicProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "WAVEGRAIN2", createLayout())
{
    formatMgr.registerBasicFormats();

    FOLEYS_SET_SOURCE_PATH(__FILE__);
    outputLevel = magicState.createAndAddObject<foleys::MagicLevelSource>("output");
    magicState.setApplicationSettingsFile(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                              .getChildFile("WaveGrainII").getChildFile("settings.xml"));

    // Random-on-strike sessions decorrelate via wall-clock entropy (message thread).
    synth.setStrikeEntropy((std::uint64_t) juce::Time::getMillisecondCounterHiRes() * 2654435761ull);

    startTimerHz(15);   // message-thread poll to re-encode the ROM codec on change
}

void WaveGrainProcessor::timerCallback()
{
    auto raw = [&](const char* id){ return apvts.getRawParameterValue(id)->load(); };
    const bool   on    = raw(pid::codecOn) > 0.5f;
    const int    sel   = (int) raw(pid::sample);
    const double ratio = raw(pid::romRatio);
    const int    bits  = (int) raw(pid::romBits);
    const int    wt    = (int) raw(pid::wavelet);
    const int    ug    = userGen_.load(std::memory_order_relaxed);

    if (!on) { if (lastOn_) { codecText_ = {}; lastOn_ = false; lastSel_ = -1; } return; }
    if (on == lastOn_ && sel == lastSel_ && ratio == lastRatio_ && bits == lastBits_ && wt == lastWt_ && ug == lastUserGen_)
        return;
    lastOn_ = on; lastSel_ = sel; lastRatio_ = ratio; lastBits_ = bits; lastWt_ = wt; lastUserGen_ = ug;

    const wavegrain::SampleData* orig = synth.originalSample(sel);
    if (orig == nullptr || orig->length == 0) { codecText_ = "codec: (no sample)"; return; }

    wavegrain::CodecStats st;
    wavegrain::SampleData coded = wavegrain::waveletEncode(*orig, ratio, bits, (wavetree::WaveletType) wt, &st);
    synth.setCodedSample(std::move(coded));
    codecText_ = "codec " + juce::String(st.ratio, 1) + ":1  " + juce::String((int) st.kbps) + " kbps";
}

// -----------------------------------------------------------------------------
WaveGrainParams WaveGrainProcessor::paramsFromState(float a, float b)
{
    auto raw = [&](const char* id){ return apvts.getRawParameterValue(id)->load(); };
    auto modded = [&](const char* base, const char* modA, const char* modB,
                      float range, float lo, float hi)
    {
        const float v = raw(base) + raw(modA) * a * range + raw(modB) * b * range;
        return juce::jlimit(lo, hi, v);
    };

    WaveGrainParams m;
    m.sample    = (int) raw(pid::sample);
    m.rootNote  = (int) raw(pid::root);

    // ---- grain clock: sync -> division ms at last-known bpm; else free ms ----
    const double rateBaseMs = raw(pid::rateSync) > 0.5f
        ? tempo::divisionToMs((int) raw(pid::rateDiv), lastBpm_) : (double) raw(pid::rateMs);
    const double sizeBaseMs = raw(pid::sizeSync) > 0.5f
        ? tempo::divisionToMs((int) raw(pid::sizeDiv), lastBpm_) : (double) raw(pid::sizeMs);
    // multiplicative modulation (+-2 octaves full-depth)
    const double rateMod = std::pow(2.0, (double) (raw(pid::rateModA) * a + raw(pid::rateModB) * b) * kRateSizeModOct);
    const double sizeMod = std::pow(2.0, (double) (raw(pid::sizeModA) * a + raw(pid::sizeModB) * b) * kRateSizeModOct);
    m.hopSamples = juce::jlimit(32.0, sr * 4.0, rateBaseMs * rateMod * 0.001 * sr);
    const double sizeSamples = juce::jlimit(256.0, 4096.0, sizeBaseMs * sizeMod * 0.001 * sr);
    m.grainSize  = (int) sizeSamples;              // DSP snaps to power of two

    m.position  = modded(pid::position, pid::posModA, pid::posModB, kPosModRange, 0.0f, 1.0f);
    m.scan      = raw(pid::scan);
    m.freeze    = raw(pid::freeze) > 0.5f;
    m.spray     = raw(pid::spray);
    m.pitchFine = raw(pid::pitchFine);
    m.pitchSpread = modded(pid::pitchSpread, pid::pspreadModA, pid::pspreadModB, kPSpreadModRange, 0.0f, 12.0f);
    m.reverseProb = raw(pid::reverseProb);
    m.spread    = modded(pid::spread, pid::spreadModA, pid::spreadModB, kSpreadModRange, 0.0f, 1.0f);
    m.window    = (GrainWindow) (int) raw(pid::window);
    m.wavelet   = (wavetree::WaveletType) (int) raw(pid::wavelet);
    m.compress  = modded(pid::compress, pid::compModA, pid::compModB, kCompModRange, 0.0f, 1.0f);
    m.tilt      = modded(pid::tilt, pid::tiltModA, pid::tiltModB, kTiltModRange, 0.0f, 1.0f);
    m.bandLow   = raw(pid::bandLow);
    m.bandHigh  = raw(pid::bandHigh);
    m.octave    = (int) raw(pid::octave);
    m.codecOn   = raw(pid::codecOn) > 0.5f;
    m.romRatio  = raw(pid::romRatio);
    m.romBits   = (int) raw(pid::romBits);
    m.seedMode  = (int) raw(pid::seedMode);
    m.seedGlobal = (std::uint64_t) (int) raw(pid::seed);
    m.attack    = raw(pid::attack); m.decay = raw(pid::decay);
    m.sustain   = raw(pid::sustain); m.release = raw(pid::release);
    // cutoff Hz -> the core's normalised 0..1 over [20, 0.45*sr]
    const double fcHz = raw(pid::cutoffHz);
    m.cutoff    = juce::jlimit(0.0, 1.0, (fcHz - 20.0) / std::max(1.0, sr * 0.45 - 20.0));
    m.reso      = raw(pid::reso);
    m.output    = raw(pid::output);
    m.numVoices = (int) raw(pid::voices);
    return m;
}

void WaveGrainProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    synth.prepare(sampleRate);
    lfoA_.prepare(sampleRate);
    lfoB_.prepare(sampleRate);
    synth.setParameters(paramsFromState(0.0f, 0.0f));
    magicState.prepareToPlay(sampleRate, samplesPerBlock);
    if (outputLevel != nullptr) outputLevel->setupSource(2, sampleRate, 500);
}

bool WaveGrainProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void WaveGrainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    // ---- transport (cache last-known, default 120) ----
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) lastBpm_ = *bpm > 0.0 ? *bpm : lastBpm_;
            if (auto ppq = pos->getPpqPosition()) { lfoA_.setTransportPpq(*ppq); lfoB_.setTransportPpq(*ppq); }
        }

    // ---- configure LFOs from params (block rate) ----
    auto raw = [&](const char* id){ return apvts.getRawParameterValue(id)->load(); };
    auto cfg = [&](acv2::Lfo& l, const char* sh, const char* sy, const char* ms,
                   const char* dv, const char* phs, const char* rt, const char* bi)
    {
        l.setShape((acv2::LfoShape) (int) raw(sh));
        l.setBpm(lastBpm_);
        if (raw(sy) > 0.5f) l.setSyncedDivision((int) raw(dv));
        else                l.setRateHz(tempo::msToHz((double) raw(ms)));
        l.setPhaseOffset((double) raw(phs));
        l.setRetrigMode((acv2::LfoRetrig) (int) raw(rt));
        l.setBipolar(raw(bi) > 0.5f);
    };
    cfg(lfoA_, pid::lfoAShape, pid::lfoARateSync, pid::lfoARateMs, pid::lfoARateDiv,
        pid::lfoAPhase, pid::lfoARetrig, pid::lfoABipolar);
    cfg(lfoB_, pid::lfoBShape, pid::lfoBRateSync, pid::lfoBRateMs, pid::lfoBRateDiv,
        pid::lfoBPhase, pid::lfoBRetrig, pid::lfoBBipolar);

    // key-retrig BEFORE evaluating this block's LFO value
    for (const auto meta : midi)
        if (meta.getMessage().isNoteOn()) { lfoA_.noteOn(); lfoB_.noteOn(); }

    const float a = lfoA_.processBlock(n);
    const float b = lfoB_.processBlock(n);
    lfoAValue_.store(a, std::memory_order_relaxed);
    lfoBValue_.store(b, std::memory_order_relaxed);

    synth.setParameters(paramsFromState(a, b));

    magicState.getKeyboardState().processNextMidiBuffer(midi, 0, n, true);
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())            synth.noteOn(m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())      synth.noteOff(m.getNoteNumber());
        else if (m.isPitchWheel())   synth.pitchBend((m.getPitchWheelValue() - 8192) / 8192.0 * 2.0);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) synth.panic();
    }

    buffer.clear();
    float* L = buffer.getWritePointer(0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    synth.renderBlock(L, R, n);

    if (outputLevel != nullptr) outputLevel->pushSamples(buffer);
    midi.clear();
}

// -----------------------------------------------------------------------------
bool WaveGrainProcessor::loadUserSample(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatMgr.createReaderFor(file));
    if (reader == nullptr) return false;
    const int nch = (int) juce::jmax(1u, reader->numChannels);
    const int len = (int) juce::jmin<juce::int64>(reader->lengthInSamples, (juce::int64) (30.0 * reader->sampleRate));
    if (len <= 0) return false;
    juce::AudioBuffer<float> buf(nch, len);
    reader->read(&buf, 0, len, 0, true, true);

    SampleData data;
    data.length = len; data.sourceSr = reader->sampleRate;
    data.rootMidi = (int) apvts.getRawParameterValue(pid::root)->load();
    data.ch.resize((size_t) nch);
    for (int ch = 0; ch < nch; ++ch)
    { data.ch[(size_t) ch].resize((size_t) len); const float* s = buf.getReadPointer(ch); std::copy(s, s + len, data.ch[(size_t) ch].begin()); }
    synth.setUserSample(std::move(data));
    userGen_.fetch_add(1, std::memory_order_relaxed);   // signal the codec poller
    userName = file.getFileName();
    if (auto* pr = apvts.getParameter(pid::sample)) pr->setValueNotifyingHost(pr->convertTo0to1(4.0f));
    return true;
}

// -----------------------------------------------------------------------------
//  foleys GUI
// -----------------------------------------------------------------------------
void WaveGrainProcessor::initialiseBuilder(foleys::MagicGUIBuilder& builder)
{
    builder.registerJUCEFactories();
    builder.registerJUCELookAndFeels();
    registerWaveGrainItems(builder);   // PluginEditor.h — custom GuiItems
}

juce::ValueTree WaveGrainProcessor::createGuiValueTree()
{
    juce::String xml(BinaryData::magic_xml, (size_t) BinaryData::magic_xmlSize);
    auto tree = juce::ValueTree::fromXml(xml);
    if (tree.isValid()) return tree;
    return foleys::MagicProcessor::createGuiValueTree();
}

juce::AudioProcessorEditor* WaveGrainProcessor::createEditor()
{
    magicState.updateParameterMap();
    auto builder = std::make_unique<foleys::MagicGUIBuilder>(magicState);
    initialiseBuilder(*builder);
    if (magicState.getGuiTree().getChildWithName(foleys::IDs::view).isValid() == false)
        magicState.setGuiValueTree(createGuiValueTree());
    return new WaveGrainEditor(*this, std::move(builder));
}

// -----------------------------------------------------------------------------
//  presets
// -----------------------------------------------------------------------------
const std::vector<WaveGrainPreset>& WaveGrainProcessor::factoryPresets()
{
    static const std::vector<WaveGrainPreset> presets = {
        { "Init", { } },
        { "Amber Pad", {
            { pid::sample, 3.0f }, { pid::sizeMs, 80.0f }, { pid::rateMs, 18.0f },
            { pid::spray, 0.25f }, { pid::spread, 0.8f }, { pid::attack, 0.6f },
            { pid::release, 1.8f }, { pid::tilt, 0.42f },
            { pid::posModB, 0.35f }, { pid::lfoBShape, 7.0f }, { pid::lfoBRateMs, 4000.0f } } },
        { "Granette Pluck", {
            { pid::sample, 0.0f }, { pid::sizeMs, 8.0f }, { pid::rateMs, 4.0f },
            { pid::spray, 0.05f }, { pid::attack, 0.003f }, { pid::decay, 0.4f },
            { pid::sustain, 0.4f }, { pid::release, 0.3f }, { pid::compress, 0.3f } } },
        { "Vowel Cloud", {
            { pid::sample, 1.0f }, { pid::sizeMs, 45.0f }, { pid::rateMs, 10.0f },
            { pid::spray, 0.5f }, { pid::pitchSpread, 3.0f }, { pid::reverseProb, 0.3f },
            { pid::spread, 1.0f }, { pid::seedMode, 2.0f }, { pid::attack, 0.2f },
            { pid::release, 1.0f } } },
        { "Crushed ROM", {
            { pid::sample, 2.0f }, { pid::codecOn, 1.0f }, { pid::romRatio, 0.92f },
            { pid::romBits, 4.0f }, { pid::sizeMs, 30.0f }, { pid::rateMs, 15.0f },
            { pid::tilt, 0.62f }, { pid::reso, 0.4f }, { pid::cutoffHz, 9000.0f } } },
        { "Reverse Rain", {
            { pid::sample, 3.0f }, { pid::rateSync, 1.0f }, { pid::rateDiv, 2.0f },
            { pid::sizeMs, 60.0f }, { pid::reverseProb, 1.0f }, { pid::pitchSpread, 7.0f },
            { pid::spray, 0.6f }, { pid::spread, 1.0f }, { pid::seedMode, 2.0f },
            { pid::freeze, 1.0f }, { pid::attack, 0.4f }, { pid::release, 2.5f },
            { pid::tiltModA, 0.4f }, { pid::lfoAShape, 6.0f }, { pid::lfoARateMs, 900.0f } } },
    };
    return presets;
}

void WaveGrainProcessor::setParamRaw(const char* id, float rawValue)
{
    if (auto* pr = apvts.getParameter(id))
        pr->setValueNotifyingHost(pr->convertTo0to1(rawValue));
}

void WaveGrainProcessor::applyFactoryPreset(int index)
{
    const auto& list = factoryPresets();
    if (index < 0 || index >= (int) list.size()) return;
    // reset all to defaults, then apply the preset's overrides
    for (auto* pr : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(pr))
            rp->setValueNotifyingHost(rp->getDefaultValue());
    for (const auto& [id, v] : list[(size_t) index].values)
        setParamRaw(id, v);
}

juce::File WaveGrainProcessor::presetDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
        .getChildFile("WaveGrain II").getChildFile("Presets");
}

bool WaveGrainProcessor::savePresetFile(const juce::String& name)
{
    auto dir = presetDirectory();
    if (!dir.isDirectory() && !dir.createDirectory()) return false;
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return false;
    return xml->writeTo(dir.getChildFile(juce::File::createLegalFileName(name) + ".xml"));
}

bool WaveGrainProcessor::loadPresetFile(const juce::File& f)
{
    auto xml = juce::XmlDocument::parse(f);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType())) return false;
    auto incoming = juce::ValueTree::fromXml(*xml);
    // apply parameter values through the host (keeps automation + GUI in sync)
    for (int i = 0; i < incoming.getNumChildren(); ++i)
    {
        auto c = incoming.getChild(i);
        if (c.hasType("PARAM") && c.hasProperty("id") && c.hasProperty("value"))
            if (auto* pr = apvts.getParameter(c["id"].toString()))
                pr->setValueNotifyingHost(pr->convertTo0to1((float) (double) c["value"]));
    }
    return true;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WaveGrainProcessor(); }
