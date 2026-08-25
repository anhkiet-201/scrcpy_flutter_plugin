#ifndef FLUTTER_PLUGIN_SCRCPY_FLUTTER_PLUGIN_H_
#define FLUTTER_PLUGIN_SCRCPY_FLUTTER_PLUGIN_H_

#include <d3d10_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/texture_registrar.h>

#include <memory>
#include <map>

#ifdef FLUTTER_PLUGIN_IMPL
#define SCRCPY_FLUTTER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define SCRCPY_FLUTTER_PLUGIN_EXPORT __declspec(dllimport)
#endif

#if defined(__cplusplus)
extern "C" {
#endif

SCRCPY_FLUTTER_PLUGIN_EXPORT void
ScrcpyFlutterPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

namespace scrcpy_flutter_plugin {

class ScrcpyVideoTexture;

/// Flutter plugin for Windows.
///
/// Handles the same MethodChannel interface as the macOS Swift plugin:
///   - createTexture        → allocate a GPU surface (D3D11 shared texture)
///   - disposeTexture       → release the GPU surface
///   - notifyFrameAvailable → signal the Flutter engine to repaint
class ScrcpyFlutterPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  explicit ScrcpyFlutterPlugin(flutter::PluginRegistrarWindows* registrar);
  ~ScrcpyFlutterPlugin() override;

  // Disallow copy and assign.
  ScrcpyFlutterPlugin(const ScrcpyFlutterPlugin&) = delete;
  ScrcpyFlutterPlugin& operator=(const ScrcpyFlutterPlugin&) = delete;

 private:
  flutter::PluginRegistrarWindows* registrar_;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;

  struct TextureInfo {
    std::unique_ptr<flutter::TextureVariant> variant;
    std::unique_ptr<ScrcpyVideoTexture> texture;
    void* bound_instance_handle = nullptr;
  };
  std::map<int64_t, TextureInfo> textures_;

  bool EnsureD3D11Device();

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

}  // namespace scrcpy_flutter_plugin

#endif  // FLUTTER_PLUGIN_SCRCPY_FLUTTER_PLUGIN_H_
