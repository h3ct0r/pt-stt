# ptstt-cli

`ptstt-cli` is a push-to-talk transcriber for the terminal, and the reference client of the library.

```bash
./build/bin/ptstt-cli [options]
```

- [Keys](#keys)
- [Options](#options)
- [Output](#output)
- [Exit codes](#exit-codes)
- [Examples](#examples)

## Keys

| Key | Action |
| --- | ------ |
| **Space** | Start listening. While listening: stop now and transcribe |
| **c** | Cancel the current recording, without transcribing |
| **q**, **Esc**, **Ctrl-C**, **Ctrl-D** | Quit |

While listening, a recording also ends on its own:

| Reason | When |
| ------ | ---- |
| `silence` | You started speaking, then were silent for `--silence` seconds (default 2) |
| `no_speech` | No speech started within `--no-speech` seconds (default 5) |
| `max_duration` | The recording reached `--max` seconds (default 30) |
| `manual` | You pressed space |
| `cancelled` | You pressed c |

## Options

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `--model PATH` | `models/pt-fastconformer-large.f32.gguf` | Speech-to-text model |
| `--vad PATH` | `models/silero-vad-f32.gguf` | Silero VAD model |
| `--decoder rnnt\|ctc` | `rnnt` | Decoder head of the hybrid model ([Tuning](tuning.md#decoder-rnn-t-or-ctc)) |
| `--silence SEC` | `2` | Silence that ends a recording, once speech started |
| `--no-speech SEC` | `5` | End the recording if no speech starts within this |
| `--max SEC` | `30` | Maximum recording length |
| `--threshold P` | `0.5` | VAD speech probability threshold, between 0 and 1 |
| `--device NAME` | system default | Capture device, matched by part of its name (not case-sensitive) |
| `--list-devices` | | Print the capture devices as JSON and exit |
| `--file PATH` | | Transcribe a whole audio file (WAV, FLAC or MP3) and exit. No microphone |
| `--mic-file PATH` | | Use an audio file in place of the microphone, keeping the keyboard UI (for testing) |
| `--once` | | Exit after the first transcript |
| `--json` | | Print each result as one JSON line |
| `--verbose` | | Show model and GPU backend logs |
| `--version` | | Print the version and exit |
| `-h`, `--help` | | Show the help |

When `--model` and `--vad` are not given, the models are looked up in `$PTSTT_MODELS_DIR`, then `./models`, then the `models/` folder of the repository the binary was built in.

## Output

The transcript goes to **stdout**, and the status lines and timings go to **stderr**, so the text can be piped. Colors are disabled when stderr is not a terminal, or when `NO_COLOR` is set.

With `--json`, each result is one line:

```json
{"text":"O robô deve virar à esquerda.","stop_reason":"silence","audio_sec":5.120,"speech_sec":3.240,"transcribe_ms":71.5}
```

| Field | Meaning |
| ----- | ------- |
| `text` | Transcript; `""` when nothing was recognized |
| `stop_reason` | `manual`, `silence`, `no_speech`, `max_duration`, `cancelled` or `error` |
| `audio_sec` | Length of the whole recording |
| `speech_sec` | Audio sent to the model: the speech plus 0.3 s on each side. `0` means the model was not run |
| `transcribe_ms` | Time spent in the model |

With `--file` and `--json`, the output is `{"text":"..."}`.

## Exit codes

| Code | Meaning |
| ---- | ------- |
| `0` | Success |
| `1` | Runtime error: a model failed to load, a device failed to open, or transcription failed |
| `2` | Usage error: bad option, models not found, or no terminal for the keyboard mode |

## Examples

Use a USB microphone with a 1.5 s silence timeout:

```bash
./build/bin/ptstt-cli --device USB --silence 1.5
```

Record one command, then hand the text to a script:

```bash
./build/bin/ptstt-cli --once --json | jq -r .text
```

Transcribe a file:

```bash
./build/bin/ptstt-cli --file recording.mp3
```

Test the key handling without a microphone:

```bash
./build/bin/ptstt-cli --mic-file tests/fixtures/speech_pause.wav
```
