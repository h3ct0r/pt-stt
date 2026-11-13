// Minimal C++ use of pt-stt: listen three times and print what was said.
//
//   ./build/bin/ptstt-example-listen models/pt-fastconformer-large.f32.gguf models/silero-vad-f32.gguf
#include <iostream>

#include "ptstt.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " ASR_MODEL.gguf VAD_MODEL.gguf [input.wav]\n";
        return 2;
    }
    ptstt::Config cfg;
    cfg.asr_model = argv[1];
    cfg.vad_model = argv[2];
    if (argc > 3) cfg.input_file = argv[3];  // optional: use a file instead of the microphone

    try {
        ptstt::Engine stt(cfg);
        stt.on_event([](const ptstt::Event& ev) {
            if (ev.type == ptstt::EventType::SpeechStart) std::cerr << "  (speech detected)\n";
        });
        for (int i = 0; i < 3; ++i) {
            std::cerr << "Speak now (" << i + 1 << "/3)...\n";
            ptstt::Result r = stt.listen();  // ends on 2 s of silence after speech
            std::cout << (r.text.empty() ? "(nothing)" : r.text) << "  [" << r.reason_name() << "]\n";
        }
    } catch (const ptstt::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
