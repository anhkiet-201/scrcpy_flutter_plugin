#include "include/scrcpy_flutter_plugin/scrcpy_flutter_plugin.h"
#include "scrcpy_video_texture.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <iostream>
#include <iomanip>

// Link external D3D11 setup function from FFI core
extern "C" {
  void ffi_scrcpy_set_d3d11_device(void* handle, void* d3d11_device);
}

namespace scrcpy_flutter_plugin {
namespace {

void LogAdapter(const wchar_t* label, IDXGIAdapter* adapter) {
  if (!adapter) {
    return;
  }
  DXGI_ADAPTER_DESC desc = {};
  const HRESULT hr = adapter->GetDesc(&desc);
  if (FAILED(hr)) {
    std::wcerr << L"FFI Windows: GetDesc(" << label
               << L") failed: HRESULT=0x" << std::hex
               << static_cast<uint32_t>(hr) << std::dec << std::endl;
    return;
  }
  std::wcerr << L"FFI Windows: " << label << L" adapter=\"" << desc.Description
             << L"\" vendor=0x" << std::hex << desc.VendorId
             << L" device=0x" << desc.DeviceId
             << L" luid=" << static_cast<uint32_t>(desc.AdapterLuid.HighPart)
             << L":" << desc.AdapterLuid.LowPart
             << L" dedicated-video-memory=" << std::dec
             << static_cast<uint64_t>(desc.DedicatedVideoMemory) << std::endl;
}

void LogVideoDecoderCapabilities(ID3D11Device* device) {
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
  HRESULT hr = device->QueryInterface(
      IID_ID3D11VideoDevice,
      reinterpret_cast<void**>(video_device.ReleaseAndGetAddressOf()));
  if (FAILED(hr) || !video_device) {
    std::cerr << "FFI Windows: ID3D11VideoDevice unavailable: HRESULT=0x"
              << std::hex << static_cast<uint32_t>(hr) << std::dec << std::endl;
    return;
  }

  BOOL h264_nv12 = FALSE;
  hr = video_device->CheckVideoDecoderFormat(
      &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &h264_nv12);
  std::cerr << "FFI Windows: video-profiles="
            << video_device->GetVideoDecoderProfileCount()
            << " H264_VLD_NOFGT+NV12="
            << (SUCCEEDED(hr) && h264_nv12 ? "yes" : "no")
            << " HRESULT=0x" << std::hex << static_cast<uint32_t>(hr)
            << std::dec << std::endl;
}

}  // namespace

// static
void ScrcpyFlutterPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "scrcpy_flutter_plugin",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<ScrcpyFlutterPlugin>(registrar);

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

ScrcpyFlutterPlugin::ScrcpyFlutterPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar) {}

ScrcpyFlutterPlugin::~ScrcpyFlutterPlugin() {
  // Clear textures and unregister them
  for (auto const& [texture_id, info] : textures_) {
    registrar_->texture_registrar()->UnregisterTexture(texture_id);
  }
  textures_.clear();
}

bool ScrcpyFlutterPlugin::EnsureD3D11Device() {
  if (d3d_device_ && d3d_context_) {
    return true;
  }

  // The decoder texture is consumed by Flutter, so both sides must use the
  // same DXGI adapter. D3D_DRIVER_TYPE_HARDWARE with a null adapter is
  // ambiguous on hybrid-GPU systems and may select a different GPU.
  flutter::FlutterView* view = registrar_->GetView();
  if (!view) {
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  adapter.Attach(view->GetGraphicsAdapter());
  if (!adapter) {
    return false;
  }
  LogAdapter(L"Flutter", adapter.Get());

  d3d_device_.Reset();
  d3d_context_.Reset();
  D3D_FEATURE_LEVEL feature_level;
  const UINT flags =
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  HRESULT hr = D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
      D3D11_SDK_VERSION, d3d_device_.ReleaseAndGetAddressOf(), &feature_level,
      d3d_context_.ReleaseAndGetAddressOf());

  if (FAILED(hr) || !d3d_device_ || !d3d_context_) {
    std::cerr << "FFI Windows: D3D11CreateDevice failed: HRESULT=0x"
              << std::hex << static_cast<uint32_t>(hr) << std::dec << std::endl;
    d3d_device_.Reset();
    d3d_context_.Reset();
    return false;
  }

  // FFmpeg's D3D11VA implementation uses the immediate context from its
  // decoding thread while Flutter uses the same device on the render thread.
  // Match FFmpeg's own D3D11 device setup by enabling thread protection.
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  hr = d3d_device_.As(&multithread);
  if (FAILED(hr) || !multithread) {
    d3d_device_.Reset();
    d3d_context_.Reset();
    return false;
  }
  multithread->SetMultithreadProtected(TRUE);

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> actual_adapter;
  if (SUCCEEDED(d3d_device_.As(&dxgi_device)) && dxgi_device &&
      SUCCEEDED(dxgi_device->GetAdapter(
          actual_adapter.ReleaseAndGetAddressOf()))) {
    LogAdapter(L"decoder", actual_adapter.Get());
  }
  std::cerr << "FFI Windows: D3D11 feature-level=0x" << std::hex
            << static_cast<uint32_t>(feature_level) << std::dec
            << " multithread-protected=yes" << std::endl;
  LogVideoDecoderCapabilities(d3d_device_.Get());
  return true;
}

void ScrcpyFlutterPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  
  if (method_call.method_name() == "createTexture") {
    if (!EnsureD3D11Device()) {
      result->Error("D3D11_DEVICE_ERROR",
                    "Could not create a D3D11 device on Flutter's adapter");
      return;
    }

    auto video_texture = std::make_unique<ScrcpyVideoTexture>(
        d3d_device_.Get(), d3d_context_.Get());
    auto texture_variant = std::make_unique<flutter::TextureVariant>(
        flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [texture_ptr = video_texture.get()](size_t width, size_t height) {
              return texture_ptr->CopyGpuSurface(width, height);
            }));

    int64_t texture_id = registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    if (texture_id < 0) {
      result->Error("TEXTURE_REGISTRATION_ERROR",
                    "Flutter rejected the D3D11 texture");
      return;
    }
    video_texture->SetTextureId(texture_id);

    TextureInfo info{
        std::move(texture_variant),
        std::move(video_texture),
    };
    textures_.emplace(texture_id, std::move(info));

    result->Success(flutter::EncodableValue(texture_id));

  } else if (method_call.method_name() == "disposeTexture") {
    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!arguments) {
      result->Error("INVALID_ARGS", "Missing arguments map");
      return;
    }

    auto it = arguments->find(flutter::EncodableValue("textureId"));
    if (it == arguments->end()) {
      result->Error("INVALID_ARGS", "Missing 'textureId' argument");
      return;
    }

    int64_t texture_id = 0;
    if (std::holds_alternative<int32_t>(it->second)) {
      texture_id = std::get<int32_t>(it->second);
    } else if (std::holds_alternative<int64_t>(it->second)) {
      texture_id = std::get<int64_t>(it->second);
    }

    auto tex_it = textures_.find(texture_id);
    if (tex_it != textures_.end()) {
      registrar_->texture_registrar()->UnregisterTexture(texture_id);
      textures_.erase(tex_it);
    }
    result->Success();

  } else if (method_call.method_name() == "setTextureHandle") {
    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!arguments) {
      result->Error("INVALID_ARGS", "Missing arguments map");
      return;
    }

    auto it_id = arguments->find(flutter::EncodableValue("textureId"));
    auto it_handle = arguments->find(flutter::EncodableValue("handle"));
    if (it_id == arguments->end() || it_handle == arguments->end()) {
      result->Error("INVALID_ARGS", "Missing 'textureId' or 'handle' argument");
      return;
    }

    int64_t texture_id = 0;
    if (std::holds_alternative<int32_t>(it_id->second)) {
      texture_id = std::get<int32_t>(it_id->second);
    } else if (std::holds_alternative<int64_t>(it_id->second)) {
      texture_id = std::get<int64_t>(it_id->second);
    }

    int64_t handle_addr = 0;
    if (std::holds_alternative<int32_t>(it_handle->second)) {
      handle_addr = std::get<int32_t>(it_handle->second);
    } else if (std::holds_alternative<int64_t>(it_handle->second)) {
      handle_addr = std::get<int64_t>(it_handle->second);
    }

    auto tex_it = textures_.find(texture_id);
    if (tex_it != textures_.end()) {
      void* instance_handle = reinterpret_cast<void*>(handle_addr);
      tex_it->second.texture->SetInstanceHandle(instance_handle);

      // The FFI function takes its own COM reference. The plugin and decoder
      // therefore share the exact device without ambiguous ownership.
      if (instance_handle &&
          tex_it->second.bound_instance_handle != instance_handle) {
        ffi_scrcpy_set_d3d11_device(instance_handle, d3d_device_.Get());
        tex_it->second.bound_instance_handle = instance_handle;
      }
    }
    result->Success();

  } else if (method_call.method_name() == "notifyFrameAvailable") {
    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!arguments) {
      result->Error("INVALID_ARGS", "Missing arguments map");
      return;
    }

    auto it = arguments->find(flutter::EncodableValue("textureId"));
    if (it == arguments->end()) {
      result->Error("INVALID_ARGS", "Missing 'textureId' argument");
      return;
    }

    int64_t texture_id = 0;
    if (std::holds_alternative<int32_t>(it->second)) {
      texture_id = std::get<int32_t>(it->second);
    } else if (std::holds_alternative<int64_t>(it->second)) {
      texture_id = std::get<int64_t>(it->second);
    }

    registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id);
    result->Success();

  } else {
    result->NotImplemented();
  }
}

}  // namespace scrcpy_flutter_plugin

void ScrcpyFlutterPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  scrcpy_flutter_plugin::ScrcpyFlutterPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
