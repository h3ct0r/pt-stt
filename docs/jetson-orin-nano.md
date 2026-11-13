# Jetson Orin Nano

This guide is for the **Jetson Orin Nano (8 GB) with JetPack 6** (Ubuntu 22.04, CUDA 12, Python 3.10). Other Orin modules should work the same way.

> **Status:** the CUDA build and live microphone use have not been run on a Jetson yet. Linux arm64 is built and tested in CI on the CPU backend. Please report what you find, and fill in the [benchmark table](#benchmark).

- [Prepare the board](#prepare-the-board)
- [Build with CUDA](#build-with-cuda)
- [Pick the microphone](#pick-the-microphone)
- [Run](#run)
- [Use from Python 3.10](#use-from-python-310)
- [Performance mode](#performance-mode)
- [Benchmark](#benchmark)
- [Run as a service](#run-as-a-service)

## Prepare the board

Check the JetPack and CUDA versions:

```bash
cat /etc/nv_tegra_release
```
```bash
/usr/local/cuda/bin/nvcc --version
```

Install the build tools:

```bash
sudo apt update && sudo apt install -y build-essential cmake git curl
```

`nvcc` is not on the `PATH` by default:

```bash
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc && source ~/.bashrc
```

## Build with CUDA

```bash
git clone --recursive https://github.com/h3ct0r/pt-stt && cd pt-stt
```
```bash
scripts/fetch_models.sh
```
```bash
cmake -B build -DPTSTT_BACKEND=cuda
```
```bash
cmake --build build -j4
```

`PTSTT_CUDA_ARCHITECTURES` defaults to `87`, the Orin GPU. The CUDA kernels of ggml take a while to compile on the board. `-j4` keeps memory use reasonable on the 8 GB module. If the build is killed for lack of memory, use `-j2` or add swap.

When the model loads, the CLI prints the device it picked:

```text
[parakeet] pk::Backend using device: CUDA0
```

`PARAKEET_DEVICE=cpu` forces the CPU, which is useful for comparisons:

```bash
PARAKEET_DEVICE=cpu ./build/bin/ptstt-cli
```

## Pick the microphone

List the capture devices:

```bash
./build/bin/ptstt-cli --list-devices
```

Pass any part of a device name to `--device` (it isn't case-sensitive):

```bash
./build/bin/ptstt-cli --device "USB"
```

Without `--device`, the system default input is used. Tips:

- USB microphones and USB sound cards work through ALSA or PulseAudio. pt-stt asks for 16 kHz mono and converts from whatever the device delivers.
- `arecord -l` lists the ALSA cards. To check a mic outside pt-stt, record 3 s and play them back:

  ```bash
  arecord -d 3 -f S16_LE -r 16000 /tmp/t.wav && aplay /tmp/t.wav
  ```

- Your user needs access to the sound devices. Add it to the `audio` group, then log in again:

  ```bash
  sudo usermod -aG audio $USER
  ```

## Run

The keyboard UI works over SSH, with no desktop needed:

```bash
./build/bin/ptstt-cli --device USB
```

For other programs, `--json` prints one result per line on stdout, and the status lines go to stderr:

```bash
./build/bin/ptstt-cli --device USB --json | tee transcripts.jsonl
```

## Use from Python 3.10

JetPack 6 ships Python 3.10, which the bindings support. No compiler is needed for Python.

```bash
pip3 install -e python/
```
```bash
python3 examples/listen.py --device USB
```

In your own code:

```python
from ptstt import Transcriber, StopReason

stt = Transcriber(device="USB")         # load once at startup
r = stt.listen()                         # blocks until silence / stop()
if r.stop_reason != StopReason.NO_SPEECH:
    handle_command(r.text)
```

To trigger listening from something other than the keyboard (a button on a GPIO pin, a ROS topic, a wake word), call `stt.start()` from that event and take the text from `stt.wait_result()` or from an `on_event` handler. See [Python API](python-api.md).

## Performance mode

The Orin Nano has several power modes. Check the current one, then switch to the highest mode your board and power supply support:

```bash
sudo nvpmodel -q
```
```bash
sudo nvpmodel -m <mode-id>
```

For stable benchmark numbers, lock the clocks:

```bash
sudo jetson_clocks
```

## Benchmark

The test fixture is 4.8 s of speech. Time it on CUDA and on the CPU:

```bash
./build/bin/test_engine --model models/pt-fastconformer-large.f32.gguf --vad models/silero-vad-f32.gguf --fixtures tests/fixtures | grep reason=silence
```
```bash
PARAKEET_DEVICE=cpu ./build/bin/test_engine --model models/pt-fastconformer-large.f32.gguf --vad models/silero-vad-f32.gguf --fixtures tests/fixtures | grep reason=silence
```

`asr=` is the model time for 4.8 s of speech.

| Device | Backend | Model | `asr=` for 4.8 s of speech |
| ------ | ------- | ----- | -------------------------- |
| Apple M3 Pro | Metal | f32 | ~85 ms |
| Apple M3 Pro | CPU | f32 | ~130 ms |
| Jetson Orin Nano 8 GB | CUDA | f32 | _to measure_ |
| Jetson Orin Nano 8 GB | CPU | f32 | _to measure_ |
| Jetson Orin Nano 8 GB | CUDA | q8_0 | _to measure_ |

## Run as a service

A minimal systemd unit for a robot that pipes transcripts to another process. Adjust the paths and the consumer.

```ini
# /etc/systemd/system/ptstt.service
[Unit]
Description=pt-stt speech-to-text
After=sound.target

[Service]
User=robot
WorkingDirectory=/home/robot/pt-stt
ExecStart=/usr/bin/python3 /home/robot/robot/voice_node.py
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

`ptstt-cli` needs a terminal for the space bar. Under systemd, drive the library from your own program (Python or C++) and start listening from your own trigger instead.
