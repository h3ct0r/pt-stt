// Minimal JSON helpers: quoting strings, and reading the VAD stream documents
// produced by parakeet_capi_vad_stream_feed_json.
#pragma once

#include <string>
#include <vector>

namespace ptstt {

// Returns `s` as a quoted JSON string.
std::string json_quote(const std::string& s);

struct VadEvent {
    bool start;   // true = speech start, false = speech end
    double time;  // seconds from the start of the stream
};

struct VadFeed {
    double frame_sec = 0.032;
    long first_frame = 0;
    std::vector<VadEvent> events;
    std::vector<float> probabilities;
};

// Parses {"frame_sec":..,"first_frame":..,"events":[{"type":"start","time":..}],"probabilities":[..]}.
// Returns false on malformed input.
bool parse_vad_feed(const char* json, VadFeed& out);

}  // namespace ptstt
