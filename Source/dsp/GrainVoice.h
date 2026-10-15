// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Abraham Joseph Okayli Masaryk
// =============================================================================
//  WaveGrain II — GrainVoice.h
//  One voice = one held MIDI note. Owns a playhead into the shared sample and a
//  fixed grain pool. Grains are PRE-RENDERED at spawn (anti-aliased cubic-read
//  window -> forward DWT -> 4 wavelet ops -> inverse DWT -> window), so the
//  per-sample loop is just windowed overlap-add. All scratch pre-allocated; no
//  audio-thread alloc.  v2: explicit hop clock (hopSamples), per-grain pitch
//  spread, per-grain reverse probability, anti-aliased multi-tap source read,
//  seed handed in from the engine's acv2::SeedPolicy.
//  namespace wavegrain, JUCE-free.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Dwt.h"
#include "Params.h"
#include "SampleRom.h"
#include "GrainPipeline.h"
#include "Rng.h"

namespace wavegrain
{
using wavetree::Rng;

// TPT state-variable low-pass (copied from WaveTree WaveletVoice.h).
struct Svf
{
    double ic1 = 0, ic2 = 0, g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    void reset() { ic1 = ic2 = 0; }
    void set(double fcHz, double sr, double reso)
    {
        fcHz = std::min(std::max(fcHz, 20.0), sr * 0.45);
        g = std::tan(M_PI * fcHz / sr);
        k = 2.0 - 1.9 * std::min(std::max(reso, 0.0), 1.0);
        a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    inline double process(double v0)
    { const double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; return v2; }
};

class GrainVoice
{
public:
    static constexpr int kMaxN      = 4096;
    static constexpr int kLevels    = 8;
    static constexpr int kMaxGrains = 48;
    static constexpr double kScanMax = 8.0;

    void prepare(double sampleRate)
    {
        sr = sampleRate;
        // Configure the DWT once at kMaxN so tmp/scratch reach kMaxN capacity;
        // later configure(N<=kMaxN) reuses capacity (no realloc on the audio thread).
        dwt.configure(wavetree::makeWavelet(curWavelet), kMaxN, kLevels);
        curN = kMaxN;
        coeff.assign(kMaxN, 0.0); idwt.assign(kMaxN, 0.0);
        mag.assign(kMaxN, 0.0); tmp.assign(kMaxN, 0.0); frame.assign(kMaxN, 0.0);
        grains.resize(kMaxGrains);
        for (auto& g : grains) g.out.assign(kMaxN, 0.0f);
        reset();
    }

    void setPitchBend(double semis) { bend = std::pow(2.0, semis / 12.0); }

    // `seed` comes from the engine's acv2::SeedPolicy — it drives ALL of this
    // voice's randomness (hop jitter, spray, pan, per-grain pitch, reverse).
    void noteOn(int midiNote, double vel, const WaveGrainParams& params,
                const SampleData* smp, std::uint64_t seed)
    {
        p_ = params; note = midiNote; velocity = vel; sample = smp;
        reconfigure();
        rootMidi = smp ? smp->rootMidi : 60;
        playPos = smp ? p_.position * (double) std::max<std::int64_t>(1, smp->length) : 0.0;
        spawnCounter = 0;
        for (auto& g : grains) g.active = false;
        env = 0.0; stage = Attack; released = false; active = true;
        svfL.reset(); svfR.reset(); dcL1 = dcLy = dcR1 = dcRy = 0.0;
        rng.reseed(seed ? seed : 1);
    }

    void noteOff() { released = true; if (stage != Idle) stage = Release; }

    void updateParameters(const WaveGrainParams& params, const SampleData* smp)
    {
        const bool reN = params.grainSize != p_.grainSize || params.wavelet != p_.wavelet;
        p_ = params; sample = smp;
        if (smp) rootMidi = smp->rootMidi;
        if (reN && active) reconfigure();
    }

    // Render one block, ADDING into L/R. `smp` is the live sample snapshot for this block.
    void renderBlock(const SampleData* smp, float* L, float* R, int n)
    {
        if (!active) return;
        sample = smp;
        // Density normalization: a Hann OLA at overlap R=N/hop sums to ~R/2, so scale
        // by 2*hop/N to keep level independent of grain density (fixes the v2 regression
        // where low overlap was both quieter and amplitude-rippling).
        const double effHop  = std::min(std::max(32.0, p_.hopSamples), (double) curN * 0.5);
        const double olaGain = 2.0 * effHop / (double) std::max(1, curN);
        for (int s = 0; s < n; ++s)
        {
            if (stage != Idle && sample && sample->length > 0 && env > 1.0e-5)
                if (--spawnCounter <= 0) { spawnGrain(); spawnCounter = nextHop(); }

            if (!p_.freeze && sample && sample->length > 0)
            {
                playPos += p_.scan * kScanMax;
                const double len = (double) sample->length;
                if (playPos >= len) playPos -= len; else if (playPos < 0) playPos += len;
            }

            double l = 0.0, r = 0.0;
            for (auto& g : grains)
                if (g.active)
                {
                    const double v = g.out[(size_t) g.writeIndex];
                    l += v * g.panL; r += v * g.panR;
                    if (++g.writeIndex >= g.len) g.active = false;
                }
            l *= olaGain; r *= olaGain;              // density-normalized OLA level

            advanceEnv();
            const double fc = 20.0 + p_.cutoff * (sr * 0.45 - 20.0);
            svfL.set(fc, sr, p_.reso); svfR.g = svfL.g; svfR.k = svfL.k; svfR.a1 = svfL.a1; svfR.a2 = svfL.a2; svfR.a3 = svfL.a3;
            l = svfL.process(l) * env * velocity;
            r = svfR.process(r) * env * velocity;
            // DC block (per channel) + finite guard
            const double dl = l - dcL1 + 0.999 * dcLy; dcL1 = l; dcLy = dl;
            const double dr = r - dcR1 + 0.999 * dcRy; dcR1 = r; dcRy = dr;
            double oL = std::isfinite(dl) ? dl : 0.0, oR = std::isfinite(dr) ? dr : 0.0;
            if (L) L[s] += (float) oL;
            if (R) R[s] += (float) oR;
        }
    }

    bool isActive()   const { return active; }
    bool isReleased() const { return released; }
    int  midiNote()   const { return note; }
    double playheadNorm() const { return (sample && sample->length > 0) ? playPos / sample->length : 0.0; }
    int  activeGrains() const { int c = 0; for (auto& g : grains) if (g.active) ++c; return c; }
    void reset() { for (auto& g : grains) g.active = false; env = 0; stage = Idle; active = false; released = false; }
    void panic() { reset(); }

    // UI telemetry: append {srcPosNorm, progress01, pan01} per active grain.
    void appendGrainMarkers(std::vector<float>& dst) const
    {
        for (const auto& g : grains)
            if (g.active && g.len > 0)
            {
                dst.push_back(g.srcNorm);
                dst.push_back((float) g.writeIndex / (float) g.len);
                dst.push_back(g.pan01);
            }
    }

private:
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    struct Grain { bool active = false; int len = 0, writeIndex = 0;
                   float panL = 0.7f, panR = 0.7f, srcNorm = 0.0f, pan01 = 0.5f;
                   std::vector<float> out; };

    static int snapPow2(int n)
    {
        int p = 256;
        while (p < 4096 && p * 2 <= n) p *= 2;
        // round up when n is closer to the next power of two
        if (p < 4096 && (n - p) > (p * 2 - n)) p *= 2;
        return p;
    }

    void reconfigure()
    {
        const int N = snapPow2(std::min(kMaxN, std::max(256, p_.grainSize)));
        if (N != curN || p_.wavelet != curWavelet)
        { curWavelet = p_.wavelet; curN = N; dwt.configure(wavetree::makeWavelet(curWavelet), N, kLevels); }
    }

    int nextHop()
    {
        double hop = std::max(32.0, p_.hopSamples);
        hop = std::min(hop, (double) curN * 0.5);   // overlap floor >=2x (Hann COLA): no gaps / sub-COLA thinning
        // Always jitter the spawn time (baseline 4% + Spray) so the grain RATE never
        // forms an audible spectral line — otherwise periodic grains impose their own
        // pitch. Smears the grain-rate artifact, leaving the sample's pitch dominant.
        hop *= (1.0 + (0.04 + p_.spray * 0.3) * rng.nextGaussian());
        return std::max(1, (int) hop);
    }

    inline double cubicRead(double pos) const
    {
        const std::int64_t i1 = (std::int64_t) std::floor(pos);
        const double f = pos - (double) i1;
        const double y0 = sample->monoAt(i1 - 1), y1 = sample->monoAt(i1), y2 = sample->monoAt(i1 + 1), y3 = sample->monoAt(i1 + 2);
        const double a = y3 - y2 - y0 + y1, b = y0 - y1 - a, c = y2 - y0;
        return ((a * f + b) * f + c) * f + y1;
    }

    // Anti-aliased source read: when the read ratio is > 1 (transposing up),
    // consecutive output samples skip source content and naive interpolation
    // folds it back as aliasing. Average M cubic taps spread across the skipped
    // span (box pre-filter with first spectral null at the fold-over frequency).
    // Grains are pre-rendered at spawn, so the extra taps never hit the
    // per-sample audio path.
    inline double readAA(double pos, double ratio) const
    {
        if (ratio <= 1.25) return cubicRead(pos);
        const int M = std::min(8, 1 + (int) ratio);
        const double span = ratio;                      // source-domain window
        double acc = 0.0;
        for (int k = 0; k < M; ++k)
        {
            const double u = ((double) k + 0.5) / (double) M - 0.5;   // (-0.5, 0.5)
            acc += cubicRead(pos + u * span);
        }
        return acc / (double) M;
    }

    void spawnGrain()
    {
        Grain* g = nullptr;
        for (auto& gr : grains) if (!gr.active) { g = &gr; break; }
        if (g == nullptr) return;                                  // bounded by kMaxGrains

        const int N = curN;
        const double srcStart = playPos + p_.spray * rng.nextGaussian() * (0.05 * sample->length);
        const double spreadSemis = p_.pitchSpread * (2.0 * rng.nextUnit() - 1.0);
        const double pitchRatio = std::pow(2.0, (note - rootMidi + spreadSemis) / 12.0 + p_.pitchFine) * bend
                                * (sample->sourceSr / sr);
        for (int i = 0; i < N; ++i) frame[(size_t) i] = readAA(srcStart + i * pitchRatio, pitchRatio);

        dwt.forward(frame.data(), coeff.data());
        applyWaveletOps(coeff.data(), dwt, p_, tmp.data(), mag.data());
        dwt.inverse(coeff.data(), idwt.data());

        const bool reversed = p_.reverseProb > 0.0 && rng.nextUnit() < p_.reverseProb;
        for (int i = 0; i < N; ++i)
        {
            const int src = reversed ? (N - 1 - i) : i;
            g->out[(size_t) i] = (float) (idwt[(size_t) src] * grainWindow(i, N, p_.window));
        }
        const double pan = 0.5 + p_.spread * (rng.nextUnit() - 0.5);
        g->panL = (float) std::cos(pan * M_PI * 0.5); g->panR = (float) std::sin(pan * M_PI * 0.5);
        g->pan01 = (float) pan;
        const double len = (double) std::max<std::int64_t>(1, sample->length);
        double sn = srcStart / len; sn -= std::floor(sn);
        g->srcNorm = (float) sn;
        g->len = N; g->writeIndex = 0; g->active = true;
    }

    void advanceEnv()
    {
        switch (stage)
        {
            case Attack:  env += 1.0 / std::max(1.0e-4, p_.attack) / sr; if (env >= 1.0) { env = 1.0; stage = Decay; } break;
            case Decay:   env -= (1.0 - p_.sustain) / std::max(1.0e-4, p_.decay) / sr; if (env <= p_.sustain) { env = p_.sustain; stage = Sustain; } break;
            case Sustain: break;
            case Release: env -= (p_.sustain > 0 ? p_.sustain : 1.0) / std::max(1.0e-4, p_.release) / sr;
                          if (env <= 0.0) { env = 0.0; stage = Idle; if (activeGrains() == 0) active = false; } break;
            default: break;
        }
    }

    double sr = 48000.0;
    wavetree::Dwt dwt;
    wavetree::WaveletType curWavelet = wavetree::WaveletType::Db4;
    int curN = kMaxN;
    std::vector<double> coeff, idwt, mag, tmp, frame;
    std::vector<Grain> grains;

    WaveGrainParams p_;
    const SampleData* sample = nullptr;
    int    note = 60, rootMidi = 60; double velocity = 1.0, bend = 1.0;
    double playPos = 0.0;
    int    spawnCounter = 0;
    double env = 0.0; Stage stage = Idle;
    bool   active = false, released = false;
    Svf    svfL, svfR;
    double dcL1 = 0, dcLy = 0, dcR1 = 0, dcRy = 0;
    Rng    rng { 1 };
};
} // namespace wavegrain
