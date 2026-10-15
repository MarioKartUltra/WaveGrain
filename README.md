# WaveGrain II

A granular sampler where **every grain is processed in the wavelet domain**. Load a sample, play
it chromatically over MIDI, and shape each grain with wavelet operations. The premise: if the DWT
had won over the FFT, sample ROMs would be stored as wavelet coefficients, and keeping only the
largest coefficients *is* the compression. VST3, AU and standalone on macOS. Version 2
("WaveGrain II", beta) installs beside version 1 under its own identity.

Sibling of WaveTree; it reuses WaveTree's perfect-reconstruction DWT core unchanged.

## How it works

A voice (one held note) owns a playhead into the sample and a grain pool. Grains are pre-rendered
at spawn: read a power-of-two window at the note's pitch (cubic interpolation gives continuous
chromatic pitch), forward DWT, apply the wavelet operations, inverse DWT, Hann window; the
per-sample path is just overlap-add. All DWT work is a bounded burst at spawn: no allocation on
the audio thread, and the sample is handed across lock-free.

The wavelet operations per grain:

- **Compression** (top-K): keep only the largest coefficients, a progressive lo-fi wavelet-codec
  degradation. Orthonormal, so always energy-bounded.
- **Scale Tilt**: per-octave spectral slope across the detail bands, a wavelet EQ.
- **Band Low / High**: reconstruct from a subset of scale bands (coarse only, fine only, a range).
- **DWT Octave**: octave scale-shift of the coefficients. Continuous pitch comes from grain
  resampling, kept separate.

### ROM Codec

Beyond the per-grain compression, **ROM Codec** wavelet-compresses the *entire sample once* and
plays from the lossy result. A block-wise wavelet audio codec: per 2048-sample block, forward DWT,
keep the top-K coefficients (**ROM Ratio**), uniform-quantise the survivors to **ROM Bits** (2 to
16), inverse DWT. The grains read the reconstructed lossy sample, and the header shows the achieved
compression ratio and bitrate. Encoding happens on the message thread when the codec settings
change and is published to the audio engine lock-free, so at a high ratio or low bit depth you hear
genuine wavelet-codec artefacts baked into the ROM.

Also: four procedural built-in ROMs (Pluck, Vowel, Hit, Pad) generated at each note's frequency,
plus your own sample dropped from the Finder or from Ableton's browser; Root Note, Grain Size,
Density, Position, Scan, Freeze, Spray, Fine Pitch, Spread, Window and Wavelet (Haar, Db2, Db4,
Db6); ADSR, filter, output and polyphony; two LFOs with a mod matrix; seeds that are fixed, per
key or rolled each strike; time controls in milliseconds or beat divisions.

## Known limitations

- DWT Octave is octave-quantised; fine and chromatic pitch come from grain resampling.
- The decimated DWT is shift-variant, so compression and band masks shimmer with grain phase. The
  Hann window and overlap mask most of it; what remains reads as texture.
- Per-grain DWT cost spikes at spawn under very high density and polyphony (bounded by the grain
  cap and a smooth-overlap hop floor).
- Up-pitch aliasing at extreme notes or DWT Octave settings.
- Grains are mono; the sample is downmixed and stereo comes from per-grain Spread panning.

## Install a release (no build needed)

Download `WaveGrain-II-2.0.1-macOS.zip` from the [Releases](https://github.com/MarioKartUltra/WaveGrain/releases)
page and unzip it; that makes a folder `WaveGrain-II-2.0.1-macOS`. Copy the two plug-in
bundles into the user plug-in folders, by dragging them in the Finder or with the two `cp` lines.
Then clear the quarantine flag: the bundles are signed ad hoc rather than notarised, so macOS marks
them after the download and the DAW will not load them until the mark is gone.

```sh
cd ~/Downloads/WaveGrain-II-2.0.1-macOS
cp -R "WaveGrain II.vst3"      ~/Library/Audio/Plug-Ins/VST3/
cp -R "WaveGrain II.component" ~/Library/Audio/Plug-Ins/Components/
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"WaveGrain II.vst3" \
                               ~/Library/Audio/Plug-Ins/Components/"WaveGrain II.component" "WaveGrain II.app"
```

Then rescan in the DAW (Ableton Live: Settings → Plug-Ins → Rescan; Logic and GarageBand pick up
the AU after a restart). `WaveGrain II.app` is the standalone version with an on-screen keyboard; drag a WAV onto the window or click Load. macOS 11 or later,
Apple Silicon or Intel.

## Build from source (macOS)

Xcode command-line tools and CMake 3.22 or later (`brew install cmake ninja`).

```sh
git clone https://github.com/MarioKartUltra/WaveGrain.git
cd WaveGrain
./build.sh                 # arm64
./build.sh --universal     # arm64 + x86_64
```

The first configure fetches JUCE 8.0.7 and the patched foleys_gui_magic (network, once). To build
against checkouts you already have, pass `-DJUCE_DIR=... -DFOLEYS_DIR=...` or symlink them as
`external/JUCE` and `external/foleys_gui_magic`.

Artefacts land in `build/WaveGrain_artefacts/Release/`. By default the VST3 and AU are also copied into `~/Library/Audio/Plug-Ins`; pass
`-DWAVEGRAIN_INSTALL_AFTER_BUILD=OFF` to keep them in the build directory. `-DWAVEGRAIN_GUI_EDITOR=ON`
opens foleys' WYSIWYG layout editor beside the plugin, for development only.

```sh
ctest --test-dir build --output-on-failure    # the offline DSP checks
auval -v aumu Wgr2 Wvgr                      # validate the installed AU
```

## Licence

WaveGrain II is free software under the GNU Affero General Public License, version 3 or later
(`LICENSE`). Third-party components and their licences are listed in `NOTICE`; JUCE is used under
its AGPLv3 option.
