#include "audio_source.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>

#include "json_util.hpp"
#include "miniaudio.h"

namespace ptstt {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string ma_err(const char* what, ma_result r) {
    return std::string(what) + ": " + ma_result_description(r);
}

// ---------------------------------------------------------------------------
// Microphone
// ---------------------------------------------------------------------------
class MicSource final : public AudioSource {
public:
    ~MicSource() override {
        if (device_ok_) {
            ma_device_uninit(&device_);
        }
        if (rb_ok_) {
            ma_pcm_rb_uninit(&rb_);
        }
        if (context_ok_) {
            ma_context_uninit(&context_);
        }
    }

    bool open(const char* want, std::string& err) {
        ma_result r = ma_context_init(nullptr, 0, nullptr, &context_);
        if (r != MA_SUCCESS) {
            err = ma_err("audio context", r);
            return false;
        }
        context_ok_ = true;

        ma_device_id id{};
        bool have_id = false;
        if (want && *want) {
            ma_device_info* infos = nullptr;
            ma_uint32 count = 0;
            r = ma_context_get_devices(&context_, nullptr, nullptr, &infos, &count);
            if (r != MA_SUCCESS) {
                err = ma_err("list capture devices", r);
                return false;
            }
            const std::string needle = lower(want);
            for (ma_uint32 i = 0; i < count; ++i) {
                if (lower(infos[i].name).find(needle) != std::string::npos) {
                    id = infos[i].id;
                    name_ = infos[i].name;
                    have_id = true;
                    break;
                }
            }
            if (!have_id) {
                err = std::string("no capture device matches '") + want + "' (see --list-devices)";
                return false;
            }
        }

        // 30 s of headroom between the audio thread and the worker.
        r = ma_pcm_rb_init(ma_format_f32, 1, kSampleRate * 30, nullptr, nullptr, &rb_);
        if (r != MA_SUCCESS) {
            err = ma_err("ring buffer", r);
            return false;
        }
        rb_ok_ = true;

        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.pDeviceID = have_id ? &id : nullptr;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 1;
        cfg.sampleRate = kSampleRate;  // miniaudio converts from the device rate
        cfg.dataCallback = &MicSource::on_data;
        cfg.pUserData = this;
        r = ma_device_init(&context_, &cfg, &device_);
        if (r != MA_SUCCESS) {
            err = ma_err("open capture device", r);
            return false;
        }
        device_ok_ = true;
        if (name_.empty()) {
            name_ = device_.capture.name;
        }
        return true;
    }

    bool start(std::string& err) override {
        active_.store(false);
        ma_pcm_rb_reset(&rb_);
        active_.store(true);
        if (ma_device_get_state(&device_) != ma_device_state_started) {
            ma_result r = ma_device_start(&device_);
            if (r != MA_SUCCESS) {
                active_.store(false);
                err = ma_err("start capture device", r);
                return false;
            }
        }
        return true;
    }

    void stop() override {
        active_.store(false);
        if (device_ok_ && ma_device_get_state(&device_) == ma_device_state_started) {
            ma_device_stop(&device_);
        }
    }

    size_t read(float* out, size_t n, int timeout_ms) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t got = 0;
        while (got < n) {
            ma_uint32 frames = static_cast<ma_uint32>(n - got);
            void* p = nullptr;
            if (ma_pcm_rb_acquire_read(&rb_, &frames, &p) != MA_SUCCESS) {
                break;
            }
            if (frames > 0) {
                std::memcpy(out + got, p, frames * sizeof(float));
                got += frames;
            }
            ma_pcm_rb_commit_read(&rb_, frames);
            if (got >= n) {
                break;
            }
            if (frames == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        return got;
    }

    std::string name() const override { return name_; }

private:
    static void on_data(ma_device* dev, void* /*out*/, const void* in, ma_uint32 frames) {
        auto* self = static_cast<MicSource*>(dev->pUserData);
        if (!self->active_.load(std::memory_order_relaxed) || in == nullptr) {
            return;
        }
        const float* src = static_cast<const float*>(in);
        while (frames > 0) {
            ma_uint32 n = frames;
            void* p = nullptr;
            if (ma_pcm_rb_acquire_write(&self->rb_, &n, &p) != MA_SUCCESS || n == 0) {
                return;  // buffer full: drop (the worker is far behind)
            }
            std::memcpy(p, src, n * sizeof(float));
            ma_pcm_rb_commit_write(&self->rb_, n);
            src += n;
            frames -= n;
        }
    }

    ma_context context_{};
    ma_device device_{};
    ma_pcm_rb rb_{};
    bool context_ok_ = false;
    bool device_ok_ = false;
    bool rb_ok_ = false;
    std::atomic<bool> active_{false};
    std::string name_;
};

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------
class FileSource final : public AudioSource {
public:
    FileSource(std::vector<float> pcm, std::string name) : pcm_(std::move(pcm)), name_(std::move(name)) {}

    bool start(std::string&) override {
        pos_ = 0;
        return true;
    }
    void stop() override {}

    size_t read(float* out, size_t n, int) override {
        const size_t take = std::min(n, pcm_.size() - std::min(pos_, pcm_.size()));
        if (take > 0) {
            std::memcpy(out, pcm_.data() + pos_, take * sizeof(float));
            pos_ += take;
        }
        std::fill(out + take, out + n, 0.0f);  // silence after the end of the file
        return n;
    }

    std::string name() const override { return name_; }

private:
    std::vector<float> pcm_;
    size_t pos_ = 0;
    std::string name_;
};

}  // namespace

bool decode_file_16k(const char* path, std::vector<float>& out, std::string& err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, kSampleRate);
    ma_decoder dec;
    ma_result r = ma_decoder_init_file(path, &cfg, &dec);
    if (r != MA_SUCCESS) {
        err = std::string("cannot read audio file '") + path + "': " + ma_result_description(r);
        return false;
    }
    out.clear();
    float buf[4096];
    for (;;) {
        ma_uint64 got = 0;
        r = ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
        out.insert(out.end(), buf, buf + got);
        if (r != MA_SUCCESS || got == 0) {
            break;
        }
    }
    ma_decoder_uninit(&dec);
    return true;
}

std::unique_ptr<AudioSource> make_mic_source(const char* device, std::string& err) {
    auto mic = std::make_unique<MicSource>();
    if (!mic->open(device, err)) {
        return nullptr;
    }
    return mic;
}

std::unique_ptr<AudioSource> make_file_source(const char* path, std::string& err) {
    std::vector<float> pcm;
    if (!decode_file_16k(path, pcm, err)) {
        return nullptr;
    }
    return std::make_unique<FileSource>(std::move(pcm), path);
}

std::string list_capture_devices_json() {
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
        return "[]";
    }
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    std::string js = "[";
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &infos, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (i) js += ",";
            js += "{\"index\":" + std::to_string(i) + ",\"name\":" + json_quote(infos[i].name) +
                  ",\"default\":" + (infos[i].isDefault ? "true" : "false") + "}";
        }
    }
    js += "]";
    ma_context_uninit(&ctx);
    return js;
}

}  // namespace ptstt
