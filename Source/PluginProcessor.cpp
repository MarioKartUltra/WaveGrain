// =============================================================================
//  WaveGrain — PluginProcessor.cpp
// =============================================================================
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/Codec.h"

using APVTS = juce::AudioProcessorValueTreeState;
using namespace wavegrain;

namespace { juce::NormalisableRange<float> unit() { return { 0.0f, 1.0f, 0.0001f }; }
            const int kGrainSizes[5] = { 256, 512, 1024, 2048, 4096 }; }

APVTS::ParameterLayout WaveGrainProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto f = [&](const char* id, const char* nm, juce::NormalisableRange<float> r, float d)
        { p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id,1}, nm, r, d)); };
    auto u = [&](const char* id, const char* nm, float d){ f(id, nm, unit(), d); };
    auto c = [&](const char* id, const char* nm, juce::StringArray ch, int d)
        { p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id,1}, nm, ch, d)); };
    auto in = [&](const char* id, const char* nm, int lo, int hi, int d)
        { p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{id,1}, nm, lo, hi, d)); };
    auto bl = [&](const char* id, const char* nm, bool d)
        { p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id,1}, nm, d)); };

    c(pid::sample, "Sample", { "Pluck","Vowel","Hit","Pad","User" }, 0);
    in(pid::root, "Root Note", 24, 84, 60);
    c(pid::grainSize, "Grain Size", { "256","512","1024","2048","4096" }, 2);
    u(pid::density, "Density", 0.5f);
    u(pid::position, "Position", 0.0f);
    f(pid::scan, "Scan", { -1.0f, 1.0f, 0.001f }, 0.0f);
    bl(pid::freeze, "Freeze", false);
    u(pid::spray, "Spray", 0.1f);
    f(pid::pitchFine, "Fine Pitch", { -1.0f, 1.0f, 0.001f }, 0.0f);
    u(pid::spread, "Spread", 0.4f);
    c(pid::window, "Window", { "Hann","Tukey","Gauss","Tri" }, 0);
    c(pid::wavelet, "Wavelet", { "Haar","Db2","Db4","Db6" }, 2);
    u(pid::compress, "Compression", 0.0f);
    u(pid::tilt, "Scale Tilt", 0.5f);
    u(pid::bandLow, "Band Low", 0.0f);
    u(pid::bandHigh, "Band High", 1.0f);
    in(pid::octave, "DWT Octave", -2, 2, 0);
    bl(pid::codecOn, "ROM Codec", false);
    u(pid::romRatio, "ROM Ratio", 0.5f);
    in(pid::romBits, "ROM Bits", 2, 16, 12);
    f(pid::attack, "Attack", { 0.001f, 2.0f, 0.0f, 0.4f }, 0.01f);
    f(pid::decay, "Decay", { 0.005f, 3.0f, 0.0f, 0.4f }, 0.2f);
    u(pid::sustain, "Sustain", 0.8f);
    f(pid::release, "Release", { 0.005f, 4.0f, 0.0f, 0.4f }, 0.4f);
    u(pid::cutoff, "Cutoff", 0.9f);
    u(pid::reso, "Resonance", 0.15f);
    f(pid::output, "Output", { 0.0f, 2.0f, 0.001f }, 0.7f);
    in(pid::voices, "Polyphony", 1, 16, 8);
    return { p.begin(), p.end() };
}

WaveGrainProcessor::WaveGrainProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "WAVEGRAIN", createLayout())
{
    formatMgr.registerBasicFormats();
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

WaveGrainParams WaveGrainProcessor::paramsFromState() const
{
    auto raw = [&](const char* id){ return apvts.getRawParameterValue(id)->load(); };
    WaveGrainParams m;
    m.sample    = (int) raw(pid::sample);
    m.rootNote  = (int) raw(pid::root);
    m.grainSize = kGrainSizes[juce::jlimit(0, 4, (int) raw(pid::grainSize))];
    m.density   = raw(pid::density);
    m.position  = raw(pid::position);
    m.scan      = raw(pid::scan);
    m.freeze    = raw(pid::freeze) > 0.5f;
    m.spray     = raw(pid::spray);
    m.pitchFine = raw(pid::pitchFine);
    m.spread    = raw(pid::spread);
    m.window    = (GrainWindow) (int) raw(pid::window);
    m.wavelet   = (wavetree::WaveletType) (int) raw(pid::wavelet);
    m.compress  = raw(pid::compress);
    m.tilt      = raw(pid::tilt);
    m.bandLow   = raw(pid::bandLow);
    m.bandHigh  = raw(pid::bandHigh);
    m.octave    = (int) raw(pid::octave);
    m.codecOn   = raw(pid::codecOn) > 0.5f;
    m.romRatio  = raw(pid::romRatio);
    m.romBits   = (int) raw(pid::romBits);
    m.attack    = raw(pid::attack); m.decay = raw(pid::decay);
    m.sustain   = raw(pid::sustain); m.release = raw(pid::release);
    m.cutoff    = raw(pid::cutoff); m.reso = raw(pid::reso);
    m.output    = raw(pid::output);
    m.numVoices = (int) raw(pid::voices);
    return m;
}

void WaveGrainProcessor::prepareToPlay(double sampleRate, int)
{
    sr = sampleRate;
    synth.prepare(sampleRate);
    synth.setParameters(paramsFromState());
}

bool WaveGrainProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void WaveGrainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    synth.setParameters(paramsFromState());
    kbState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

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
    synth.renderBlock(L, R, buffer.getNumSamples());
}

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

juce::AudioProcessorEditor* WaveGrainProcessor::createEditor() { return new WaveGrainEditor(*this); }

void WaveGrainProcessor::getStateInformation(juce::MemoryBlock& dest)
{ if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest); }
void WaveGrainProcessor::setStateInformation(const void* data, int size)
{ if (auto xml = getXmlFromBinary(data, size)) if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml)); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WaveGrainProcessor(); }
