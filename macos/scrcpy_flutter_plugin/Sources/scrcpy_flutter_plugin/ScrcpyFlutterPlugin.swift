import Cocoa
import FlutterMacOS

/// Flutter plugin entry point for macOS.
///
/// Handles texture lifecycle via MethodChannel calls from Dart's [ScrcpyPlatform]:
///   - `createTexture`        → registers a [ScrcpyVideoTexture] and returns its ID
///   - `disposeTexture`       → unregisters and removes the texture
///   - `notifyFrameAvailable` → tells the Flutter engine to pull the next frame
public class ScrcpyFlutterPlugin: NSObject, FlutterPlugin {
    private let registrar: FlutterPluginRegistrar
    private let methodChannel: FlutterMethodChannel

    /// Active textures keyed by their Flutter texture ID.
    private var textures: [Int64: ScrcpyVideoTexture] = [:]

    public static func register(with registrar: FlutterPluginRegistrar) {
        configureADBPath()

        let channel = FlutterMethodChannel(
            name: "scrcpy_flutter_plugin",
            binaryMessenger: registrar.messenger
        )
        let instance = ScrcpyFlutterPlugin(registrar: registrar, channel: channel)
        registrar.addMethodCallDelegate(instance, channel: channel)
    }

    private static func findSystemADB() -> String? {
        let task = Process()
        // Use zsh -l to ensure user's PATH (e.g. homebrew) is loaded
        task.launchPath = "/bin/zsh"
        task.arguments = ["-l", "-c", "which adb"]
        
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = Pipe()
        
        do {
            try task.run()
            task.waitUntilExit()
            
            if task.terminationStatus == 0 {
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                if let output = String(data: data, encoding: .utf8) {
                    let path = output.trimmingCharacters(in: .whitespacesAndNewlines)
                    if !path.isEmpty && FileManager.default.isExecutableFile(atPath: path) {
                        return path
                    }
                }
            }
        } catch {
            NSLog("scrcpy_flutter_plugin: Failed to execute which adb: \\(error)")
        }
        
        return nil
    }

    private static func configureADBPath() {
        typealias ConfigureADB = @convention(c) (
            UnsafePointer<CChar>
        ) -> Bool
        guard let configureADB: ConfigureADB = ScrcpyNativeLibrary.symbol(
            "ffi_scrcpy_set_adb_path",
            as: ConfigureADB.self
        ) else {
            NSLog("scrcpy_flutter_plugin: native ADB configuration is unavailable")
            return
        }

        if let systemADB = findSystemADB() {
            let configured = systemADB.withCString {
                configureADB($0)
            }
            if configured {
                NSLog("scrcpy_flutter_plugin: using system ADB at %@", systemADB)
                return
            }
        }

#if SWIFT_PACKAGE
        let resourceBundle = Bundle.module
#else
        let resourceBundle = Bundle(for: ScrcpyFlutterPlugin.self)
#endif

        guard let resourceURL = resourceBundle.url(
            forResource: "adb_macos",
            withExtension: nil
        ) else {
            NSLog("scrcpy_flutter_plugin: adb_macos resource is missing")
            return
        }

        let executableURL = resourceURL
            .resolvingSymlinksInPath()
            .standardizedFileURL
        guard FileManager.default.isExecutableFile(atPath: executableURL.path) else {
            NSLog(
                "scrcpy_flutter_plugin: adb_macos is not executable at %@",
                executableURL.path
            )
            return
        }

        let configured = executableURL.path.withCString {
            configureADB($0)
        }
        guard configured else {
            NSLog("scrcpy_flutter_plugin: could not configure the ADB environment")
            return
        }
        NSLog("scrcpy_flutter_plugin: using bundled ADB at %@", executableURL.path)
    }

    init(registrar: FlutterPluginRegistrar, channel: FlutterMethodChannel) {
        self.registrar = registrar
        self.methodChannel = channel
        super.init()
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {

        case "createTexture":
            let texture = ScrcpyVideoTexture()
            let textureId = registrar.textures.register(texture)
            texture.textureId = textureId
            textures[textureId] = texture
            result(textureId)

        case "disposeTexture":
            guard let args = call.arguments as? [String: Any],
                  let textureId = args["textureId"] as? Int64 else {
                result(FlutterError(
                    code: "INVALID_ARGS",
                    message: "Missing 'textureId' argument",
                    details: nil
                ))
                return
            }
            if textures.removeValue(forKey: textureId) != nil {
                registrar.textures.unregisterTexture(textureId)
            }
            result(nil)

        case "notifyFrameAvailable":
            guard let args = call.arguments as? [String: Any],
                  let textureId = args["textureId"] as? Int64 else {
                result(FlutterError(
                    code: "INVALID_ARGS",
                    message: "Missing 'textureId' argument",
                    details: nil
                ))
                return
            }
            registrar.textures.textureFrameAvailable(textureId)
            result(nil)

        case "setTextureHandle":
            guard let args = call.arguments as? [String: Any],
                  let textureId = args["textureId"] as? Int64,
                  let handleAddress = args["handle"] as? Int64 else {
                result(FlutterError(
                    code: "INVALID_ARGS",
                    message: "Missing 'textureId' or 'handle' argument",
                    details: nil
                ))
                return
            }
            if let texture = textures[textureId] {
                texture.instanceHandle = UnsafeMutableRawPointer(bitPattern: Int(handleAddress))
            }
            result(nil)

        default:
            result(FlutterMethodNotImplemented)
        }
    }
}
