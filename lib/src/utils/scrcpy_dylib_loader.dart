import 'dart:ffi';
import 'dart:io';

/// Loads the [DynamicLibrary] for the scrcpy_ffi native library.
///
/// Supports macOS, Linux, and Windows.
class ScrcpyDylibLoader {
  ScrcpyDylibLoader._();

  static const String _libName = 'scrcpy_ffi';

  /// Opens and returns the platform-appropriate [DynamicLibrary].
  ///
  /// Throws [UnsupportedError] on unsupported platforms.
  static DynamicLibrary load() {
    if (Platform.isMacOS) {
      // The SPM/CocoaPods target links the dynamic framework into the process.
      // Resolve its exported symbols without depending on an app-bundle path.
      return DynamicLibrary.process();
    }
    if (Platform.isLinux) {
      return DynamicLibrary.open('lib$_libName.so');
    }
    if (Platform.isWindows) {
      return DynamicLibrary.open('$_libName.dll');
    }
    throw UnsupportedError(
      'scrcpy_flutter_plugin is not supported on ${Platform.operatingSystem}.',
    );
  }
}
