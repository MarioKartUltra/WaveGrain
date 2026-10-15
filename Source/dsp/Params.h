// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Abraham Joseph Okayli Masaryk
// =============================================================================
//  WaveGrain II — Params.h   (plain seam between JUCE and the DSP core)
//  v2: explicit grain clock (hop in samples, host-side ms/beat-sync resolved
//  before this struct), per-grain pitch spread + reverse probability, and the
//  shared v2 seed policy (acv2::SeedPolicy) driving all grain randomness.
// =============================================================================
#pragma once
#include <cstdint>
#include "Wavelets.h"     // wavetree::WaveletType

namespace wavegrain
{
enum class GrainWindow { Hann = 0, Tukey = 1, Gauss = 2, Tri = 3 };

struct WaveGrainParams
{
    // ---- source ----
    int    sample     = 0;      // ROM index (0..3) or user (4)
    int    rootNote   = 60;     // MIDI note that plays the sample at unity

    // ---- grain clock (resolved host-side: ms / beat-division -> samples) ----
    int    grainSize  = 1024;   // power-of-two grain frame N (256..4096)
    double hopSamples = 576.0;  // spawn interval in samples (>= 32)

    // ---- grain cloud ----
    double position   = 0.0;    // 0..1 playhead into the sample
    double scan       = 0.0;    // -1..1 playhead scan rate
    bool   freeze     = false;  // hold the playhead (cloud loops in place)
    double spray      = 0.1;    // 0..1 position/time jitter
    double pitchFine  = 0.0;    // -1..1 octaves fine pitch
    double pitchSpread = 0.0;   // 0..12 semitones random per-grain pitch offset
    double reverseProb = 0.0;   // 0..1 probability a grain plays reversed
    double spread     = 0.4;    // 0..1 stereo pan spread
    GrainWindow window = GrainWindow::Hann;

    // ---- wavelet-domain ops (per grain) ----
    wavetree::WaveletType wavelet = wavetree::WaveletType::Db4;
    double compress   = 0.0;    // 0..1 -> keep 100%..1% of coefficients (top-K)
    double tilt       = 0.5;    // 0.5 flat; spectral slope across scales
    double bandLow    = 0.0;    // 0..1 lowest kept band (scale subset)
    double bandHigh   = 1.0;    // 0..1 highest kept band
    int    octave     = 0;      // -2..+2 DWT octave scale-shift

    // ---- seed policy (acv2) ----
    int           seedMode   = 0;         // 0 Fixed, 1 Per-Key, 2 Random-on-strike
    std::uint64_t seedGlobal = 1234;      // global seed

    // ---- ROM codec (whole-sample wavelet compression, baked once) ----
    bool   codecOn    = false;  // play from a lossily wavelet-compressed ROM
    double romRatio   = 0.5;    // 0..1 sparsity (fraction of coefficients discarded)
    int    romBits    = 12;     // 2..16 quantisation bit depth of survivors

    // ---- amp envelope / tone / out ----
    double attack  = 0.01;      // seconds (host resolves ms / beat sync)
    double decay   = 0.2;
    double sustain = 0.8;
    double release = 0.4;
    double cutoff  = 0.9;
    double reso    = 0.15;
    double output  = 0.7;
    int    numVoices = 8;
};
} // namespace wavegrain
