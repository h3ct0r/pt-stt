/*
 * ptstt.h - push-to-talk Portuguese speech-to-text (C API)
 *
 * One engine owns the speech model, the voice activity detector (VAD) and the
 * microphone. A listen session starts with ptstt_start() and ends when:
 *   - the caller calls ptstt_stop()                       (PTSTT_STOP_MANUAL)
 *   - speech started and then silence_timeout_ms passed   (PTSTT_STOP_SILENCE)
 *   - no speech started within no_speech_timeout_ms        (PTSTT_STOP_NO_SPEECH)
 *   - the session reached max_utterance_ms                 (PTSTT_STOP_MAX_DURATION)
 *   - the caller called ptstt_cancel()                     (PTSTT_STOP_CANCELLED)
 * The speech part of the audio is then transcribed and delivered as a result,
 * through the event callback and/or ptstt_wait_result().
 *
 * Threading: all functions are thread-safe. Events are delivered on the
 * engine's worker thread; keep the callback short. Do not call
 * ptstt_destroy() from inside the callback.
 *
 * Strings returned as `char*` are owned by the caller: free them with
 * ptstt_free_string(). Strings returned as `const char*` are owned by the
 * library.
 */
#ifndef PTSTT_H
#define PTSTT_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  ifdef PTSTT_BUILDING_LIBRARY
#    define PTSTT_API __declspec(dllexport)
#  else
#    define PTSTT_API __declspec(dllimport)
#  endif
#else
#  define PTSTT_API __attribute__((visibility("default")))
#endif

#define PTSTT_ABI_VERSION 1

typedef struct ptstt_engine ptstt_engine;

/* Engine state (ptstt_state). */
enum {
    PTSTT_IDLE         = 0,
    PTSTT_LISTENING    = 1,
    PTSTT_TRANSCRIBING = 2
};

/* Why a listen session ended (ptstt_result.stop_reason). */
enum {
    PTSTT_STOP_MANUAL       = 0,
    PTSTT_STOP_SILENCE      = 1,
    PTSTT_STOP_NO_SPEECH    = 2,
    PTSTT_STOP_MAX_DURATION = 3,
    PTSTT_STOP_CANCELLED    = 4,
    PTSTT_STOP_ERROR        = 5
};

/* Event types (ptstt_event.type). */
enum {
    PTSTT_EV_LISTENING    = 0, /* session started, mic is open             */
    PTSTT_EV_SPEECH_START = 1, /* VAD detected the start of speech          */
    PTSTT_EV_TRANSCRIBING = 2, /* session ended (stop_reason), ASR running  */
    PTSTT_EV_RESULT       = 3, /* result is ready (result != NULL)          */
    PTSTT_EV_ERROR        = 4  /* error (error != NULL); a RESULT follows   */
};

/* Decoder head of the hybrid model. */
enum {
    PTSTT_DECODER_DEFAULT = 0, /* transducer (RNN-T) */
    PTSTT_DECODER_CTC     = 1,
    PTSTT_DECODER_RNNT    = 2
};

typedef struct {
    int         size;                 /* sizeof(ptstt_config); set by ptstt_config_default */
    const char *asr_model;            /* path to the speech-to-text GGUF (required)       */
    const char *vad_model;            /* path to the Silero VAD GGUF (required)           */
    int         decoder;              /* PTSTT_DECODER_*, default PTSTT_DECODER_DEFAULT   */
    float       vad_threshold;        /* speech when VAD probability >= this; 0.5         */
    int         silence_timeout_ms;   /* stop after this much silence once speech began; 2000 */
    int         no_speech_timeout_ms; /* stop if no speech starts within this; 5000       */
    int         max_utterance_ms;     /* hard limit per session; 30000                    */
    int         pad_ms;               /* audio kept before/after the speech; 300          */
    const char *capture_device;       /* NULL = system default; else substring of a device name */
    const char *input_file;           /* NULL = microphone; else read this audio file instead
                                         of the mic (tests, offline runs). After the file ends,
                                         the source produces silence. */
    int         verbose;              /* 0 = hide model/GPU backend logs (default); 1 = show them */
} ptstt_config;

typedef struct {
    char *text;            /* UTF-8 transcript, "" when nothing was said; owned by the result */
    int   stop_reason;     /* PTSTT_STOP_*                                                     */
    float audio_sec;       /* length of the whole session                                      */
    float speech_sec;      /* length of the audio sent to the model (0 = model not run)        */
    float transcribe_ms;   /* time spent in the model                                          */
} ptstt_result;

typedef struct {
    int                 type;        /* PTSTT_EV_*                                     */
    int                 stop_reason; /* for TRANSCRIBING and RESULT                    */
    const ptstt_result *result;      /* for RESULT; valid only during the callback     */
    const char         *error;       /* for ERROR; valid only during the callback      */
} ptstt_event;

typedef void (*ptstt_event_cb)(const ptstt_event *event, void *user_data);

/* Version of this ABI (PTSTT_ABI_VERSION of the library) and the library version string. */
PTSTT_API int         ptstt_abi_version(void);
PTSTT_API const char *ptstt_version(void);

/* Fill `config` with the defaults above (models left NULL). */
PTSTT_API void ptstt_config_default(ptstt_config *config);

/* Load both models and open the audio device. NULL on failure: see
 * ptstt_last_error(NULL). The config strings are copied. */
PTSTT_API ptstt_engine *ptstt_create(const ptstt_config *config);

/* Free the engine. Cancels a running session first. Safe on NULL. */
PTSTT_API void ptstt_destroy(ptstt_engine *engine);

/* Last error message of `engine`, or of the last failed ptstt_create on this
 * thread when `engine` is NULL. Never NULL ("" when there is no error). */
PTSTT_API const char *ptstt_last_error(ptstt_engine *engine);

/* Install (or with cb = NULL remove) the event callback. */
PTSTT_API void ptstt_set_callback(ptstt_engine *engine, ptstt_event_cb cb, void *user_data);

/* Start a listen session. Clears results not yet taken by ptstt_wait_result.
 * 0 on success, -1 if busy (not IDLE) or on error. */
PTSTT_API int ptstt_start(ptstt_engine *engine);

/* End the session now and transcribe it (PTSTT_STOP_MANUAL).
 * 0 on success, -1 if not listening. */
PTSTT_API int ptstt_stop(ptstt_engine *engine);

/* End the session and drop the audio (PTSTT_STOP_CANCELLED, empty text).
 * 0 on success, -1 if not listening. */
PTSTT_API int ptstt_cancel(ptstt_engine *engine);

/* Current PTSTT_IDLE / PTSTT_LISTENING / PTSTT_TRANSCRIBING. */
PTSTT_API int ptstt_state(ptstt_engine *engine);

/* Block until the next result of a listen session is ready, then copy it into
 * `out` (release with ptstt_result_clear). timeout_ms < 0 waits forever.
 * Returns 0 on success, 1 on timeout, -1 on error. */
PTSTT_API int ptstt_wait_result(ptstt_engine *engine, int timeout_ms, ptstt_result *out);

/* Free the text inside `result` and zero it. Safe on a zeroed result. */
PTSTT_API void ptstt_result_clear(ptstt_result *result);

/* Transcribe audio directly, without the microphone or VAD.
 * Mono float PCM in [-1, 1] at any sample rate (resampled to 16 kHz). */
PTSTT_API char *ptstt_transcribe_pcm(ptstt_engine *engine, const float *pcm, int n_samples, int sample_rate);

/* Transcribe an audio file (WAV, FLAC or MP3; any rate and channel count). */
PTSTT_API char *ptstt_transcribe_file(ptstt_engine *engine, const char *path);

/* Capture devices as JSON: [{"index":0,"name":"...","default":true}, ...]. */
PTSTT_API char *ptstt_list_devices_json(void);

/* Free a string returned by this library. Safe on NULL. */
PTSTT_API void ptstt_free_string(char *s);

/* Name of a stop reason ("manual", "silence", ...). Never NULL. */
PTSTT_API const char *ptstt_stop_reason_name(int stop_reason);

#ifdef __cplusplus
}
#endif

#endif /* PTSTT_H */
