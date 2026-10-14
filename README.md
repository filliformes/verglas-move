# Verglas

Mutable Instruments Clouds granular processor as audio FX for Ableton Move, built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

**Verglas** is a faithful port of Emilie Gillet's [Clouds](https://mutable-instruments.net/modules/clouds/) — four processing modes (granular, time-stretch, looping delay, spectral), feedback, reverb, freeze, and quality degradation. Extended with output HPF/LPF filters, low-shelf EQ, and a tape-style soft limiter.

## Install

Available from the Move Everything Module Store on your device, or manually:

```bash
./scripts/build.sh              # or: docker build -t verglas-builder . && docker cp $(docker create verglas-builder):/build/modules/verglas ./modules/verglas
./scripts/install.sh            # deploys to Move via SSH
```

## Parameters

| Knob | Parameter | Range |
|------|-----------|-------|
| 1 | Position | 0–100% |
| 2 | Size | 0–100% |
| 3 | Pitch | -24 to +24 st |
| 4 | Density | 0–100% |
| 5 | Texture | 0–100% |
| 6 | Feedback | 0–100% |
| 7 | Reverb | 0–100% |
| 8 | Mix | 0–100% |

Additional pages: **Filters** (HPF, LPF, Limiter, Pre Gain, Post Gain, Low Boost, Low Freq, Low Q) and **Parameters** (the main knobs plus Mode, Freeze, Quality, Spread).

See [MANUAL.md](MANUAL.md) for full documentation.

## What's New in v1.2.5

- **Fixed** loud passages hard-clipping before the Limiter (the output stage now runs in float and clamps once)
- **Fixed** clicks when engaging HPF, LPF, Limiter or Low Boost, and zipper noise on Pre/Post Gain
- **Fixed** CPU spikes from denormals on decaying tails (flush-to-zero during processing)
- **Changed** pages, labels and units now come only from module.json (Schwung 1.x knob grid); the old jog-menu UI is gone
- **Added** native test suite, run on every build and release

## What's New in v1.1.0

- **Fixed** volume drop at Mix 0% (dry signal no longer attenuated by -3dB)
- **Fixed** Spectral mode volume matching with other modes
- **Fixed** pitch-dependent volume loss (wet signal gain compensation)
- **Added** texture knob smoothing (eliminates zipper noise)
- **Added** low-shelf EQ — 3 parameters (Low Boost, Low Freq, Low Q) on Filters page knobs 6-7-8

## Credits

- **DSP**: Emilie Gillet / [Mutable Instruments](https://github.com/pichenettes/eurorack) (MIT License)
- **Soft clipper**: Inspired by [Chowdhury DSP](https://github.com/Chowdhury-DSP) and [Airwindows](https://github.com/airwindows/airwindows)
- **Move port**: filliformes
- **Framework**: [Move Everything](https://github.com/charlesvestal/move-everything)

## License

DSP code: MIT (Emilie Gillet). Port and wrapper: MIT.
