#include "scrcpy_ffi_internal.h"

// Keep playback responsive when the Flutter process is busy or the device
// resumes from sleep. SDL owns the thread-safe, device-specific buffering; this
// cap prevents its queue from becoming an audible delay.
#define SCRCPY_FFI_AUDIO_MAX_QUEUE_MS 120

// SDL's audio subsystem has process-wide reference counting, while scrcpy
// sessions may be started/stopped concurrently. Initialize it once from the
// caller thread (before decoder threads exist) and retain it for the Flutter
// process lifetime. This prevents one session's teardown from silencing
// another session that is still playing.
static atomic_flag audio_init_lock = ATOMIC_FLAG_INIT;
static atomic_bool audio_subsystem_ready;

bool
scrcpy_ffi_prepare_audio_output(void) {
    while (atomic_flag_test_and_set_explicit(&audio_init_lock,
                                             memory_order_acquire)) {
        SDL_Delay(1);
    }

    if (!atomic_load_explicit(&audio_subsystem_ready, memory_order_acquire)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOGW("FFI Audio: could not initialize local audio: %s",
                 SDL_GetError());
        } else {
            atomic_store_explicit(&audio_subsystem_ready, true,
                                  memory_order_release);
            LOGD("FFI Audio: local output subsystem initialized");
        }
    }

    bool ready = atomic_load_explicit(&audio_subsystem_ready,
                                      memory_order_acquire);
    atomic_flag_clear_explicit(&audio_init_lock, memory_order_release);
    return ready;
}

static void
ffi_audio_output_close(FfiAudioSink *sink) {
    if (sink->playback_stream) {
        SDL_DestroyAudioStream(sink->playback_stream);
        sink->playback_stream = NULL;
    }
    sink->playback_sample_rate = 0;
    sink->playback_channels = 0;
}

static bool
ffi_audio_output_open(FfiAudioSink *sink, int sample_rate, int channels) {
    if (sample_rate <= 0 || channels <= 0) {
        LOGW("FFI Audio: invalid playback format: %d Hz, %d channels",
             sample_rate, channels);
        return false;
    }

    if (sink->playback_stream
            && sink->playback_sample_rate == sample_rate
            && sink->playback_channels == channels) {
        return true;
    }

    ffi_audio_output_close(sink);

    if (!atomic_load_explicit(&audio_subsystem_ready, memory_order_acquire)) {
        LOGW("FFI Audio: local output subsystem was not initialized");
        return false;
    }

    SDL_AudioSpec spec = {
        .format = SDL_AUDIO_S16,
        .channels = channels,
        .freq = sample_rate,
    };
    sink->playback_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!sink->playback_stream) {
        LOGW("FFI Audio: could not open local playback device: %s",
             SDL_GetError());
        ffi_audio_output_close(sink);
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(sink->playback_stream)) {
        LOGW("FFI Audio: could not start local playback: %s", SDL_GetError());
        ffi_audio_output_close(sink);
        return false;
    }

    sink->playback_sample_rate = sample_rate;
    sink->playback_channels = channels;
    LOGD("FFI Audio: local playback opened: %d Hz, %d channels",
         sample_rate, channels);
    return true;
}

static void
ffi_audio_output_enqueue(ScrcpyFfiInstance *s, const uint8_t *data,
                         int byte_count) {
    FfiAudioSink *sink = &s->audio_sink;
    if (!sink->playback_stream || byte_count <= 0
            || atomic_load_explicit(&s->decode_paused, memory_order_acquire)) {
        return;
    }

    int64_t max_queued = (int64_t)sink->playback_sample_rate
                       * sink->playback_channels * (int)sizeof(int16_t)
                       * SCRCPY_FFI_AUDIO_MAX_QUEUE_MS / 1000;
    int queued = SDL_GetAudioStreamQueued(sink->playback_stream);
    if (queued > 0 && (int64_t)queued + byte_count > max_queued) {
        // Retain the newest audio rather than letting playback drift behind
        // the video. The next decoded block starts a new short queue.
        SDL_ClearAudioStream(sink->playback_stream);
    }

    if (!SDL_PutAudioStreamData(sink->playback_stream, data, byte_count)) {
        LOGW("FFI Audio: could not queue local PCM: %s", SDL_GetError());
    }
}

void
scrcpy_ffi_audio_output_pause(ScrcpyFfiInstance *s) {
    FfiAudioSink *sink = &s->audio_sink;
    if (!sink->playback_stream) {
        return;
    }
    SDL_ClearAudioStream(sink->playback_stream);
    if (!SDL_PauseAudioStreamDevice(sink->playback_stream)) {
        LOGW("FFI Audio: could not pause local playback: %s", SDL_GetError());
    }
}

void
scrcpy_ffi_audio_output_resume(ScrcpyFfiInstance *s) {
    FfiAudioSink *sink = &s->audio_sink;
    if (!sink->playback_stream) {
        return;
    }
    SDL_ClearAudioStream(sink->playback_stream);
    if (!SDL_ResumeAudioStreamDevice(sink->playback_stream)) {
        LOGW("FFI Audio: could not resume local playback: %s", SDL_GetError());
    }
}

/**
 * @brief Opens the custom video frame sink. Allocates the internal AVFrame buffer.
 */
static bool ffi_video_sink_open(struct sc_frame_sink* sink,
                                const AVCodecContext* ctx,
                                const struct sc_stream_session* session) {
    (void)session;
    FfiVideoSink* s_sink = (FfiVideoSink*)sink;
    ScrcpyFfiInstance* s = s_sink->state;

    LOGD("FFI Video sink opened: %dx%d", ctx->width, ctx->height);

    sc_mutex_lock(&s->buffer_mutex);
    s->frame_width = ctx->width;
    s->frame_height = ctx->height;
    if (s->current_frame) {
        av_frame_free(&s->current_frame);
    }
    s->current_frame = av_frame_alloc();
    sc_mutex_unlock(&s->buffer_mutex);

    return s->current_frame != NULL;
}

/**
 * @brief Closes the custom video frame sink and frees the AVFrame.
 */
static void ffi_video_sink_close(struct sc_frame_sink* sink) {
    FfiVideoSink* s_sink = (FfiVideoSink*)sink;
    ScrcpyFfiInstance* s = s_sink->state;
    sc_mutex_lock(&s->buffer_mutex);
    if (s->current_frame) {
        av_frame_free(&s->current_frame);
        s->current_frame = NULL;
    }
    sc_mutex_unlock(&s->buffer_mutex);
    LOGD("FFI Video sink closed");
}

/**
 * @brief Receives a decoded video frame, copies it to the active buffer,
 * and notifies the Dart callback that a new frame is ready to render.
 */
static bool ffi_video_sink_push(struct sc_frame_sink* sink,
                                const AVFrame* frame) {
    FfiVideoSink* s_sink = (FfiVideoSink*)sink;
    ScrcpyFfiInstance* s = s_sink->state;

    if (!atomic_load(&s->running) || atomic_load(&s->decode_paused)) {
        return true;
    }

    on_video_frame_ready_cb video_cb = NULL;

    sc_mutex_lock(&s->buffer_mutex);
    s->frame_width = frame->width;
    s->frame_height = frame->height;
    s->frame_serial++;

    if (s->current_frame) {
        av_frame_unref(s->current_frame);
        av_frame_ref(s->current_frame, (AVFrame*)frame);
    }
    video_cb = s->video_cb;
    sc_mutex_unlock(&s->buffer_mutex);

    if (atomic_load(&s->running) && video_cb) {
        video_cb(s, frame->width, frame->height);
    }
    return true;
}

/**
 * @brief Handles stream session updates. Stub function.
 */
static bool ffi_video_sink_push_session(struct sc_frame_sink* sink,
                                        const struct sc_stream_session* session) {
    (void)sink; (void)session;
    return true;
}

/**
 * @brief Video frame sink operations mapping for scrcpy core.
 */
const struct sc_frame_sink_ops kVideoSinkOps = {
    .open         = ffi_video_sink_open,
    .close        = ffi_video_sink_close,
    .push         = ffi_video_sink_push,
    .push_session = ffi_video_sink_push_session,
};

/**
 * @brief Opens the custom audio frame sink and resets the resampling context.
 */
static bool ffi_audio_sink_open(struct sc_frame_sink* sink,
                                const AVCodecContext* ctx,
                                const struct sc_stream_session* session) {
    (void)session;
    FfiAudioSink* s = (FfiAudioSink*)sink;
    // This should normally be a fresh sink. Be defensive if an upstream
    // reconnect reopens it without an intervening close.
    ffi_audio_output_close(s);
    if (s->swr_ctx) {
        swr_free(&s->swr_ctx);
    }
    free(s->swr_buf);
    s->swr_ctx = NULL;
    s->swr_buf = NULL;
    s->swr_buf_allocated_samples = 0;
    s->playback_stream = NULL;
    s->playback_sample_rate = 0;
    s->playback_channels = 0;
    s->applied_decode_generation = atomic_load(&s->state->decode_generation);

    LOGD("FFI Audio sink opened: sample_rate=%d channels=%d format=%d",
         ctx->sample_rate, ctx->ch_layout.nb_channels, ctx->sample_fmt);
    // SDL3 selects CoreAudio/WASAPI/PipeWire/PulseAudio/ALSA for the active
    // platform. Failure only disables speaker output; decoding can still feed
    // the optional legacy Dart PCM callback.
    ffi_audio_output_open(s, ctx->sample_rate, ctx->ch_layout.nb_channels);
    return true;
}

/**
 * @brief Closes the audio sink, releasing the FFmpeg resampler and buffers.
 */
static void ffi_audio_sink_close(struct sc_frame_sink* sink) {
    FfiAudioSink* s_sink = (FfiAudioSink*)sink;
    ffi_audio_output_close(s_sink);
    if (s_sink->swr_ctx) {
        swr_free(&s_sink->swr_ctx);
        s_sink->swr_ctx = NULL;
    }
    if (s_sink->swr_buf) {
        free(s_sink->swr_buf);
        s_sink->swr_buf = NULL;
    }
    s_sink->swr_buf_allocated_samples = 0;
    LOGD("FFI Audio sink closed");
}

/**
 * @brief Receives audio frames, resamples them to S16 stereo format using swresample,
 * and queues it directly to the local SDL3 playback device. The optional Dart
 * callback is a diagnostics/advanced-processing path and is not required for
 * native audio output.
 */
static bool ffi_audio_sink_push(struct sc_frame_sink* sink,
                                const AVFrame* frame) {
    FfiAudioSink* s_sink = (FfiAudioSink*)sink;
    ScrcpyFfiInstance* s = s_sink->state;
    if (atomic_load(&s->decode_paused)) return true;
    if (!frame->data[0]) return true;

    uint64_t generation = atomic_load_explicit(&s->decode_generation,
                                               memory_order_acquire);
    if (s_sink->applied_decode_generation != generation) {
        // avcodec_flush_buffers() runs in the decoder override. Reset the
        // companion resampler on this same decoder thread so no pre-pause PCM
        // sample can bleed into playback after resume.
        if (s_sink->swr_ctx) {
            swr_free(&s_sink->swr_ctx);
        }
        s_sink->applied_decode_generation = generation;
    }

    if (!s_sink->swr_ctx) {
        int ret = swr_alloc_set_opts2(
            &s_sink->swr_ctx,
            &frame->ch_layout,
            AV_SAMPLE_FMT_S16,
            frame->sample_rate,
            &frame->ch_layout,
            frame->format,
            frame->sample_rate,
            0, NULL
        );
        if (ret < 0 || swr_init(s_sink->swr_ctx) < 0) {
            LOGE("FFI Audio: Could not initialize Resampler");
            return false;
        }
    }

    int out_samples = frame->nb_samples;
    if (out_samples > s_sink->swr_buf_allocated_samples) {
        int bytes_per_sample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) * frame->ch_layout.nb_channels;
        uint8_t *new_buf = realloc(s_sink->swr_buf,
                                   out_samples * bytes_per_sample);
        if (!new_buf) {
            LOG_OOM();
            return false;
        }
        s_sink->swr_buf = new_buf;
        s_sink->swr_buf_allocated_samples = out_samples;
    }

    int ret = swr_convert(
        s_sink->swr_ctx,
        &s_sink->swr_buf,
        out_samples,
        (const uint8_t**)frame->data,
        frame->nb_samples
    );
    if (ret < 0) {
        LOGE("FFI Audio: Resampling failed");
        return true;
    }

    if (ret > 0) {
        int byte_count = ret * frame->ch_layout.nb_channels
                       * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
        ffi_audio_output_enqueue(s, s_sink->swr_buf, byte_count);
        if (s->audio_cb) {
            s->audio_cb(
                s,
                (const int16_t*)s_sink->swr_buf,
                ret,
                frame->sample_rate,
                frame->ch_layout.nb_channels
            );
        }
    }
    return true;
}

/**
 * @brief Audio frame sink operations mapping for scrcpy core.
 */
const struct sc_frame_sink_ops kAudioSinkOps = {
    .open  = ffi_audio_sink_open,
    .close = ffi_audio_sink_close,
    .push  = ffi_audio_sink_push,
    .push_session = NULL,
};
