#ifndef FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_
#define FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_

#include <flutter/texture_registrar.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>


namespace scrcpy_flutter_plugin {

// Converts the decoder's NV12/P010 D3D surface into a BGRA texture on the GPU.
// Flutter's Windows external-texture bridge imports RGBA-compatible D3D11
// textures; handing it the decoder's NV12 texture is not valid.
//
// When D3D11VA hardware acceleration is unavailable the class falls back to a
// software path: sws_scale converts the CPU YUV420P AVFrame to BGRA, which is
// then uploaded via UpdateSubresource().
class ScrcpyVideoTexture {
 public:
  ScrcpyVideoTexture(ID3D11Device* device, ID3D11DeviceContext* context);
  ~ScrcpyVideoTexture();

  void SetInstanceHandle(void* handle) { instance_handle_ = handle; }
  int64_t GetTextureId() const { return texture_id_; }
  void SetTextureId(int64_t id) { texture_id_ = id; }

  const FlutterDesktopGpuSurfaceDescriptor* CopyGpuSurface(size_t width,
                                                            size_t height);

 private:
  void* instance_handle_ = nullptr;
  int64_t texture_id_ = -1;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> processor_enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> output_texture_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
  std::unique_ptr<FlutterDesktopGpuSurfaceDescriptor> gpu_descriptor_;
  std::mutex mutex_;
  HANDLE shared_handle_ = nullptr;

  // HW path tracking
  size_t last_source_width_ = 0;
  size_t last_source_height_ = 0;
  size_t last_visible_width_ = 0;
  size_t last_visible_height_ = 0;
  DXGI_FORMAT last_input_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t last_serial_ = 0;

  // SW path: YUV420P → BGRA via ffi_scrcpy_convert_software_frame_to_bgra + UpdateSubresource
  std::vector<uint8_t> bgra_buffer_;
  UINT last_sw_width_ = 0;
  UINT last_sw_height_ = 0;

  bool ConfigureProcessor(const D3D11_TEXTURE2D_DESC& input_desc,
                          UINT visible_width, UINT visible_height);
  bool ConvertToBgra(ID3D11Texture2D* source, UINT source_index,
                     UINT visible_width, UINT visible_height);
  bool EnsureSoftwareOutputTexture(UINT width, UINT height);
  bool UploadSoftwareFrame(void* av_frame_ptr, int32_t width, int32_t height);
  void ReleaseResources();
};

}  // namespace scrcpy_flutter_plugin

#endif  // FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_
