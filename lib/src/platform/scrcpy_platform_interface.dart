import 'package:plugin_platform_interface/plugin_platform_interface.dart';
import 'method_channel_scrcpy.dart';

/// The platform interface for scrcpy texture operations.
///
/// Platform-specific implementations must extend this class (not implement it).
/// The default implementation uses a [MethodChannel].
abstract class ScrcpyPlatform extends PlatformInterface {
  ScrcpyPlatform() : super(token: _token);

  static final Object _token = Object();

  static ScrcpyPlatform _instance = MethodChannelScrcpy();

  /// The current active instance of [ScrcpyPlatform].
  static ScrcpyPlatform get instance => _instance;

  /// Overrides the default instance for testing or alternative implementations.
  static set instance(ScrcpyPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// Creates a new hardware texture on the platform side.
  ///
  /// Returns the texture ID that can be used with [Texture] widget.
  Future<int?> createTexture();

  /// Releases a texture created by [createTexture].
  Future<void> disposeTexture(int textureId);

  /// Signals the Flutter engine that a new video frame is ready to be drawn.
  Future<void> notifyFrameAvailable(int textureId);

  /// Associates a texture ID with a native scrcpy instance handle.
  Future<void> setTextureHandle(int textureId, int handle);
}
