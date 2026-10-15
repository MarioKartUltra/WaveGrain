// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Abraham Joseph Okayli Masaryk
// WaveGrain II — offline JUCE-free check.
// v1 suite: DWT PR + per-ROM render/pitch + compression + grain cap + codec.
// v2 additions: tempo-sync table math, LFO shape bounds/period/determinism,
// seed policy (fixed / per-key / random-on-strike) end-to-end through the
// engine, per-grain reverse + pitch spread stay bounded, anti-aliased read.
#include "../Source/dsp/Dwt.h"
#include "../Source/dsp/GranularEngine.h"
#include "../Source/dsp/Codec.h"
#include "../Source/common/acv2/dsp/TempoSync.h"
#include "../Source/common/acv2/dsp/Lfo.h"
#include "../Source/common/acv2/dsp/SeedPolicy.h"
#include "../Source/common/acv2/dsp/Units.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
using namespace wavegrain;

static int fails = 0;
static void check(bool ok, const char* name, const std::string& d = "")
{ printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, d.empty() ? "" : "  -> ", d.c_str()); if (!ok) ++fails; }

static void prTest()
{
    const int N = 1024, L = 6;
    for (int wt = 0; wt < 4; ++wt)
    {
        wavetree::Dwt dwt; dwt.configure(wavetree::makeWavelet((wavetree::WaveletType) wt), N, L);
        std::vector<double> x(N), c(N), xr(N);
        unsigned s = 999u + wt;
        for (int i = 0; i < N; ++i) { s = s * 1664525u + 1013904223u; x[i] = ((s >> 9) * (1.0 / 8388608.0)) - 1.0; }
        dwt.forward(x.data(), c.data()); dwt.inverse(c.data(), xr.data());
        double mx = 0; for (int i = 0; i < N; ++i) mx = std::fmax(mx, std::fabs(x[i] - xr[i]));
        check(mx < 1e-9, (std::string("PR ") + wavetree::makeWavelet((wavetree::WaveletType) wt).name).c_str(), "err=" + std::to_string(mx));
    }
}

static double dom(const std::vector<float>& v, double sr)
{
    int n = (int) v.size(); double mean = 0; for (float x : v) mean += x; mean /= n;
    int lo = (int) (sr / 2000), hi = (int) (sr / 60); double best = -1; int bl = 0;
    for (int lag = lo; lag < hi && lag < n; ++lag)
    { double a = 0; for (int i = 0; i + lag < n; ++i) a += (v[i] - mean) * (v[i + lag] - mean); if (a > best) { best = a; bl = lag; } }
    return bl > 0 ? sr / bl : 0;
}

static std::vector<float> render(WaveGrainParams p, int note, double sr, double secs)
{
    GranularEngine eng; eng.prepare(sr); eng.setParameters(p);
    const int total = (int) (secs * sr), block = 256;
    std::vector<float> out((size_t) total, 0), L((size_t) block), R((size_t) block);
    bool on = false;
    for (int nprocessed = 0; nprocessed < total; )
    {
        if (!on) { eng.noteOn(note, 0.9); on = true; }
        const int m = std::min(block, total - nprocessed);
        eng.renderBlock(L.data(), R.data(), m);
        for (int i = 0; i < m; ++i) out[(size_t) (nprocessed + i)] = L[(size_t) i];
        nprocessed += m;
    }
    return out;
}

static double rmsOf(const std::vector<float>& v)
{ double ss = 0; for (float s : v) ss += (double) s * s; return std::sqrt(ss / std::max<size_t>(1, v.size())); }

static bool identical(const std::vector<float>& a, const std::vector<float>& b)
{ if (a.size() != b.size()) return false; for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false; return true; }

// -----------------------------------------------------------------------------
static void tempoSyncTests()
{
    using namespace acv2::tempo;
    check(kNumDivisions == 16, "division table frozen at 16");
    check(std::fabs(divisionToMs(8, 120.0) - 500.0) < 1e-9, "1/4 @ 120bpm = 500 ms");
    check(std::fabs(divisionToMs(5, 120.0) - 250.0) < 1e-9, "1/8 @ 120bpm = 250 ms");
    check(std::fabs(divisionToMs(10, 120.0) - 750.0) < 1e-9, "1/4. dotted = 750 ms");
    check(std::fabs(divisionToMs(6, 120.0) - 500.0 * 2.0 / 3.0) < 1e-9, "1/4T triplet = 333.3 ms");
    check(std::fabs(divisionToMs(15, 60.0) - 16000.0) < 1e-9, "4/1 @ 60bpm = 16 s");
    check(indexOfLabel("1/4") == 8 && indexOfLabel("1/16T") == 1, "label lookup");
    check(std::fabs(msToHz(250.0) - 4.0) < 1e-12, "msToHz");
    // monotonic non-decreasing durations across the frozen list
    bool mono = true;
    for (int i = 1; i < kNumDivisions; ++i) if (wholeNotes(i) < wholeNotes(i - 1)) mono = false;
    check(mono, "division durations monotonic");
    check(acv2::units::msToText(500.0) == "500 ms" && acv2::units::msToText(1500.0) == "1.50 s", "units ms text");
}

static void lfoTests()
{
    using acv2::Lfo; using acv2::LfoShape;
    const double sr = 48000.0;

    // Shape bounds: every shape stays in [-1,1] bipolar / [0,1] unipolar.
    for (int sh = 0; sh < acv2::kNumLfoShapes; ++sh)
    {
        Lfo l; l.prepare(sr); l.setShape((LfoShape) sh); l.setRateHz(3.7); l.reseed(77);
        float mn = 1e9f, mx = -1e9f;
        for (int i = 0; i < 3000; ++i) { const float v = l.processBlock(64); mn = std::min(mn, v); mx = std::max(mx, v); }
        check(mn >= -1.0f && mx <= 1.0f, (std::string("LFO bounds ") + acv2::lfoShapeName((LfoShape) sh)).c_str(),
              "min=" + std::to_string(mn) + " max=" + std::to_string(mx));
    }

    // Period: a 2 Hz sine at 48 kHz crosses zero upward every 24000 samples.
    {
        Lfo l; l.prepare(sr); l.setShape(LfoShape::Sine); l.setRateHz(2.0);
        double prev = l.processSample(); int firstUp = -1, secondUp = -1;
        for (int i = 1; i < 60000; ++i)
        {
            const double v = l.processSample();
            if (prev < 0.0 && v >= 0.0) { if (firstUp < 0) firstUp = i; else if (secondUp < 0) { secondUp = i; break; } }
            prev = v;
        }
        check(secondUp > 0 && std::abs((secondUp - firstUp) - 24000) < 30, "LFO sine period 2Hz",
              "period=" + std::to_string(secondUp - firstUp));
    }

    // Synced division: 1/4 @ 120 bpm = 2 Hz.
    {
        Lfo l; l.prepare(sr); l.setBpm(120.0); l.setSyncedDivision(8);
        check(std::fabs(l.rateHz() - 2.0) < 1e-9, "LFO synced 1/4@120 = 2 Hz", "hz=" + std::to_string(l.rateHz()));
    }

    // Determinism: same seed => identical S&H stream; different seed differs.
    {
        auto run = [&](std::uint64_t seed)
        {
            Lfo l; l.prepare(sr); l.setShape(LfoShape::SampleHold); l.setRateHz(50.0); l.reseed(seed);
            std::vector<float> v; for (int i = 0; i < 400; ++i) v.push_back(l.processBlock(128));
            return v;
        };
        auto a = run(123), b = run(123), c = run(124);
        check(identical(a, b), "LFO S&H deterministic (same seed)");
        check(!identical(a, c), "LFO S&H differs across seeds");
    }
}

static void seedPolicyTests()
{
    using acv2::SeedPolicy; using acv2::SeedMode;
    {
        SeedPolicy sp; sp.setMode(SeedMode::Fixed); sp.setGlobalSeed(42);
        check(sp.onNoteOn(60) == 42 && sp.onNoteOn(72) == 42, "seed Fixed: constant");
    }
    {
        SeedPolicy sp; sp.setMode(SeedMode::PerKey); sp.setGlobalSeed(42);
        const auto a = sp.onNoteOn(60), b = sp.onNoteOn(60), c = sp.onNoteOn(61);
        check(a == b && a != c, "seed PerKey: stable per key, distinct across keys");
        check(acv2::perKeySeed(42, 60) == a, "seed PerKey matches perKeySeed()");
    }
    {
        SeedPolicy sp; sp.setMode(SeedMode::RandomOnStrike); sp.setGlobalSeed(42);
        const auto a = sp.onNoteOn(60), b = sp.onNoteOn(60);
        check(a != b, "seed RandomOnStrike: fresh per strike");
        check(sp.lastUsedSeed() == b, "seed lastUsedSeed reported");
    }

    // End-to-end: engine renders are bit-identical for Fixed/PerKey and differ
    // across strikes for RandomOnStrike (spray gives the randomness a voice).
    const double sr = 48000.0;
    auto renderSeeded = [&](int mode, std::uint64_t seed)
    {
        WaveGrainParams p; p.sample = 0; p.grainSize = 1024; p.hopSamples = 300.0;
        p.spray = 0.5; p.pitchSpread = 4.0; p.spread = 1.0;
        p.seedMode = mode; p.seedGlobal = seed;
        return render(p, 57, sr, 0.5);
    };
    auto f1 = renderSeeded(0, 999), f2 = renderSeeded(0, 999);
    check(identical(f1, f2), "engine Fixed-seed render bit-identical");
    auto k1 = renderSeeded(1, 999), k2 = renderSeeded(1, 999);
    check(identical(k1, k2), "engine PerKey render bit-identical");
    auto r1 = renderSeeded(2, 999), r2 = renderSeeded(2, 999);
    check(identical(r1, r2), "engine RandomOnStrike deterministic from global seed (no entropy)");
    // two strikes inside ONE engine instance must differ in RandomOnStrike
    {
        WaveGrainParams p; p.sample = 0; p.grainSize = 1024; p.hopSamples = 300.0;
        p.spray = 0.5; p.seedMode = 2; p.seedGlobal = 999;
        GranularEngine eng; eng.prepare(sr); eng.setParameters(p);
        eng.noteOn(57, 0.9);
        const auto s1 = eng.lastUsedSeed();
        eng.noteOn(57, 0.9);
        const auto s2 = eng.lastUsedSeed();
        check(s1 != s2, "engine RandomOnStrike: consecutive strikes get fresh seeds");
    }
}

static void v2FeatureTests()
{
    const double sr = 48000.0;
    // reverse probability 1: every grain reversed — must stay finite & audible
    {
        WaveGrainParams p; p.sample = 0; p.grainSize = 1024; p.hopSamples = 300.0; p.reverseProb = 1.0;
        auto out = render(p, 57, sr, 0.8);
        bool fin = true; double pk = 0;
        for (float s : out) { if (!std::isfinite(s)) fin = false; pk = std::fmax(pk, std::fabs(s)); }
        check(fin && pk < 4.0 && rmsOf(out) > 1e-4, "reverseProb=1 finite+bounded+audible");
    }
    // pitch spread 12 st: worst-case per-grain transposition stays bounded (AA path)
    {
        WaveGrainParams p; p.sample = 3; p.grainSize = 2048; p.hopSamples = 400.0; p.pitchSpread = 12.0;
        auto out = render(p, 84, sr, 0.8);   // note 84 = 2 octaves up + spread
        bool fin = true; double pk = 0;
        for (float s : out) { if (!std::isfinite(s)) fin = false; pk = std::fmax(pk, std::fabs(s)); }
        check(fin && pk < 4.0, "pitchSpread=12 @ note 84 (AA read) finite+bounded");
    }
    // hop clock: halving hopSamples ups grain concurrency
    {
        WaveGrainParams p; p.sample = 3; p.grainSize = 1024; p.freeze = true;
        auto count = [&](double hop)
        {
            p.hopSamples = hop;
            GranularEngine eng; eng.prepare(sr); eng.setParameters(p); eng.noteOn(57, 0.9);
            std::vector<float> L(256), R(256); int mx = 0;
            for (int b = 0; b < 200; ++b) { eng.renderBlock(L.data(), R.data(), 256); mx = std::max(mx, eng.activeGrains()); }
            return mx;
        };
        const int slow = count(512.0), fast = count(128.0);
        check(fast > slow, "hop clock: shorter hop => denser cloud",
              "hop512=" + std::to_string(slow) + " hop128=" + std::to_string(fast));
    }
    // grain markers telemetry well-formed
    {
        WaveGrainParams p; p.sample = 3; p.grainSize = 1024; p.hopSamples = 200.0; p.spread = 1.0;
        GranularEngine eng; eng.prepare(sr); eng.setParameters(p); eng.noteOn(57, 0.9);
        std::vector<float> L(256), R(256);
        for (int b = 0; b < 60; ++b) eng.renderBlock(L.data(), R.data(), 256);
        std::vector<float> mk; eng.copyGrainMarkers(mk);
        bool ok = !mk.empty() && mk.size() % 3 == 0;
        for (size_t i = 0; i < mk.size(); ++i) if (!(mk[i] >= -0.01f && mk[i] <= 1.01f)) ok = false;
        check(ok, "grain markers: non-empty triplets in [0,1]", "n=" + std::to_string(mk.size() / 3));
    }
}

int main()
{
    const double sr = 48000;
    printf("== perfect reconstruction ==\n"); prTest();

    printf("\n== render each ROM (note 57 -> 220 Hz for pitched ROMs) ==\n");
    const char* names[4] = { "Pluck", "Vowel", "Hit", "Pad" };
    const bool pitched[4] = { true, true, false, true };
    for (int rom = 0; rom < 4; ++rom)
    {
        WaveGrainParams p; p.sample = rom; p.grainSize = 2048; p.hopSamples = 512.0; p.position = 0.25;
        p.compress = 0.0; p.tilt = 0.5; p.spray = 0.0; p.window = GrainWindow::Hann;
        auto out = render(p, 57, sr, 1.2);
        double peak = 0; bool fin = true;
        for (float s : out) { if (!std::isfinite(s)) fin = false; peak = std::fmax(peak, std::fabs(s)); }
        const double rms = rmsOf(out);
        std::vector<float> win(out.begin() + (int) (0.3 * sr), out.begin() + (int) (0.8 * sr));
        const double f = dom(win, sr);
        std::string d = "peak=" + std::to_string(peak).substr(0, 5) + " rms=" + std::to_string(rms).substr(0, 6) + " f=" + std::to_string(f).substr(0, 6);
        check(fin && peak < 4.0 && rms > 1e-4, names[rom], d);
        if (pitched[rom]) check(std::fabs(f - 220.0) / 220.0 < 0.06, (std::string(names[rom]) + " pitch ~220Hz").c_str(), "f=" + std::to_string(f).substr(0, 6));
    }

    printf("\n== extreme compression stays bounded ==\n");
    { WaveGrainParams p; p.sample = 0; p.grainSize = 1024; p.compress = 0.95;
      auto out = render(p, 57, sr, 1.0);
      double peak = 0; bool fin = true;
      for (float s : out) { if (!std::isfinite(s)) fin = false; peak = std::fmax(peak, std::fabs(s)); }
      check(fin && peak < 4.0 && rmsOf(out) > 1e-5, "compression=0.95 finite+bounded+audible",
            "peak=" + std::to_string(peak).substr(0, 5)); }

    printf("\n== grain cap under dense hop, freeze ==\n");
    { WaveGrainParams p; p.sample = 3; p.grainSize = 512; p.hopSamples = 32.0; p.freeze = true;
      GranularEngine eng; eng.prepare(sr); eng.setParameters(p); eng.noteOn(57, 0.9);
      std::vector<float> L(256), R(256); int maxG = 0;
      for (int b = 0; b < 400; ++b) { eng.renderBlock(L.data(), R.data(), 256); maxG = std::max(maxG, eng.activeGrains()); }
      check(maxG <= GrainVoice::kMaxGrains, "active grains within kMaxGrains", "max=" + std::to_string(maxG)); }

    printf("\n== ROM codec (whole-sample wavelet compression) ==\n");
    {
        GranularEngine eng; eng.prepare(sr);
        WaveGrainParams p; p.sample = 3; p.codecOn = true; p.grainSize = 1024;
        eng.setParameters(p);
        const SampleData* orig = eng.originalSample(3);
        CodecStats st;
        SampleData coded = waveletEncode(*orig, 0.9, 6, wavetree::WaveletType::Db4, &st);
        bool fin = true; float pk = 0;
        for (auto& c : coded.ch) for (float v : c) { if (!std::isfinite(v)) fin = false; pk = std::fmax(pk, std::fabs(v)); }
        eng.setCodedSample(std::move(coded));
        eng.noteOn(57, 0.9);
        std::vector<float> L(256), R(256); double opk = 0; bool ofin = true;
        for (int b = 0; b < 200; ++b) { eng.renderBlock(L.data(), R.data(), 256); for (int i = 0; i < 256; ++i) { if (!std::isfinite(L[i])) ofin = false; opk = std::fmax(opk, std::fabs((double) L[i])); } }
        check(fin && pk < 4.0 && st.ratio > 1.5, "codec encodes bounded + compresses (ratio>1.5)",
              "ratio=" + std::to_string(st.ratio).substr(0, 4) + ":1 kbps=" + std::to_string((int) st.kbps));
        check(ofin && opk > 1e-4, "codec-ROM renders finite + audible", "peak=" + std::to_string(opk).substr(0, 5));
    }

    printf("\n== v2: tempo-sync table ==\n"); tempoSyncTests();
    printf("\n== v2: LFO ==\n"); lfoTests();
    printf("\n== v2: seed policy ==\n"); seedPolicyTests();
    printf("\n== v2: grain features ==\n"); v2FeatureTests();

    printf("\n%d issue(s)\n", fails);
    return fails;
}
