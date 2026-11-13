// ptstt-cli: push-to-talk Portuguese speech-to-text in the terminal.
//
// Space starts listening. Listening ends on the next space press, or after
// 2 s of silence once you have started speaking. The transcript goes to stdout
// and the status lines go to stderr, so the output can be piped.
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ptstt.h"

namespace {

const char* kAsrFile = "pt-fastconformer-large.f32.gguf";
const char* kVadFile = "silero-vad-f32.gguf";

std::atomic<bool> g_quit{false};
std::atomic<bool> g_got_result{false};
std::mutex g_out_mu;
bool g_color = false;
bool g_json = false;

struct Options {
    std::string model, vad, device, file, mic_file;
    int decoder = PTSTT_DECODER_DEFAULT;
    double silence = 2.0, no_speech = 5.0, max = 30.0, threshold = 0.5;
    bool json = false, once = false, verbose = false, list_devices = false;
};

void usage(FILE* f) {
    std::fprintf(f,
        "usage: ptstt-cli [options]\n"
        "\n"
        "Push-to-talk Portuguese speech-to-text. Press SPACE to start, SPACE again to\n"
        "stop (or stay silent for --silence seconds). c cancels, q / Esc / Ctrl-C quits.\n"
        "\n"
        "options:\n"
        "  --model PATH        speech model GGUF   (default: models/%s)\n"
        "  --vad PATH          Silero VAD GGUF     (default: models/%s)\n"
        "  --decoder NAME      rnnt (default) or ctc\n"
        "  --silence SEC       stop after this much silence once speech began (default 2)\n"
        "  --no-speech SEC     stop if no speech starts within this (default 5)\n"
        "  --max SEC           maximum length of one recording (default 30)\n"
        "  --threshold P       VAD speech probability threshold, 0-1 (default 0.5)\n"
        "  --device NAME       capture device (substring of its name)\n"
        "  --list-devices      print the capture devices and exit\n"
        "  --file PATH         transcribe an audio file (WAV/FLAC/MP3) and exit\n"
        "  --mic-file PATH     use an audio file as the microphone (testing)\n"
        "  --once              exit after the first transcript\n"
        "  --json              print each result as one JSON line\n"
        "  --verbose           show model and GPU backend logs\n"
        "  --version           print the version and exit\n"
        "  -h, --help          show this help\n"
        "\n"
        "Models are looked up in $PTSTT_MODELS_DIR, ./models and <repo>/models.\n",
        kAsrFile, kVadFile);
}

bool file_exists(const std::string& p) { return access(p.c_str(), R_OK) == 0; }

std::string dirname_of(const std::string& p) {
    const auto s = p.find_last_of('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}

// Finds `name` in the usual model directories; returns "" if absent.
std::string find_model(const char* name, const char* argv0) {
    std::vector<std::string> dirs;
    if (const char* d = std::getenv("PTSTT_MODELS_DIR")) dirs.push_back(d);
    dirs.push_back("models");
    char real[4096];
    if (realpath(argv0, real)) {
        const std::string exe_dir = dirname_of(real);
        dirs.push_back(exe_dir + "/../../models");          // build/bin -> repo/models
        dirs.push_back(exe_dir + "/../share/pt-stt/models");  // installed layout
    }
    for (const auto& d : dirs) {
        const std::string p = d + "/" + name;
        if (file_exists(p)) return p;
    }
    return "";
}

std::string json_quote(const char* s) {
    std::string o = "\"";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += static_cast<char>(*p); }
        else if (*p == '\n') o += "\\n";
        else if (*p < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", *p); o += b; }
        else o += static_cast<char>(*p);
    }
    return o + "\"";
}

const char* c(const char* code) { return g_color ? code : ""; }

void status(const char* color, const char* text) {
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fprintf(stderr, "\r\033[K%s%s%s", c(color), text, c("\033[0m"));
    std::fflush(stderr);
}

void print_result(const ptstt_result& r) {
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fprintf(stderr, "\r\033[K");
    if (g_json) {
        std::printf("{\"text\":%s,\"stop_reason\":\"%s\",\"audio_sec\":%.3f,\"speech_sec\":%.3f,\"transcribe_ms\":%.1f}\n",
                    json_quote(r.text ? r.text : "").c_str(), ptstt_stop_reason_name(r.stop_reason), r.audio_sec,
                    r.speech_sec, r.transcribe_ms);
        std::fflush(stdout);
        return;
    }
    if (r.text && *r.text) {
        std::printf("%s%s%s\n", c("\033[1;32m"), r.text, c("\033[0m"));
    } else {
        std::printf("%s(nada reconhecido)%s\n", c("\033[33m"), c("\033[0m"));
    }
    std::fflush(stdout);
    std::fprintf(stderr, "%s  [%s · áudio %.1fs · fala %.1fs · %.0f ms]%s\n", c("\033[2m"),
                 ptstt_stop_reason_name(r.stop_reason), r.audio_sec, r.speech_sec, r.transcribe_ms, c("\033[0m"));
}

void hint() { status("\033[2m", "pressione ESPAÇO para falar · c cancela · q sai"); }

void on_event(const ptstt_event* ev, void*) {
    switch (ev->type) {
        case PTSTT_EV_LISTENING: status("\033[1;31m", "● ouvindo…  (ESPAÇO para parar)"); break;
        case PTSTT_EV_SPEECH_START: status("\033[1;31m", "● fala detectada…  (ESPAÇO para parar)"); break;
        case PTSTT_EV_TRANSCRIBING: status("\033[36m", "◌ transcrevendo…"); break;
        case PTSTT_EV_ERROR: {
            std::lock_guard<std::mutex> lk(g_out_mu);
            std::fprintf(stderr, "\r\033[Kerro: %s\n", ev->error ? ev->error : "?");
            break;
        }
        case PTSTT_EV_RESULT:
            if (ev->result) print_result(*ev->result);
            g_got_result = true;
            if (!g_quit) hint();
            break;
    }
}

// Raw keyboard input for the lifetime of the object.
class RawTerminal {
public:
    RawTerminal() {
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        termios raw = saved_;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        ok_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawTerminal() {
        if (ok_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    bool ok() const { return ok_; }

private:
    termios saved_{};
    bool ok_ = false;
};

void on_signal(int) { g_quit = true; }

bool parse(int argc, char** argv, Options& o) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = nullptr;
        if (a == "-h" || a == "--help") { usage(stdout); std::exit(0); }
        else if (a == "--version") { std::printf("ptstt-cli %s\n", ptstt_version()); std::exit(0); }
        else if (a == "--json") o.json = true;
        else if (a == "--once") o.once = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--list-devices") o.list_devices = true;
        else if (a == "--model") { if (!(v = need(i))) return false; o.model = v; }
        else if (a == "--vad") { if (!(v = need(i))) return false; o.vad = v; }
        else if (a == "--device") { if (!(v = need(i))) return false; o.device = v; }
        else if (a == "--file") { if (!(v = need(i))) return false; o.file = v; }
        else if (a == "--mic-file") { if (!(v = need(i))) return false; o.mic_file = v; }
        else if (a == "--silence") { if (!(v = need(i))) return false; o.silence = std::atof(v); }
        else if (a == "--no-speech") { if (!(v = need(i))) return false; o.no_speech = std::atof(v); }
        else if (a == "--max") { if (!(v = need(i))) return false; o.max = std::atof(v); }
        else if (a == "--threshold") { if (!(v = need(i))) return false; o.threshold = std::atof(v); }
        else if (a == "--decoder") {
            if (!(v = need(i))) return false;
            const std::string d = v;
            if (d == "ctc") o.decoder = PTSTT_DECODER_CTC;
            else if (d == "rnnt" || d == "tdt") o.decoder = PTSTT_DECODER_RNNT;
            else if (d == "default") o.decoder = PTSTT_DECODER_DEFAULT;
            else { std::fprintf(stderr, "error: --decoder must be rnnt or ctc\n"); return false; }
        } else {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", argv[i]);
            return false;
        }
    }
    if (o.silence <= 0 || o.no_speech <= 0 || o.max <= 0 || o.threshold <= 0 || o.threshold > 1) {
        std::fprintf(stderr, "error: --silence, --no-speech and --max must be > 0; --threshold in (0, 1]\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;
    g_json = o.json;

    if (o.list_devices) {
        char* js = ptstt_list_devices_json();
        std::printf("%s\n", js ? js : "[]");
        ptstt_free_string(js);
        return 0;
    }

    if (o.model.empty()) o.model = find_model(kAsrFile, argv[0]);
    if (o.vad.empty()) o.vad = find_model(kVadFile, argv[0]);
    if (o.model.empty() || o.vad.empty()) {
        std::fprintf(stderr, "error: models not found. Run scripts/fetch_models.sh or pass --model and --vad.\n");
        return 2;
    }

    const bool interactive = o.file.empty();
    if (interactive && !isatty(STDIN_FILENO)) {
        std::fprintf(stderr, "error: the keyboard mode needs a terminal (use --file for non-interactive runs)\n");
        return 2;
    }
    g_color = isatty(STDERR_FILENO) && !std::getenv("NO_COLOR");

    ptstt_config cfg;
    ptstt_config_default(&cfg);
    cfg.asr_model = o.model.c_str();
    cfg.vad_model = o.vad.c_str();
    cfg.decoder = o.decoder;
    cfg.vad_threshold = static_cast<float>(o.threshold);
    cfg.silence_timeout_ms = static_cast<int>(o.silence * 1000);
    cfg.no_speech_timeout_ms = static_cast<int>(o.no_speech * 1000);
    cfg.max_utterance_ms = static_cast<int>(o.max * 1000);
    cfg.capture_device = o.device.empty() ? nullptr : o.device.c_str();
    const std::string& input = o.file.empty() ? o.mic_file : o.file;
    cfg.input_file = input.empty() ? nullptr : input.c_str();
    cfg.verbose = o.verbose ? 1 : 0;

    if (interactive) status("\033[2m", "carregando modelos…");
    ptstt_engine* e = ptstt_create(&cfg);
    if (!e) {
        std::fprintf(stderr, "\r\033[Kerror: %s\n", ptstt_last_error(nullptr));
        return 1;
    }

    if (!interactive) {
        char* text = ptstt_transcribe_file(e, o.file.c_str());
        int rc = 0;
        if (!text) {
            std::fprintf(stderr, "error: %s\n", ptstt_last_error(e));
            rc = 1;
        } else if (o.json) {
            std::printf("{\"text\":%s}\n", json_quote(text).c_str());
        } else {
            std::printf("%s\n", text);
        }
        ptstt_free_string(text);
        ptstt_destroy(e);
        return rc;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    ptstt_set_callback(e, on_event, nullptr);

    int rc = 0;
    {
        RawTerminal term;
        if (!term.ok()) {
            std::fprintf(stderr, "\r\033[Kerror: cannot switch the terminal to raw mode\n");
            ptstt_destroy(e);
            return 1;
        }
        hint();
        while (!g_quit) {
            if (o.once && g_got_result) break;
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) != 1) continue;
            if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3 || ch == 4) break;
            if (ch == ' ') {
                const int st = ptstt_state(e);
                if (st == PTSTT_IDLE) {
                    if (ptstt_start(e) != 0) {
                        std::lock_guard<std::mutex> lk(g_out_mu);
                        std::fprintf(stderr, "\r\033[Kerro: %s\n", ptstt_last_error(e));
                        rc = 1;
                    }
                } else if (st == PTSTT_LISTENING) {
                    ptstt_stop(e);
                }
            } else if (ch == 'c' || ch == 'C') {
                ptstt_cancel(e);
            }
        }
        g_quit = true;
        ptstt_set_callback(e, nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lk(g_out_mu);
            std::fprintf(stderr, "\r\033[K");
        }
    }
    ptstt_destroy(e);
    return rc;
}
