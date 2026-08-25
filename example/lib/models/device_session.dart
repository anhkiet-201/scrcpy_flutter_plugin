import 'dart:async';
import 'package:flutter/material.dart';
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';

/// Represents a single Scrcpy device session, managing its controller,
/// connection state, aspect ratio, clipboard sync, and lifecycle subscriptions.
class DeviceSession extends ChangeNotifier {
  /// Unique identifier for this device session (e.g., 1, 2).
  final int id;

  /// Display name for the device session.
  final String name;

  /// Text editing controller for the device IP or ADB serial.
  final TextEditingController serialController;

  /// Underlying Scrcpy controller interfacing with native scrcpy engine.
  final ScrcpyController controller;

  /// Current connection state of the device.
  ScrcpyState _state = ScrcpyState.disconnected;

  /// Calculated aspect ratio (width / height) based on incoming video frames.
  double _aspectRatio = 9 / 16;

  /// Last received text from the device clipboard.
  String _lastClipboard = '';

  /// Optional callback invoked when new clipboard text is synced from the device.
  void Function(String text)? onClipboardReceived;

  /// Stream subscriptions for listening to controller events.
  StreamSubscription<ScrcpyState>? _stateSub;
  StreamSubscription<Size>? _frameSub;
  StreamSubscription<String>? _clipboardSub;

  /// Creates a new [DeviceSession] with an initial serial/IP string.
  DeviceSession({
    required this.id,
    required this.name,
    String initialSerial = '',
    this.onClipboardReceived,
  })  : serialController = TextEditingController(text: initialSerial),
        controller = ScrcpyController() {
    _initSubscriptions();
  }

  /// Current connection state.
  ScrcpyState get state => _state;

  /// Aspect ratio of the video stream.
  double get aspectRatio => _aspectRatio;

  /// Last synced clipboard text.
  String get lastClipboard => _lastClipboard;

  /// Whether the session is currently connected.
  bool get isConnected => _state == ScrcpyState.connected;

  /// Whether the session is currently in the process of connecting.
  bool get isConnecting => _state == ScrcpyState.connecting;

  /// Whether video rendering is locally paused on this client.
  bool get isPaused => controller.isPaused;

  /// Initializes stream listeners for connection state changes, video frame sizes,
  /// and clipboard synchronization.
  void _initSubscriptions() {
    // Listen to connection state updates
    _stateSub = controller.onStateChanged.listen((newState) {
      _state = newState;
      notifyListeners();
    });

    // Listen to video frame dimension changes to calculate exact aspect ratio
    _frameSub = controller.onVideoFrame.listen((size) {
      if (size.height > 0) {
        final newRatio = size.width / size.height;
        if ((_aspectRatio - newRatio).abs() > 0.001) {
          _aspectRatio = newRatio;
          notifyListeners();
        }
      }
    });

    // Listen to clipboard sync events from the device
    _clipboardSub = controller.onSyncClipboard.listen((text) {
      _lastClipboard = text;
      onClipboardReceived?.call(text);
      notifyListeners();
    });
  }

  /// Starts mirroring the device using specified or default Scrcpy options.
  Future<void> start([ScrcpyOptions? customOptions]) async {
    final serial = serialController.text.trim();
    final options = customOptions ??
        ScrcpyOptions(
          serial: serial,
          portRangeFirst: 27100 + (id * 10),
          portRangeLast: 27100 + (id * 10) + 9,
          maxSize: 1080,
          videoCodec: VideoCodec.h264,
          maxFps: 60.0,
          videoBitRate: 8000000,
          control: true,
          audio: true,
          showTouches: false,
          stayAwake: true,
          cleanup: false,
        );

    try {
      await controller.start(options);
    } catch (e) {
      _state = ScrcpyState.error;
      notifyListeners();
      rethrow;
    }
  }

  /// Stops the Scrcpy session and disconnects from the device.
  void stop() {
    controller.stop();
  }

  /// Pauses client-side video texture rendering.
  Future<void> pause() async {
    await controller.pause();
    notifyListeners();
  }

  /// Resumes client-side video texture rendering.
  Future<void> resume() async {
    await controller.resume();
    notifyListeners();
  }

  /// Cancels all active subscriptions and disposes of controllers.
  @override
  void dispose() {
    _stateSub?.cancel();
    _frameSub?.cancel();
    _clipboardSub?.cancel();
    controller.dispose();
    serialController.dispose();
    super.dispose();
  }
}
