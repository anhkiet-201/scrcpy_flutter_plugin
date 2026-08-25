#ifndef FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_
#define FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_

#include <flutter/texture_registrar.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace scrcpy_flutter_plugin {

// Converts the decoder's NV12/P010 D3D surface into a BGRA texture on the GPU.
// Flutter's Windows external-texture bridge imports RGBA-compatible D3D11
// textures; handing it the decoder's NV12 texture is not valid.
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

  size_t last_source_width_ = 0;
  size_t last_source_height_ = 0;
  size_t last_visible_width_ = 0;
  size_t last_visible_height_ = 0;
  DXGI_FORMAT last_input_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t last_serial_ = 0;

  bool ConfigureProcessor(const D3D11_TEXTURE2D_DESC& input_desc,
                          UINT visible_width, UINT visible_height);
  bool ConvertToBgra(ID3D11Texture2D* source, UINT source_index,
                     UINT visible_width, UINT visible_height);
  void ReleaseResources();
};

}  // namespace scrcpy_flutter_plugin

#endif  // FLUTTER_PLUGIN_SCRCPY_VIDEO_TEXTURE_H_
