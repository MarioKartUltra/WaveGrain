// =============================================================================
//  WaveGrain — GrainPipeline.h
//  The four wavelet-domain operations applied to a grain's DWT coefficients
//  (between forward and inverse DWT), plus grain window functions. Operates on a
//  wavetree::Dwt already configured for the grain frame N.  Header-only, no alloc
//  (all scratch is caller-provided).  namespace wavegrain.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include "Dwt.h"
#include "Params.h"

namespace wavegrain
{
// Linear-resample src[0..srcLen) into dst[0..dstLen).
inline void linResample(const double* src, int srcLen, double* dst, int dstLen)
{
    if (srcLen <= 1) { for (int j = 0; j < dstLen; ++j) dst[j] = srcLen ? src[0] : 0.0; return; }
    for (int j = 0; j < dstLen; ++j)
    {
        const double pos = (double) j * (srcLen - 1) / std::max(1, dstLen - 1);
        const int i0 = (int) pos; const double f = pos - i0;
        const double a = src[i0], b = (i0 + 1 < srcLen) ? src[i0 + 1] : src[i0];
        dst[j] = a + f * (b - a);
    }
}

// Apply octave-shift -> tilt x band-subset -> top-K compression to `coeff` in place.
// `tmp` and `mag` are caller-owned scratch of length >= dwt.frameSize().
inline void applyWaveletOps(double* coeff, wavetree::Dwt& dwt, const WaveGrainParams& p,
                            double* tmp, double* mag)
{
    const int N  = dwt.frameSize();
    const int nb = dwt.numBands();     // L + 1
    const int L  = dwt.levels();

    // ---- Op 4: octave scale-shift (octave-quantized) ----
    if (p.octave != 0)
    {
        for (int i = 0; i < N; ++i) tmp[i] = 0.0;
        for (int k = 0; k < dwt.bandSize(0); ++k)                 // approx kept unshifted
            tmp[dwt.bandOff(0) + k] = coeff[dwt.bandOff(0) + k];
        for (int b = 1; b < nb; ++b)
        {
            const int srcB = b - p.octave;   // +octave => content to finer/higher bands => pitch up
            if (srcB >= 1 && srcB < nb)
                linResample(coeff + dwt.bandOff(srcB), dwt.bandSize(srcB),
                            tmp   + dwt.bandOff(b),     dwt.bandSize(b));
        }
        for (int i = 0; i < N; ++i) coeff[i] = tmp[i];
    }

    // ---- Ops 2+3: scale tilt x band subset (one per-band multiply) ----
    const double tiltSlope = (p.tilt - 0.5) * 4.0;
    const int blo = (int) std::lround(p.bandLow  * L);
    const int bhi = (int) std::lround(p.bandHigh * L);
    for (int b = 0; b < nb; ++b)
    {
        const int oct = (b == 0) ? 0 : (b - 1);
        double g = std::pow(2.0, -oct * tiltSlope);
        if (b < blo || b > bhi) g = 0.0;
        const int off = dwt.bandOff(b), sz = dwt.bandSize(b);
        for (int k = 0; k < sz; ++k) coeff[off + k] *= g;
    }

    // ---- Op 1: compression / top-K (hard threshold, energy-bounded) ----
    if (p.compress > 0.0)
    {
        const double keepFrac = 1.0 - p.compress * 0.99;
        int K = (int) std::lround(keepFrac * N);
        K = std::max(1, std::min(K, N));
        if (K < N)
        {
            for (int i = 0; i < N; ++i) mag[i] = std::fabs(coeff[i]);
            std::nth_element(mag, mag + (N - K), mag + N);
            const double thresh = mag[N - K];
            for (int i = 0; i < N; ++i) if (std::fabs(coeff[i]) < thresh) coeff[i] = 0.0;
        }
    }
}

// Grain window (applied to the time-domain grain after IDWT).
inline double grainWindow(int i, int N, GrainWindow w)
{
    const double x = (double) i / (N - 1);                 // 0..1
    switch (w)
    {
        case GrainWindow::Hann:  return 0.5 - 0.5 * std::cos(2.0 * M_PI * x);
        case GrainWindow::Tukey: { const double a = 0.5;
            if (x < a * 0.5) return 0.5 * (1.0 + std::cos(M_PI * (2.0 * x / a - 1.0)));
            if (x > 1.0 - a * 0.5) return 0.5 * (1.0 + std::cos(M_PI * (2.0 * x / a - 2.0 / a + 1.0)));
            return 1.0; }
        case GrainWindow::Gauss: { const double d = (x - 0.5) / 0.2; return std::exp(-0.5 * d * d); }
        case GrainWindow::Tri:   return 1.0 - std::fabs(2.0 * x - 1.0);
    }
    return 1.0;
}
} // namespace wavegrain
