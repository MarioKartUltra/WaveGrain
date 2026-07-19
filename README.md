# WaveGrain — a DWT-based granular ROMpler

Load a sample, play it **chromatically via MIDI**, through a **granular** engine
where **every grain is processed in the wavelet domain**. The premise: if the DWT
had won over the FFT, sample "ROMs" would be stored as wavelet coefficients — so
keeping only the largest coefficients *is* the compression.

Sibling of WaveTree; it reuses WaveTree's perfect-reconstruction DWT core
(`Dwt.h`/`Wavelets.h`/`Rng.h`) unchanged.

## How it works
A voice (one held note) owns a **playhead** into the sample and a grain pool.
Grains are **pre-rendered at spawn**: read a power-of-two window at the note's
pitch (cubic-interpolated → continuous chromatic pitch), **forward-DWT**, apply the
four wavelet ops, **inverse-DWT**, Hann-window; the per-sample path is just
overlap-add. All DWT work is a bounded burst at spawn (RT-safe: no audio-thread
allocation; the sample is handed across lock-free via `shared_ptr` + atomic).

The four wavelet ops per grain:
- **Compression** (top-K): keep only the largest coefficients → progressive lo-fi
  "wavelet codec" degradation. Orthonormal ⇒ always energy-bounded.
- **Scale Tilt** — per-octave spectral slope across the detail bands (a wavelet EQ).
- **Band Low/High** — reconstruct from a scale-band *subset* (coarse-only, fine-only,
  a range) → scale-focused wavelet grains.
- **DWT Octave** — octave scale-shift of the coefficients (octave-quantized;
  continuous pitch comes from grain resampling, kept separate).

### ROM Codec (lean into the codec)
Beyond the per-grain compression, **ROM Codec** wavelet-compresses the *entire
sample once* and plays from the lossy result — the "if the DWT had won over the
FFT, a CD would store wavelet coefficients" idea, made literal. It runs a block-wise
wavelet audio codec on the whole sample: per 2048-sample block, forward-DWT, keep
the top-K coefficients (**ROM Ratio**), uniform-quantise the survivors to **ROM
Bits** (2–16), inverse-DWT. The reconstructed lossy sample is what the grains read,
and the header shows the achieved **compression ratio + bitrate** (e.g. `codec
10.1:1  75 kbps`). Encoding happens on the message thread when the codec settings
change and is published to the audio engine lock-free — so at high Ratio / low Bits
you hear genuine wavelet-codec artifacts (sparse-coefficient shimmer, quantisation
grit) baked into the "ROM." Distinct from the per-grain **Compression**, which is a
transient real-time effect; ROM Codec bakes the loss into the stored sample.

Plus: **Sample** (4 procedural ROMs — Pluck/Vowel/Hit/Pad — + User), **Root Note**,
**Grain Size**, **Density**, **Position**, **Scan**, **Freeze**, **Spray**, **Fine
Pitch**, **Spread**, **Window**, **Wavelet** (Haar/Db2/Db4/Db6), ADSR, filter,
output, polyphony. Built-in ROMs are generated procedurally (no binary assets) at
each note's frequency, so they play in tune.

## Build (Apple Silicon)
```bash
./build.sh          # Apple clang + Xcode SDK pinned, conda env scrubbed
```
`COPY_PLUGIN_AFTER_BUILD` installs VST3+AU to your user folders. Ad-hoc sign + copy
to the system folder for a DAW that scans it:
```bash
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/VST3/WaveGrain.vst3
cp -R ~/Library/Audio/Plug-Ins/VST3/WaveGrain.vst3 /Library/Audio/Plug-Ins/VST3/
codesign --force --deep -s - /Library/Audio/Plug-Ins/VST3/WaveGrain.vst3
```
> JUCE resolves from `external/JUCE` → `~/projects/JUCE` (macOS TCC blocks `~/Downloads`).

## Test / play
- `ctest --test-dir build` — DWT perfect reconstruction + per-ROM render/pitch +
  compression bound + grain-cap.
- Standalone: `open build/WaveGrain_artefacts/Release/Standalone/WaveGrain.app`,
  pick an output device, play the on-screen keyboard; **drag a WAV onto the window**
  or click **Load…** to bring in your own sample; sweep Position/Density/Freeze and
  Compression/Octave/Band-Low/High.
- `auval -v aumu Wgrn Wvgr`.

## Known limitations
- **Octave-quantized DWT pitch** (the DWT Octave control); fine/chromatic pitch is
  grain resampling.
- **Decimated-DWT shift-variance** — grain-phase-dependent compression/mask shimmer;
  masked by the Hann window + overlap (reads as texture).
- **Per-grain DWT cost** spikes at spawn under very high density × polyphony
  (bounded by the grain cap + a smooth-overlap hop floor).
- **Up-pitch aliasing** at extreme notes / DWT-Octave (no anti-alias on the read).
- **Mono grains** — the sample is downmixed to mono; stereo is synthesized by
  per-grain Spread panning.
- Periodic-grain "grain-rate tone" is suppressed by always-on spawn-time jitter.
