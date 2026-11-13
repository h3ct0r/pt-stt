// Engine tests driven by audio files instead of the microphone (no device needed).
// Time inside the engine is counted in samples, so the results are deterministic.
//
//   test_engine --model M.gguf --vad V.gguf --fixtures tests/fixtures
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ptstt.h"

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static std::string g_model, g_vad, g_fixtures;

static ptstt_engine* make(const std::string& file, int max_ms = 30000) {
    ptstt_config c;
    ptstt_config_default(&c);
    c.asr_model = g_model.c_str();
    c.vad_model = g_vad.c_str();
    std::string path = g_fixtures + "/" + file;
    c.input_file = path.c_str();
    c.max_utterance_ms = max_ms;
    ptstt_engine* e = ptstt_create(&c);
    if (!e) std::fprintf(stderr, "  create failed: %s\n", ptstt_last_error(nullptr));
    return e;
}

static ptstt_result listen(ptstt_engine* e) {
    ptstt_result r{};
    CHECK(ptstt_start(e) == 0);
    CHECK(ptstt_wait_result(e, 60000, &r) == 0);
    return r;
}

static void print(const char* name, const ptstt_result& r) {
    std::printf("  %-14s reason=%-12s audio=%.2fs speech=%.2fs asr=%.0fms text=\"%s\"\n", name,
                ptstt_stop_reason_name(r.stop_reason), r.audio_sec, r.speech_sec, r.transcribe_ms,
                r.text ? r.text : "");
}

// Callback actions used by the tests.
struct Hook {
    enum { NONE, STOP_ON_SPEECH, CANCEL_ON_SPEECH, START_WHILE_BUSY } action = NONE;
    ptstt_engine* engine = nullptr;
    int busy_rc = 0;
    int events = 0;
};

static void on_event(const ptstt_event* ev, void* user) {
    auto* h = static_cast<Hook*>(user);
    ++h->events;
    if (ev->type == PTSTT_EV_SPEECH_START) {
        if (h->action == Hook::STOP_ON_SPEECH) ptstt_stop(h->engine);
        if (h->action == Hook::CANCEL_ON_SPEECH) ptstt_cancel(h->engine);
    }
    if (ev->type == PTSTT_EV_LISTENING && h->action == Hook::START_WHILE_BUSY) {
        h->busy_rc = ptstt_start(h->engine);
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--model")) g_model = argv[i + 1];
        else if (!std::strcmp(argv[i], "--vad")) g_vad = argv[i + 1];
        else if (!std::strcmp(argv[i], "--fixtures")) g_fixtures = argv[i + 1];
    }
    if (g_model.empty() || g_vad.empty() || g_fixtures.empty()) {
        std::fprintf(stderr, "usage: %s --model M --vad V --fixtures DIR\n", argv[0]);
        return 2;
    }

    std::printf("config errors\n");
    {
        ptstt_config c;
        ptstt_config_default(&c);
        CHECK(ptstt_create(&c) == nullptr);
        CHECK(std::strlen(ptstt_last_error(nullptr)) > 0);
        c.asr_model = "/nonexistent.gguf";
        c.vad_model = g_vad.c_str();
        CHECK(ptstt_create(&c) == nullptr);
        CHECK(std::strstr(ptstt_last_error(nullptr), "nonexistent") != nullptr);
    }

    // speech_pause.wav = 0.5 s silence + sentence (~4.1 s) + 3 s silence.
    ptstt_engine* e = make("speech_pause.wav");
    if (!e) return 1;
    Hook hook;
    hook.engine = e;
    ptstt_set_callback(e, on_event, &hook);

    std::printf("silence timeout\n");
    {
        ptstt_result r = listen(e);
        print("silence", r);
        CHECK(r.stop_reason == PTSTT_STOP_SILENCE);
        CHECK(r.text && std::strstr(r.text, "esquerda") != nullptr);
        CHECK(r.text && std::strstr(r.text, "dois metros") != nullptr);
        // Stopped 2 s after the voice ended: before the file's 3 s of trailing silence ran out.
        CHECK(r.audio_sec > 5.5f && r.audio_sec < 7.6f);
        CHECK(r.speech_sec > 3.0f && r.speech_sec < r.audio_sec - 1.5f);
        CHECK(hook.events >= 4);  // LISTENING, SPEECH_START, TRANSCRIBING, RESULT
        ptstt_result_clear(&r);
    }

    std::printf("manual stop\n");
    {
        hook.action = Hook::STOP_ON_SPEECH;
        ptstt_result r = listen(e);
        print("manual", r);
        CHECK(r.stop_reason == PTSTT_STOP_MANUAL);
        CHECK(r.audio_sec < 2.0f);
        ptstt_result_clear(&r);
    }

    std::printf("cancel\n");
    {
        hook.action = Hook::CANCEL_ON_SPEECH;
        ptstt_result r = listen(e);
        print("cancel", r);
        CHECK(r.stop_reason == PTSTT_STOP_CANCELLED);
        CHECK(r.text && r.text[0] == '\0');
        CHECK(r.speech_sec == 0.0f);
        ptstt_result_clear(&r);
    }

    std::printf("start while busy\n");
    {
        hook.action = Hook::START_WHILE_BUSY;
        hook.busy_rc = 0;
        ptstt_result r = listen(e);
        CHECK(hook.busy_rc == -1);
        CHECK(std::strstr(ptstt_last_error(e), "busy") != nullptr);
        CHECK(ptstt_state(e) == PTSTT_IDLE);
        CHECK(ptstt_stop(e) == -1);  // nothing to stop
        ptstt_result_clear(&r);
    }

    std::printf("wait timeout\n");
    {
        ptstt_result r{};
        CHECK(ptstt_wait_result(e, 10, &r) == 1);
    }

    std::printf("transcribe_file\n");
    {
        const std::string p = g_fixtures + "/speech.wav";
        char* t = ptstt_transcribe_file(e, p.c_str());
        std::printf("  file           text=\"%s\"\n", t ? t : "(null)");
        CHECK(t && std::strstr(t, "virar") != nullptr);
        ptstt_free_string(t);
        CHECK(ptstt_transcribe_file(e, "/nonexistent.wav") == nullptr);
    }
    ptstt_destroy(e);

    std::printf("no speech\n");
    {
        ptstt_engine* s = make("silence.wav");
        CHECK(s != nullptr);
        if (s) {
            ptstt_result r = listen(s);
            print("no_speech", r);
            CHECK(r.stop_reason == PTSTT_STOP_NO_SPEECH);
            CHECK(std::fabs(r.audio_sec - 5.0f) < 0.1f);
            CHECK(r.text && r.text[0] == '\0');
            CHECK(r.speech_sec == 0.0f);
            ptstt_result_clear(&r);
            ptstt_destroy(s);
        }
    }

    std::printf("max duration\n");
    {
        ptstt_engine* m = make("speech.wav", 1500);
        CHECK(m != nullptr);
        if (m) {
            ptstt_result r = listen(m);
            print("max", r);
            CHECK(r.stop_reason == PTSTT_STOP_MAX_DURATION);
            CHECK(std::fabs(r.audio_sec - 1.5f) < 0.05f);
            ptstt_result_clear(&r);
            ptstt_destroy(m);
        }
    }

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
