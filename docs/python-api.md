# Python API

The `ptstt` package wraps `libptstt` with ctypes. It supports **Python 3.10 and newer** (the JetPack 6 system Python is 3.10) and has no dependencies. NumPy is accepted when present but not required.

- [Install](#install)
- [Quick start](#quick-start)
- [Transcriber](#transcriber)
- [Results](#results)
- [Events](#events)
- [Finding the library and the models](#finding-the-library-and-the-models)
- [Patterns](#patterns)

## Install

Build the C++ project first ([Getting started](getting-started.md)), then install the package in editable mode:

```bash
pip install -e python/
```

Or skip installing and point `PYTHONPATH` at it:

```bash
PYTHONPATH=python python3 examples/listen.py
```

## Quick start

```python
from ptstt import Transcriber, StopReason

with Transcriber() as stt:                 # loads the models (a second or two)
    while True:
        input("Enter to talk ")
        r = stt.listen()                   # ends after 2 s of silence
        if r.stop_reason == StopReason.NO_SPEECH:
            continue
        print(r.text)
```

## Transcriber

```python
Transcriber(
    asr_model=None,          # path; default: <models>/pt-fastconformer-large.f32.gguf
    vad_model=None,          # path; default: <models>/silero-vad-f32.gguf
    *,
    decoder=Decoder.DEFAULT, # DEFAULT/RNNT (transducer) or CTC
    vad_threshold=0.5,
    silence_timeout=2.0,     # seconds of silence that end a session, once speech started
    no_speech_timeout=5.0,   # seconds to wait for speech to start
    max_utterance=30.0,      # hard limit per session, seconds
    pad=0.3,                 # seconds kept around the speech
    device=None,             # part of a capture device name; None = default
    input_file=None,         # use an audio file instead of the microphone
    verbose=False,           # show backend logs
    lib_path=None,           # explicit path to libptstt
)
```

| Method / property | Description |
| ----------------- | ----------- |
| `listen(timeout=None) -> Result` | Start a session and block until it ends. Raises `TimeoutError` (and cancels) if `timeout` seconds pass |
| `start()` | Start listening; raises `PtsttError` if a session is running |
| `stop() -> bool` | End the session now and transcribe it. `False` if nothing was running |
| `cancel() -> bool` | End the session and drop its audio |
| `wait_result(timeout=None) -> Result \| None` | Wait for the next result; `None` on timeout. Ctrl-C interrupts it |
| `state -> State` | `IDLE`, `LISTENING` or `TRANSCRIBING` |
| `on_event(handler)` | Set (or clear with `None`) a function called with each `Event` |
| `transcribe_file(path) -> str` | Transcribe a whole WAV/FLAC/MP3 file (no VAD) |
| `transcribe_pcm(samples, sample_rate=16000) -> str` | Transcribe mono float32 samples: NumPy array, `array('f')`, raw float32 bytes, or a list of floats |
| `close()` | Free the models. Also called by `with`, by garbage collection, and at interpreter exit |

Module functions: `ptstt.list_devices() -> list[dict]` and `ptstt.version() -> str`.

Every library error raises `ptstt.PtsttError` (a `RuntimeError`).

## Results

```python
@dataclass(frozen=True)
class Result:
    text: str                 # "" when nothing was recognized
    stop_reason: StopReason   # MANUAL, SILENCE, NO_SPEECH, MAX_DURATION, CANCELLED, ERROR
    audio_sec: float          # whole session
    speech_sec: float         # audio sent to the model (0 = not run)
    transcribe_ms: float      # model time
```

See the [C API](c-api.md#results-and-stop-reasons) for exactly when each stop reason happens.

## Events

```python
@dataclass(frozen=True)
class Event:
    type: EventType            # LISTENING, SPEECH_START, TRANSCRIBING, RESULT, ERROR
    stop_reason: StopReason
    result: Result | None      # for RESULT
    error: str | None          # for ERROR
```

Handlers run on the library's worker thread, not your main thread. Keep them short: set a flag, push to a `queue.Queue`, or call `stt.stop()`. An exception raised in a handler is logged on the `ptstt` logger and does not stop the engine.

```python
import queue
from ptstt import Transcriber, EventType

texts = queue.Queue()
stt = Transcriber()
stt.on_event(lambda ev: ev.type == EventType.RESULT and ev.result.text and texts.put(ev.result.text))

stt.start()            # e.g. from a button callback
print(texts.get())     # blocks until something was said
```

## Finding the library and the models

The library is looked up in this order:

1. the `lib_path` argument
2. the `PTSTT_LIB` environment variable (the full path to the `.so`/`.dylib`)
3. next to the package (`ptstt/libptstt.so`)
4. `<repo>/build/lib`, when the package is used from the repository

The default models are looked up in `$PTSTT_MODELS_DIR`, `./models`, then `<repo>/models`.

## Patterns

**Push to talk with a hardware button.** Call `stt.start()` when the button goes down and `stt.stop()` when it goes up. The silence timeout still ends the session if the button is held too long.

**Hands-free command loop.** Call `listen()` in a loop. A `NO_SPEECH` result after 5 s just means nobody spoke.

**ROS 2.** Create the `Transcriber` in the node's constructor. Start sessions from a service or topic callback, and publish `ev.result.text` from an `on_event` handler. The handler runs on the engine's thread, and `rclpy` publishers are thread-safe.

**Your own audio.** If audio already comes from elsewhere (a ROS audio topic, a file), skip the microphone and call `transcribe_pcm()` with the samples.
