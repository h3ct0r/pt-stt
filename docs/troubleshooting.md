# Troubleshooting

- [Build](#build)
- [Models](#models)
- [Microphone](#microphone)
- [GPU](#gpu)
- [Transcripts](#transcripts)
- [Python](#python)

## Build

**`third_party/parakeet.cpp is missing`**: the submodules were not cloned.

```bash
git submodule update --init --recursive
```

**`nvcc` not found, or CUDA not detected** (Jetson): add CUDA to the `PATH` and start again from an empty build folder.

```bash
export PATH=/usr/local/cuda/bin:$PATH && rm -rf build && cmake -B build -DPTSTT_BACKEND=cuda
```

**The build is killed (out of memory) on the Jetson:** build with fewer jobs (`cmake --build build -j2`), or add swap.

**The `third_party/ggml` submodule shows as modified:** this is expected. parakeet.cpp applies its patches to ggml at configure time.

## Models

**`models not found`**: download them, or say where they are.

```bash
scripts/fetch_models.sh
```
```bash
export PTSTT_MODELS_DIR=/path/to/models
```

**`cannot load ASR model ... failed to open GGUF file`**: the path is wrong, or the download was interrupted. Delete the file and run `scripts/fetch_models.sh` again. The F32 model must be 452,192,800 bytes.

## Microphone

**macOS: no audio, or the session always ends with `no_speech`.** The terminal app has no microphone permission. Enable it in **System Settings › Privacy & Security › Microphone** and restart the terminal.

**`no capture device matches '...'`**: list the device names and pass part of one:

```bash
./build/bin/ptstt-cli --list-devices
```

**Linux: `open capture device` fails.** Check that the system sees the device (`arecord -l`), that your user is in the `audio` group, and that no other program holds the device exclusively. Try `--device` with the card name shown by `--list-devices`.

**Sessions never end on silence.** Background noise is being detected as speech. Raise the VAD threshold (`--threshold 0.65`) or move the microphone away from fans and motors. See [Tuning](tuning.md#vad-threshold).

**The start of the first word is cut off.** Raise `pad` (Python `pad=0.5`, C `pad_ms = 500`).

## GPU

**Check which device is used.** At startup, parakeet prints `[parakeet] pk::Backend using device: …`. `CUDA0` or `MTL0` means the GPU; no line means the CPU. Use `--verbose` for the full backend logs.

**Force the CPU** to compare speed or rule out a GPU problem:

```bash
PARAKEET_DEVICE=cpu ./build/bin/ptstt-cli
```

**The program aborts at exit with `GGML_ASSERT([rsets->data count] == 0)` (Metal) or "driver shutting down" (CUDA).** An engine was still alive when the process exited. Call `ptstt_destroy` (C), let the `ptstt::Engine` go out of scope (C++), or `close()` the `Transcriber` or use `with` (Python).

**The first run on macOS is slow (about 10 s).** Metal compiles its shaders once, and macOS caches them. Later starts are fast.

## Transcripts

**Empty text with `stop_reason` `manual`.** You pressed space before the VAD detected any speech. The model only runs when speech was detected.

**Words are made up on silence or noise.** The model is only run on the speech part, so this is rare. If it happens, raise the VAD threshold.

**Wrong words.** See [Tuning › Accuracy](tuning.md#accuracy). Try the other decoder (`--decoder ctc`) to compare.

## Python

**`libptstt not found`**: build the project, or set the full path:

```bash
export PTSTT_LIB=$HOME/pt-stt/build/lib/libptstt.so
```

**`libptstt ABI … does not match these bindings`**: the Python package and the library come from different versions. Rebuild the library, or reinstall the package, from the same checkout.

**Ctrl-C does not stop `listen()`.** It does, within 0.2 s; the wait is done in short steps for that reason. If you call `wait_result()` from a thread other than the main one, Python delivers Ctrl-C to the main thread only.
