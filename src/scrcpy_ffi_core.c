#include "scrcpy_ffi_internal.h"

#include <dart_api_dl.h>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif
/**
 * @brief Thread Local Storage handle to track the current scrcpy instance bound to the executing thread.
 * This is crucial for SDL overrides that lack custom user data parameters.
 */
thread_local void* t_instance_handle = NULL;
// avcodec_open2() invokes get_format synchronously on the decoder thread.
// Keep the selected format in TLS rather than changing scrcpy's decoder API.
static thread_local enum AVPixelFormat t_expected_hw_format = AV_PIX_FMT_NONE;

/**
 * @brief Global collection of active scrcpy instances. Used to prevent callback leaks during Hot Restart.
 */
ScrcpyInstanceNode* g_instances_head = NULL;
static atomic_bool g_dart_api_initialized = false;

static void
scrcpy_ffi_publish_state(ScrcpyFfiInstance *s, int32_t state) {
    atomic_store_explicit(&s->connection_state, state, memory_order_release);

    if (!atomic_load_explicit(&g_dart_api_initialized, memory_order_acquire)) {
        return;
    }

    int64_t port = atomic_load_explicit(&s->state_port, memory_order_acquire);
    if (port) {
        Dart_PostInteger_DL((Dart_Port_DL)port, state);
    }
}

#ifdef _WIN32
#include <windows.h>
static SRWLOCK g_instances_lock = SRWLOCK_INIT;
#define LOCK_GLOBAL() AcquireSRWLockExclusive(&g_instances_lock)
#define UNLOCK_GLOBAL() ReleaseSRWLockExclusive(&g_instances_lock)

static void
scrcpy_ffi_release_d3d11_device(ScrcpyFfiInstance *s) {
    ID3D11Device *device = (ID3D11Device *)s->d3d11_device;
    if (device) {
        s->d3d11_device = NULL;
        device->lpVtbl->Release(device);
    }
}
#else
#include <pthread.h>
static pthread_mutex_t g_instances_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_GLOBAL() pthread_mutex_lock(&g_instances_mutex)
#define UNLOCK_GLOBAL() pthread_mutex_unlock(&g_instances_mutex)
#endif

/**
 * @brief Version print stub. Required by scrcpy dependencies.
 */
void scrcpy_print_version(void) {}

FFI_EXPORT bool
ffi_scrcpy_set_adb_path(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

#ifdef _WIN32
    return SetEnvironmentVariableA("ADB", path);
#else
    return setenv("ADB", path, 1) == 0;
#endif
}

/**
 * @brief Dispatches the callback run context on the main GUI thread.
 * For our headless/plugin usage, we execute it inline immediately.
 */
bool sc_run_on_main_thread(void (*run)(void *userdata), void *userdata, bool wait_complete) {
    (void)wait_complete;
    run(userdata);
    return true;
}

/**
 * @brief Thread bootstrap helper. Unwraps thread payloads and binds FFI instance handles to TLS.
 */
int SDLCALL thread_wrapper_fn(void* data) {
    ThreadWrapperData* wrapper_data = (ThreadWrapperData*)data;
    SDL_ThreadFunction original_fn = wrapper_data->original_fn;
    void* original_userdata = wrapper_data->original_userdata;
    
    t_instance_handle = wrapper_data->instance_handle;
    
    free(wrapper_data);
    return original_fn(original_userdata);
}

/**
 * @brief Overrides SDL thread creation to propagate the dynamic TLS handles to all children threads.
 */
SDL_Thread* sc_SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    ThreadWrapperData* wrapper_data = malloc(sizeof(ThreadWrapperData));
    if (!wrapper_data) {
        return SDL_CreateThread(fn, name, data);
    }
    wrapper_data->original_fn = fn;
    wrapper_data->original_userdata = data;
    wrapper_data->instance_handle = t_instance_handle;
    
    SDL_Thread* thread = SDL_CreateThread(thread_wrapper_fn, name, wrapper_data);
    if (!thread) {
        free(wrapper_data);
    }
    return thread;
}

/**
 * @brief Safely removes an instance pointer from the global hot restart tracking list.
 */
void remove_instance_from_global(ScrcpyFfiInstance* s) {
    if (!s) return;
    LOCK_GLOBAL();
    ScrcpyInstanceNode** curr = &g_instances_head;
    while (*curr) {
        if ((*curr)->instance == s) {
            ScrcpyInstanceNode* to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            break;
        }
        curr = &(*curr)->next;
    }
    UNLOCK_GLOBAL();
}

/**
 * @brief Callback triggered when the server connection handshake fails.
 */
static void on_server_connection_failed(struct sc_server* server, void* ud) {
    (void)server;
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)ud;

    if (!atomic_load(&s->running)) {
        LOGD("FFI: Server connection interrupted by user stop");
        sc_mutex_lock(&s->mutex);
        s->server_connection_failed = true;
        sc_cond_signal(&s->cond);
        sc_mutex_unlock(&s->mutex);
        return;
    }

    LOGE("FFI: Server connection failed");

    sc_mutex_lock(&s->mutex);
    s->server_connection_failed = true;
    sc_cond_signal(&s->cond);
    sc_mutex_unlock(&s->mutex);

    scrcpy_ffi_publish_state(s, SCRCPY_STATE_ERROR);
    atomic_store(&s->running, false);
}

/**
 * @brief Callback triggered when socket connection succeeds.
 */
static void on_server_connected(struct sc_server* server, void* ud) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)ud;
    const char* device_name = server->info.device_name;
    LOGD("FFI: Server connected: %s", device_name ? device_name : "(unknown)");

    sc_mutex_lock(&s->mutex);
    s->server_connected = true;
    sc_cond_signal(&s->cond);
    sc_mutex_unlock(&s->mutex);
}

/**
 * @brief Callback triggered when device disconnects.
 */
static void on_server_disconnected(struct sc_server* server, void* ud) {
    (void)server;
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)ud;
    LOGD("FFI: Server disconnected");
    atomic_store(&s->running, false);
    scrcpy_ffi_publish_state(s, SCRCPY_STATE_DISCONNECTED);
}

/**
 * @brief Callback triggered when the video demuxer thread ends.
 */
static void on_video_demuxer_ended(struct sc_demuxer* d, enum sc_demuxer_status status, void* ud) {
    (void)d; (void)status;
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)ud;
    LOGD("FFI: Video demuxer ended");
    atomic_store(&s->running, false);
}

/**
 * @brief Callback triggered when the audio demuxer thread ends.
 */
static void on_audio_demuxer_ended(struct sc_demuxer* d, enum sc_demuxer_status status, void* ud) {
    (void)d; (void)status; (void)ud;
    LOGD("FFI: Audio demuxer ended");
}

/**
 * @brief Callback triggered when the input injection controller loop ends.
 */
static void on_controller_ended(struct sc_controller* c, bool error, void* ud) {
    (void)c; (void)error; (void)ud;
    LOGD("FFI: Controller ended");
}

/**
 * @brief Worker thread entrypoint managing socket life cycle, frame decoding pipeline and control synchronization.
 */
static int scrcpy_worker_thread(void* arg) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)arg;
    t_instance_handle = s;

#ifdef _WIN32
    // The Flutter texture registrar owns the D3D11 device. Do not start the
    // server/decoder until Dart has bound that device through the plugin.
    sc_mutex_lock(&s->mutex);
    while (atomic_load(&s->running)
            && !atomic_load(&s->gpu_context_ready)) {
        sc_cond_wait(&s->cond, &s->mutex);
    }
    sc_mutex_unlock(&s->mutex);
    if (!atomic_load(&s->running)) {
        return 0;
    }
#endif

    scrcpy_ffi_publish_state(s, SCRCPY_STATE_CONNECTING);

    static const struct sc_server_callbacks server_cbs = {
        .on_connection_failed = on_server_connection_failed,
        .on_connected         = on_server_connected,
        .on_disconnected      = on_server_disconnected,
    };

    // Kết hợp time + PID + monotonic clock để seed luôn unique ngay cả khi
    // nhiều tiến trình scrcpy khởi động trong cùng 1 giây (tránh collision socket name).
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned int seed = (unsigned int)ts.tv_sec
                      ^ (unsigned int)(ts.tv_nsec)
                      ^ (unsigned int)getpid();
    uint32_t scid = ((uint32_t)rand_r(&seed)) & 0x7FFFFFFF;

    // Force the Java server to use WARN if the requested level is INFO, to suppress [server] INFO logs
    enum sc_log_level server_log_level = s->options.log_level == SC_LOG_LEVEL_INFO ? SC_LOG_LEVEL_WARN : s->options.log_level;

    struct sc_server_params params = {
        .scid = scid,
        .req_serial = s->options.serial,
        .select_usb = s->options.select_usb,
        .select_tcpip = s->options.select_tcpip,
        .log_level = server_log_level,
        .video_codec = s->options.video_codec,
        .audio_codec = s->options.audio_codec,
        .video_source = s->options.video_source,
        .audio_source = s->options.audio_source,
        .camera_facing = s->options.camera_facing,
        .crop = s->options.crop,
        .port_range = s->options.port_range,
        .tunnel_host = s->options.tunnel_host,
        .tunnel_port = s->options.tunnel_port,
        .min_size_alignment = s->options.min_size_alignment,
        .max_size = s->options.max_size,
        .video_bit_rate = s->options.video_bit_rate,
        .audio_bit_rate = s->options.audio_bit_rate,
        .max_fps = s->options.max_fps,
        .angle = s->options.angle,
        .screen_off_timeout = s->options.screen_off_timeout,
        .capture_orientation = s->options.capture_orientation,
        .capture_orientation_lock = s->options.capture_orientation_lock,
        .control = s->options.control,
        .display_id = s->options.display_id,
        .new_display = s->options.new_display,
        .display_ime_policy = s->options.display_ime_policy,
        .video = s->options.video,
        .audio = s->options.audio,
        .audio_dup = s->options.audio_dup,
        .show_touches = s->options.show_touches,
        .stay_awake = s->options.stay_awake,
        .video_codec_options = s->options.video_codec_options,
        .audio_codec_options = s->options.audio_codec_options,
        .video_encoder = s->options.video_encoder,
        .audio_encoder = s->options.audio_encoder,
        .camera_id = s->options.camera_id,
        .camera_size = s->options.camera_size,
        .camera_ar = s->options.camera_ar,
        .camera_fps = s->options.camera_fps,
        .force_adb_forward = s->options.force_adb_forward,
        .power_off_on_close = s->options.power_off_on_close,
        .clipboard_autosync = s->options.clipboard_autosync,
        .downsize_on_error = s->options.downsize_on_error,
        .tcpip = s->options.tcpip,
        .tcpip_dst = s->options.tcpip_dst,
        .cleanup = s->options.cleanup,
        .power_on = s->options.power_on,
        .kill_adb_on_close = s->options.kill_adb_on_close,
        .camera_high_speed = s->options.camera_high_speed,
        .camera_torch = s->options.camera_torch,
        .camera_zoom = s->options.camera_zoom,
        .vd_destroy_content = s->options.vd_destroy_content,
        .vd_system_decorations = s->options.vd_system_decorations,
        .keep_active = s->options.keep_active,
        .flex_display = s->options.flex_display,
        .ignore_video_encoder_constraints = s->options.ignore_video_encoder_constraints,
        .list = s->options.list,
    };

    if (!sc_server_init(&s->server, &params, &server_cbs, s)) {
        LOGE("FFI: Failed to init sc_server");
        scrcpy_ffi_publish_state(s, SCRCPY_STATE_ERROR);
        atomic_store(&s->running, false);
        return 0;
    }

    // sc_adb_init() đã được gọi bên trong sc_server_init() ở đây.
    // adb_executable hợp lệ từ điểm này → an toàn để kill zombie server.
    scrcpy_ffi_kill_zombie_server(s->options.serial);

    if (!sc_server_start(&s->server)) {
        LOGE("FFI: Failed to start sc_server");
        scrcpy_ffi_publish_state(s, SCRCPY_STATE_ERROR);
        sc_server_destroy(&s->server);
        atomic_store(&s->running, false);
        return 0;
    }
    s->server_started = true;

    sc_mutex_lock(&s->mutex);
    while (atomic_load(&s->running) && !s->server_connected && !s->server_connection_failed) {
        sc_cond_wait(&s->cond, &s->mutex);
    }
    bool connected = s->server_connected;
    sc_mutex_unlock(&s->mutex);

    if (!connected || !atomic_load(&s->running)) {
        if (!atomic_load(&s->running)) {
            LOGD("FFI: Server connection aborted by user stop");
        } else {
            LOGE("FFI: Server failed to connect");
            scrcpy_ffi_publish_state(s, SCRCPY_STATE_ERROR);
        }
        goto cleanup_server;
    }

    scrcpy_ffi_publish_state(s, SCRCPY_STATE_CONNECTED);

    static const struct sc_demuxer_callbacks video_demuxer_cbs = {
        .on_ended = on_video_demuxer_ended,
    };
    sc_demuxer_init(&s->video_demuxer, "video",
                    s->server.video_socket, &video_demuxer_cbs, s);

    if (s->options.audio) {
        static const struct sc_demuxer_callbacks audio_demuxer_cbs = {
            .on_ended = on_audio_demuxer_ended,
        };
        sc_demuxer_init(&s->audio_demuxer, "audio",
                        s->server.audio_socket, &audio_demuxer_cbs, s);
    }

    sc_decoder_init(&s->video_decoder, "video");
    sc_packet_source_add_sink(&s->video_demuxer.packet_source,
                              &s->video_decoder.packet_sink);

    if (s->options.audio) {
        sc_decoder_init(&s->audio_decoder, "audio");
        sc_packet_source_add_sink(&s->audio_demuxer.packet_source,
                                  &s->audio_decoder.packet_sink);
    }

    s->video_sink.base.ops = &kVideoSinkOps;
    s->video_sink.state    = s;
    sc_frame_source_add_sink(&s->video_decoder.frame_source,
                             &s->video_sink.base);

    if (s->options.audio) {
        // Native SDL playback is the default path. Do not make the audio sink
        // depend on a Dart PCM callback: that callback is optional diagnostics
        // only and deliberately disabled for realtime playback by default.
        s->audio_sink.base.ops = &kAudioSinkOps;
        s->audio_sink.state    = s;
        sc_frame_source_add_sink(&s->audio_decoder.frame_source,
                                 &s->audio_sink.base);
    }

    if (s->options.control) {
        static const struct sc_controller_callbacks ctrl_cbs = {
            .on_ended = on_controller_ended,
        };
        if (sc_controller_init(&s->controller, s->server.control_socket,
                               &ctrl_cbs, s)) {
            sc_controller_configure(&s->controller, NULL, NULL);
            if (sc_controller_start(&s->controller)) {
                s->controller_started = true;
            }
        }
    }

    if (sc_demuxer_start(&s->video_demuxer)) {
        s->video_demuxer_started = true;
    }
    if (s->options.audio && sc_demuxer_start(&s->audio_demuxer)) {
        s->audio_demuxer_started = true;
    }

    while (atomic_load(&s->running)) {
#ifdef _WIN32
        Sleep(50);
#else
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 };
        nanosleep(&ts, NULL);
#endif
    }

    LOGD("FFI worker: shutting down...");

    if (s->controller_started) {
        sc_controller_stop(&s->controller);
    }
    sc_server_stop(&s->server);

    if (s->server.video_socket != SC_SOCKET_NONE) {
        net_close(s->server.video_socket);
        s->server.video_socket = SC_SOCKET_NONE;
    }
    if (s->server.audio_socket != SC_SOCKET_NONE) {
        net_close(s->server.audio_socket);
        s->server.audio_socket = SC_SOCKET_NONE;
    }
    if (s->server.control_socket != SC_SOCKET_NONE) {
        net_close(s->server.control_socket);
        s->server.control_socket = SC_SOCKET_NONE;
    }

    if (s->video_demuxer_started) {
        sc_demuxer_join(&s->video_demuxer);
    }
    if (s->audio_demuxer_started) {
        sc_demuxer_join(&s->audio_demuxer);
    }
    if (s->controller_started) {
        sc_controller_join(&s->controller);
        sc_controller_destroy(&s->controller);
        s->controller_started = false;
    }

cleanup_server:
    sc_server_stop(&s->server);
    sc_server_join(&s->server);
    sc_server_destroy(&s->server);

    s->server_started        = false;
    s->video_demuxer_started = false;
    s->audio_demuxer_started = false;

    scrcpy_ffi_publish_state(s, SCRCPY_STATE_DISCONNECTED);
    s->state_cb = NULL;

    LOGD("FFI worker: done");
    return 0;
}

#if defined(_WIN32)
  #if defined(_M_ARM64)
    #define ADB_EXECUTABLE "adb_windows_arm64.exe"
  #else
    #define ADB_EXECUTABLE "adb_windows_x64.exe"
  #endif
#elif defined(__linux__)
  #if defined(__aarch64__)
    #define ADB_EXECUTABLE "adb_linux_arm64"
  #else
    #define ADB_EXECUTABLE "adb_linux_x64"
  #endif
#endif

FFI_EXPORT void* ffi_scrcpy_start(
    int argc,
    const char** argv,
    on_video_frame_ready_cb     video_cb,
    on_audio_pcm_ready_cb       audio_cb,
    on_connection_state_cb      state_cb,
    on_clipboard_changed_cb     clipboard_cb)
{
    ScrcpyFfiInstance* s = calloc(1, sizeof(ScrcpyFfiInstance));
    if (!s) return NULL;

    sc_mutex_init(&s->mutex);
    sc_cond_init(&s->cond);
    atomic_init(&s->running, true);
    atomic_init(&s->stop_called, false);
    atomic_init(&s->gpu_context_ready, false);
    atomic_init(&s->connection_state, SCRCPY_STATE_CONNECTING);
    atomic_init(&s->state_port, 0);
    atomic_init(&s->decode_paused, false);
    atomic_init(&s->await_video_keyframe, false);
    atomic_init(&s->decode_generation, 0);

    LOCK_GLOBAL();
    ScrcpyInstanceNode* node = malloc(sizeof(ScrcpyInstanceNode));
    if (!node) {
        UNLOCK_GLOBAL();
        LOGE("FFI: Out of memory allocating instance node");
        sc_cond_destroy(&s->cond);
        sc_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    node->instance = s;
    node->next = g_instances_head;
    g_instances_head = node;
    UNLOCK_GLOBAL();

    t_instance_handle = s;

#if !defined(__APPLE__)
    char adb_path[1024] = {0};
#endif
    char server_path[1024] = {0};

#if defined(__APPLE__)
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        CFURLRef bundleURL = CFBundleCopyBundleURL(mainBundle);
        char bundle_path[PATH_MAX];
        if (bundleURL
                && CFURLGetFileSystemRepresentation(bundleURL, true,
                                                    (UInt8 *) bundle_path,
                                                    sizeof(bundle_path))) {
            snprintf(server_path, sizeof(server_path),
                     "%s/Contents/Frameworks/App.framework/Resources/"
                     "flutter_assets/packages/scrcpy_flutter_plugin/assets/"
                     "scrcpy-server",
                     bundle_path);
        }
        if (bundleURL) {
            CFRelease(bundleURL);
        }
    }
    if (server_path[0] != '\0') {
        setenv("SCRCPY_SERVER_PATH", server_path, 1);
        LOGD("FFI dynamic SCRCPY_SERVER_PATH: %s", server_path);
    }
#elif defined(__linux__)
    char exe_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        char* last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0'; // Lấy thư mục chứa file thực thi
            snprintf(adb_path, sizeof(adb_path), "%s/data/flutter_assets/packages/scrcpy_flutter_plugin/assets/%s", exe_path, ADB_EXECUTABLE);
            snprintf(server_path, sizeof(server_path), "%s/data/flutter_assets/packages/scrcpy_flutter_plugin/assets/scrcpy-server", exe_path);
            
            chmod(adb_path, 0755);
            
            setenv("ADB", adb_path, 1);
            setenv("SCRCPY_SERVER_PATH", server_path, 1);
            LOGD("FFI dynamic ADB path: %s", adb_path);
            LOGD("FFI dynamic SCRCPY_SERVER_PATH: %s", server_path);
        }
    } else {
        LOGW("FFI: Failed to read /proc/self/exe");
        setenv("ADB", "adb", 0);
        setenv("SCRCPY_SERVER_PATH", "scrcpy-server", 0);
    }
#elif defined(_WIN32)
    wchar_t exe_path_w[1024] = {0};
    DWORD len = GetModuleFileNameW(NULL, exe_path_w, 1024);
    if (len > 0) {
        wchar_t* last_slash = wcsrchr(exe_path_w, L'\\');
        if (last_slash) {
            *last_slash = L'\0';
            
            wchar_t adb_path_w[1024] = {0};
            wchar_t server_path_w[1024] = {0};
            swprintf(adb_path_w, 1024, L"%ls\\data\\flutter_assets\\packages\\scrcpy_flutter_plugin\\assets\\%hs", exe_path_w, ADB_EXECUTABLE);
            swprintf(server_path_w, 1024, L"%ls\\data\\flutter_assets\\packages\\scrcpy_flutter_plugin\\assets\\scrcpy-server", exe_path_w);
            
            SetEnvironmentVariableW(L"ADB", adb_path_w);
            SetEnvironmentVariableW(L"SCRCPY_SERVER_PATH", server_path_w);
            
            LOGD("FFI dynamic ADB path and SCRCPY_SERVER_PATH configured.");
        }
    } else {
        LOGW("FFI: Failed to get module file name on Windows.");
        SetEnvironmentVariableA("ADB", "adb");
        SetEnvironmentVariableA("SCRCPY_SERVER_PATH", "scrcpy-server");
    }
#endif

    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
        .pause_on_exit = SC_PAUSE_ON_EXIT_UNDEFINED,
    };
    
    int filtered_argc = 0;
    const char** filtered_argv = malloc(argc * sizeof(char*));
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--software-decoding") == 0) {
            s->software_decoding = true;
        } else {
            filtered_argv[filtered_argc++] = argv[i];
        }
    }
    
    if (!scrcpy_parse_args(&args, filtered_argc, (char**)filtered_argv)) {
        LOGE("FFI: Failed to parse arguments");
        free(filtered_argv);
        remove_instance_from_global(s);
        sc_cond_destroy(&s->cond);
        sc_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    
    free(filtered_argv);

    s->options = args.opts;
    s->video_cb     = video_cb;
    s->audio_cb     = audio_cb;
    // State callbacks are intentionally not invoked from decoder/server
    // threads. Dart polls ffi_scrcpy_get_connection_state() instead.
    (void)state_cb;
    s->state_cb     = NULL;
    s->clipboard_cb = clipboard_cb;
    sc_mutex_init(&s->buffer_mutex);

    // SDL initialization must happen before the worker creates any decoder
    // thread. It is process-wide; a failed local device setup never prevents
    // the optional Dart PCM callback from receiving decoded frames.
    if (s->options.audio && !scrcpy_ffi_prepare_audio_output()) {
        LOGW("FFI Audio: native local output is unavailable for this process");
    }

    if (!net_init()) {
        remove_instance_from_global(s);
        sc_cond_destroy(&s->cond);
        sc_mutex_destroy(&s->mutex);
        sc_mutex_destroy(&s->buffer_mutex);
        free(s);
        return NULL;
    }

    bool ok = sc_thread_create(&s->worker_thread, scrcpy_worker_thread, "scrcpy_worker", s);
    if (!ok) {
        net_cleanup();
        remove_instance_from_global(s);
        sc_cond_destroy(&s->cond);
        sc_mutex_destroy(&s->mutex);
        sc_mutex_destroy(&s->buffer_mutex);
        free(s);
        return NULL;
    }

    return s;
}

FFI_EXPORT void ffi_scrcpy_stop(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) return;

    if (atomic_exchange(&s->stop_called, true)) {
        return;
    }

    remove_instance_from_global(s);
    atomic_store(&s->running, false);

    sc_mutex_lock(&s->buffer_mutex);
    s->video_cb     = NULL;
    s->audio_cb     = NULL;
    s->state_cb     = NULL;
    s->clipboard_cb = NULL;
    sc_mutex_unlock(&s->buffer_mutex);

    if (s->worker_thread.thread != NULL) {
        sc_mutex_lock(&s->mutex);
        sc_cond_signal(&s->cond);
        sc_mutex_unlock(&s->mutex);

        LOGD("FFI: Calling sc_server_stop to interrupt pending connections...");
        sc_server_stop(&s->server);

        LOGD("FFI: joining worker thread...");
        sc_thread_join(&s->worker_thread, NULL);
        s->worker_thread.thread = NULL;
    }

    scrcpy_ffi_publish_state(s, SCRCPY_STATE_DISCONNECTED);

#if defined(_WIN32)
    scrcpy_ffi_release_d3d11_device(s);
#endif

    sc_cond_destroy(&s->cond);
    sc_mutex_destroy(&s->mutex);
    sc_mutex_destroy(&s->buffer_mutex);

    net_cleanup();
    
#if defined(__APPLE__)
    if (s->software_pixel_buffer) {
        CVPixelBufferRelease((CVPixelBufferRef)s->software_pixel_buffer);
        s->software_pixel_buffer = NULL;
    }
#endif

    free(s);
}

FFI_EXPORT void ffi_scrcpy_cleanup_all(void) {
    LOGW("FFI: Cleaning up all active scrcpy instances...");
    LOCK_GLOBAL();
    while (g_instances_head != NULL) {
        ScrcpyInstanceNode* node = g_instances_head;
        g_instances_head = node->next;

        ScrcpyFfiInstance* s = node->instance;
        free(node);

        if (s != NULL) {
            LOGW("FFI: Force stopping orphaned instance %p", s);

            if (atomic_exchange(&s->stop_called, true)) {
                continue;
            }

            atomic_store(&s->running, false);

            sc_mutex_lock(&s->mutex);
            sc_cond_signal(&s->cond);
            sc_mutex_unlock(&s->mutex);

            if (s->worker_thread.thread != NULL) {
                sc_thread_join(&s->worker_thread, NULL);
                s->worker_thread.thread = NULL;
            }

            s->video_cb     = NULL;
            s->audio_cb     = NULL;
            s->state_cb     = NULL;
            s->clipboard_cb = NULL;

#if defined(_WIN32)
            scrcpy_ffi_release_d3d11_device(s);
#endif

            sc_cond_destroy(&s->cond);
            sc_mutex_destroy(&s->mutex);
            sc_mutex_destroy(&s->buffer_mutex);
            
            net_cleanup();

            free(s);
        }
    }
    UNLOCK_GLOBAL();
    LOGW("FFI: Cleanup all completed");
}

FFI_EXPORT bool ffi_scrcpy_is_running(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    return s ? atomic_load(&s->running) : false;
}

FFI_EXPORT bool ffi_scrcpy_pause(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s || !atomic_load(&s->running)) {
        return false;
    }

    if (!atomic_exchange(&s->decode_paused, true)) {
        // The next packet observed by each decoder flushes that decoder on its
        // own thread. Never touch AVCodecContext from the Dart/UI thread.
        atomic_fetch_add(&s->decode_generation, 1);
        // Drop queued PCM and pause the local device immediately. The Android
        // server, audio socket and demuxer remain connected and draining.
        scrcpy_ffi_audio_output_pause(s);
        LOGD("FFI: local media decode paused");
    }
    return true;
}

FFI_EXPORT bool ffi_scrcpy_resume(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s || !atomic_load(&s->running)) {
        return false;
    }

    if (!atomic_exchange(&s->decode_paused, false)) {
        return true;
    }

    atomic_fetch_add(&s->decode_generation, 1);
    atomic_store(&s->await_video_keyframe, s->options.video);
    // The next decoded PCM block is queued directly to the local SDL stream;
    // unlike video it does not require a keyframe before playback may resume.
    scrcpy_ffi_audio_output_resume(s);

    // Missing inter-frames cannot be decoded after a real pause. Ask the
    // server for a reset immediately; it creates a fresh stream/keyframe
    // without reconnecting ADB, sockets or the GPU device.
    if (s->options.video && s->controller_started) {
        struct sc_control_msg msg = {0};
        msg.type = SC_CONTROL_MSG_TYPE_RESET_VIDEO;
        if (!sc_controller_push_msg(&s->controller, &msg)) {
            LOGW("FFI: could not request video reset on resume");
        }
    }

    LOGD("FFI: local media decode resumed; waiting for video keyframe");
    return true;
}

FFI_EXPORT bool ffi_scrcpy_is_paused(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    return s && atomic_load(&s->decode_paused);
}

FFI_EXPORT bool ffi_scrcpy_init_dart_api(void* data) {
    if (atomic_load_explicit(&g_dart_api_initialized, memory_order_acquire)) {
        return true;
    }
    if (!data || Dart_InitializeApiDL(data) != 0) {
        LOGE("FFI: failed to initialize Dart dynamic API");
        return false;
    }
    sc_log_configure();
    atomic_store_explicit(&g_dart_api_initialized, true, memory_order_release);
    return true;
}

FFI_EXPORT void ffi_scrcpy_set_state_port(void* handle, int64_t port) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) {
        return;
    }

    atomic_store_explicit(&s->state_port, port, memory_order_release);
    if (port && atomic_load_explicit(&g_dart_api_initialized, memory_order_acquire)) {
        int32_t state = atomic_load_explicit(&s->connection_state,
                                             memory_order_acquire);
        Dart_PostInteger_DL((Dart_Port_DL)port, state);
    }
}

FFI_EXPORT int32_t ffi_scrcpy_get_connection_state(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    return s ? atomic_load_explicit(&s->connection_state, memory_order_acquire)
             : SCRCPY_STATE_DISCONNECTED;
}

FFI_EXPORT const uint8_t* ffi_scrcpy_get_yuv_planes(
    void* handle,
    int32_t* out_width,
    int32_t* out_height,
    const uint8_t** out_y,
    const uint8_t** out_u,
    const uint8_t** out_v,
    int32_t* out_y_stride,
    int32_t* out_u_stride,
    int32_t* out_v_stride
) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) return NULL;

    sc_mutex_lock(&s->buffer_mutex);
    if (!s->current_frame) {
        sc_mutex_unlock(&s->buffer_mutex);
        return NULL;
    }
    if (s->current_frame->hw_frames_ctx) {
        sc_mutex_unlock(&s->buffer_mutex);
        return NULL;
    }
    if (out_width)  *out_width  = s->current_frame->width;
    if (out_height) *out_height = s->current_frame->height;
    if (out_y)      *out_y      = s->current_frame->data[0];
    if (out_u)      *out_u      = s->current_frame->data[1];
    if (out_v)      *out_v      = s->current_frame->data[2];
    if (out_y_stride)  *out_y_stride  = s->current_frame->linesize[0];
    if (out_u_stride)  *out_u_stride  = s->current_frame->linesize[1];
    if (out_v_stride)  *out_v_stride  = s->current_frame->linesize[2];
    
    return (const uint8_t*)s->current_frame;
}

FFI_EXPORT void ffi_scrcpy_release_yuv_planes(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (s) {
        sc_mutex_unlock(&s->buffer_mutex);
    }
}

FFI_EXPORT void ffi_scrcpy_free_string(char* str) {
    if (str) {
        free(str);
    }
}

// ---------------------------------------------------------------------------
// Hardware Acceleration (GPU Decoder) Setup
// ---------------------------------------------------------------------------

#if defined(_WIN32)
static void
scrcpy_ffi_log_d3d11_capabilities(ID3D11Device *device,
                                  const AVCodecContext *codec_ctx);
#endif

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    LOGD("FFI: decoder pixel formats offered for codec=%s size=%dx%d:",
         ctx && ctx->codec ? ctx->codec->name : "unknown",
         ctx ? ctx->coded_width : 0, ctx ? ctx->coded_height : 0);
    unsigned index = 0;
    for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
        const char *name = av_get_pix_fmt_name(*p);
        LOGD("FFI:   format[%u]=%s (%d)%s", index++,
             name ? name : "unknown", *p,
             *p == t_expected_hw_format ? " [required]" : "");
        if (*p == t_expected_hw_format) {
            LOGD("FFI: selected required hardware pixel format %s",
                 name ? name : "unknown");
#if defined(_WIN32)
            if (ctx && ctx->hw_device_ctx) {
                AVHWDeviceContext *device_ctx =
                    (AVHWDeviceContext *)ctx->hw_device_ctx->data;
                AVD3D11VADeviceContext *d3d11_ctx =
                    (AVD3D11VADeviceContext *)device_ctx->hwctx;
                if (d3d11_ctx && d3d11_ctx->device) {
                    // At get_format() time FFmpeg has parsed the H.264 SPS, so
                    // coded_width/coded_height are available for a real
                    // decoder-configuration probe.
                    scrcpy_ffi_log_d3d11_capabilities(d3d11_ctx->device, ctx);
                }
            }
#endif
            return *p;
        }
    }
    LOGE("FFI: decoder did not offer required hardware pixel format %s; "
         "D3D11VA may have removed it after hardware initialization failed",
         av_get_pix_fmt_name(t_expected_hw_format));
    return AV_PIX_FMT_NONE;
}

FFI_EXPORT void ffi_scrcpy_set_d3d11_device(void* handle, void* d3d11_device) {
#if defined(_WIN32)
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    ID3D11Device *device = (ID3D11Device *)d3d11_device;
    if (s && device) {
        sc_mutex_lock(&s->mutex);
        if (!s->d3d11_device) {
            // Keep one instance-owned reference. The AVHWDeviceContext takes
            // a separate reference when the video decoder is initialized.
            device->lpVtbl->AddRef(device);
            s->d3d11_device = device;
        } else if (s->d3d11_device != device) {
            LOGE("FFI: refusing to replace the D3D11 device of a live session");
            sc_mutex_unlock(&s->mutex);
            return;
        }
        atomic_store(&s->gpu_context_ready, true);
        sc_cond_broadcast(&s->cond);
        sc_mutex_unlock(&s->mutex);
    }
#else
    (void)handle;
    (void)d3d11_device;
#endif
}

static bool
scrcpy_ffi_select_hw_config(const AVCodec *codec, enum AVHWDeviceType type,
                            enum AVPixelFormat *out_format) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
        if (!config) {
            return false;
        }
        if (config->device_type == type
                && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            *out_format = config->pix_fmt;
            return true;
        }
    }
}

bool scrcpy_ffi_setup_hwaccel(AVCodecContext *ctx, const AVCodec *codec);

static ScrcpyCodecBinding *
scrcpy_ffi_codec_binding(AVCodecContext *avctx) {
    return avctx ? (ScrcpyCodecBinding *)avctx->opaque : NULL;
}

int
sc_avcodec_send_packet(AVCodecContext *avctx, const AVPacket *packet) {
    ScrcpyCodecBinding *binding = scrcpy_ffi_codec_binding(avctx);
    if (!binding || !binding->instance) {
        return avcodec_send_packet(avctx, packet);
    }

    ScrcpyFfiInstance *s = binding->instance;
    uint64_t generation = atomic_load_explicit(&s->decode_generation,
                                               memory_order_acquire);
    if (binding->applied_decode_generation != generation) {
        // This runs exclusively on the decoder thread. It discards any
        // delayed frame without racing AVCodecContext from the Dart thread.
        avcodec_flush_buffers(avctx);
        binding->applied_decode_generation = generation;
    }

    if (atomic_load_explicit(&s->decode_paused, memory_order_acquire)) {
        return 0; // demuxer already consumed the packet, so there is no backlog
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO
            && atomic_load_explicit(&s->await_video_keyframe,
                                    memory_order_acquire)) {
        if (!packet || !(packet->flags & AV_PKT_FLAG_KEY)) {
            return 0;
        }
        atomic_store_explicit(&s->await_video_keyframe, false,
                              memory_order_release);
        LOGD("FFI: resume keyframe accepted by GPU decoder");
    }

    return avcodec_send_packet(avctx, packet);
}

int
sc_avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    ScrcpyCodecBinding *binding = scrcpy_ffi_codec_binding(avctx);
    if (binding && binding->instance) {
        ScrcpyFfiInstance *s = binding->instance;
        if (atomic_load_explicit(&s->decode_paused, memory_order_acquire)
                || (avctx->codec_type == AVMEDIA_TYPE_VIDEO
                    && atomic_load_explicit(&s->await_video_keyframe,
                                            memory_order_acquire))) {
            return AVERROR(EAGAIN);
        }
    }
    return avcodec_receive_frame(avctx, frame);
}

void
sc_avcodec_free_context(AVCodecContext **avctx) {
    if (avctx && *avctx) {
        ScrcpyCodecBinding *binding = scrcpy_ffi_codec_binding(*avctx);
        if (binding) {
            (*avctx)->opaque = NULL;
            free(binding);
        }
    }
    avcodec_free_context(avctx);
}

int sc_avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options) {
    ScrcpyFfiInstance *s = (ScrcpyFfiInstance *)t_instance_handle;
    ScrcpyCodecBinding *binding = NULL;
    if (s && !avctx->opaque
            && (avctx->codec_type == AVMEDIA_TYPE_VIDEO
                || avctx->codec_type == AVMEDIA_TYPE_AUDIO)) {
        binding = calloc(1, sizeof(*binding));
        if (!binding) {
            return AVERROR(ENOMEM);
        }
        binding->instance = s;
        binding->applied_decode_generation = atomic_load(&s->decode_generation);
        avctx->opaque = binding;
    }

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (s && s->software_decoding) {
            LOGD("FFI: Software decoding requested via options, skipping hardware acceleration setup.");
        } else if (!scrcpy_ffi_setup_hwaccel(avctx, codec)) {
            // Hardware acceleration failed.
            LOGE("FFI: Failed to setup hardware acceleration, but fallback is disabled. Returning ENOSYS.");
            avctx->opaque = NULL;
            free(binding);
            return AVERROR(ENOSYS);
        }
    }
    int ret = avcodec_open2(avctx, codec, options);
    if (ret < 0 && binding) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, error, sizeof(error));
        LOGE("FFI: avcodec_open2 failed for codec=%s: %s (%d)",
             codec ? codec->name : "unknown", error, ret);
        avctx->opaque = NULL;
        free(binding);
    } else if (ret >= 0 && avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        const char *pixel_format = av_get_pix_fmt_name(avctx->pix_fmt);
        const char *software_format = av_get_pix_fmt_name(avctx->sw_pix_fmt);
        LOGD("FFI: video decoder opened: codec=%s format=%s software-format=%s",
             codec ? codec->name : "unknown",
             pixel_format ? pixel_format : "unset",
             software_format ? software_format : "unset");
    }
    return ret;
}

#if defined(_WIN32)
static void
scrcpy_ffi_log_d3d11_capabilities(ID3D11Device *device,
                                  const AVCodecContext *codec_ctx) {
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC adapter_desc;
    HRESULT hr = device->lpVtbl->QueryInterface(
        device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr) && dxgi_device) {
        hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, &adapter);
        if (SUCCEEDED(hr) && adapter
                && SUCCEEDED(adapter->lpVtbl->GetDesc(adapter, &adapter_desc))) {
            LOGD("FFI: D3D11 adapter vendor=0x%04x device=0x%04x "
                 "luid=%08lx:%08lx dedicated-video-memory=%llu",
                 adapter_desc.VendorId, adapter_desc.DeviceId,
                 (unsigned long)adapter_desc.AdapterLuid.HighPart,
                 (unsigned long)adapter_desc.AdapterLuid.LowPart,
                 (unsigned long long)adapter_desc.DedicatedVideoMemory);
        }
    }

    ID3D11VideoDevice *video_device = NULL;
    hr = device->lpVtbl->QueryInterface(
        device, &IID_ID3D11VideoDevice, (void **)&video_device);
    if (FAILED(hr) || !video_device) {
        LOGE("FFI: ID3D11VideoDevice is unavailable: HRESULT=0x%08lx",
             (unsigned long)hr);
        goto cleanup;
    }

    UINT profile_count =
        video_device->lpVtbl->GetVideoDecoderProfileCount(video_device);
    BOOL h264_nv12 = FALSE;
    hr = video_device->lpVtbl->CheckVideoDecoderFormat(
        video_device, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
        DXGI_FORMAT_NV12, &h264_nv12);
    LOGD("FFI: D3D11 video profiles=%u H264_VLD_NOFGT+NV12=%s "
         "HRESULT=0x%08lx",
         profile_count, SUCCEEDED(hr) && h264_nv12 ? "yes" : "no",
         (unsigned long)hr);

    if (codec_ctx && codec_ctx->codec_id == AV_CODEC_ID_H264) {
        UINT width = codec_ctx->coded_width > 0
                   ? (UINT)codec_ctx->coded_width : (UINT)codec_ctx->width;
        UINT height = codec_ctx->coded_height > 0
                    ? (UINT)codec_ctx->coded_height : (UINT)codec_ctx->height;
        if (width && height && SUCCEEDED(hr) && h264_nv12) {
            D3D11_VIDEO_DECODER_DESC desc = {
                .Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
                .SampleWidth = width,
                .SampleHeight = height,
                .OutputFormat = DXGI_FORMAT_NV12,
            };
            UINT config_count = 0;
            hr = video_device->lpVtbl->GetVideoDecoderConfigCount(
                video_device, &desc, &config_count);
            LOGD("FFI: H.264 D3D11 decoder probe size=%ux%u configs=%u "
                 "HRESULT=0x%08lx",
                 width, height, config_count, (unsigned long)hr);
        }
    }

cleanup:
    if (video_device) {
        video_device->lpVtbl->Release(video_device);
    }
    if (adapter) {
        adapter->lpVtbl->Release(adapter);
    }
    if (dxgi_device) {
        dxgi_device->lpVtbl->Release(dxgi_device);
    }
}
#endif

bool scrcpy_ffi_setup_hwaccel(AVCodecContext *ctx, const AVCodec *codec) {
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    #if defined(_WIN32)
        type = AV_HWDEVICE_TYPE_D3D11VA;
    #elif defined(__linux__)
        type = AV_HWDEVICE_TYPE_VAAPI;
    #elif defined(__APPLE__)
        type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    #endif

    enum AVPixelFormat selected_format = AV_PIX_FMT_NONE;
    if (type == AV_HWDEVICE_TYPE_NONE
            || !scrcpy_ffi_select_hw_config(codec, type, &selected_format)) {
        LOGE("FFI: codec '%s' has no usable hardware configuration for device type %s",
             codec ? codec->name : "unknown", av_hwdevice_get_type_name(type));
        return false;
    }

    AVBufferRef *hw_device_ctx = NULL;
#if defined(_WIN32)
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)t_instance_handle;
    if (!s || !s->d3d11_device) {
        LOGE("FFI: Flutter D3D11 device was not bound before decoder initialization");
        return false;
    }

    ID3D11Device *external_device = (ID3D11Device *)s->d3d11_device;
    scrcpy_ffi_log_d3d11_capabilities(external_device, ctx);

    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hw_device_ctx) {
        AVHWDeviceContext *device_ctx =
            (AVHWDeviceContext *)hw_device_ctx->data;
        AVD3D11VADeviceContext *d3d11_ctx =
            (AVD3D11VADeviceContext *)device_ctx->hwctx;

        // This is deliberately the only field supplied by the application.
        // FFmpeg derives device_context/video_device/video_context and installs
        // its recursive internal lock in av_hwdevice_ctx_init().
        external_device->lpVtbl->AddRef(external_device);
        d3d11_ctx->device = external_device;
    } else {
        LOGE("FFI: could not allocate D3D11VA device context");
        return false;
    }
#else
    if (av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0) < 0) {
        LOGE("FFI: failed to create %s hardware device context",
             av_hwdevice_get_type_name(type));
        return false;
    }
#endif

    // av_hwdevice_ctx_create() initializes non-Windows devices itself. The
    // manually allocated D3D11 context is the only one that needs init here.
#if defined(_WIN32)
    int init_result = av_hwdevice_ctx_init(hw_device_ctx);
    if (init_result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(init_result, error, sizeof(error));
        LOGE("FFI: failed to initialize %s hardware device context: %s (%d)",
             av_hwdevice_get_type_name(type), error, init_result);
        av_buffer_unref(&hw_device_ctx);
        return false;
    }
    AVHWDeviceContext *initialized_device_ctx =
        (AVHWDeviceContext *)hw_device_ctx->data;
    AVD3D11VADeviceContext *initialized_d3d11_ctx =
        (AVD3D11VADeviceContext *)initialized_device_ctx->hwctx;
    LOGD("FFI: D3D11VA context initialized by FFmpeg: "
         "device=%p context=%p video-device=%p video-context=%p "
         "internal-lock=%s",
         initialized_d3d11_ctx->device,
         initialized_d3d11_ctx->device_context,
         initialized_d3d11_ctx->video_device,
         initialized_d3d11_ctx->video_context,
         initialized_d3d11_ctx->lock ? "yes" : "no");
#endif

    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    av_buffer_unref(&hw_device_ctx);
    if (!ctx->hw_device_ctx) {
        return false;
    }
    t_expected_hw_format = selected_format;
    ctx->get_format = get_hw_format;
    ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    LOGD("FFI: GPU-only decoder configured: codec=%s device=%s format=%s",
         codec ? codec->name : "unknown", av_hwdevice_get_type_name(type),
         av_get_pix_fmt_name(selected_format));
    return true;
}

// ---------------------------------------------------------------------------
// GPU Texture/Surface Extraction APIs
// ---------------------------------------------------------------------------

FFI_EXPORT void* ffi_scrcpy_get_cv_pixel_buffer(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) return NULL;

    void* pb = NULL;
    sc_mutex_lock(&s->buffer_mutex);
    if (s->current_frame) {
        if (s->current_frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            pb = (void*)s->current_frame->data[3];
#if defined(__APPLE__)
            if (pb) {
                CVPixelBufferRetain((CVPixelBufferRef)pb);
            }
#endif
        }
#if defined(__APPLE__)
        else if (s->current_frame->format == AV_PIX_FMT_YUV420P || s->current_frame->format == AV_PIX_FMT_YUVJ420P) {
            AVFrame *f = s->current_frame;
            // Check if we already have a cached pixel buffer with matching dimensions
            if (s->software_pixel_buffer) {
                size_t w = CVPixelBufferGetWidth((CVPixelBufferRef)s->software_pixel_buffer);
                size_t h = CVPixelBufferGetHeight((CVPixelBufferRef)s->software_pixel_buffer);
                if (w != f->width || h != f->height) {
                    CVPixelBufferRelease((CVPixelBufferRef)s->software_pixel_buffer);
                    s->software_pixel_buffer = NULL;
                }
            }

            CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)s->software_pixel_buffer;
            CVReturn err = kCVReturnSuccess;

            if (!pixelBuffer) {
                CFDictionaryRef empty_dict = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, empty_dict);
                
                err = CVPixelBufferCreate(
                    kCFAllocatorDefault,
                    f->width,
                    f->height,
                    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                    attrs,
                    &pixelBuffer
                );
                
                CFRelease(empty_dict);
                CFRelease(attrs);
                
                if (err == kCVReturnSuccess && pixelBuffer) {
                    s->software_pixel_buffer = (void*)pixelBuffer;
                }
            }

            if (err == kCVReturnSuccess && pixelBuffer) {
                CVPixelBufferLockBaseAddress(pixelBuffer, 0);
                
                size_t y_src_stride = f->linesize[0];
                size_t u_src_stride = f->linesize[1];
                size_t v_src_stride = f->linesize[2];
                
                uint8_t *y_src = f->data[0];
                uint8_t *u_src = f->data[1];
                uint8_t *v_src = f->data[2];
                
                uint8_t *y_dst = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
                uint8_t *uv_dst = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
                
                size_t y_dst_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
                size_t uv_dst_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
                
                for (int y = 0; y < f->height; y++) {
                    memcpy(y_dst + y * y_dst_stride, y_src + y * y_src_stride, f->width);
                }
                for (int y = 0; y < f->height / 2; y++) {
                    uint8_t *uv_row = uv_dst + y * uv_dst_stride;
                    uint8_t *u_row = u_src + y * u_src_stride;
                    uint8_t *v_row = v_src + y * v_src_stride;
                    for (int x = 0; x < f->width / 2; x++) {
                        uv_row[x * 2] = u_row[x];
                        uv_row[x * 2 + 1] = v_row[x];
                    }
                }
                
                CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
                
                // Retain once for Flutter texture transfer
                CVPixelBufferRetain(pixelBuffer);
                pb = (void*)pixelBuffer;
            } else {
                LOGE("FFI: CVPixelBufferCreate failed with error: %d", err);
            }
        } else if (s->current_frame->format == AV_PIX_FMT_NONE) {
            // Frame allocated but no data pushed yet. Just return NULL silently.
        } else {
            LOGE("FFI: Unsupported software decoding pixel format: %d", s->current_frame->format);
        }
#endif
    }
    sc_mutex_unlock(&s->buffer_mutex);
    return pb;
}

FFI_EXPORT bool
ffi_scrcpy_acquire_gpu_frame(void* handle, ScrcpyGpuFrame* out_frame) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s || !out_frame) {
        return false;
    }
    memset(out_frame, 0, sizeof(*out_frame));

    sc_mutex_lock(&s->buffer_mutex);
    if (!s->current_frame || !s->current_frame->hw_frames_ctx) {
        sc_mutex_unlock(&s->buffer_mutex);
        return false;
    }

    AVFrame *frame = av_frame_clone(s->current_frame);
    uint64_t serial = s->frame_serial;
    sc_mutex_unlock(&s->buffer_mutex);
    if (!frame) {
        return false;
    }

    int32_t backend = SCRCPY_GPU_FRAME_NONE;
#if defined(_WIN32)
    if (frame->format == AV_PIX_FMT_D3D11 || frame->format == AV_PIX_FMT_D3D11VA) {
        backend = SCRCPY_GPU_FRAME_D3D11;
    }
#elif defined(__APPLE__)
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        backend = SCRCPY_GPU_FRAME_VIDEOTOOLBOX;
    }
#elif defined(__linux__)
    if (frame->format == AV_PIX_FMT_VAAPI) {
        backend = SCRCPY_GPU_FRAME_VAAPI;
    }
#endif
    if (backend == SCRCPY_GPU_FRAME_NONE) {
        av_frame_free(&frame);
        return false;
    }

    out_frame->frame_ref = frame;
    // AV_PIX_FMT_D3D11 stores ID3D11Texture2D* in data[0] and its array slice
    // in data[1]. data[3]/data[4] are used by other hardware frame backends.
#if defined(_WIN32)
    if (backend == SCRCPY_GPU_FRAME_D3D11) {
        out_frame->native_handle = frame->data[0];
        out_frame->texture_index = (int32_t)(intptr_t)frame->data[1];
    } else
#endif
    {
        out_frame->native_handle = frame->data[3];
        out_frame->texture_index = (int32_t)(intptr_t)frame->data[4];
    }
    out_frame->serial = serial;
    out_frame->backend = backend;
    out_frame->width = frame->width;
    out_frame->height = frame->height;
    out_frame->pixel_format = frame->format;
    out_frame->color_space = frame->colorspace;
    out_frame->color_range = frame->color_range;
    return true;
}

FFI_EXPORT void
ffi_scrcpy_release_gpu_frame(ScrcpyGpuFrame* frame) {
    if (!frame) {
        return;
    }
    AVFrame *avframe = (AVFrame *)frame->frame_ref;
    av_frame_free(&avframe);
    memset(frame, 0, sizeof(*frame));
}

FFI_EXPORT void* ffi_scrcpy_get_d3d11_texture(void* handle, int* out_index) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) return NULL;

    void* texture = NULL;
    sc_mutex_lock(&s->buffer_mutex);
    if (s->current_frame && s->current_frame->format == AV_PIX_FMT_D3D11) {
        texture = (void*)s->current_frame->data[0];
        if (out_index) {
            *out_index = (int)(intptr_t)s->current_frame->data[1];
        }
    }
    sc_mutex_unlock(&s->buffer_mutex);
    return texture;
}

FFI_EXPORT void* ffi_scrcpy_get_vaapi_surface(void* handle) {
    ScrcpyFfiInstance* s = (ScrcpyFfiInstance*)handle;
    if (!s) return NULL;

    void* surface = NULL;
    sc_mutex_lock(&s->buffer_mutex);
    if (s->current_frame && s->current_frame->format == AV_PIX_FMT_VAAPI) {
        surface = (void*)(uintptr_t)s->current_frame->data[3];
    }
    sc_mutex_unlock(&s->buffer_mutex);
    return surface;
}
