# pt-stt

**Push-to-talk Portuguese speech-to-text for the Jetson Orin Nano and macOS, with C, C++ and Python APIs.**

[![CI](https://github.com/h3ct0r/pt-stt/actions/workflows/ci.yml/badge.svg)](https://github.com/h3ct0r/pt-stt/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Python 3.10+](https://img.shields.io/badge/python-3.10%2B-blue.svg)](docs/python-api.md)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](docs/cpp-api.md)

Press space and speak Portuguese. The text comes back when you press space again, or after 2 seconds of silence. Everything runs on the device, with no cloud service and no Python at inference time.

```text
$ ./build/bin/ptstt-cli
pressione ESPAÇO para falar · c cancela · q sai
● fala detectada…  (ESPAÇO para parar)
O robô deve virar à esquerda e seguir em frente por dois metros.
  [silence · áudio 6.7s · fala 4.8s · 86 ms]
```

pt-stt wraps NVIDIA's [Portuguese FastConformer](https://huggingface.co/nvidia/stt_pt_fastconformer_hybrid_large_pc) model (115M parameters, with punctuation and capitalization). The model runs through [parakeet.cpp](https://github.com/mudler/parakeet.cpp) on CUDA, Metal or the CPU, and [Silero VAD](https://github.com/snakers4/silero-vad) decides when you have stopped talking.

## Features

- **Push to talk:** start on demand; stop on a key press, after a silence timeout, when no speech starts, or at a maximum length.
- **One library, three APIs:**
  - a stable C ABI (`libptstt`)
  - a header-only C++17 wrapper
  - Python 3.10+ bindings (ctypes, nothing to compile)
- **Fast:** on an Apple M3 Pro, 4.8 s of speech is transcribed in about 85 ms on Metal and about 130 ms on the CPU. Only the speech part of each recording goes through the model.
- **Easy on the robot:** a terminal UI that works over SSH, and JSON output for piping. The microphone is opened only while listening.
- **Testable without a microphone:** any audio file can stand in for the mic, so the test suite runs in CI.

## Quick start

### macOS (Apple Silicon)

```bash
brew install cmake
```
```bash
git clone --recursive https://github.com/h3ct0r/pt-stt && cd pt-stt
```
```bash
scripts/fetch_models.sh
```
```bash
cmake -B build && cmake --build build -j
```
```bash
./build/bin/ptstt-cli
```

The first time, macOS asks to let your terminal use the microphone.

### Jetson Orin Nano (JetPack 6)

```bash
sudo apt install -y build-essential cmake git curl
```
```bash
git clone --recursive https://github.com/h3ct0r/pt-stt && cd pt-stt && scripts/fetch_models.sh
```
```bash
export PATH=/usr/local/cuda/bin:$PATH && cmake -B build -DPTSTT_BACKEND=cuda && cmake --build build -j4
```
```bash
./build/bin/ptstt-cli --list-devices
```
```bash
./build/bin/ptstt-cli --device USB
```

See [docs/jetson-orin-nano.md](docs/jetson-orin-nano.md) for power modes, microphones and benchmarking.

## Use it from your code

**Python 3.10+**

```python
from ptstt import Transcriber

with Transcriber() as stt:          # loads the models once
    result = stt.listen()           # opens the mic; ends after 2 s of silence
    print(result.text, result.stop_reason.name)
```

**C++17**

```cpp
#include "ptstt.hpp"

ptstt::Config cfg;
cfg.asr_model = "models/pt-fastconformer-large.f32.gguf";
cfg.vad_model = "models/silero-vad-f32.gguf";
ptstt::Engine stt(cfg);
std::cout << stt.listen().text << "\n";
```

**C**

```c
#include "ptstt.h"

ptstt_config cfg;
ptstt_config_default(&cfg);
cfg.asr_model = "models/pt-fastconformer-large.f32.gguf";
cfg.vad_model = "models/silero-vad-f32.gguf";
ptstt_engine *e = ptstt_create(&cfg);

ptstt_result r;
ptstt_start(e);                       /* push to talk: call ptstt_stop(e) to end early */
ptstt_wait_result(e, -1, &r);
printf("%s\n", r.text);
ptstt_result_clear(&r);
ptstt_destroy(e);
```

## Documentation

| Page | Contents |
| ---- | -------- |
| [Getting started](docs/getting-started.md) | Requirements, build options, models, first run |
| [Jetson Orin Nano](docs/jetson-orin-nano.md) | JetPack setup, CUDA build, microphones, benchmarking |
| [CLI](docs/cli.md) | `ptstt-cli` keys, flags, JSON output, exit codes |
| [C API](docs/c-api.md) | Every function, configuration, events, threading and ownership |
| [C++ API](docs/cpp-api.md) | The `ptstt::Engine` RAII wrapper |
| [Python API](docs/python-api.md) | `Transcriber`, results, events, finding the library |
| [Architecture](docs/architecture.md) | Components, threads and the session state machine |
| [Tuning](docs/tuning.md) | Timeouts, VAD threshold, CTC vs RNN-T, quantization |
| [Troubleshooting](docs/troubleshooting.md) | Microphone, models, GPU and empty transcripts |

## Project layout

```text
include/ptstt.h       C API (the stable ABI)
include/ptstt.hpp     C++17 wrapper
src/                  engine, audio capture, VAD parsing
cli/                  ptstt-cli
python/ptstt/         Python bindings
examples/             listen.cpp, listen.py
tests/                engine (C) and binding (Python) tests, WAV fixtures
scripts/              model download, test-audio generation
third_party/          parakeet.cpp (submodule), miniaudio
```

## Credits and licenses

pt-stt is released under the [MIT License](LICENSE). It builds on:

| Component | Use | License |
| --------- | --- | ------- |
| [nvidia/stt_pt_fastconformer_hybrid_large_pc](https://huggingface.co/nvidia/stt_pt_fastconformer_hybrid_large_pc), as converted to GGUF by [Singla0009](https://huggingface.co/Singla0009/Portuguese-FastConformer-Hybrid-GGUF) | Speech-to-text model | CC-BY-4.0 |
| [Silero VAD](https://github.com/snakers4/silero-vad), GGUF from [mudler/parakeet-cpp-gguf](https://huggingface.co/mudler/parakeet-cpp-gguf) | Voice activity detection | MIT |
| [parakeet.cpp](https://github.com/mudler/parakeet.cpp) and [ggml](https://github.com/ggml-org/ggml) | Inference engine | MIT |
| [miniaudio](https://github.com/mackron/miniaudio) | Microphone capture and audio decoding | Public domain / MIT-0 |

The model weights are not in this repository. `scripts/fetch_models.sh` downloads them, and their licenses apply to your use of them.
