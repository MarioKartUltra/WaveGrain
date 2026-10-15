// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Abraham Joseph Okayli Masaryk
// =============================================================================
//  WaveGrain — SampleRom.h
//  The sample "ROM": a JUCE-free container + a few procedurally-generated default
//  samples (so the plugin ships self-contained, no binary audio). Each ROM is
//  synthesised at the note frequency of its rootMidi, so granular resampling plays
//  it chromatically in tune.  Header-only, std-only, namespace wavegrain.
// =============================================================================
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "Rng.h"

namespace wavegrain
{
using wavetree::Rng;      // deterministic PRNG from the shared DWT core

struct SampleData
{
    std::vector<std::vector<float>> ch;   // per-channel PCM
    std::int64_t length = 0;
    double sourceSr = 48000.0;            // sr the data was authored/recorded at
    int    rootMidi = 60;                 // note that plays the sample at unity

    int numChannels() const { return (int) ch.size(); }

    // Mono value at an integer sample index (channel-averaged, zero outside range).
    inline float monoAt(std::int64_t i) const
    {
        if (i < 0 || i >= length || ch.empty()) return 0.0f;
        float s = 0.0f;
        for (const auto& c : ch) s += c[(size_t) i];
        return s / (float) ch.size();
    }
};

namespace detail
{
    inline double noteHz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

    inline void normalize(std::vector<float>& x, float peak = 0.95f)
    {
        float mx = 1.0e-9f;
        for (float v : x) mx = std::max(mx, std::fabs(v));
        const float g = peak / mx;
        for (float& v : x) v *= g;
    }

    inline void fadeEdges(std::vector<float>& x, double sr, double ms = 15.0)
    {
        const int f = std::min((int) (ms * 0.001 * sr), (int) x.size() / 2);
        for (int i = 0; i < f; ++i)
        {
            const float g = (float) i / f;
            x[(size_t) i] *= g;
            x[x.size() - 1 - (size_t) i] *= g;
        }
    }

    inline SampleData mono(std::vector<float> x, double sr, int root)
    {
        SampleData s; s.length = (std::int64_t) x.size(); s.sourceSr = sr; s.rootMidi = root;
        s.ch.push_back(std::move(x));
        return s;
    }
}

// ---- Pluck: harmonic stack, slight inharmonicity, exponential decay ----------
inline SampleData makePluck(double sr)
{
    const int root = 48; const double f = detail::noteHz(root);
    const std::int64_t n = (std::int64_t) (2.0 * sr);
    std::vector<float> x((size_t) n, 0.0f);
    const int H = 24;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const double t = i / sr, env = std::exp(-t * 3.5);
        double s = 0.0;
        for (int h = 1; h <= H; ++h)
            s += (1.0 / h) * std::sin(2.0 * M_PI * f * h * (1.0 + 0.0004 * h * h) * t);
        x[(size_t) i] = (float) (env * s);
    }
    detail::normalize(x); detail::fadeEdges(x, sr);
    return detail::mono(std::move(x), sr, root);
}

// ---- Vowel: buzzy glottal source shaped by three formant resonances ("aah") --
inline SampleData makeVowel(double sr)
{
    const int root = 48; const double f = detail::noteHz(root);
    const std::int64_t n = (std::int64_t) (2.0 * sr);
    std::vector<float> x((size_t) n, 0.0f);
    const double Fc[3] = { 700.0, 1220.0, 2600.0 }, BW[3] = { 130.0, 180.0, 250.0 }, Fg[3] = { 1.0, 0.6, 0.35 };
    const int H = 60;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const double t = i / sr;
        double s = 0.0;
        for (int h = 1; h <= H; ++h)
        {
            const double fh = f * h; if (fh > 0.45 * sr) break;
            double form = 0.0;
            for (int k = 0; k < 3; ++k) { const double d = (fh - Fc[k]) / BW[k]; form += Fg[k] / (1.0 + d * d); }
            s += (1.0 / h) * form * std::sin(2.0 * M_PI * fh * t);
        }
        x[(size_t) i] = (float) s;
    }
    detail::normalize(x); detail::fadeEdges(x, sr);
    return detail::mono(std::move(x), sr, root);
}

// ---- Hit: inharmonic metallic partials + noise transient, fast decay ---------
inline SampleData makeHit(double sr)
{
    const int root = 60; const double f = detail::noteHz(root);
    const std::int64_t n = (std::int64_t) (1.2 * sr);
    std::vector<float> x((size_t) n, 0.0f);
    const double ratio[5] = { 1.0, 2.76, 5.40, 8.93, 13.3 };
    Rng rng(7);
    for (std::int64_t i = 0; i < n; ++i)
    {
        const double t = i / sr;
        double s = 0.0;
        for (int k = 0; k < 5; ++k)
            s += std::exp(-t * (6.0 + 2.0 * k)) * std::sin(2.0 * M_PI * f * ratio[k] * t) / (k + 1.0);
        s += std::exp(-t * 60.0) * (rng.nextUnit() * 2.0 - 1.0) * 0.6;   // noise transient
        x[(size_t) i] = (float) s;
    }
    detail::normalize(x); detail::fadeEdges(x, sr, 4.0);
    return detail::mono(std::move(x), sr, root);
}

// ---- Pad: many detuned harmonics, slow swell, sustained ----------------------
inline SampleData makePad(double sr)
{
    const int root = 48; const double f = detail::noteHz(root);
    const std::int64_t n = (std::int64_t) (2.0 * sr);
    std::vector<float> x((size_t) n, 0.0f);
    const int H = 16; Rng rng(3);
    double detune[H + 1], phase[H + 1];
    for (int h = 1; h <= H; ++h) { detune[h] = 1.0 + 0.003 * (rng.nextUnit() - 0.5); phase[h] = rng.nextUnit() * 2.0 * M_PI; }
    for (std::int64_t i = 0; i < n; ++i)
    {
        const double t = i / sr, env = std::min(1.0, t / 0.4);          // slow swell
        double s = 0.0;
        for (int h = 1; h <= H; ++h)
            s += (1.0 / h) * std::sin(2.0 * M_PI * f * h * detune[h] * t + phase[h]);
        x[(size_t) i] = (float) (env * s);
    }
    detail::normalize(x); detail::fadeEdges(x, sr);
    return detail::mono(std::move(x), sr, root);
}

inline std::array<SampleData, 4> makeRomSamples(double sr)
{ return { makePluck(sr), makeVowel(sr), makeHit(sr), makePad(sr) }; }

inline const char* romName(int i)
{ static const char* n[4] = { "Pluck", "Vowel", "Hit", "Pad" }; return (i >= 0 && i < 4) ? n[i] : "User"; }
} // namespace wavegrain
