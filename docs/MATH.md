# WaveGrain II — math notes

Terse technical notes on the DSP. Code: `Source/dsp/` (JUCE-free, header-only).
The DWT machinery (`Dwt.h`, `Wavelets.h`, `Rng.h`) is WaveTree's, reused
unchanged: periodic multi-level orthonormal DWT/IDWT, perfect reconstruction
(inverse = exact adjoint, verified ~1e-12 in CTest). Wavelets: Haar/Db2/Db4/Db6.

## 1. Granular engine

Grains are **pre-rendered at spawn**; the per-sample audio path is only
overlap-add of finished grain buffers (no per-sample DWT work).

Spawn pipeline for one grain of length `N = 2^L` samples:

1. **Source read** (anti-aliased, cubic): read the ROM at position `p` with
   ratio `r = 2^((note - root + fine)/12) · 2^(spread·u/12)`, `u ~ U(-1,1)`
   per grain. For `r > 1` (transposing up) a plain cubic read folds source
   content above `f_N/r` back as aliasing, so the read averages `M = ceil(r)`
   cubic taps spread uniformly across the skipped span — a box pre-filter with
   first null at the fold frequency (cheap quasi-sinc decimation).
   With probability `P_rev` the grain's read direction is reversed.
2. **Forward DWT**: `c = W x`, `L` levels (orthonormal, periodic).
3. **Wavelet ops** on `c`, in place, in this order
   (`GrainPipeline.h::applyWaveletOps`):
   - **Octave scale-shift** (op 4): move detail band `j` to `j + s` (octave
     quantized); the approximation band stays. Coefficients falling outside
     the tree are dropped. This shifts content by whole octaves *in the
     wavelet domain*, i.e. it time-scales each band's grid rather than
     resampling — the characteristic "wrong but musical" DWT pitch shift.
   - **Scale tilt** (op 2) × **band subset** (op 3), one fused per-band
     multiply: band gain `g_j = 2^{-oct_j · m}` with slope
     `m = (tilt - 0.5)·4` (tilt 0..1 → slope ±2 per octave), zeroed outside
     `[bandLow, bandHigh]`.
   - **Top-K compression** (op 1): keep the `K = round((1 - 0.99·compress)·N)`
     largest-|c| coefficients, zero the rest (`nth_element` threshold).
     Orthonormality ⇒ kept energy = `Σ kept c²` — compression is an energy
     lower bound, never a gain.
4. **Inverse DWT**: `x' = Wᵀ c` (exact adjoint).
5. **Window**: Hann `w(x) = ½ - ½cos(2πx)` (or the alternate shapes in
   `GrainWindow`), then overlap-add into the voice bus.

**Grain clock**: hop `H` = grain rate as period; rate and size each follow the
three-param sync convention (`*Sync` bool, `*Ms` float, `*Div` frozen 16-entry
beat-division list; beat time from host BPM, cached, default 120). Spray
jitters spawn position; density = overlap `N/H`.

**Determinism / seed policy** (`acv2::SeedPolicy`): Fixed (one global seed),
Per-Key (`seed = hash(global, midiNote)` — each key reproducible), or
Random-on-strike (fresh seed per note-on, last seed published to the UI so it
can be locked). All grain randomness (spray, pitch spread `u`, reverse draw,
pan) derives from the per-voice RNG stream — no wall-clock entropy on the
audio thread.

**LFO mod matrix** (JUCE side, block rate): two `acv2::Lfo` sources; each
destination `d` reads its APVTS value `v_d` and applies
`v'_d = clamp(v_d + depth_A·lfoA + depth_B·lfoB)` *before* the `Params`
struct is filled — the DSP core never sees the mod matrix. Depths are ±1
(shown ±%). LFO rates are free Hz or the same frozen division list; retrigger
free / key / transport (phase from ppq).

## 2. ROM codec (`Codec.h`)

Whole-sample wavelet codec, run once on the message thread on parameter
change, published to the audio thread as an atomic `shared_ptr` snapshot
(lock-free hand-off; old ROM freed on the message thread).

Per block of `B = 2048` samples, `L = 9` levels:

1. `c = W x` (orthonormal DWT).
2. **Sparsity**: keep top `K = round(B / ratio)` coefficients by |c|
   (`nth_element` threshold `th`), zero `|c| < th`.
3. **Bit depth**: uniform mid-tread quantization of survivors to `bits`
   (2..16; 16 = passthrough) with a per-frame scale `max|c|`:
   `q(c) = round(c/Δ)·Δ`, `Δ = max|c| / (2^{bits-1} - 1)`.
4. `x' = Wᵀ c` — the loss is *baked into the ROM*; playback cost is zero.

Reported stats: achieved ratio ≈ `B·16 / (K·bits)` bits kept per block, and
bitrate `kbps = kept-bits · f_s / (B·1000)`. Orthonormal DWT ⇒ the error
energy equals the discarded-coefficient energy: the codec is energy-bounded
and cannot exceed source amplitude by more than the quantization step.

## 3. RT safety

No allocation/locks/IO on the audio path: grain pool and DWT scratch are
preallocated at `prepareToPlay`; sample swap is `std::atomic<shared_ptr>`
snapshot; `ScopedNoDenormals`; `isfinite` guards; DC-block + soft-limit on
the output. LFO/telemetry values cross to the UI via atomics only.
