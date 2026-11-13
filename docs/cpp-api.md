# C++ API

`include/ptstt.hpp` is a header-only C++17 wrapper over the [C API](c-api.md). It manages the engine lifetime, uses `std::string`, `std::optional` and `std::function`, and reports errors as exceptions. Link with `libptstt` as for C.

```cpp
#include "ptstt.hpp"
```

- [Engine](#engine)
- [Config](#config)
- [Result and Event](#result-and-event)
- [Example](#example)

## Engine

```cpp
class ptstt::Engine {
public:
    explicit Engine(const Config&);           // loads the models; throws ptstt::Error
    ~Engine();                                // ptstt_destroy
    Engine(const Engine&) = delete;           // not copyable or movable: hold it by value
    Engine(Engine&&) = delete;                // or in a std::unique_ptr

    void start();                             // throws if busy
    bool stop();                              // false if not listening
    bool cancel();                            // false if not listening
    State state() const;                      // Idle, Listening, Transcribing

    std::optional<Result> wait_result(int timeout_ms = -1);   // nullopt on timeout
    Result listen();                          // start() + wait_result(-1)

    void on_event(std::function<void(const Event&)> handler); // runs on the worker thread

    std::string transcribe_file(const std::string& path);
    std::string transcribe_pcm(const float* pcm, size_t n, int sample_rate = 16000);

    static std::string list_devices_json();
    ptstt_engine* handle() const;             // the underlying C handle
};
```

All failures throw `ptstt::Error`, a `std::runtime_error` carrying the library's message.

## Config

```cpp
struct ptstt::Config {
    std::string asr_model, vad_model;         // required
    Decoder decoder = Decoder::Default;       // Default (RNN-T), Rnnt, Ctc
    float vad_threshold = 0.5f;
    int silence_timeout_ms = 2000;
    int no_speech_timeout_ms = 5000;
    int max_utterance_ms = 30000;
    int pad_ms = 300;
    std::string capture_device;               // empty = system default
    std::string input_file;                   // non-empty = use a file instead of the mic
    bool verbose = false;
};
```

The fields have the same meaning as in the [C configuration](c-api.md#configuration).

## Result and Event

```cpp
struct ptstt::Result {
    std::string text;
    StopReason stop_reason;     // Manual, Silence, NoSpeech, MaxDuration, Cancelled, Error
    float audio_sec, speech_sec, transcribe_ms;
    const char* reason_name() const;   // "silence", ...
};

struct ptstt::Event {
    EventType type;             // Listening, SpeechStart, Transcribing, Result, Error
    StopReason stop_reason;
    std::optional<Result> result;   // for EventType::Result
    std::string error;              // for EventType::Error
};
```

## Example

`examples/listen.cpp` listens three times and prints what was said:

```bash
./build/bin/ptstt-example-listen models/pt-fastconformer-large.f32.gguf models/silero-vad-f32.gguf
```

A robot node that starts listening on an external trigger and hands the text to a queue:

```cpp
#include <condition_variable>
#include <mutex>
#include <queue>
#include "ptstt.hpp"

class VoiceInput {
public:
    explicit VoiceInput(const ptstt::Config& cfg) : stt_(cfg) {
        stt_.on_event([this](const ptstt::Event& ev) {
            if (ev.type != ptstt::EventType::Result || ev.result->text.empty()) return;
            std::lock_guard<std::mutex> lk(mu_);
            texts_.push(ev.result->text);
            cv_.notify_one();
        });
    }

    void on_button_pressed() {                 // e.g. from a GPIO or ROS callback
        if (stt_.state() == ptstt::State::Idle) stt_.start();
        else stt_.stop();
    }

    std::string next_command() {               // blocks until something was said
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !texts_.empty(); });
        std::string t = texts_.front();
        texts_.pop();
        return t;
    }

private:
    ptstt::Engine stt_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::string> texts_;
};
```

### Using pt-stt from another CMake project

The simplest way is to install pt-stt (`cmake --install build --prefix /opt/pt-stt`) and link the installed library:

```cmake
find_library(PTSTT_LIB ptstt PATHS /opt/pt-stt/lib REQUIRED)
target_include_directories(my_robot PRIVATE /opt/pt-stt/include)
target_link_libraries(my_robot PRIVATE ${PTSTT_LIB})
set_target_properties(my_robot PROPERTIES BUILD_RPATH /opt/pt-stt/lib INSTALL_RPATH /opt/pt-stt/lib)
```
