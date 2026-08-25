// scrcpy_ffi.h — Public API for Flutter Dart FFI
// Do not modify any original scrcpy source files.
// This file declares all functions and types exported from libscrcpy_ffi.

#ifndef SCRCPY_FFI_H
#define SCRCPY_FFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Platform export macro
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  define FFI_EXPORT __declspec(dllexport)
#else
#  define FFI_EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// Connection States (state_code in on_connection_state_cb)
// ---------------------------------------------------------------------------
#define SCRCPY_STATE_DISCONNECTED   0  // Device disconnected normally or not yet connected
#define SCRCPY_STATE_CONNECTING     1  // Initializing and connecting
#define SCRCPY_STATE_CONNECTED      2  // Connected successfully, video stream started
#define SCRCPY_STATE_ERROR          3  // Unrecoverable error occurred

// ---------------------------------------------------------------------------
// Callback types — Dart side registers using NativeCallable.listener
// ---------------------------------------------------------------------------

/**
 * Triggered when a new video frame is decoded and the buffer is updated.
 * Dart side receives this and calls markTextureFrameAvailable() to redraw.
 * @param width  frame width in pixels
 * @param height frame height in pixels
 */
typedef void (*on_video_frame_ready_cb)(void* handle, int32_t width, int32_t height);

/**
 * Triggered when raw PCM audio samples (16-bit signed, interleaved stereo) are ready.
 * @param handle      session instance handle pointer
 * @param samples     pointer to the PCM sample array (valid until callback returns)
 * @param sample_count total sample count (not per-channel)
 * @param sample_rate  audio sample rate (typically 48000 Hz)
 * @param channels     channel count (typically 2)
 */
typedef void (*on_audio_pcm_ready_cb)(void* handle,
                                      const int16_t* samples,
                                      int32_t sample_count,
                                      int32_t sample_rate,
                                      int32_t channels);

/**
 * Triggered when the connection state changes.
 * @param handle      session instance handle pointer
 * @param state_code  one of the SCRCPY_STATE_* constants
 * @param device_name Android device model name (UTF-8, NULL if not available)
 */
typedef void (*on_connection_state_cb)(void* handle,
                                       int32_t state_code,
                                       const char* device_name);

/**
 * Triggered when the Android device's clipboard changes.
 * @param handle      session instance handle pointer
 * @param text new clipboard text content (UTF-8 string, NULL-terminated)
 */
typedef void (*on_clipboard_changed_cb)(void* handle, const char* text);

/**
 * Configures the absolute ADB executable path discovered by the host platform.
 * This also provides a direct link reference for dynamic framework consumers.
 */
FFI_EXPORT bool ffi_scrcpy_set_adb_path(const char* path);

/**
 * Starts scrcpy: pushes server jar to device, configures sockets,
 * and starts video and audio decoding threads.
 * This function is non-blocking — the server runs on a background thread.
 *
 * Parameters are passed identically to the scrcpy CLI (e.g. ["--show-touches", "--max-fps=60"]).
 *
 * @return the instance handle pointer (void*) if starting successfully,
 *         or NULL if an immediate error occurs.
 *         The actual connection result is reported asynchronously via state_cb.
 */
FFI_EXPORT void* ffi_scrcpy_start(
    int argc,
    const char** argv,
    on_video_frame_ready_cb     video_cb,
    on_audio_pcm_ready_cb       audio_cb,
    on_connection_state_cb      state_cb,
    on_clipboard_changed_cb     clipboard_cb
);

/**
 * Stops the scrcpy session: closes sockets, kills the server on the device,
 * and frees memory. Safe to call multiple times.
 */
FFI_EXPORT void ffi_scrcpy_stop(void* handle);

/**
 * Cleans up and frees all scrcpy instances running in the background.
 * Call this when the app starts (or after a Hot Restart) to prevent orphaned FFI callbacks.
 */
FFI_EXPORT void ffi_scrcpy_cleanup_all(void);

/** Returns true if the scrcpy instance is currently running and active. */
FFI_EXPORT bool ffi_scrcpy_is_running(void* handle);

/**
 * Stops local video/audio decoding and texture updates while keeping the
 * server connection, packet drain and GPU context alive. Input/control events
 * from Flutter are ignored until resume. Idempotent.
 */
FFI_EXPORT bool ffi_scrcpy_pause(void* handle);

/**
 * Resumes local decoding. The next frame is accepted only from a fresh video
 * keyframe so no stale inter-frame data is rendered. Idempotent.
 */
FFI_EXPORT bool ffi_scrcpy_resume(void* handle);

/** Returns whether local media decoding is currently paused. */
FFI_EXPORT bool ffi_scrcpy_is_paused(void* handle);

/**
 * Initializes Dart's dynamically linked native API. Call once on the Dart UI
 * isolate before assigning a state port.
 */
FFI_EXPORT bool ffi_scrcpy_init_dart_api(void* data);

/** Assigns the ReceivePort native id for event-driven state notifications. */
FFI_EXPORT void ffi_scrcpy_set_state_port(void* handle, int64_t port);

/** Returns the most recently published connection state for diagnostics. */
FFI_EXPORT int32_t ffi_scrcpy_get_connection_state(void* handle);

// ---------------------------------------------------------------------------
// GPU video-frame API
// ---------------------------------------------------------------------------

/** The native surface type held by a [ScrcpyGpuFrame]. */
typedef enum {
    SCRCPY_GPU_FRAME_NONE = 0,
    SCRCPY_GPU_FRAME_D3D11 = 1,
    SCRCPY_GPU_FRAME_VIDEOTOOLBOX = 2,
    SCRCPY_GPU_FRAME_VAAPI = 3,
} ScrcpyGpuFrameBackend;

/**
 * A retained hardware frame. `frame_ref` is opaque and must be returned to
 * ffi_scrcpy_release_gpu_frame().  `native_handle` is valid until then.
 *
 * This lease avoids returning a raw D3D/VAAPI handle after the decoder thread
 * has released the AVFrame that owns it.
 */
typedef struct {
    void* frame_ref;
    void* native_handle;
    uint64_t serial;
    int32_t backend;
    int32_t width;
    int32_t height;
    int32_t texture_index;
    int32_t pixel_format;
    int32_t color_space;
    int32_t color_range;
} ScrcpyGpuFrame;

/**
 * Retains the latest decoded hardware frame. Returns false if no hardware
 * frame is ready. The caller must release a successful result exactly once.
 */
FFI_EXPORT bool ffi_scrcpy_acquire_gpu_frame(void* handle,
                                             ScrcpyGpuFrame* out_frame);

/** Releases a frame returned by ffi_scrcpy_acquire_gpu_frame(). */
FFI_EXPORT void ffi_scrcpy_release_gpu_frame(ScrcpyGpuFrame* frame);

// ---------------------------------------------------------------------------
// Legacy software-frame API
// ---------------------------------------------------------------------------

/**
 * Gets pointers to the 3 YUV planes (Y, U, V) containing the latest video frame.
 * The internal mutex is LOCKED until ffi_scrcpy_release_yuv_planes() is called.
 *
 * @param handle        session instance handle
 * @param out_width     [out] frame width in pixels
 * @param out_height    [out] frame height in pixels
 * @param out_y         [out] receives the Y plane address pointer
 * @param out_u         [out] receives the U plane address pointer
 * @param out_v         [out] receives the V plane address pointer
 * @param out_y_stride  [out] plane Y line stride
 * @param out_u_stride  [out] plane U line stride
 * @param out_v_stride  [out] plane V line stride
 * @return the AVFrame pointer representation, or NULL if no frame is available.
 */
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
);

/**
 * Releases the lock on the YUV buffer after the Texture Provider has copied the data.
 */
FFI_EXPORT void ffi_scrcpy_release_yuv_planes(void* handle);

// ---------------------------------------------------------------------------
// Control API — sends events from PC down to Android device
// ---------------------------------------------------------------------------

#define SCRCPY_TOUCH_ACTION_DOWN   0
#define SCRCPY_TOUCH_ACTION_UP     1
#define SCRCPY_TOUCH_ACTION_MOVE   2

/**
 * Sends a touch event to the Android device.
 * @param handle     session instance handle pointer
 * @param action     SCRCPY_TOUCH_ACTION_*
 * @param pointer_id pointer identifier (e.g. touch finger index)
 * @param x, y       normalized coordinates (0.0 to 1.0)
 * @param pressure   touch pressure (0.0 to 1.0)
 */
FFI_EXPORT void ffi_scrcpy_send_touch(void*    handle,
                                      uint8_t  action,
                                      uint64_t pointer_id,
                                      float    x,
                                      float    y,
                                      float    pressure);

#define SCRCPY_KEY_ACTION_DOWN  0
#define SCRCPY_KEY_ACTION_UP    1

/**
 * Sends a key event to the Android device.
 * @param handle    session instance handle pointer
 * @param keycode   Android keycode (android.view.KeyEvent.KEYCODE_*)
 * @param action    SCRCPY_KEY_ACTION_*
 * @param repeat    repeat count (usually 0)
 * @param metastate modifier state mask (e.g. shift, control)
 */
FFI_EXPORT void ffi_scrcpy_send_key(void*    handle,
                                    uint32_t keycode,
                                    uint8_t  action,
                                    uint32_t repeat,
                                    uint32_t metastate);

/**
 * Sends a scroll event to the Android device.
 * @param handle     session instance handle pointer
 * @param x, y       normalized pointer coordinates
 * @param h_scroll   horizontal scroll steps (negative = left, positive = right)
 * @param v_scroll   vertical scroll steps (negative = up, positive = down)
 */
FFI_EXPORT void ffi_scrcpy_send_scroll(void* handle, float x, float y,
                                       float h_scroll, float v_scroll);

/** Sends the Back button press or wakes the screen if it is turned off. */
FFI_EXPORT void ffi_scrcpy_send_back_or_screen_on(void* handle);

/**
 * Sets the clipboard content on the Android device.
 * @param handle session instance handle pointer
 * @param text  UTF-8 string content (NULL-terminated)
 * @param paste if true, Android automatically pastes into the focused field
 */
FFI_EXPORT void ffi_scrcpy_set_clipboard(void* handle, const char* text, bool paste);

/**
 * Injects text directly into the focused field on the Android device.
 * @param handle session instance handle pointer
 * @param text UTF-8 text string to inject
 */
FFI_EXPORT void ffi_scrcpy_inject_text(void* handle, const char* text);

/**
 * Frees a string allocated by the native FFI layer.
 * Used to release strings returned via callbacks (e.g., clipboard text).
 * @param str pointer to the string to free (can be NULL)
 */
FFI_EXPORT void ffi_scrcpy_free_string(char* str);

/**
 * Copies a software-decoded current frame's Y plane and interleaves U/V planes
 * into a destination NV12 buffer. This API deliberately rejects hardware
 * frames; GPU presentation must use ffi_scrcpy_acquire_gpu_frame().
 * If dst_y or dst_uv is NULL, only width and height are returned (useful for buffer allocation).
 */
FFI_EXPORT bool ffi_scrcpy_copy_to_nv12(
    void* handle,
    int32_t* out_width,
    int32_t* out_height,
    uint8_t* dst_y,
    int32_t dst_y_stride,
    uint8_t* dst_uv,
    int32_t dst_uv_stride
);

/**
 * Gets the macOS CVPixelBufferRef pointer (retained) from the GPU decoded frame.
 */
FFI_EXPORT void* ffi_scrcpy_get_cv_pixel_buffer(void* handle);

/**
 * Gets the Windows ID3D11Texture2D pointer and texture array index from the GPU decoded frame.
 */
FFI_EXPORT void* ffi_scrcpy_get_d3d11_texture(void* handle, int* out_index);

/**
 * Sets the Windows D3D11 device pointer to be shared with FFmpeg.
 * The function retains its own COM reference; the caller passes a borrowed
 * pointer and keeps ownership of its original reference.
 */
FFI_EXPORT void ffi_scrcpy_set_d3d11_device(void* handle, void* d3d11_device);

/**
 * Gets the Linux VASurfaceID (cast to void*) from the GPU decoded frame.
 */
FFI_EXPORT void* ffi_scrcpy_get_vaapi_surface(void* handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SCRCPY_FFI_H
