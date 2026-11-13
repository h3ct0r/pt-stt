// ptstt.hpp - header-only C++17 wrapper over the ptstt C API.
//
//   ptstt::Config cfg;
//   cfg.asr_model = "models/pt-fastconformer-large.f32.gguf";
//   cfg.vad_model = "models/silero-vad-f32.gguf";
//   ptstt::Engine stt(cfg);
//   ptstt::Result r = stt.listen();          // start + wait for the result
//   std::cout << r.text << " (" << r.reason_name() << ")\n";
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "ptstt.h"

namespace ptstt {

enum class State { Idle = PTSTT_IDLE, Listening = PTSTT_LISTENING, Transcribing = PTSTT_TRANSCRIBING };

enum class StopReason {
    Manual = PTSTT_STOP_MANUAL,
    Silence = PTSTT_STOP_SILENCE,
    NoSpeech = PTSTT_STOP_NO_SPEECH,
    MaxDuration = PTSTT_STOP_MAX_DURATION,
    Cancelled = PTSTT_STOP_CANCELLED,
    Error = PTSTT_STOP_ERROR
};

enum class EventType {
    Listening = PTSTT_EV_LISTENING,
    SpeechStart = PTSTT_EV_SPEECH_START,
    Transcribing = PTSTT_EV_TRANSCRIBING,
    Result = PTSTT_EV_RESULT,
    Error = PTSTT_EV_ERROR
};

enum class Decoder { Default = PTSTT_DECODER_DEFAULT, Ctc = PTSTT_DECODER_CTC, Rnnt = PTSTT_DECODER_RNNT };

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Config {
    std::string asr_model;
    std::string vad_model;
    Decoder decoder = Decoder::Default;
    float vad_threshold = 0.5f;
    int silence_timeout_ms = 2000;
    int no_speech_timeout_ms = 5000;
    int max_utterance_ms = 30000;
    int pad_ms = 300;
    std::string capture_device;  // empty = system default
    std::string input_file;      // non-empty = read this file instead of the mic
    bool verbose = false;
};

struct Result {
    std::string text;
    StopReason stop_reason = StopReason::Manual;
    float audio_sec = 0, speech_sec = 0, transcribe_ms = 0;

    const char* reason_name() const { return ptstt_stop_reason_name(static_cast<int>(stop_reason)); }

    static Result from_c(const ptstt_result& r) {
        Result o;
        o.text = r.text ? r.text : "";
        o.stop_reason = static_cast<StopReason>(r.stop_reason);
        o.audio_sec = r.audio_sec;
        o.speech_sec = r.speech_sec;
        o.transcribe_ms = r.transcribe_ms;
        return o;
    }
};

struct Event {
    EventType type;
    StopReason stop_reason;
    std::optional<Result> result;  // set for EventType::Result
    std::string error;             // set for EventType::Error
};

class Engine {
public:
    using EventHandler = std::function<void(const Event&)>;

    explicit Engine(const Config& c) {
        ptstt_config cc;
        ptstt_config_default(&cc);
        cc.asr_model = c.asr_model.c_str();
        cc.vad_model = c.vad_model.c_str();
        cc.decoder = static_cast<int>(c.decoder);
        cc.vad_threshold = c.vad_threshold;
        cc.silence_timeout_ms = c.silence_timeout_ms;
        cc.no_speech_timeout_ms = c.no_speech_timeout_ms;
        cc.max_utterance_ms = c.max_utterance_ms;
        cc.pad_ms = c.pad_ms;
        cc.capture_device = c.capture_device.empty() ? nullptr : c.capture_device.c_str();
        cc.input_file = c.input_file.empty() ? nullptr : c.input_file.c_str();
        cc.verbose = c.verbose ? 1 : 0;
        h_ = ptstt_create(&cc);
        if (!h_) throw Error(ptstt_last_error(nullptr));
        ptstt_set_callback(h_, &Engine::trampoline, this);
    }

    ~Engine() {
        if (h_) ptstt_destroy(h_);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Called on the engine's worker thread. Keep it short.
    void on_event(EventHandler h) {
        std::lock_guard<std::mutex> lk(handler_mu_);
        handler_ = std::move(h);
    }

    void start() { check(ptstt_start(h_)); }
    // Return false when no session was running.
    bool stop() { return ptstt_stop(h_) == 0; }
    bool cancel() { return ptstt_cancel(h_) == 0; }
    State state() const { return static_cast<State>(ptstt_state(h_)); }

    // Wait for the next result; std::nullopt on timeout. timeout_ms < 0 waits forever.
    std::optional<Result> wait_result(int timeout_ms = -1) {
        ptstt_result r{};
        const int rc = ptstt_wait_result(h_, timeout_ms, &r);
        if (rc == 1) return std::nullopt;
        if (rc != 0) throw Error(ptstt_last_error(h_));
        Result out = Result::from_c(r);
        ptstt_result_clear(&r);
        return out;
    }

    // Start a session and block until its result.
    Result listen() {
        start();
        return *wait_result(-1);
    }

    std::string transcribe_file(const std::string& path) { return take(ptstt_transcribe_file(h_, path.c_str())); }

    std::string transcribe_pcm(const float* pcm, size_t n, int sample_rate = 16000) {
        return take(ptstt_transcribe_pcm(h_, pcm, static_cast<int>(n), sample_rate));
    }

    static std::string list_devices_json() {
        char* s = ptstt_list_devices_json();
        std::string out = s ? s : "[]";
        ptstt_free_string(s);
        return out;
    }

    ptstt_engine* handle() const { return h_; }

private:
    void check(int rc) const {
        if (rc != 0) throw Error(ptstt_last_error(h_));
    }

    std::string take(char* s) const {
        if (!s) throw Error(ptstt_last_error(h_));
        std::string out = s;
        ptstt_free_string(s);
        return out;
    }

    static void trampoline(const ptstt_event* ev, void* user) {
        auto* self = static_cast<Engine*>(user);
        EventHandler h;
        {
            std::lock_guard<std::mutex> lk(self->handler_mu_);
            h = self->handler_;
        }
        if (!h) return;
        Event e{static_cast<EventType>(ev->type), static_cast<StopReason>(ev->stop_reason), std::nullopt,
                ev->error ? ev->error : ""};
        if (ev->result) e.result = Result::from_c(*ev->result);
        h(e);
    }

    ptstt_engine* h_ = nullptr;
    std::mutex handler_mu_;
    EventHandler handler_;
};

}  // namespace ptstt
