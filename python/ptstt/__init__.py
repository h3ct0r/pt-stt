"""Push-to-talk Portuguese speech-to-text (Python bindings for libptstt).

    from ptstt import Transcriber

    with Transcriber() as stt:            # models found in $PTSTT_MODELS_DIR or <repo>/models
        result = stt.listen()             # opens the mic; ends after 2 s of silence
        print(result.text, result.stop_reason.name)

The bindings use ctypes, so nothing is compiled for Python. They need the
shared library built by CMake (libptstt.so / libptstt.dylib). It is looked up in
$PTSTT_LIB, next to this package, then in <repo>/build/lib.
"""

from __future__ import annotations

import atexit
import ctypes
import enum
import json
import logging
import os
import sys
import threading
import weakref
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Optional, Union

__all__ = [
    "Decoder",
    "Event",
    "EventType",
    "PtsttError",
    "Result",
    "State",
    "StopReason",
    "Transcriber",
    "list_devices",
    "version",
]

__version__ = "0.1.0"

_log = logging.getLogger("ptstt")

ASR_MODEL_FILE = "pt-fastconformer-large.f32.gguf"
VAD_MODEL_FILE = "silero-vad-f32.gguf"


class PtsttError(RuntimeError):
    """Raised when the library reports an error."""


class State(enum.IntEnum):
    IDLE = 0
    LISTENING = 1
    TRANSCRIBING = 2


class StopReason(enum.IntEnum):
    MANUAL = 0
    SILENCE = 1
    NO_SPEECH = 2
    MAX_DURATION = 3
    CANCELLED = 4
    ERROR = 5


class EventType(enum.IntEnum):
    LISTENING = 0
    SPEECH_START = 1
    TRANSCRIBING = 2
    RESULT = 3
    ERROR = 4


class Decoder(enum.IntEnum):
    DEFAULT = 0  # transducer (RNN-T)
    CTC = 1
    RNNT = 2


@dataclass(frozen=True)
class Result:
    """The outcome of one listen session."""

    text: str
    stop_reason: StopReason
    audio_sec: float  # length of the whole session
    speech_sec: float  # audio sent to the model (0 = model not run)
    transcribe_ms: float  # time spent in the model


@dataclass(frozen=True)
class Event:
    type: EventType
    stop_reason: StopReason
    result: Optional[Result] = None  # set for EventType.RESULT
    error: Optional[str] = None  # set for EventType.ERROR


# ---------------------------------------------------------------------------
# ctypes mirror of include/ptstt.h
# ---------------------------------------------------------------------------
class _Config(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_int),
        ("asr_model", ctypes.c_char_p),
        ("vad_model", ctypes.c_char_p),
        ("decoder", ctypes.c_int),
        ("vad_threshold", ctypes.c_float),
        ("silence_timeout_ms", ctypes.c_int),
        ("no_speech_timeout_ms", ctypes.c_int),
        ("max_utterance_ms", ctypes.c_int),
        ("pad_ms", ctypes.c_int),
        ("capture_device", ctypes.c_char_p),
        ("input_file", ctypes.c_char_p),
        ("verbose", ctypes.c_int),
    ]


class _Result(ctypes.Structure):
    _fields_ = [
        ("text", ctypes.c_void_p),
        ("stop_reason", ctypes.c_int),
        ("audio_sec", ctypes.c_float),
        ("speech_sec", ctypes.c_float),
        ("transcribe_ms", ctypes.c_float),
    ]


class _Event(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("stop_reason", ctypes.c_int),
        ("result", ctypes.POINTER(_Result)),
        ("error", ctypes.c_char_p),
    ]


_EventCb = ctypes.CFUNCTYPE(None, ctypes.POINTER(_Event), ctypes.c_void_p)

_ABI_VERSION = 1
_lib: Optional[ctypes.CDLL] = None
_lib_lock = threading.Lock()


def _lib_names() -> list[str]:
    if sys.platform == "darwin":
        return ["libptstt.dylib"]
    if sys.platform == "win32":
        return ["ptstt.dll"]
    return ["libptstt.so", "libptstt.so.0"]


def _find_library(explicit: Optional[str] = None) -> str:
    candidates: list[Path] = []
    for value in (explicit, os.environ.get("PTSTT_LIB")):
        if value:
            candidates.append(Path(value))
    here = Path(__file__).resolve().parent
    for directory in (here, here.parent.parent / "build" / "lib"):
        candidates.extend(directory / name for name in _lib_names())
    for path in candidates:
        if path.is_file():
            return str(path)
    tried = "\n  ".join(str(p) for p in candidates)
    raise PtsttError(f"libptstt not found; build the project or set PTSTT_LIB. Tried:\n  {tried}")


def _load(explicit: Optional[str] = None) -> ctypes.CDLL:
    global _lib
    with _lib_lock:
        if _lib is not None:
            return _lib
        lib = ctypes.CDLL(_find_library(explicit))
        P = ctypes.POINTER
        sigs: dict[str, tuple[Any, list[Any]]] = {
            "ptstt_abi_version": (ctypes.c_int, []),
            "ptstt_version": (ctypes.c_char_p, []),
            "ptstt_config_default": (None, [P(_Config)]),
            "ptstt_create": (ctypes.c_void_p, [P(_Config)]),
            "ptstt_destroy": (None, [ctypes.c_void_p]),
            "ptstt_last_error": (ctypes.c_char_p, [ctypes.c_void_p]),
            "ptstt_set_callback": (None, [ctypes.c_void_p, _EventCb, ctypes.c_void_p]),
            "ptstt_start": (ctypes.c_int, [ctypes.c_void_p]),
            "ptstt_stop": (ctypes.c_int, [ctypes.c_void_p]),
            "ptstt_cancel": (ctypes.c_int, [ctypes.c_void_p]),
            "ptstt_state": (ctypes.c_int, [ctypes.c_void_p]),
            "ptstt_wait_result": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_int, P(_Result)]),
            "ptstt_result_clear": (None, [P(_Result)]),
            "ptstt_transcribe_pcm": (ctypes.c_void_p, [ctypes.c_void_p, P(ctypes.c_float), ctypes.c_int, ctypes.c_int]),
            "ptstt_transcribe_file": (ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
            "ptstt_list_devices_json": (ctypes.c_void_p, []),
            "ptstt_free_string": (None, [ctypes.c_void_p]),
        }
        for name, (restype, argtypes) in sigs.items():
            fn = getattr(lib, name)
            fn.restype = restype
            fn.argtypes = argtypes
        abi = lib.ptstt_abi_version()
        if abi != _ABI_VERSION:
            raise PtsttError(f"libptstt ABI {abi} does not match these bindings (ABI {_ABI_VERSION})")
        _lib = lib
        return lib


def _take_string(lib: ctypes.CDLL, ptr: Optional[int]) -> Optional[str]:
    """Copy a library-owned char* into a str and free it."""
    if not ptr:
        return None
    try:
        return ctypes.string_at(ptr).decode("utf-8", errors="replace")
    finally:
        lib.ptstt_free_string(ptr)


def _to_result(r: _Result) -> Result:
    text = ctypes.string_at(r.text).decode("utf-8", errors="replace") if r.text else ""
    return Result(text, StopReason(r.stop_reason), float(r.audio_sec), float(r.speech_sec), float(r.transcribe_ms))


def _default_model(name: str) -> str:
    dirs = []
    if os.environ.get("PTSTT_MODELS_DIR"):
        dirs.append(Path(os.environ["PTSTT_MODELS_DIR"]))
    dirs.append(Path.cwd() / "models")
    dirs.append(Path(__file__).resolve().parent.parent.parent / "models")
    for directory in dirs:
        if (directory / name).is_file():
            return str(directory / name)
    raise PtsttError(f"model {name} not found in {', '.join(map(str, dirs))}; run scripts/fetch_models.sh")


def version() -> str:
    """Version of the loaded libptstt."""
    return _load().ptstt_version().decode()


def list_devices() -> list[dict]:
    """Capture devices: [{"index": 0, "name": "...", "default": True}, ...]."""
    lib = _load()
    return json.loads(_take_string(lib, lib.ptstt_list_devices_json()) or "[]")


_live: "weakref.WeakSet[Transcriber]" = weakref.WeakSet()


@atexit.register
def _close_all() -> None:
    # The GPU backend must be released before the interpreter exits.
    for t in list(_live):
        t.close()


PcmLike = Union[Any, bytes, bytearray, memoryview, Iterable[float]]  # NumPy array, array('f'), bytes, floats


class Transcriber:
    """One speech model + VAD + microphone. Reuse it: loading takes a second or two.

    Times are in seconds. Callbacks (``on_event``) run on the library's worker
    thread; keep them short. All methods are thread-safe.
    """

    def __init__(
        self,
        asr_model: Optional[Union[str, os.PathLike]] = None,
        vad_model: Optional[Union[str, os.PathLike]] = None,
        *,
        decoder: Decoder = Decoder.DEFAULT,
        vad_threshold: float = 0.5,
        silence_timeout: float = 2.0,
        no_speech_timeout: float = 5.0,
        max_utterance: float = 30.0,
        pad: float = 0.3,
        device: Optional[str] = None,
        input_file: Optional[Union[str, os.PathLike]] = None,
        verbose: bool = False,
        lib_path: Optional[str] = None,
    ) -> None:
        self._lib = _load(lib_path)
        self._handle: Optional[int] = None
        self._handler: Optional[Callable[[Event], None]] = None
        self._handler_lock = threading.Lock()

        asr = str(asr_model) if asr_model else _default_model(ASR_MODEL_FILE)
        vad = str(vad_model) if vad_model else _default_model(VAD_MODEL_FILE)

        cfg = _Config()
        self._lib.ptstt_config_default(ctypes.byref(cfg))
        cfg.asr_model = os.fsencode(asr)
        cfg.vad_model = os.fsencode(vad)
        cfg.decoder = int(decoder)
        cfg.vad_threshold = float(vad_threshold)
        cfg.silence_timeout_ms = int(round(silence_timeout * 1000))
        cfg.no_speech_timeout_ms = int(round(no_speech_timeout * 1000))
        cfg.max_utterance_ms = int(round(max_utterance * 1000))
        cfg.pad_ms = int(round(pad * 1000))
        cfg.capture_device = device.encode() if device else None
        cfg.input_file = os.fsencode(str(input_file)) if input_file else None
        cfg.verbose = 1 if verbose else 0

        handle = self._lib.ptstt_create(ctypes.byref(cfg))
        if not handle:
            raise PtsttError(self._lib.ptstt_last_error(None).decode("utf-8", errors="replace"))
        self._handle = handle
        self._cb = _EventCb(self._dispatch)  # keep a reference for the library's lifetime
        self._lib.ptstt_set_callback(handle, self._cb, None)
        _live.add(self)

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop any session and free the models. Safe to call twice."""
        handle, self._handle = self._handle, None
        if handle:
            self._lib.ptstt_destroy(handle)
        _live.discard(self)

    def __enter__(self) -> "Transcriber":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    # -- sessions ----------------------------------------------------------
    @property
    def state(self) -> State:
        return State(self._lib.ptstt_state(self._h))

    def start(self) -> None:
        """Start listening. Raises PtsttError if a session is already running."""
        if self._lib.ptstt_start(self._h) != 0:
            raise PtsttError(self._error())

    def stop(self) -> bool:
        """End the session now and transcribe it. False if nothing was running."""
        return self._lib.ptstt_stop(self._h) == 0

    def cancel(self) -> bool:
        """End the session and drop its audio. False if nothing was running."""
        return self._lib.ptstt_cancel(self._h) == 0

    def wait_result(self, timeout: Optional[float] = None) -> Optional[Result]:
        """Wait for the next session result; None on timeout. Ctrl-C interrupts the wait."""
        deadline = None if timeout is None else timeout
        out = _Result()
        while True:
            step = 200 if deadline is None else max(0, min(200, int(deadline * 1000)))
            rc = self._lib.ptstt_wait_result(self._h, step, ctypes.byref(out))
            if rc == 0:
                try:
                    return _to_result(out)
                finally:
                    self._lib.ptstt_result_clear(ctypes.byref(out))
            if rc < 0:
                raise PtsttError(self._error() or "engine closed")
            if deadline is not None:
                deadline -= step / 1000
                if deadline <= 0:
                    return None

    def listen(self, timeout: Optional[float] = None) -> Result:
        """Start a session and block until it ends (silence, stop(), limits)."""
        self.start()
        try:
            result = self.wait_result(timeout)
        except BaseException:
            self.cancel()
            raise
        if result is None:
            self.cancel()
            raise TimeoutError("no result within the timeout")
        return result

    def on_event(self, handler: Optional[Callable[[Event], None]]) -> None:
        """Set (or clear with None) the event handler. Runs on the worker thread."""
        with self._handler_lock:
            self._handler = handler

    # -- direct transcription ---------------------------------------------
    def transcribe_file(self, path: Union[str, os.PathLike]) -> str:
        """Transcribe a WAV/FLAC/MP3 file (whole file, no VAD)."""
        text = _take_string(self._lib, self._lib.ptstt_transcribe_file(self._h, os.fsencode(str(path))))
        if text is None:
            raise PtsttError(self._error())
        return text

    def transcribe_pcm(self, samples: PcmLike, sample_rate: int = 16000) -> str:
        """Transcribe mono float32 samples in [-1, 1].

        Accepts a NumPy array, an ``array('f')``, raw float32 bytes, or any
        iterable of floats. Other sample rates are resampled to 16 kHz.
        """
        buf = _as_float32(samples)
        n = len(buf) // 4
        if n == 0:
            return ""
        ptr = (ctypes.c_float * n).from_buffer(buf)
        text = _take_string(self._lib, self._lib.ptstt_transcribe_pcm(self._h, ptr, n, int(sample_rate)))
        if text is None:
            raise PtsttError(self._error())
        return text

    # -- internals ---------------------------------------------------------
    @property
    def _h(self) -> int:
        if not self._handle:
            raise PtsttError("Transcriber is closed")
        return self._handle

    def _error(self) -> str:
        return self._lib.ptstt_last_error(self._handle).decode("utf-8", errors="replace")

    def _dispatch(self, ev_ptr: Any, _user: Any) -> None:
        with self._handler_lock:
            handler = self._handler
        if handler is None:
            return
        try:
            ev = ev_ptr.contents
            result = _to_result(ev.result.contents) if ev.result else None
            error = ev.error.decode("utf-8", errors="replace") if ev.error else None
            handler(Event(EventType(ev.type), StopReason(ev.stop_reason), result, error))
        except Exception:
            _log.exception("ptstt event handler raised")


def _as_float32(samples: PcmLike) -> bytearray:
    """Return a writable float32 buffer (copied) for ctypes."""
    if hasattr(samples, "astype") and hasattr(samples, "tobytes"):  # NumPy, without importing it
        return bytearray(samples.astype("<f4", copy=False).tobytes())
    if isinstance(samples, (bytes, bytearray, memoryview)):
        return bytearray(samples)
    import array

    if isinstance(samples, array.array) and samples.typecode == "f":
        return bytearray(samples.tobytes())
    return bytearray(array.array("f", samples).tobytes())
