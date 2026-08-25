import Foundation

/// Locates the embedded native library by its exported API, without depending
/// on its filename or the host application's absolute filesystem layout.
enum ScrcpyNativeLibrary {
    private static let markerSymbol = "ffi_scrcpy_set_adb_path"

    private static let handle: UnsafeMutableRawPointer? = {
        if let processHandle = dlopen(nil, RTLD_NOW | RTLD_GLOBAL),
           dlsym(processHandle, markerSymbol) != nil {
            return processHandle
        }

        guard let frameworksURL = Bundle.main.privateFrameworksURL,
              let entries = try? FileManager.default.contentsOfDirectory(
                  at: frameworksURL,
                  includingPropertiesForKeys: nil
              ) else {
            NSLog("scrcpy_flutter_plugin: native library directory is missing")
            return nil
        }

        for entry in entries where entry.pathExtension == "dylib" {
            guard let candidate = dlopen(
                entry.path,
                RTLD_NOW | RTLD_GLOBAL
            ) else {
                continue
            }
            if dlsym(candidate, markerSymbol) != nil {
                return candidate
            }
            dlclose(candidate)
        }

        NSLog("scrcpy_flutter_plugin: native library is missing")
        return nil
    }()

    static func symbol<T>(_ name: String, as type: T.Type) -> T? {
        guard let handle,
              let rawSymbol = dlsym(handle, name) else {
            return nil
        }
        return unsafeBitCast(rawSymbol, to: type)
    }
}
