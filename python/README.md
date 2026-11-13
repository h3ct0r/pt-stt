# ptstt (Python)

Python 3.10+ bindings for [pt-stt](https://github.com/h3ct0r/pt-stt), a push-to-talk Portuguese speech-to-text library.

These bindings are pure ctypes: nothing is compiled for Python. You do need the shared library built by CMake:

```bash
pip install -e python/
```

```python
from ptstt import Transcriber

with Transcriber() as stt:
    print(stt.listen().text)
```

See [docs/python-api.md](https://github.com/h3ct0r/pt-stt/blob/main/docs/python-api.md) for the full API.
