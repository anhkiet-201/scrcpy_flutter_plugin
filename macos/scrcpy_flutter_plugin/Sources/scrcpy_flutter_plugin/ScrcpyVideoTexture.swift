import Cocoa
import FlutterMacOS

/// Provides CVPixelBuffer frames from the scrcpy native YUV buffer to the Flutter texture engine.
///
/// This class is registered with Flutter's texture registry and is called by the engine
/// whenever `textureFrameAvailable(_:)` is invoked from `ScrcpyFlutterPlugin`.
class ScrcpyVideoTexture: NSObject, FlutterTexture {
    var textureId: Int64 = 0
    var instanceHandle: UnsafeMutableRawPointer? = nil

    // Loaded once; shared across all instances. GPU-only mode deliberately
    // exposes no CPU NV12 copy fallback.
    private static var getCVPixelBufferFunc: (@convention(c) (
        UnsafeMutableRawPointer?
    ) -> UnsafeMutableRawPointer?)?
    private static var dlLoaded = false

    private static func loadDylib() -> Bool {
        if dlLoaded { return getCVPixelBufferFunc != nil }
        dlLoaded = true

        typealias GetCVPixelBufferFunc = @convention(c) (
            UnsafeMutableRawPointer?
        ) -> UnsafeMutableRawPointer?

        getCVPixelBufferFunc = ScrcpyNativeLibrary.symbol(
            "ffi_scrcpy_get_cv_pixel_buffer",
            as: GetCVPixelBufferFunc.self
        )
        return getCVPixelBufferFunc != nil
    }

    /// Called by the Flutter engine when `textureFrameAvailable` fires.
    func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
        guard Self.loadDylib() else { return nil }

        // ffi_scrcpy_get_cv_pixel_buffer() returns an existing +1 retain.
        // Transfer it to Flutter; passRetained would leak one buffer per frame.
        if let getCVPixelBuffer = Self.getCVPixelBufferFunc {
            if let pbPtr = getCVPixelBuffer(instanceHandle) {
                return Unmanaged<CVPixelBuffer>.fromOpaque(pbPtr)
            }
        }
        return nil
    }
}
