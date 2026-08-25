#include "scrcpy_video_texture.h"

#include "../src/scrcpy_ffi.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <drm_fourcc.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixfmt.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <unistd.h>

namespace {

constexpr size_t kMaxPendingFrames = 4;

struct ImportedFrame {
  ScrcpyGpuFrame lease = {};
  VADRMPRIMESurfaceDescriptor descriptor = {};
  EGLImageKHR y_image = EGL_NO_IMAGE_KHR;
  EGLImageKHR uv_image = EGL_NO_IMAGE_KHR;
  GLuint y_texture = 0;
  GLuint uv_texture = 0;
  GLsync fence = nullptr;
};

static GLuint compile_shader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }
  glDeleteShader(shader);
  return 0;
}

static GLuint create_program() {
  // GLSL 1.20 keeps this usable on the GL compatibility context used by the
  // Linux embedding. The output is still a GPU-owned RGBA8 texture.
  static const char kVertexShader[] =
      "attribute vec2 a_position;\n"
      "attribute vec2 a_texcoord;\n"
      "varying vec2 v_texcoord;\n"
      "void main() {\n"
      "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
      "  v_texcoord = a_texcoord;\n"
      "}\n";
  static const char kFragmentShader[] =
      "uniform sampler2D u_y;\n"
      "uniform sampler2D u_uv;\n"
      "uniform float u_limited_range;\n"
      "uniform float u_bt601;\n"
      "varying vec2 v_texcoord;\n"
      "void main() {\n"
      "  float y = texture2D(u_y, v_texcoord).r;\n"
      "  vec2 uv = texture2D(u_uv, v_texcoord).rg - vec2(0.5);\n"
      "  y = mix(y, (y - 16.0 / 255.0) * (255.0 / 219.0), u_limited_range);\n"
      "  vec3 rgb601 = vec3(y + 1.402 * uv.y,\n"
      "                     y - 0.344136 * uv.x - 0.714136 * uv.y,\n"
      "                     y + 1.772 * uv.x);\n"
      "  vec3 rgb709 = vec3(y + 1.5748 * uv.y,\n"
      "                     y - 0.187324 * uv.x - 0.468124 * uv.y,\n"
      "                     y + 1.8556 * uv.x);\n"
      "  gl_FragColor = vec4(clamp(mix(rgb709, rgb601, u_bt601), 0.0, 1.0), 1.0);\n"
      "}\n";

  GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex || !fragment) {
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "a_position");
  glBindAttribLocation(program, 1, "a_texcoord");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    return program;
  }
  glDeleteProgram(program);
  return 0;
}

}  // namespace

struct _ScrcpyVideoTexture {
  FlTextureGL parent_instance;
  void* instance_handle;
  std::mutex mutex;

  EGLDisplay display;
  GLuint output_texture;
  GLuint framebuffer;
  GLuint program;
  GLuint vertex_buffer;
  int32_t width;
  int32_t height;
  uint64_t last_serial;
  std::vector<ImportedFrame> pending_frames;
};

G_DEFINE_TYPE(ScrcpyVideoTexture, scrcpy_video_texture, fl_texture_gl_get_type())

static void close_exported_fds(VADRMPRIMESurfaceDescriptor* descriptor) {
  for (uint32_t i = 0; i < descriptor->num_objects; ++i) {
    if (descriptor->objects[i].fd >= 0) {
      close(descriptor->objects[i].fd);
      descriptor->objects[i].fd = -1;
    }
  }
}

static void release_imported_frame(ScrcpyVideoTexture* self, ImportedFrame* frame) {
  if (frame->fence) {
    glDeleteSync(frame->fence);
    frame->fence = nullptr;
  }
  if (frame->y_texture) glDeleteTextures(1, &frame->y_texture);
  if (frame->uv_texture) glDeleteTextures(1, &frame->uv_texture);
  if (frame->y_image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(self->display, frame->y_image);
  }
  if (frame->uv_image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(self->display, frame->uv_image);
  }
  close_exported_fds(&frame->descriptor);
  ffi_scrcpy_release_gpu_frame(&frame->lease);
  *frame = ImportedFrame{};
}

static void retire_completed_frames(ScrcpyVideoTexture* self) {
  auto& frames = self->pending_frames;
  auto it = frames.begin();
  while (it != frames.end()) {
    GLenum result = glClientWaitSync(it->fence, 0, 0);
    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
      release_imported_frame(self, &*it);
      it = frames.erase(it);
    } else {
      ++it;
    }
  }
}

static bool ensure_output_texture(ScrcpyVideoTexture* self, int32_t width,
                                  int32_t height) {
  if (!self->output_texture) {
    glGenTextures(1, &self->output_texture);
    glGenFramebuffers(1, &self->framebuffer);
  }
  if (self->width == width && self->height == height) {
    return true;
  }
  glBindTexture(GL_TEXTURE_2D, self->output_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  self->width = width;
  self->height = height;
  return glGetError() == GL_NO_ERROR;
}

static bool initialize_gl(ScrcpyVideoTexture* self) {
  if (self->program) return true;
  self->display = eglGetCurrentDisplay();
  if (self->display == EGL_NO_DISPLAY) return false;
  self->program = create_program();
  if (!self->program) return false;
  static const GLfloat kQuad[] = {
      -1.f, -1.f, 0.f, 0.f, 1.f, -1.f, 1.f, 0.f,
      -1.f,  1.f, 0.f, 1.f, 1.f,  1.f, 1.f, 1.f,
  };
  glGenBuffers(1, &self->vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  return glGetError() == GL_NO_ERROR;
}

static EGLImageKHR import_layer(ScrcpyVideoTexture* self,
                                const VADRMPRIMESurfaceDescriptor& descriptor,
                                const VADRMPRIMESurfaceDescriptorLayer& layer,
                                int width, int height) {
  if (layer.num_planes != 1 || layer.planes[0].object_index >= descriptor.num_objects) {
    return EGL_NO_IMAGE_KHR;
  }
  const auto& plane = layer.planes[0];
  const auto& object = descriptor.objects[plane.object_index];
  std::array<EGLint, 32> attributes = {
      EGL_WIDTH, width,
      EGL_HEIGHT, height,
      EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(layer.drm_format),
      EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.pitch),
      EGL_NONE,
  };
  size_t count = 12;
  if (object.drm_format_modifier != DRM_FORMAT_MOD_INVALID) {
    attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attributes[count++] = static_cast<EGLint>(object.drm_format_modifier & 0xffffffffULL);
    attributes[count++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attributes[count++] = static_cast<EGLint>(object.drm_format_modifier >> 32U);
    attributes[count++] = EGL_NONE;
  }
  return eglCreateImageKHR(self->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           nullptr, attributes.data());
}

static bool bind_image_texture(EGLImageKHR image, GLuint* texture) {
  if (image == EGL_NO_IMAGE_KHR) return false;
  glGenTextures(1, texture);
  glBindTexture(GL_TEXTURE_2D, *texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
  return glGetError() == GL_NO_ERROR;
}

static bool import_vaapi_frame(ScrcpyVideoTexture* self, ScrcpyGpuFrame lease,
                               ImportedFrame* imported) {
  imported->lease = lease;
  for (size_t i = 0; i < std::size(imported->descriptor.objects); ++i) {
    imported->descriptor.objects[i].fd = -1;
  }
  auto* frame = static_cast<AVFrame*>(lease.frame_ref);
  if (!frame || !frame->hw_frames_ctx || !frame->hw_frames_ctx->data) return false;
  auto* frames = static_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
  if (!frames->device_ref || !frames->device_ref->data) return false;
  auto* device = static_cast<AVHWDeviceContext*>(frames->device_ref->data);
  auto* vaapi = static_cast<AVVAAPIDeviceContext*>(device->hwctx);
  if (!vaapi || !vaapi->display) return false;

  VAStatus status = vaExportSurfaceHandle(
      vaapi->display, static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3])),
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
      &imported->descriptor);
  if (status != VA_STATUS_SUCCESS || imported->descriptor.num_layers != 2) {
    return false;
  }

  const VADRMPRIMESurfaceDescriptorLayer* y_layer = nullptr;
  const VADRMPRIMESurfaceDescriptorLayer* uv_layer = nullptr;
  for (uint32_t i = 0; i < imported->descriptor.num_layers; ++i) {
    const auto& layer = imported->descriptor.layers[i];
    if (layer.drm_format == DRM_FORMAT_R8) y_layer = &layer;
    if (layer.drm_format == DRM_FORMAT_GR88) uv_layer = &layer;
  }
  if (!y_layer || !uv_layer) return false;

  imported->y_image = import_layer(self, imported->descriptor, *y_layer,
                                   lease.width, lease.height);
  imported->uv_image = import_layer(self, imported->descriptor, *uv_layer,
                                    lease.width / 2, lease.height / 2);
  return bind_image_texture(imported->y_image, &imported->y_texture) &&
         bind_image_texture(imported->uv_image, &imported->uv_texture);
}

static bool render_imported_frame(ScrcpyVideoTexture* self, ImportedFrame* frame) {
  if (!ensure_output_texture(self, frame->lease.width, frame->lease.height)) return false;

  GLint old_program = 0;
  GLint old_framebuffer = 0;
  GLint old_array_buffer = 0;
  GLint old_active_texture = 0;
  GLint old_texture0 = 0;
  GLint old_texture1 = 0;
  GLint old_viewport[4] = {};
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_texture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture0);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture1);
  glGetIntegerv(GL_VIEWPORT, old_viewport);

  glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         self->output_texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
  glViewport(0, 0, frame->lease.width, frame->lease.height);
  glUseProgram(self->program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, frame->y_texture);
  glUniform1i(glGetUniformLocation(self->program, "u_y"), 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, frame->uv_texture);
  glUniform1i(glGetUniformLocation(self->program, "u_uv"), 1);
  glUniform1f(glGetUniformLocation(self->program, "u_limited_range"),
              frame->lease.color_range == AVCOL_RANGE_JPEG ? 0.f : 1.f);
  const bool bt601 = frame->lease.color_space == AVCOL_SPC_BT470BG ||
                     frame->lease.color_space == AVCOL_SPC_SMPTE170M;
  glUniform1f(glGetUniformLocation(self->program, "u_bt601"), bt601 ? 1.f : 0.f);
  glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  frame->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
  glBindBuffer(GL_ARRAY_BUFFER, old_array_buffer);
  glUseProgram(old_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, old_texture0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, old_texture1);
  glActiveTexture(old_active_texture);
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
  return frame->fence != nullptr;
}

static gboolean scrcpy_video_texture_populate(FlTextureGL* texture,
                                              uint32_t* target,
                                              uint32_t* name,
                                              uint32_t* width,
                                              uint32_t* height,
                                              GError** error) {
  (void)error;
  auto* self = SCRCPY_VIDEO_TEXTURE(texture);
  std::lock_guard<std::mutex> lock(self->mutex);
  if (!self->instance_handle || !initialize_gl(self)) return FALSE;

  retire_completed_frames(self);
  ScrcpyGpuFrame lease = {};
  if (ffi_scrcpy_acquire_gpu_frame(self->instance_handle, &lease)) {
    if (lease.backend == SCRCPY_GPU_FRAME_VAAPI && lease.serial != self->last_serial &&
        self->pending_frames.size() < kMaxPendingFrames) {
      ImportedFrame imported = {};
      if (import_vaapi_frame(self, lease, &imported) &&
          render_imported_frame(self, &imported)) {
        self->last_serial = lease.serial;
        self->pending_frames.emplace_back(std::move(imported));
      } else {
        release_imported_frame(self, &imported);
      }
    } else {
      ffi_scrcpy_release_gpu_frame(&lease);
    }
  }

  if (!self->output_texture || self->width <= 0 || self->height <= 0) return FALSE;
  *target = GL_TEXTURE_2D;
  *name = self->output_texture;
  *width = self->width;
  *height = self->height;
  return TRUE;
}

static void scrcpy_video_texture_class_init(ScrcpyVideoTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = scrcpy_video_texture_populate;
}

static void scrcpy_video_texture_init(ScrcpyVideoTexture* self) {
  self->instance_handle = nullptr;
  self->display = EGL_NO_DISPLAY;
  self->output_texture = 0;
  self->framebuffer = 0;
  self->program = 0;
  self->vertex_buffer = 0;
  self->width = 0;
  self->height = 0;
  self->last_serial = 0;
}

ScrcpyVideoTexture* scrcpy_video_texture_new(void) {
  return SCRCPY_VIDEO_TEXTURE(g_object_new(scrcpy_video_texture_get_type(), nullptr));
}

void scrcpy_video_texture_set_handle(ScrcpyVideoTexture* self, void* handle) {
  std::lock_guard<std::mutex> lock(self->mutex);
  self->instance_handle = handle;
}
