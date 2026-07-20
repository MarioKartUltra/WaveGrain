// =============================================================================
//  WaveGrain — GranularEngine.h
//  Polyphony over GrainVoice + RT-safe sample hand-off + UI telemetry. The four
//  procedural ROMs are permanent (never freed); a user-loaded sample uses a
//  shared_ptr + atomic snapshot published from the message thread, so the audio
//  thread never locks, allocates, or touches a refcount.  namespace wavegrain.
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <memory>
#include <vector>
#include "GrainVoice.h"
#include "SampleRom.h"
#include "Params.h"
#include "../common/acv2/dsp/SeedPolicy.h"

namespace wavegrain
{
class GranularEngine
{
public:
    static constexpr int kMaxVoices = 16;

    void prepare(double sampleRate)
    {
        sr = sampleRate;
        roms_ = makeRomSamples(sampleRate);
        for (auto& v : voices) v.prepare(sampleRate);
        scope_.assign(scopeSize, 0.0f); scopePos_ = 0;
    }

    // ---- message thread: install a user-loaded sample (RT-safe publish) -------
    void setUserSample(SampleData s)
    {
        auto owned = std::make_shared<const SampleData>(std::move(s));
        retireForAudio(userOwner_);            // park until audio provably past it
        userOwner_ = owned;
        userLive_.store(owned.get(), std::memory_order_release);
    }
    bool hasUserSample() const { return userOwner_ != nullptr; }

    // ---- message thread: the ROM-codec pipeline --------------------------------
    // Original (pre-codec) sample for a selection — for the codec encoder to read.
    const SampleData* originalSample(int sel) const
    { if (sel >= 0 && sel < 4) return &roms_[(size_t) sel]; return userOwner_.get(); }
    // Publish a codec-encoded sample (RT-safe, same pattern as setUserSample).
    void setCodedSample(SampleData s)
    {
        auto owned = std::make_shared<const SampleData>(std::move(s));
        retireForAudio(codedOwner_); codedOwner_ = owned;
        codedLive_.store(owned.get(), std::memory_order_release);
    }

    void setParameters(const WaveGrainParams& p)
    {
        seedPolicy_.setMode((acv2::SeedMode) p.seedMode);
        if (p.seedGlobal != params_.seedGlobal || !seedInit_)
        { seedPolicy_.setGlobalSeed(p.seedGlobal); seedInit_ = true; }
        params_ = p;
        const SampleData* smp = audioSample();
        for (auto& v : voices) if (v.isActive()) v.updateParameters(params_, smp);
    }
    const WaveGrainParams& parameters() const { return params_; }

    // Optional session entropy for Random-on-strike (call from a non-RT thread
    // before rendering, e.g. prepare; the CLI test skips it for determinism).
    void setStrikeEntropy(std::uint64_t e) { seedPolicy_.setStrikeEntropy(e); }

    void noteOn(int note, double vel)
    {
        const int maxV = std::min(std::max(params_.numVoices, 1), kMaxVoices);
        int slot = -1;
        for (int i = 0; i < maxV; ++i) if (!voices[i].isActive()) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < maxV; ++i) if (voices[i].isReleased()) { slot = i; break; }
        if (slot < 0) slot = 0;
        const std::uint64_t seed = seedPolicy_.onNoteOn(note);
        lastSeed_.store(seed, std::memory_order_relaxed);
        voices[slot].setPitchBend(bend_);
        voices[slot].noteOn(note, vel, params_, audioSample(), seed);
        lastVoice_ = slot;
    }

    // Last seed handed to a voice (UI: display + "lock this seed").
    std::uint64_t lastUsedSeed() const { return lastSeed_.load(std::memory_order_relaxed); }
    void noteOff(int note)
    { for (auto& v : voices) if (v.isActive() && !v.isReleased() && v.midiNote() == note) v.noteOff(); }
    void pitchBend(double semis) { bend_ = semis; for (auto& v : voices) v.setPitchBend(semis); }
    void panic() { for (auto& v : voices) v.panic(); }

    void renderBlock(float* L, float* R, int n)
    {
        blockEpoch_.fetch_add(1, std::memory_order_relaxed);   // coarse clock for retireForAudio
        const SampleData* smp = audioSample();
        for (int i = 0; i < n; ++i) { if (L) L[i] = 0.0f; if (R) R[i] = 0.0f; }
        for (auto& v : voices) if (v.isActive()) v.renderBlock(smp, L, R, n);

        const double gain = params_.output;
        for (int i = 0; i < n; ++i)
        {
            double l = (L ? L[i] : 0.0) * gain, r = (R ? R[i] : 0.0) * gain;
            if (!std::isfinite(l)) l = 0.0; if (!std::isfinite(r)) r = 0.0;
            l = std::tanh(l); r = std::tanh(r);
            if (L) L[i] = (float) l; if (R) R[i] = (float) r;
            scope_[(size_t) scopePos_] = (float) l; scopePos_ = (scopePos_ + 1) % scopeSize;
        }
    }

    // ---- UI-thread telemetry --------------------------------------------------
    int activeVoices() const { int c = 0; for (auto& v : voices) if (v.isActive()) ++c; return c; }
    int activeGrains() const { int c = 0; for (auto& v : voices) c += v.activeGrains(); return c; }

    void copyScope(std::vector<float>& dst) const
    { dst.resize(scopeSize); for (int i = 0; i < scopeSize; ++i) dst[(size_t) i] = scope_[(size_t) ((scopePos_ + i) % scopeSize)]; }

    // Decimated mono overview of the currently-selected sample (min/max per bin).
    void copyOverview(std::vector<float>& lo, std::vector<float>& hi, int bins) const
    {
        lo.assign((size_t) bins, 0.0f); hi.assign((size_t) bins, 0.0f);
        const SampleData* s = uiSample();
        if (!s || s->length <= 0) return;
        for (int b = 0; b < bins; ++b)
        {
            const std::int64_t a = (std::int64_t) ((double) b / bins * s->length);
            const std::int64_t c = (std::int64_t) ((double) (b + 1) / bins * s->length);
            float mn = 1e9f, mx = -1e9f;
            for (std::int64_t i = a; i < c && i < s->length; ++i) { const float v = s->monoAt(i); mn = std::min(mn, v); mx = std::max(mx, v); }
            if (mn > mx) { mn = mx = 0.0f; }
            lo[(size_t) b] = mn; hi[(size_t) b] = mx;
        }
    }
    void copyPlayheads(std::vector<float>& dst) const
    { dst.clear(); for (auto& v : voices) if (v.isActive()) dst.push_back((float) v.playheadNorm()); }

    // Per-grain UI markers: triplets {srcPosNorm, progress01, pan01} (see GrainVoice).
    void copyGrainMarkers(std::vector<float>& dst) const
    { dst.clear(); for (auto& v : voices) if (v.isActive()) v.appendGrainMarkers(dst); }

    // For the offline test: the sample that audio is currently reading.
    const SampleData* currentSample() const { return audioSample(); }

private:
    // Audio thread: codec ON -> the wavelet-coded snapshot; else ROM ptr / user snapshot.
    const SampleData* audioSample() const
    {
        if (params_.codecOn)
        { const SampleData* c = codedLive_.load(std::memory_order_acquire); if (c) return c; }
        const int sel = params_.sample;
        if (sel >= 0 && sel < 4) return &roms_[(size_t) sel];
        return userLive_.load(std::memory_order_acquire);
    }
    // UI thread: show the coded sample when the codec is on (so the degraded
    // waveform is visible); else the selected original.
    const SampleData* uiSample() const
    {
        if (params_.codecOn && codedOwner_) return codedOwner_.get();
        const int sel = params_.sample;
        if (sel >= 0 && sel < 4) return &roms_[(size_t) sel];
        return userOwner_.get();
    }

    double sr = 48000.0;
    WaveGrainParams params_;
    std::array<SampleData, 4> roms_;
    std::shared_ptr<const SampleData> userOwner_;
    std::atomic<const SampleData*> userLive_ { nullptr };
    std::shared_ptr<const SampleData> codedOwner_;                  // ROM-codec output
    std::atomic<const SampleData*> codedLive_ { nullptr };

    // Message thread: park a just-replaced sample until the audio thread has
    // provably moved past any raw pointer to it. renderBlock() bumps blockEpoch_
    // at block start and re-loads the live pointer each block, so an entry
    // retired at epoch e can still be read during blocks e and e+1; it is free
    // only once the observed epoch reaches e+2. This makes retirement safe for
    // any number of publishes inside one audio block (was: depth-1 slot).
    struct RetiredSample { std::uint64_t epoch; std::shared_ptr<const SampleData> ptr; };
    void retireForAudio(const std::shared_ptr<const SampleData>& old)
    {
        const std::uint64_t now = blockEpoch_.load(std::memory_order_relaxed);
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                       [now](const RetiredSample& r) { return r.epoch + 2 <= now; }),
                       retired_.end());
        if (old != nullptr) retired_.push_back({ now, old });
    }
    std::vector<RetiredSample> retired_;
    std::atomic<std::uint64_t> blockEpoch_ { 0 };

    std::array<GrainVoice, kMaxVoices> voices;
    double bend_ = 0.0; int lastVoice_ = -1;

    acv2::SeedPolicy seedPolicy_;
    bool seedInit_ = false;
    std::atomic<std::uint64_t> lastSeed_ { 0 };

    static constexpr int scopeSize = 4096;
    std::vector<float> scope_; int scopePos_ = 0;
};
} // namespace wavegrain
