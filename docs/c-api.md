# C API

`include/ptstt.h` is the stable interface of pt-stt. The C++ wrapper and the Python bindings are both thin layers over it. Link against `libptstt`:

```bash
cc -std=c11 app.c -Ipt-stt/include -Lpt-stt/build/lib -lptstt -Wl,-rpath,pt-stt/build/lib -o app
```

or, from CMake, `target_link_libraries(app PRIVATE ptstt)` when pt-stt is part of your build.

- [Overview](#overview)
- [Configuration](#configuration)
- [Functions](#functions)
- [Results and stop reasons](#results-and-stop-reasons)
- [Events](#events)
- [Threading and ownership](#threading-and-ownership)
- [Complete example](#complete-example)

## Overview

An **engine** (`ptstt_engine*`) owns the speech model, the VAD model and the audio source. Create one at startup and reuse it, because loading the models takes a second or two.

Each call to `ptstt_start()` begins a **session**:

1. The microphone opens and the audio goes through the VAD.
2. The session ends on `ptstt_stop()`, after `silence_timeout_ms` of silence once speech has started, if no speech starts within `no_speech_timeout_ms`, at `max_utterance_ms`, or on `ptstt_cancel()`.
3. The speech part of the audio, plus `pad_ms` on each side, is transcribed.
4. The **result** is delivered to the event callback and queued for `ptstt_wait_result()`.

There are two ways to consume results. Use either one, or both:

- **Blocking:** `ptstt_start()`, then `ptstt_wait_result()`.
- **Event-driven:** `ptstt_set_callback()` once, then `ptstt_start()` whenever you want to listen. The text arrives in a `PTSTT_EV_RESULT` event.

## Configuration

Always start from `ptstt_config_default()`, then change the fields you need:

```c
ptstt_config cfg;
ptstt_config_default(&cfg);
cfg.asr_model = "models/pt-fastconformer-large.f32.gguf";
cfg.vad_model = "models/silero-vad-f32.gguf";
cfg.silence_timeout_ms = 1500;
```

| Field | Default | Meaning |
| ----- | ------- | ------- |
| `size` | `sizeof(ptstt_config)` | Set by `ptstt_config_default`; lets later versions add fields |
| `asr_model` | *required* | Path to the speech-to-text GGUF |
| `vad_model` | *required* | Path to the Silero VAD GGUF |
| `decoder` | `PTSTT_DECODER_DEFAULT` | `PTSTT_DECODER_DEFAULT` / `PTSTT_DECODER_RNNT` (transducer) or `PTSTT_DECODER_CTC` |
| `vad_threshold` | `0.5` | A frame is speech when the VAD probability is at or above this |
| `silence_timeout_ms` | `2000` | Silence that ends a session, counted after speech started |
| `no_speech_timeout_ms` | `5000` | End the session if no speech starts within this |
| `max_utterance_ms` | `30000` | Hard limit for one session |
| `pad_ms` | `300` | Audio kept before the first and after the last speech frame |
| `capture_device` | `NULL` | `NULL` = system default; otherwise part of a device name (not case-sensitive) |
| `input_file` | `NULL` | Read this audio file in place of the microphone (tests, offline runs). After the end of the file, the source produces silence |
| `verbose` | `0` | `1` shows ggml backend logs; `0` shows only errors |

Strings are copied by `ptstt_create`, so the caller's buffers can be freed right after.

## Functions

### Lifecycle

| Function | Description |
| -------- | ----------- |
| `void ptstt_config_default(ptstt_config *c)` | Fill `c` with the defaults |
| `ptstt_engine *ptstt_create(const ptstt_config *c)` | Load the models, open the audio source, start the worker thread. `NULL` on failure: read `ptstt_last_error(NULL)` |
| `void ptstt_destroy(ptstt_engine *e)` | Cancel any session, free everything. Safe on `NULL`. Call it before your program exits (see below) |
| `const char *ptstt_last_error(ptstt_engine *e)` | Last error of `e`, or of the last failed `ptstt_create` on this thread when `e` is `NULL`. Never `NULL` |
| `int ptstt_abi_version(void)` | `PTSTT_ABI_VERSION` of the library; compare with the header's |
| `const char *ptstt_version(void)` | Library version, e.g. `"0.1.0"` |

> Call `ptstt_destroy` for every engine before the process exits. The last one releases the shared GPU backend. Exiting with a live engine can abort inside the Metal or CUDA driver teardown.

### Sessions

| Function | Returns | Description |
| -------- | ------- | ----------- |
| `int ptstt_start(ptstt_engine *e)` | `0` / `-1` | Start listening. Fails if a session is running (`"busy"`) or the device cannot start. Drops results that `ptstt_wait_result` has not taken |
| `int ptstt_stop(ptstt_engine *e)` | `0` / `-1` | End the session now and transcribe it (`PTSTT_STOP_MANUAL`). `-1` if not listening |
| `int ptstt_cancel(ptstt_engine *e)` | `0` / `-1` | End the session and drop its audio (`PTSTT_STOP_CANCELLED`). `-1` if not listening |
| `int ptstt_state(ptstt_engine *e)` | state | `PTSTT_IDLE`, `PTSTT_LISTENING` or `PTSTT_TRANSCRIBING` |
| `int ptstt_wait_result(ptstt_engine *e, int timeout_ms, ptstt_result *out)` | `0` / `1` / `-1` | Wait for the next result and copy it into `out`. `1` on timeout, `-1` when the engine is being destroyed. `timeout_ms < 0` waits forever |
| `void ptstt_result_clear(ptstt_result *r)` | | Free `r->text` and zero `r` |
| `void ptstt_set_callback(ptstt_engine *e, ptstt_event_cb cb, void *user)` | | Install the event callback; `cb = NULL` removes it |

### Direct transcription (no microphone, no VAD)

| Function | Description |
| -------- | ----------- |
| `char *ptstt_transcribe_file(ptstt_engine *e, const char *path)` | Transcribe a whole WAV, FLAC or MP3 file (any rate, any channel count) |
| `char *ptstt_transcribe_pcm(ptstt_engine *e, const float *pcm, int n, int rate)` | Transcribe mono float samples in [-1, 1]; other rates are resampled to 16 kHz |

Both return a string to free with `ptstt_free_string`, or `NULL` on error. They can be called while a session is running; the model calls are serialized.

### Utilities

| Function | Description |
| -------- | ----------- |
| `char *ptstt_list_devices_json(void)` | Capture devices: `[{"index":0,"name":"MacBook Pro Microphone","default":true}]` |
| `void ptstt_free_string(char *s)` | Free a `char*` returned by the library. Safe on `NULL` |
| `const char *ptstt_stop_reason_name(int r)` | `"manual"`, `"silence"`, `"no_speech"`, `"max_duration"`, `"cancelled"`, `"error"` |

## Results and stop reasons

```c
typedef struct {
    char *text;          /* UTF-8, "" when nothing was said */
    int   stop_reason;   /* PTSTT_STOP_* */
    float audio_sec;     /* whole session */
    float speech_sec;    /* audio sent to the model; 0 = model not run */
    float transcribe_ms; /* model time */
} ptstt_result;
```

| `stop_reason` | Model runs? | Meaning |
| ------------- | ----------- | ------- |
| `PTSTT_STOP_MANUAL` | if speech was detected | `ptstt_stop()` was called |
| `PTSTT_STOP_SILENCE` | yes | `silence_timeout_ms` of silence after speech |
| `PTSTT_STOP_NO_SPEECH` | no | No speech within `no_speech_timeout_ms` |
| `PTSTT_STOP_MAX_DURATION` | if speech was detected | The session reached `max_utterance_ms` |
| `PTSTT_STOP_CANCELLED` | no | `ptstt_cancel()` was called, or the engine was destroyed mid-session |
| `PTSTT_STOP_ERROR` | no | The VAD failed. A `PTSTT_EV_ERROR` event carries the message |

If the model itself fails, the result keeps its stop reason, `text` is `""`, and a `PTSTT_EV_ERROR` event is emitted.

## Events

```c
typedef struct {
    int type;                    /* PTSTT_EV_* */
    int stop_reason;             /* TRANSCRIBING and RESULT */
    const ptstt_result *result;  /* RESULT only, valid during the callback */
    const char *error;           /* ERROR only, valid during the callback */
} ptstt_event;
```

The events of one session, in order:

| Event | When |
| ----- | ---- |
| `PTSTT_EV_LISTENING` | The worker started the session |
| `PTSTT_EV_SPEECH_START` | The VAD detected speech (at most once per session) |
| `PTSTT_EV_TRANSCRIBING` | The session ended (`stop_reason` set); the model is running |
| `PTSTT_EV_ERROR` | Something failed (optional) |
| `PTSTT_EV_RESULT` | The result is ready. The engine is already `PTSTT_IDLE`, so the callback may call `ptstt_start()` to listen again. The result is queued for `ptstt_wait_result()` just before this event, so a waiting thread can wake before the callback runs |

## Threading and ownership

- All functions are thread-safe.
- Callbacks run on the engine's **worker thread**. While a callback runs, no audio is processed, so keep it short: copy the text, signal your own thread, return. `ptstt_start`, `ptstt_stop` and `ptstt_cancel` may be called from the callback. `ptstt_destroy` must not be.
- One session at a time per engine. Several engines can coexist (for example two microphones), each with its own models.
- `char*` results are owned by the caller (`ptstt_free_string`, `ptstt_result_clear`). `const char*` results are owned by the library.
- The microphone is open only during a session.

## Complete example

Event-driven, as a robot would use it. Another part of the program calls `ptstt_start` (for example on a button press), and the text arrives in the callback.

```c
#include <stdio.h>
#include <string.h>
#include "ptstt.h"

static void on_event(const ptstt_event *ev, void *user) {
    (void)user;
    switch (ev->type) {
    case PTSTT_EV_SPEECH_START:
        fprintf(stderr, "speech...\n");
        break;
    case PTSTT_EV_RESULT:
        if (ev->result->text[0])
            printf("heard: %s (%s)\n", ev->result->text, ptstt_stop_reason_name(ev->stop_reason));
        break;
    case PTSTT_EV_ERROR:
        fprintf(stderr, "error: %s\n", ev->error);
        break;
    }
}

int main(void) {
    ptstt_config cfg;
    ptstt_config_default(&cfg);
    cfg.asr_model = "models/pt-fastconformer-large.f32.gguf";
    cfg.vad_model = "models/silero-vad-f32.gguf";

    ptstt_engine *e = ptstt_create(&cfg);
    if (!e) {
        fprintf(stderr, "ptstt: %s\n", ptstt_last_error(NULL));
        return 1;
    }
    ptstt_set_callback(e, on_event, NULL);

    for (int i = 0; i < 3; ++i) {
        ptstt_result r;
        if (ptstt_start(e) != 0) break;
        ptstt_wait_result(e, -1, &r);   /* or wait for your own signal from the callback */
        ptstt_result_clear(&r);
    }
    ptstt_destroy(e);
    return 0;
}
```
