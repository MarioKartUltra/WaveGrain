// =============================================================================
//  WaveGrain — Codec.h
//  A block-wise wavelet audio codec applied to the WHOLE sample "ROM" once (on
//  the message thread), so the instrument plays from lossily wavelet-compressed
//  data — the "if the DWT had won over the FFT, CDs would store wavelet
//  coefficients" premise. Two lossy stages per block: keep only the top-K
//  coefficients (sparsity) and quantise the survivors to `bits` (bit depth).
//  Orthonormal DWT => energy-bounded. Reports an approximate compression ratio.
//  JUCE-free, namespace wavegrain.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Dwt.h"
#include "SampleRom.h"

namespace wavegrain
{
struct CodecStats { double ratio = 1.0; double kbps = 0.0; long long keptCoeffs = 0; };

// Encode `src` into a new (lossy) SampleData. romRatio 0..1 (0 = keep all),
// bits 2..16 (16 = no quantisation). `wt` = the analysing wavelet.
inline SampleData waveletEncode(const SampleData& src, double romRatio, int bits,
                                wavetree::WaveletType wt, CodecStats* stats = nullptr)
{
    const int block = 2048, L = 9;                 // codec frame (block >> L = 4)
    wavetree::Dwt dwt; dwt.configure(wavetree::makeWavelet(wt), block, L);
    std::vector<double> frame(block), coeff(block), rec(block), mag(block);

    SampleData out; out.length = src.length; out.sourceSr = src.sourceSr; out.rootMidi = src.rootMidi;
    out.ch.resize(src.numChannels());

    const double keepFrac = 1.0 - romRatio * 0.98;         // 0 -> keep all, 1 -> keep ~2%
    const int K = std::max(1, std::min(block, (int) std::lround(keepFrac * block)));
    long long kept = 0;

    for (int c = 0; c < src.numChannels(); ++c)
    {
        out.ch[(size_t) c].assign((size_t) src.length, 0.0f);
        for (std::int64_t start = 0; start < src.length; start += block)
        {
            for (int i = 0; i < block; ++i)
            { const std::int64_t j = start + i; frame[(size_t) i] = (j < src.length) ? (double) src.ch[(size_t) c][(size_t) j] : 0.0; }

            dwt.forward(frame.data(), coeff.data());

            // ---- sparsity: keep top-K magnitudes ----
            if (K < block)
            {
                for (int i = 0; i < block; ++i) mag[(size_t) i] = std::fabs(coeff[(size_t) i]);
                std::nth_element(mag.begin(), mag.begin() + (block - K), mag.end());
                const double th = mag[(size_t) (block - K)];
                for (int i = 0; i < block; ++i) if (std::fabs(coeff[(size_t) i]) < th) coeff[(size_t) i] = 0.0;
            }
            // ---- bit depth: uniform-quantise the survivors (per-frame max) ----
            if (bits < 16)
            {
                double cmax = 1.0e-12;
                for (int i = 0; i < block; ++i) cmax = std::max(cmax, std::fabs(coeff[(size_t) i]));
                const double step = cmax / (double) (1 << (bits - 1));
                if (step > 0.0)
                    for (int i = 0; i < block; ++i) coeff[(size_t) i] = std::round(coeff[(size_t) i] / step) * step;
            }

            dwt.inverse(coeff.data(), rec.data());
            for (int i = 0; i < block; ++i)
            { const std::int64_t j = start + i; if (j < src.length) out.ch[(size_t) c][(size_t) j] = (float) rec[(size_t) i]; }
            for (int i = 0; i < block; ++i) if (coeff[(size_t) i] != 0.0) ++kept;
        }
    }

    if (stats)
    {
        const double total = (double) src.length * src.numChannels();
        // ~1 bit/coefficient significance map + bits per surviving coefficient.
        const double codedBits = total * 1.0 + (double) kept * bits;
        const double pcmBits = total * 16.0;
        stats->ratio = pcmBits / std::max(1.0, codedBits);
        const double dur = (double) src.length / std::max(1.0, src.sourceSr);
        stats->kbps = (codedBits / 1000.0) / std::max(1.0e-6, dur);
        stats->keptCoeffs = kept;
    }
    return out;
}
} // namespace wavegrain
