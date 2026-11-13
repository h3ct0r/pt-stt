// The ptstt engine: microphone -> Silero VAD -> session state machine ->
// parakeet speech-to-text. See include/ptstt.h for the public contract.
#include "ptstt.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_source.hpp"
#include "ggml.h"
#include "json_util.hpp"
#include "parakeet_capi.h"

#ifndef PTSTT_VERSION
#define PTSTT_VERSION "0.0.0"
#endif

// parakeet.cpp keeps one process-wide GPU backend. It must be freed before the
// process exits (after the models), or ggml's Metal/CUDA teardown aborts. The C
// API does not expose it, so call the exported C++ function directly.
namespace pk {
void shutdown_backend();
}

using ptstt::kSampleRate;

namespace {

std::mutex g_engines_mu;
int g_live_engines = 0;

// Frees parakeet's global GPU backend when no engine uses it.
void release_backend_if_unused() {
    std::lock_guard<std::mutex> lk(g_engines_mu);
    if (g_live_engines == 0) pk::shutdown_backend();
}

constexpr size_t kBlock = 512;         // 32 ms at 16 kHz, one Silero frame
constexpr size_t kMaxQueuedResults = 16;

thread_local std::string t_create_error;  // last failed ptstt_create on this thread
thread_local std::string t_error_view;    // storage behind ptstt_last_error's pointer

char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

std::string trim(const char* s) {
    if (!s) return {};
    std::string t(s);
    const auto b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = t.find_last_not_of(" \t\r\n");
    return t.substr(b, e - b + 1);
}

void quiet_log(enum ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR) std::fputs(text, stderr);
}

struct OwnedResult {
    std::string text;
    int stop_reason = PTSTT_STOP_MANUAL;
    float audio_sec = 0, speech_sec = 0, transcribe_ms = 0;
};

}  // namespace

struct ptstt_engine {
    // Configuration (strings copied).
    ptstt_config cfg{};
    std::string asr_path, vad_path, device, input_file;

    parakeet_ctx* asr = nullptr;
    parakeet_ctx* vad = nullptr;
    parakeet_vad_stream* vad_stream = nullptr;
    std::unique_ptr<ptstt::AudioSource> source;
    std::mutex asr_mu;  // the ASR context is used by the worker and by ptstt_transcribe_*

    // Session control, guarded by mu.
    std::mutex mu;
    std::condition_variable cv;         // wakes the worker
    std::condition_variable result_cv;  // wakes ptstt_wait_result
    int state = PTSTT_IDLE;
    bool session_pending = false;
    int stop_request = -1;  // -1 = none, else PTSTT_STOP_*
    bool quit = false;
    std::deque<OwnedResult> results;

    std::mutex cb_mu;
    ptstt_event_cb cb = nullptr;
    void* cb_user = nullptr;

    std::mutex err_mu;
    std::string last_error;

    std::thread worker;

    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lk(err_mu);
        last_error = e;
    }

    void emit(int type, int reason = 0, const OwnedResult* r = nullptr, const char* err = nullptr) {
        ptstt_event_cb fn;
        void* user;
        {
            std::lock_guard<std::mutex> lk(cb_mu);
            fn = cb;
            user = cb_user;
        }
        if (!fn) return;
        ptstt_result view{};
        if (r) {
            view.text = const_cast<char*>(r->text.c_str());
            view.stop_reason = r->stop_reason;
            view.audio_sec = r->audio_sec;
            view.speech_sec = r->speech_sec;
            view.transcribe_ms = r->transcribe_ms;
        }
        ptstt_event ev{type, reason, r ? &view : nullptr, err};
        fn(&ev, user);
    }

    // Runs the model on 16 kHz mono PCM. Returns false and sets the error on failure.
    bool transcribe(const float* pcm, size_t n, int rate, std::string& text, float* ms) {
        std::lock_guard<std::mutex> lk(asr_mu);
        const auto t0 = std::chrono::steady_clock::now();
        char* raw = parakeet_capi_transcribe_pcm(asr, pcm, static_cast<int>(n), rate, cfg.decoder);
        const auto t1 = std::chrono::steady_clock::now();
        if (ms) *ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        if (!raw) {
            set_error(std::string("transcription failed: ") + parakeet_capi_last_error(asr));
            return false;
        }
        text = trim(raw);
        parakeet_capi_free_string(raw);
        return true;
    }

    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return quit || session_pending; });
                if (quit) return;
                session_pending = false;
            }
            run_session();
        }
    }

    void run_session() {
        emit(PTSTT_EV_LISTENING);
        parakeet_capi_vad_stream_reset(vad_stream);

        const double silence_s = cfg.silence_timeout_ms / 1000.0;
        const double no_speech_s = cfg.no_speech_timeout_ms / 1000.0;
        const double max_s = cfg.max_utterance_ms / 1000.0;
        const double pad_s = cfg.pad_ms / 1000.0;

        std::vector<float> audio;
        audio.reserve(static_cast<size_t>(kSampleRate) * 15);
        bool speech = false;
        double first_speech = 0, last_voice_end = 0;
        int reason = -1;
        std::string error;
        float block[kBlock];
        ptstt::VadFeed feed;

        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (quit) {
                    reason = PTSTT_STOP_CANCELLED;
                } else if (stop_request >= 0) {
                    reason = stop_request;
                }
                stop_request = -1;
            }
            if (reason >= 0) break;

            const size_t n = source->read(block, kBlock, 50);
            if (n == 0) continue;
            audio.insert(audio.end(), block, block + n);

            char* js = parakeet_capi_vad_stream_feed_json(vad_stream, block, static_cast<int>(n), 0);
            if (!js) {
                error = std::string("VAD failed: ") + parakeet_capi_last_error(vad);
                reason = PTSTT_STOP_ERROR;
                break;
            }
            const bool ok = ptstt::parse_vad_feed(js, feed);
            parakeet_capi_free_string(js);
            if (!ok) {
                error = "VAD returned malformed JSON";
                reason = PTSTT_STOP_ERROR;
                break;
            }

            for (size_t i = 0; i < feed.probabilities.size(); ++i) {
                if (feed.probabilities[i] >= cfg.vad_threshold) {
                    const double end = (feed.first_frame + static_cast<long>(i) + 1) * feed.frame_sec;
                    last_voice_end = std::max(last_voice_end, end);
                }
            }
            for (const auto& ev : feed.events) {
                if (ev.start && !speech) {
                    speech = true;
                    first_speech = std::max(0.0, ev.time);
                    last_voice_end = std::max(last_voice_end, ev.time);
                    emit(PTSTT_EV_SPEECH_START);
                }
            }

            const double now = static_cast<double>(audio.size()) / kSampleRate;
            if (speech && now - last_voice_end >= silence_s) {
                reason = PTSTT_STOP_SILENCE;
            } else if (!speech && now >= no_speech_s) {
                reason = PTSTT_STOP_NO_SPEECH;
            } else if (now >= max_s) {
                reason = PTSTT_STOP_MAX_DURATION;
            }
            if (reason >= 0) break;
        }
        source->stop();

        {
            std::lock_guard<std::mutex> lk(mu);
            state = PTSTT_TRANSCRIBING;
        }
        emit(PTSTT_EV_TRANSCRIBING, reason);

        OwnedResult r;
        r.stop_reason = reason;
        r.audio_sec = static_cast<float>(audio.size()) / kSampleRate;
        const bool run_model = speech && reason != PTSTT_STOP_CANCELLED && reason != PTSTT_STOP_ERROR;
        if (run_model) {
            const double now = r.audio_sec;
            const size_t s = static_cast<size_t>(std::max(0.0, first_speech - pad_s) * kSampleRate);
            const size_t e = static_cast<size_t>(std::min(now, last_voice_end + pad_s) * kSampleRate);
            if (e > s) {
                r.speech_sec = static_cast<float>(e - s) / kSampleRate;
                if (!transcribe(audio.data() + s, e - s, kSampleRate, r.text, &r.transcribe_ms)) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    error = last_error;
                }
            }
        }
        if (!error.empty()) {
            set_error(error);
            emit(PTSTT_EV_ERROR, reason, nullptr, error.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(mu);
            if (results.size() >= kMaxQueuedResults) results.pop_front();
            results.push_back(r);
            state = PTSTT_IDLE;
        }
        result_cv.notify_all();
        emit(PTSTT_EV_RESULT, r.stop_reason, &r);
    }
};

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------
extern "C" {

int ptstt_abi_version(void) { return PTSTT_ABI_VERSION; }
const char* ptstt_version(void) { return PTSTT_VERSION; }

void ptstt_config_default(ptstt_config* c) {
    if (!c) return;
    std::memset(c, 0, sizeof *c);
    c->size = sizeof *c;
    c->decoder = PTSTT_DECODER_DEFAULT;
    c->vad_threshold = 0.5f;
    c->silence_timeout_ms = 2000;
    c->no_speech_timeout_ms = 5000;
    c->max_utterance_ms = 30000;
    c->pad_ms = 300;
}

ptstt_engine* ptstt_create(const ptstt_config* user_cfg) {
    t_create_error.clear();
    if (!user_cfg || user_cfg->size <= 0) {
        t_create_error = "config is NULL or config.size is not set (call ptstt_config_default)";
        return nullptr;
    }
    auto e = std::make_unique<ptstt_engine>();
    ptstt_config_default(&e->cfg);
    std::memcpy(&e->cfg, user_cfg, std::min<size_t>(static_cast<size_t>(user_cfg->size), sizeof(ptstt_config)));
    e->cfg.size = sizeof(ptstt_config);
    auto& c = e->cfg;

    if (!c.asr_model || !*c.asr_model || !c.vad_model || !*c.vad_model) {
        t_create_error = "asr_model and vad_model paths are required";
        return nullptr;
    }
    if (c.decoder < 0 || c.decoder > 2) {
        t_create_error = "decoder must be 0 (default), 1 (CTC) or 2 (RNN-T)";
        return nullptr;
    }
    if (c.vad_threshold <= 0 || c.vad_threshold > 1 || c.silence_timeout_ms <= 0 ||
        c.no_speech_timeout_ms <= 0 || c.max_utterance_ms <= 0 || c.pad_ms < 0) {
        t_create_error = "invalid timing or threshold in config";
        return nullptr;
    }
    e->asr_path = c.asr_model;
    e->vad_path = c.vad_model;
    e->device = c.capture_device ? c.capture_device : "";
    e->input_file = c.input_file ? c.input_file : "";
    c.asr_model = e->asr_path.c_str();
    c.vad_model = e->vad_path.c_str();
    c.capture_device = e->device.empty() ? nullptr : e->device.c_str();
    c.input_file = e->input_file.empty() ? nullptr : e->input_file.c_str();

    ggml_log_set(c.verbose ? nullptr : quiet_log, nullptr);

    e->asr = parakeet_capi_load(c.asr_model);
    if (!e->asr) {
        t_create_error = std::string("cannot load ASR model '") + c.asr_model + "': " + parakeet_capi_load_error();
        release_backend_if_unused();
        return nullptr;
    }
    e->vad = parakeet_capi_load(c.vad_model);
    if (!e->vad) {
        t_create_error = std::string("cannot load VAD model '") + c.vad_model + "': " + parakeet_capi_load_error();
        parakeet_capi_free(e->asr);
        release_backend_if_unused();
        return nullptr;
    }
    char opts[160];
    std::snprintf(opts, sizeof opts, "{\"threshold\":%.3f,\"min_speech\":0.25,\"probabilities\":true}", c.vad_threshold);
    e->vad_stream = parakeet_capi_vad_stream_begin(e->vad, kSampleRate, opts);
    if (!e->vad_stream) {
        t_create_error = std::string("cannot start VAD stream: ") + parakeet_capi_last_error(e->vad);
        parakeet_capi_free(e->vad);
        parakeet_capi_free(e->asr);
        release_backend_if_unused();
        return nullptr;
    }

    std::string err;
    e->source = c.input_file ? ptstt::make_file_source(c.input_file, err) : ptstt::make_mic_source(c.capture_device, err);
    if (!e->source) {
        t_create_error = err;
        parakeet_capi_vad_stream_free(e->vad_stream);
        parakeet_capi_free(e->vad);
        parakeet_capi_free(e->asr);
        release_backend_if_unused();
        return nullptr;
    }

    // Warm-up: the first GPU run compiles kernels; do it now, not on the first utterance.
    {
        std::vector<float> silence(kSampleRate / 2, 0.0f);
        std::string ignored;
        e->transcribe(silence.data(), silence.size(), kSampleRate, ignored, nullptr);
        e->set_error("");
    }

    ptstt_engine* raw = e.release();
    raw->worker = std::thread([raw] { raw->run(); });
    {
        std::lock_guard<std::mutex> lk(g_engines_mu);
        ++g_live_engines;
    }
    return raw;
}

void ptstt_destroy(ptstt_engine* e) {
    if (!e) return;
    {
        std::lock_guard<std::mutex> lk(e->mu);
        e->quit = true;
    }
    e->cv.notify_all();
    e->result_cv.notify_all();
    if (e->worker.joinable()) e->worker.join();
    e->source.reset();
    parakeet_capi_vad_stream_free(e->vad_stream);
    parakeet_capi_free(e->vad);
    parakeet_capi_free(e->asr);
    delete e;

    {
        std::lock_guard<std::mutex> lk(g_engines_mu);
        --g_live_engines;
    }
    release_backend_if_unused();  // recreated lazily by the next engine
}

const char* ptstt_last_error(ptstt_engine* e) {
    if (!e) {
        t_error_view = t_create_error;
    } else {
        std::lock_guard<std::mutex> lk(e->err_mu);
        t_error_view = e->last_error;
    }
    return t_error_view.c_str();
}

void ptstt_set_callback(ptstt_engine* e, ptstt_event_cb cb, void* user) {
    if (!e) return;
    std::lock_guard<std::mutex> lk(e->cb_mu);
    e->cb = cb;
    e->cb_user = user;
}

int ptstt_start(ptstt_engine* e) {
    if (!e) return -1;
    std::lock_guard<std::mutex> lk(e->mu);
    if (e->state != PTSTT_IDLE || e->session_pending) {
        e->set_error("busy: a session is already running");
        return -1;
    }
    std::string err;
    if (!e->source->start(err)) {
        e->set_error(err);
        return -1;
    }
    e->results.clear();
    e->stop_request = -1;
    e->state = PTSTT_LISTENING;
    e->session_pending = true;
    e->cv.notify_all();
    return 0;
}

static int request_stop(ptstt_engine* e, int reason) {
    if (!e) return -1;
    std::lock_guard<std::mutex> lk(e->mu);
    if (e->state != PTSTT_LISTENING) {
        e->set_error("not listening");
        return -1;
    }
    if (e->stop_request < 0) e->stop_request = reason;
    return 0;
}

int ptstt_stop(ptstt_engine* e) { return request_stop(e, PTSTT_STOP_MANUAL); }
int ptstt_cancel(ptstt_engine* e) { return request_stop(e, PTSTT_STOP_CANCELLED); }

int ptstt_state(ptstt_engine* e) {
    if (!e) return PTSTT_IDLE;
    std::lock_guard<std::mutex> lk(e->mu);
    return e->state;
}

int ptstt_wait_result(ptstt_engine* e, int timeout_ms, ptstt_result* out) {
    if (!e || !out) return -1;
    std::unique_lock<std::mutex> lk(e->mu);
    auto ready = [&] { return e->quit || !e->results.empty(); };
    if (timeout_ms < 0) {
        e->result_cv.wait(lk, ready);
    } else if (!e->result_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready)) {
        return 1;
    }
    if (e->results.empty()) return -1;  // engine is shutting down
    const OwnedResult r = std::move(e->results.front());
    e->results.pop_front();
    lk.unlock();
    out->text = dup_cstr(r.text);
    out->stop_reason = r.stop_reason;
    out->audio_sec = r.audio_sec;
    out->speech_sec = r.speech_sec;
    out->transcribe_ms = r.transcribe_ms;
    return out->text ? 0 : -1;
}

void ptstt_result_clear(ptstt_result* r) {
    if (!r) return;
    std::free(r->text);
    std::memset(r, 0, sizeof *r);
}

char* ptstt_transcribe_pcm(ptstt_engine* e, const float* pcm, int n, int rate) {
    if (!e || !pcm || n <= 0 || rate <= 0) {
        if (e) e->set_error("invalid PCM arguments");
        return nullptr;
    }
    std::string text;
    if (!e->transcribe(pcm, static_cast<size_t>(n), rate, text, nullptr)) return nullptr;
    return dup_cstr(text);
}

char* ptstt_transcribe_file(ptstt_engine* e, const char* path) {
    if (!e || !path) return nullptr;
    std::vector<float> pcm;
    std::string err;
    if (!ptstt::decode_file_16k(path, pcm, err)) {
        e->set_error(err);
        return nullptr;
    }
    if (pcm.empty()) return dup_cstr("");
    std::string text;
    if (!e->transcribe(pcm.data(), pcm.size(), kSampleRate, text, nullptr)) return nullptr;
    return dup_cstr(text);
}

char* ptstt_list_devices_json(void) { return dup_cstr(ptstt::list_capture_devices_json()); }

void ptstt_free_string(char* s) { std::free(s); }

const char* ptstt_stop_reason_name(int r) {
    switch (r) {
        case PTSTT_STOP_MANUAL: return "manual";
        case PTSTT_STOP_SILENCE: return "silence";
        case PTSTT_STOP_NO_SPEECH: return "no_speech";
        case PTSTT_STOP_MAX_DURATION: return "max_duration";
        case PTSTT_STOP_CANCELLED: return "cancelled";
        case PTSTT_STOP_ERROR: return "error";
        default: return "unknown";
    }
}

}  // extern "C"
