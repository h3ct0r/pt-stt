# Getting started

This page covers building pt-stt, downloading the models, and running the CLI for the first time. For the Jetson, also read [Jetson Orin Nano](jetson-orin-nano.md).

- [Requirements](#requirements)
- [Get the code](#get-the-code)
- [Download the models](#download-the-models)
- [Build](#build)
- [First run](#first-run)
- [Install](#install)
- [Run the tests](#run-the-tests)

## Requirements

| | macOS | Linux / Jetson |
| - | ----- | -------------- |
| OS | macOS on Apple Silicon (Intel Macs: CPU backend, untested) | Ubuntu 22.04 (JetPack 6) or newer |
| Compiler | Xcode Command Line Tools | GCC 11+ |
| CMake | 3.18+ (`brew install cmake`) | 3.18+ (`apt install cmake`) |
| GPU backend | Metal (default) | CUDA 12 on the Jetson, or CPU |
| Python (optional) | 3.10+ | 3.10+ (JetPack 6 ships 3.10) |
| Disk | About 500 MB for the models, 300 MB for the build | same |

Linux needs ALSA or PulseAudio at runtime for the microphone. miniaudio loads them on demand, so no `-dev` packages are needed to build.

## Get the code

parakeet.cpp is a submodule, so clone recursively:

```bash
git clone --recursive https://github.com/h3ct0r/pt-stt
```

If you cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

The submodule is pinned to a specific parakeet.cpp commit, because the streaming VAD API pt-stt uses is not in a tagged release yet.

## Download the models

```bash
scripts/fetch_models.sh
```

This puts two files in `models/` (set `PTSTT_MODELS_DIR` to use another folder):

| File | Size | What |
| ---- | ---- | ---- |
| `pt-fastconformer-large.f32.gguf` | 452 MB | Portuguese speech-to-text, FastConformer hybrid RNN-T/CTC |
| `silero-vad-f32.gguf` | 2 MB | Silero voice activity detector |

The script skips files that are already there. Once the project is built, `scripts/fetch_models.sh --q8` also writes an 8-bit copy of the speech model (see [Tuning](tuning.md#quantization)).

## Build

```bash
cmake -B build
```
```bash
cmake --build build -j
```

The first build compiles parakeet.cpp and ggml, which takes a few minutes. The results are:

| Path | What |
| ---- | ---- |
| `build/lib/libptstt.{so,dylib}` | the pt-stt library |
| `build/lib/libparakeet.*`, `build/lib/libggml*` | the inference engine, loaded by libptstt |
| `build/bin/ptstt-cli` | the push-to-talk CLI |
| `build/bin/ptstt-example-listen` | the C++ example |
| `build/bin/parakeet-cli` | parakeet.cpp's own CLI (quantization, debugging) |

### CMake options

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `PTSTT_BACKEND` | `metal` on Apple Silicon, else `cpu` | `cpu`, `metal`, `cuda` or `vulkan` |
| `PTSTT_CUDA_ARCHITECTURES` | `87` | CUDA compute capability (87 = Jetson Orin) |
| `PTSTT_NATIVE` | `ON` | Optimize for the build machine. Turn off for a binary you copy to other machines |
| `PTSTT_BUILD_CLI` | `ON` | Build `ptstt-cli` |
| `PTSTT_BUILD_EXAMPLES` | `ON` | Build `ptstt-example-listen` |
| `PTSTT_BUILD_TESTS` | `ON` | Build the tests (`ctest`) |

Example: a CPU-only build on a Mac.

```bash
cmake -B build-cpu -DPTSTT_BACKEND=cpu && cmake --build build-cpu -j
```

## First run

```bash
./build/bin/ptstt-cli
```

1. Wait for the models to load (a second or two).
2. Press **space** and speak Portuguese.
3. Stop talking. After 2 s of silence the transcript is printed. You can also press **space** again to stop right away.
4. Press **q** to quit.

On macOS, the first start asks for microphone access for your terminal app (Terminal, iTerm, VS Code…). If you refused it, enable it in **System Settings › Privacy & Security › Microphone**.

To check the setup without a microphone:

```bash
./build/bin/ptstt-cli --file tests/fixtures/speech.wav
```

## Install

```bash
cmake --install build --prefix /usr/local
```

This installs `ptstt-cli` and `parakeet-cli` to `bin/`, the libraries to `lib/` (with relative rpaths), and `ptstt.h` and `ptstt.hpp` to `include/`. On Linux, run `sudo ldconfig` afterwards so the ggml libraries are found. Models are not installed. Point `--model`/`--vad` or `PTSTT_MODELS_DIR` at them.

For Python, install the bindings in editable mode. They find `build/lib` in the repository on their own:

```bash
pip install -e python/
```

## Run the tests

```bash
cd build && ctest --output-on-failure
```

The tests use the WAV files in `tests/fixtures/` in place of the microphone, so they need the models but no audio device. `scripts/make_test_audio.sh` regenerates the fixtures with macOS's Portuguese voice.
