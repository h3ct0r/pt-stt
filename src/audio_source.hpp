// Audio sources for the engine: the microphone (miniaudio) or a file.
// Both deliver 16 kHz mono float samples.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ptstt {

constexpr int kSampleRate = 16000;

class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Begin producing audio from a clean state (drops anything buffered).
    virtual bool start(std::string& err) = 0;
    // Stop producing audio. Safe to call when not started.
    virtual void stop() = 0;
    // Copy up to n samples into out. Waits up to timeout_ms for data.
    // Returns the number of samples copied (0 on timeout).
    virtual size_t read(float* out, size_t n, int timeout_ms) = 0;
    // Human-readable name of the device or file.
    virtual std::string name() const = 0;
};

// Opens the capture device whose name contains `device` (case-insensitive),
// or the system default when `device` is null or empty. Returns null on error.
std::unique_ptr<AudioSource> make_mic_source(const char* device, std::string& err);

// Reads a whole audio file; after its end the source produces silence.
std::unique_ptr<AudioSource> make_file_source(const char* path, std::string& err);

// Decodes an audio file (WAV, FLAC, MP3) to 16 kHz mono float.
bool decode_file_16k(const char* path, std::vector<float>& out, std::string& err);

// Capture devices as a JSON array.
std::string list_capture_devices_json();

}  // namespace ptstt
