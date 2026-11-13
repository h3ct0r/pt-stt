#!/usr/bin/env python3
"""Press Enter, speak Portuguese, get text. Ctrl-C quits.

    PYTHONPATH=python python3 examples/listen.py            # microphone
    PYTHONPATH=python python3 examples/listen.py --device USB

Listening ends after 2 s of silence once you start speaking, or after 5 s if
you say nothing. Works with Python 3.10+.
"""

from __future__ import annotations

import argparse
import sys

from ptstt import EventType, StopReason, Transcriber


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", help="capture device (substring of its name)")
    ap.add_argument("--silence", type=float, default=2.0, help="seconds of silence that end a recording")
    ap.add_argument("--input-file", help="use an audio file instead of the microphone")
    args = ap.parse_args()

    with Transcriber(device=args.device, silence_timeout=args.silence, input_file=args.input_file) as stt:
        stt.on_event(lambda ev: ev.type == EventType.SPEECH_START and print("  (ouvindo fala…)", file=sys.stderr))
        while True:
            try:
                input("Enter para falar (Ctrl-C sai) ")
                result = stt.listen()
            except (KeyboardInterrupt, EOFError):
                print()
                return 0
            if result.stop_reason == StopReason.NO_SPEECH:
                print("  nada foi dito")
            else:
                print(f"  {result.text}   [{result.stop_reason.name.lower()}, {result.transcribe_ms:.0f} ms]")


if __name__ == "__main__":
    sys.exit(main())
