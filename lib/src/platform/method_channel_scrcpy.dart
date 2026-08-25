import 'package:flutter/services.dart';
import 'scrcpy_platform_interface.dart';

/// [MethodChannel]-based implementation of [ScrcpyPlatform].
///
/// Used as the default implementation on macOS, Linux, and Windows.
class MethodChannelScrcpy extends ScrcpyPlatform {
  static const MethodChannel _channel = MethodChannel('scrcpy_flutter_plugin');

  @override
  Future<int?> createTexture() {
    return _channel.invokeMethod<int>('createTexture');
  }

  @override
  Future<void> disposeTexture(int textureId) {
    return _channel.invokeMethod<void>('disposeTexture', {
      'textureId': textureId,
    });
  }

  @override
  Future<void> notifyFrameAvailable(int textureId) {
    return _channel.invokeMethod<void>('notifyFrameAvailable', {
      'textureId': textureId,
    });
  }

  @override
  Future<void> setTextureHandle(int textureId, int handle) {
    return _channel.invokeMethod<void>('setTextureHandle', {
      'textureId': textureId,
      'handle': handle,
    });
  }
}
