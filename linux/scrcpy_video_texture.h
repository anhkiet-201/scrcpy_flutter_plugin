#ifndef SCRCPY_VIDEO_TEXTURE_H_
#define SCRCPY_VIDEO_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

#define SCRCPY_TYPE_VIDEO_TEXTURE (scrcpy_video_texture_get_type())
G_DECLARE_FINAL_TYPE(ScrcpyVideoTexture, scrcpy_video_texture, SCRCPY,
                     VIDEO_TEXTURE, FlTextureGL)

ScrcpyVideoTexture* scrcpy_video_texture_new(void);
void scrcpy_video_texture_set_handle(ScrcpyVideoTexture* self, void* handle);

#endif  // SCRCPY_VIDEO_TEXTURE_H_
