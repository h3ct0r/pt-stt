"""Tests for the Python bindings. Uses audio files instead of the microphone.

    PTSTT_LIB=build/lib/libptstt.dylib PYTHONPATH=python python3 tests/test_python.py
"""

from __future__ import annotations

import array
import os
import sys
import threading
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import ptstt  # noqa: E402
from ptstt import EventType, PtsttError, State, StopReason, Transcriber  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures"
MODELS = Path(os.environ.get("PTSTT_MODELS_DIR", ROOT / "models"))
ASR = MODELS / ptstt.ASR_MODEL_FILE
VAD = MODELS / ptstt.VAD_MODEL_FILE


def read_wav(path: Path) -> tuple[array.array, int]:
    with wave.open(str(path)) as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        raw = array.array("h", w.readframes(w.getnframes()))
        rate = w.getframerate()
    return array.array("f", (s / 32768.0 for s in raw)), rate


class TranscriberTest(unittest.TestCase):
    stt: Transcriber

    @classmethod
    def setUpClass(cls) -> None:
        cls.stt = Transcriber(ASR, VAD, input_file=FIXTURES / "speech_pause.wav")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.stt.close()

    def test_listen_stops_on_silence(self) -> None:
        events: list[EventType] = []
        done = threading.Event()

        def on_event(ev: ptstt.Event) -> None:
            events.append(ev.type)
            if ev.type == EventType.RESULT:
                self.assertIsNotNone(ev.result)
                done.set()

        self.stt.on_event(on_event)
        try:
            r = self.stt.listen(timeout=60)
        finally:
            self.stt.on_event(None)
        self.assertEqual(r.stop_reason, StopReason.SILENCE)
        self.assertIn("esquerda", r.text)
        self.assertGreater(r.speech_sec, 3.0)
        self.assertTrue(done.wait(5))
        self.assertEqual(events[:3], [EventType.LISTENING, EventType.SPEECH_START, EventType.TRANSCRIBING])
        self.assertEqual(self.stt.state, State.IDLE)

    def test_stop_from_callback(self) -> None:
        self.stt.on_event(lambda ev: ev.type == EventType.SPEECH_START and self.stt.stop())
        try:
            r = self.stt.listen(timeout=60)
        finally:
            self.stt.on_event(None)
        self.assertEqual(r.stop_reason, StopReason.MANUAL)

    def test_start_twice_raises(self) -> None:
        errors: list[Exception] = []

        def on_event(ev: ptstt.Event) -> None:
            if ev.type == EventType.LISTENING:
                try:
                    self.stt.start()
                except PtsttError as e:
                    errors.append(e)

        self.stt.on_event(on_event)
        try:
            self.stt.listen(timeout=60)
        finally:
            self.stt.on_event(None)
        self.assertEqual(len(errors), 1)
        self.assertIn("busy", str(errors[0]))

    def test_transcribe_file(self) -> None:
        self.assertIn("dois metros", self.stt.transcribe_file(FIXTURES / "speech.wav"))
        with self.assertRaises(PtsttError):
            self.stt.transcribe_file("/nonexistent.wav")

    def test_transcribe_pcm(self) -> None:
        pcm, rate = read_wav(FIXTURES / "speech.wav")
        self.assertIn("virar", self.stt.transcribe_pcm(pcm, rate))
        self.assertIn("virar", self.stt.transcribe_pcm(pcm.tobytes(), rate))
        self.assertEqual(self.stt.transcribe_pcm(b""), "")

    def test_wait_timeout(self) -> None:
        self.assertIsNone(self.stt.wait_result(timeout=0.05))


class MiscTest(unittest.TestCase):
    def test_no_speech(self) -> None:
        with Transcriber(ASR, VAD, input_file=FIXTURES / "silence.wav") as stt:
            r = stt.listen(timeout=60)
        self.assertEqual(r.stop_reason, StopReason.NO_SPEECH)
        self.assertEqual(r.text, "")
        self.assertAlmostEqual(r.audio_sec, 5.0, delta=0.1)

    def test_bad_model(self) -> None:
        with self.assertRaises(PtsttError) as cm:
            Transcriber("/nonexistent.gguf", VAD)
        self.assertIn("nonexistent", str(cm.exception))

    def test_closed(self) -> None:
        stt = Transcriber(ASR, VAD, input_file=FIXTURES / "speech.wav")
        stt.close()
        stt.close()
        with self.assertRaises(PtsttError):
            stt.start()

    def test_version(self) -> None:
        self.assertEqual(ptstt.version(), ptstt.__version__)
        self.assertIsInstance(ptstt.list_devices(), list)


if __name__ == "__main__":
    unittest.main(verbosity=2)
