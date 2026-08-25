#ifndef SCRCPY_FFI_INTERNAL_H
#define SCRCPY_FFI_INTERNAL_H

#include "scrcpy_ffi.h"

// scrcpy core headers
#include "../../app/src/adb/adb.h"
#include "../../app/src/controller.h"
#include "../../app/src/control_msg.h"
#include "../../app/src/decoder.h"
#include "../../app/src/demuxer.h"
#include "../../app/src/device_msg.h"
#include "../../app/src/options.h"
#include "../../app/src/server.h"
#include "../../app/src/trait/frame_sink.h"
#include "../../app/src/trait/frame_source.h"
#include "../../app/src/trait/packet_source.h"
#include "../../app/src/util/log.h"
#include "../../app/src/util/net.h"
#include "../../app/src/util/rand.h"
#include "../../app/src/cli.h"
#include "../../app/src/audio_player.h"

#include "config.h"
#include "../../app/src/util/process.h"
#include "../../app/src/util/intr.h"

// FFmpeg
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>

#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <sys/stat.h>
#include <limits.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include "../../app/src/util/thread.h"

#ifdef _MSC_VER
#define thread_local __declspec(thread)
#else
#define thread_local __thread
#endif

#ifdef _WIN32
#include <d3d11.h>
#include <libavutil/hwcontext_d3d11va.h>
#endif

// Forward declaration
typedef struct ScrcpyFfiInstance ScrcpyFfiInstance;

/** Per-codec attachment owned by the FFI layer, stored in AVCodecContext::opaque. */
typedef struct {
    ScrcpyFfiInstance* instance;
    uint64_t applied_decode_generation;
    enum AVPixelFormat expected_hw_format;
} ScrcpyCodecBinding;

/**
 * @brief Custom frame sink structure for decoding and passing YUV video frames.
 */
typedef struct {
    struct sc_frame_sink base;   /**< Base frame sink required by scrcpy core */
    ScrcpyFfiInstance*   state;  /**< Pointer back to the owning scrcpy FFI session state */
} FfiVideoSink;

/**
 * @brief Custom frame sink structure for audio stream decoding, resampling and PCM routing.
 */
typedef struct {
    struct sc_frame_sink base;                      /**< Base frame sink required by scrcpy core */
    ScrcpyFfiInstance*   state;                     /**< Pointer back to the owning scrcpy FFI session state */
    struct SwrContext*   swr_ctx;                   /**< FFmpeg audio resampler context */
    uint8_t*             swr_buf;                   /**< Destination buffer for resampled S16 PCM audio samples */
    int                  swr_buf_allocated_samples; /**< Current allocated size of the resampling buffer in samples */
    SDL_AudioStream*     playback_stream;           /**< Local SDL3 playback stream, owned by this sink */
    int                  playback_sample_rate;      /**< PCM rate accepted by playback_stream */
    int                  playback_channels;         /**< PCM channels accepted by playback_stream */
    uint64_t             applied_decode_generation; /**< Flushes resampler on pause/resume phase changes */
} FfiAudioSink;

/**
 * @brief Thread safe session instance holding all active components for one scrcpy mirror session.
 */
struct ScrcpyFfiInstance {
    struct sc_server      server;             /**< Handles connection/handshake with scrcpy-server on Android */
    struct sc_demuxer     video_demuxer;      /**< Demuxes H264/H265 packets from video socket */
    struct sc_demuxer     audio_demuxer;      /**< Demuxes Opus/AAC PCM packets from audio socket */
    struct sc_decoder     video_decoder;      /**< Decodes video packets using FFmpeg libavcodec */
    struct sc_decoder     audio_decoder;      /**< Decodes audio packets using FFmpeg libavcodec */
    struct sc_controller  controller;         /**< Controls input injection socket from PC to device */

    // Latest received AVFrame
    AVFrame*        current_frame;            /**< Decoded video frame buffer */
    int32_t         frame_width;              /**< Tracked video frame width */
    int32_t         frame_height;             /**< Tracked video frame height */
    uint64_t        frame_serial;             /**< Monotonic identifier for latest frame */
    sc_mutex        buffer_mutex;             /**< Protects access to current_frame and size metrics */

    // Dart callbacks
    on_video_frame_ready_cb  video_cb;        /**< Called when a new YUV video frame is copied */
    on_audio_pcm_ready_cb    audio_cb;        /**< Called when a new chunk of resampled PCM is ready */
    on_connection_state_cb   state_cb;        /**< Called when device connection state transitions */
    on_clipboard_changed_cb  clipboard_cb;    /**< Called when the device's clipboard changes */

    // Config & Lifecycle flags
    struct scrcpy_options options;            /**< Parsed command-line style option structure */
    atomic_bool running;                      /**< True if the connection thread loop is currently running */
    atomic_bool stop_called;                  /**< Safety flag to avoid duplicate calls to stop APIs */
    atomic_int connection_state;              /**< Latest SCRCPY_STATE_* value, emitted to Dart port */
    atomic_int_fast64_t state_port;           /**< Dart ReceivePort id for state events, 0 when detached */
    atomic_bool decode_paused;                /**< Local CPU/GPU media decode is suspended */
    atomic_bool await_video_keyframe;         /**< Drop video packets until post-resume IDR/key frame */
    atomic_uint_fast64_t decode_generation;   /**< Forces each decoder to flush on phase changes */

    bool server_started;                      /**< Flag indicating the server connection manager started */
    bool controller_started;                  /**< Flag indicating input injection loop is active */
    bool video_demuxer_started;               /**< Flag indicating video demuxing loop started */
    bool audio_demuxer_started;               /**< Flag indicating audio demuxing loop started */
    bool audio_player_started;                /**< Flag indicating audio playing (unused) */
    
    bool software_decoding;                   /**< If true, skips hardware decoder setup completely */
    void* software_pixel_buffer;              /**< Cached CVPixelBufferRef for CPU zero-copy fallback */

    sc_thread    worker_thread;               /**< Handle to the background worker thread managing the scrcpy life cycle */

    sc_mutex mutex;                           /**< Synchronization mutex for handshake signaling */
    sc_cond cond;                             /**< Condition variable for connection handshake state updates */
    bool server_connected;                    /**< True if socket handshake with scrcpy-server succeeded */
    bool server_connection_failed;            /**< True if connection failed early */

    FfiVideoSink video_sink;                  /**< Managed video decoder frame destination */
    FfiAudioSink audio_sink;                  /**< Managed audio decoder frame destination */
    struct sc_audio_player audio_player;      /**< Native audio player context (unused) */
    void* d3d11_device;                       /**< Owned ID3D11Device reference from Flutter Windows */
    atomic_bool gpu_context_ready;            /**< Windows device has been bound before decode */
};

/**
 * @brief Thread Wrapper Data structure to pass dynamic instance TLS handles down to newly created SDL threads.
 */
typedef struct {
    SDL_ThreadFunction original_fn;       /**< Main target function to execute in the thread */
    void* original_userdata;             /**< Arguments payload to send to target function */
    void* instance_handle;               /**< TLS scrcpy instance handle to bind to target thread */
} ThreadWrapperData;

// Thread Local Storage
extern thread_local void* t_instance_handle;

// Global Instances Tracking for Hot Restart Safety & Unlimited Concurrency
typedef struct ScrcpyInstanceNode {
    ScrcpyFfiInstance* instance;
    struct ScrcpyInstanceNode* next;
} ScrcpyInstanceNode;

extern ScrcpyInstanceNode* g_instances_head;

// Extern functions and objects
extern const struct sc_frame_sink_ops kVideoSinkOps;
extern const struct sc_frame_sink_ops kAudioSinkOps;

// Local playback is SDL3-backed: CoreAudio on macOS, WASAPI on Windows and
// the configured PipeWire/PulseAudio/ALSA backend on Linux. No PCM crosses the
// Dart isolate unless the caller explicitly opts into the legacy callback.
// This is called on the FFI start thread, before scrcpy creates decoder
// workers. SDL subsystem setup is global and deliberately stays alive for the
// Flutter process so concurrent sessions cannot tear down each other's audio.
extern bool scrcpy_ffi_prepare_audio_output(void);
extern void scrcpy_ffi_audio_output_pause(ScrcpyFfiInstance* s);
extern void scrcpy_ffi_audio_output_resume(ScrcpyFfiInstance* s);

extern void remove_instance_from_global(ScrcpyFfiInstance* s);
extern int SDLCALL thread_wrapper_fn(void* data);
extern SDL_Thread* sc_SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);

// FFmpeg overrides configured only for decoder.c/demuxer.c by CMake.
extern int sc_avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt);
extern int sc_avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame);
extern void sc_avcodec_free_context(AVCodecContext **avctx);

// ADB & Process Overrides (called by server.c / intr.c via macro redefinitions)
extern bool scrcpy_ffi_adb_init(void);
extern void scrcpy_ffi_adb_destroy(void);
extern bool scrcpy_ffi_adb_push(struct sc_intr *intr, const char *serial, const char *local, const char *remote, unsigned flags);
extern sc_pid scrcpy_ffi_adb_execute(const char *const argv[], unsigned flags);
extern bool scrcpy_ffi_adb_start_server(struct sc_intr *intr, unsigned flags);
extern bool scrcpy_ffi_process_terminate(sc_pid pid);
extern bool scrcpy_ffi_adb_select_device(struct sc_intr *intr,
                                          const struct sc_adb_device_selector *selector,
                                          unsigned flags,
                                          struct sc_adb_device *out_device);

// Kill zombie scrcpy-server on Android to prevent "Address already in use"
extern void scrcpy_ffi_kill_zombie_server(const char *serial);

// SDL Clipboard overrides
extern bool sc_SDL_IsMainThread(void);
extern int sc_SDL_SetClipboardText(const char *text);
// Log configuration
extern void sc_log_configure(void);

extern sc_exit_code scrcpy_ffi_process_wait(sc_pid pid, bool close);
extern enum sc_process_result scrcpy_ffi_process_execute_p(const char *const argv[], sc_pid *pid, unsigned flags,
                                                           sc_pipe *pin, sc_pipe *pout, sc_pipe *perr);

#endif // SCRCPY_FFI_INTERNAL_H
