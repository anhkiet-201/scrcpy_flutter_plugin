# scrcpy_flutter_plugin

A Flutter desktop plugin that embeds scrcpy for high-performance Android screen mirroring, low-latency HID input control, GPU-backed video texture rendering, and native local audio playback.

Supported host platforms: **macOS**, **Windows**, and **Linux**.

---

## 🖥️ Supported Host Operating Systems & CPU Architectures

| Host Operating System | Supported CPU Architectures | Native Binary Output | Hardware Acceleration (GPU) | Native Audio Backend |
| :--- | :--- | :--- | :--- | :--- |
| **macOS** (11.0+) | `arm64` (Apple Silicon) | `libscrcpy_ffi.xcframework` | VideoToolbox (H.264, HEVC, AV1) | CoreAudio (SDL3) |
| **Linux** (Ubuntu/Debian) | `x86_64`, `aarch64` | `libscrcpy_ffi.so` | VA-API / DRM | PipeWire / PulseAudio / ALSA (SDL3) |
| **Windows** (10/11) | `x86_64` (x64) | `scrcpy_ffi.dll` | D3D11VA / DXVA2 | WASAPI (SDL3) |

### 📱 Target Android Devices
- **Android OS Version**: Android 5.0+ (API Level 21+)
- **Supported ABIs**: `arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64` (executed via bundled `scrcpy-server`)

---

## Key Features

- **High-Performance Video Decoding**: Direct FFI integration with scrcpy core & FFmpeg for GPU hardware-accelerated video rendering.
- **Low-Latency Input Forwarding**: Direct mouse click, scroll, and touch event forwarding to Android devices.
- **Native Audio Streaming**: Decoded PCM audio is passed directly to native SDL3 audio streams (CoreAudio on macOS, WASAPI on Windows, PipeWire/PulseAudio/ALSA on Linux).
- **Multi-Device Concurrent Mirroring**: Support for controlling multiple independent Android devices concurrently.
- **Device Clipboard Synchronization**: Automatic real-time device clipboard listener and synchronization.
- **Client-Side Pause / Resume**: Control local texture rendering and audio stream playback independently without disconnecting the underlying server session.

---

## Public API

Import only the supported package entry point:

```dart
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';
```

### Basic Controller Usage

`ScrcpyController` is an abstract public contract. Its default factory creates the bundled FFI implementation, while native handles, callbacks, platform transport, and resource lifecycle remain isolated under internal `lib/src`.

```dart
final ScrcpyController controller = ScrcpyController();

// Listen to connection state updates
controller.onStateChanged.listen((state) {
  print('Device state: $state');
});

// Start mirroring session
await controller.start(
  ScrcpyOptions(
    serial: '192.168.1.100:5555', // Or USB ADB serial
    maxSize: 1080,
    videoBitRate: 8000000,
    maxFps: 60.0,
    control: true,
    audio: true,
  ),
);

// Render the native GPU video texture inside your widget tree
ScrcpyTextureWidget(controller: controller);

// Stop mirroring session
controller.stop();

// Dispose controller and stream resources when done
controller.dispose();
```

---

## Lifecycle & Session Management

- **`start(ScrcpyOptions options)`**: Starts the scrcpy server on the target device and begins video/audio demuxing.
- **`pause()`**: Pauses client-side video decoding and audio playback while preserving the active ADB server socket.
- **`resume()`**: Resumes client-side video texture rendering and audio stream playback.
- **`stop()`**: Disconnects the device session and terminates the native scrcpy server.
- **`dispose()`**: Releases Dart stream controllers, texture resources, and native bindings.
- **`ScrcpyController.cleanupAll()`**: Clears any orphaned native scrcpy sessions (e.g. after a Flutter hot restart).

---

## Audio Pipeline

When configuring `ScrcpyOptions(audio: true)`, decoded PCM audio is routed directly to native SDL3 audio drivers. This zero-copy approach eliminates Dart GC pressure and per-frame allocation.

If your application requires raw PCM audio frames for visualizers or processing, initialize the controller with:

```dart
final controller = ScrcpyController(forwardAudioFramesToDart: true);
```

---

## Package & Example App Structure

- `lib/scrcpy_flutter_plugin.dart` — Supported public entry point.
- `lib/src/controllers/` — Abstract controller contract and FFI implementation.
- `lib/src/bindings/`, `lib/src/platform/` — Internal native FFI bindings and texture transport.
- `src/` — Native C/CMake integration linking scrcpy core.

### Modular Example Application (`example/`)

The bundled application in `example/` showcases multi-device mirroring with modular architecture and full English DartDocs:

- `example/lib/models/device_session.dart` — Device session wrapper (`ChangeNotifier`) handling lifecycle and stream listeners.
- `example/lib/widgets/status_badge.dart` — Connection state & pause status badge.
- `example/lib/widgets/device_controls_bar.dart` — Serial input and connection control buttons.
- `example/lib/widgets/device_card.dart` — Integrated card widget hosting controls and `ScrcpyTextureWidget`.
- `example/lib/main.dart` — Multi-device desktop layout demo.
