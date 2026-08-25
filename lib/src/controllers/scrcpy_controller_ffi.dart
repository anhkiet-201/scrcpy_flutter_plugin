import 'dart:async';
import 'dart:ffi' hide Size;
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui';
import 'package:ffi/ffi.dart';
import '../bindings/scrcpy_bindings.dart';
import '../models/scrcpy_audio_frame.dart';
import '../models/scrcpy_options.dart';
import '../models/scrcpy_state.dart';
import '../platform/scrcpy_platform_interface.dart';
import '../utils/scrcpy_dylib_loader.dart';
import 'scrcpy_controller.dart';

/// Internal FFI-backed implementation of [ScrcpyController].
///
/// This type intentionally lives under `lib/src`; consumers should depend on
/// the public [ScrcpyController] contract instead of this implementation.
final class FfiScrcpyController implements ScrcpyController {
  /// Map of active scrcpy handles to their corresponding controllers.
  static final Map<int, FfiScrcpyController> _controllers = {};
  static bool _dartApiInitialized = false;

  /// Clean up and stop all active scrcpy instances running in the background.
  ///
  /// This should be called on app startup to prevent orphaned FFI callbacks
  /// from crashing the Dart VM after a Hot Restart.
  static void cleanupAll() {
    try {
      final bindings = ScrcpyFfiBindings(ScrcpyDylibLoader.load());
      bindings.ffi_scrcpy_cleanup_all();
    } catch (e) {
      // ignore: avoid_print
      print('Error during scrcpy global cleanup: $e');
    }
  }

  /// Creates a new controller.
  ///
  /// Native playback is enabled whenever [ScrcpyOptions.audio] is true. Set
  /// [forwardAudioFramesToDart] only for analysis or custom processing: it
  /// copies every PCM block into Dart and is therefore not suitable for the
  /// normal realtime playback path.
  FfiScrcpyController({this.forwardAudioFramesToDart = false}) {
    _bindings = ScrcpyFfiBindings(ScrcpyDylibLoader.load());
  }

  late final ScrcpyFfiBindings _bindings;

  @override
  final bool forwardAudioFramesToDart;
  Pointer<Void>? _handle;
  int? _textureId;
  bool _isPaused = false;

  /// The active texture ID allocated on the platform side.
  /// Null if the video stream is not initialized.
  @override
  int? get textureId => _textureId;

  @override
  Size? get videoSize => _lastVideoSize;

  /// True when this controller has stopped local video/audio decoding.
  @override
  bool get isPaused => _isPaused;

  final _stateController = StreamController<ScrcpyState>.broadcast();
  final _clipboardController = StreamController<String>.broadcast();
  final _videoFrameController = StreamController<Size>.broadcast();
  final _audioFrameController = StreamController<ScrcpyAudioFrame>.broadcast();

  /// The current state of the scrcpy session.
  @override
  ScrcpyState? currentState;

  /// Emits the new [ScrcpyState] each time the connection state changes.
  @override
  Stream<ScrcpyState> get onStateChanged => _stateController.stream;

  /// Emits clipboard text when the Android device's clipboard changes.
  @override
  Stream<String> get onSyncClipboard => _clipboardController.stream;

  /// Emits the [Size] of the video frame each time a new frame is decoded.
  @override
  Stream<Size> get onVideoFrame => _videoFrameController.stream;

  /// Emits decoded raw PCM audio frames only when [forwardAudioFramesToDart]
  /// was enabled before starting. Native playback never uses this stream.
  @override
  Stream<ScrcpyAudioFrame> get onAudioFrame => _audioFrameController.stream;

  NativeCallable<Void Function(Pointer<Void>, Int32, Int32)>?
  _videoFrameListener;
  NativeCallable<
    Void Function(Pointer<Void>, Pointer<Int16>, Int32, Int32, Int32)
  >?
  _audioFrameListener;
  NativeCallable<Void Function(Pointer<Void>, Pointer<Char>)>?
  _clipboardListener;

  Pointer<Pointer<Utf8>>? _currentArgv;
  List<Pointer<Utf8>> _allocatedStrings = [];
  Size? _lastVideoSize;
  bool _frameNotificationPending = false;
  int _renderSession = 0;
  ReceivePort? _statePort;
  StreamSubscription<dynamic>? _statePortSubscription;

  /// Disposes of the controller and releases resources.
  @override
  void dispose() {
    stop();
    if (_textureId != null) {
      ScrcpyPlatform.instance.disposeTexture(_textureId!);
      _textureId = null;
    }
    _stateController.close();
    _clipboardController.close();
    _videoFrameController.close();
    _audioFrameController.close();
  }

  /// Publishes a state change without changing the native session lifecycle.
  ///
  /// This is deliberately separate from [_addState]: an explicit [stop] must
  /// not schedule a later cleanup which could close callbacks belonging to a
  /// session started immediately afterwards.
  void _emitState(ScrcpyState state) {
    currentState = state;
    if (!_stateController.isClosed) {
      _stateController.add(currentState!);
    }
  }

  void _addState(ScrcpyState state) {
    _emitState(state);
    if (state == ScrcpyState.disconnected || state == ScrcpyState.error) {
      if (_handle != null) {
        // State notifications arrive through a Dart ReceivePort, not on a
        // native callback stack. Cleanup is therefore safe here and, unlike a
        // deferred microtask, cannot tear down a newly started session.
        stop();
      }
    }
  }

  void _releaseCallbacks() {
    _detachStateEvents();
    final oldVideo = _videoFrameListener;
    final oldAudio = _audioFrameListener;
    final oldClip = _clipboardListener;

    _videoFrameListener = null;
    _audioFrameListener = null;
    _clipboardListener = null;
    _freeArgs();

    if (oldVideo != null || oldAudio != null || oldClip != null) {
      Timer(const Duration(milliseconds: 100), () {
        oldVideo?.close();
        oldAudio?.close();
        oldClip?.close();
      });
    }

    if (_handle != null) {
      _controllers.remove(_handle!.address);
      _handle = null;
    }
  }

  /// Starts a scrcpy session with the given [options].
  ///
  /// Throws an [Exception] if the native layer fails to start.
  /// The connection status is reported asynchronously via [onStateChanged].
  @override
  Future<void> start(ScrcpyOptions options) async {
    if (_handle != null) {
      // A terminal native state may be observed just before its ReceivePort
      // event is dispatched. Finish that old session before replacing its
      // callbacks and argument storage.
      stop();
    }

    if (!_dartApiInitialized) {
      final initialized = _bindings.ffi_scrcpy_init_dart_api(
        NativeApi.initializeApiDLData,
      );
      if (!initialized) {
        throw Exception('Failed to initialize Dart native message API');
      }
      _dartApiInitialized = true;
    }

    _textureId ??= await ScrcpyPlatform.instance.createTexture();
    if (_textureId == null) {
      throw Exception('Failed to create texture');
    }

    final argsList = ['scrcpy', ...options.toArgs()];
    final argc = argsList.length;

    final argv = calloc<Pointer<Utf8>>(argc + 1);
    final allocatedStrings = <Pointer<Utf8>>[];

    for (var i = 0; i < argc; i++) {
      final ptr = argsList[i].toNativeUtf8();
      argv[i] = ptr;
      allocatedStrings.add(ptr);
    }
    argv[argc] = nullptr;

    _videoFrameListener =
        NativeCallable<Void Function(Pointer<Void>, Int32, Int32)>.listener(
          _videoFrameCallback,
        );
    final Pointer<
      NativeFunction<
        Void Function(Pointer<Void>, Pointer<Int16>, Int32, Int32, Int32)
      >
    >
    audioCallback;
    if (forwardAudioFramesToDart) {
      _audioFrameListener =
          NativeCallable<
            Void Function(Pointer<Void>, Pointer<Int16>, Int32, Int32, Int32)
          >.listener(_audioFrameCallback);
      audioCallback = _audioFrameListener!.nativeFunction;
    } else {
      audioCallback = nullptr
          .cast<
            NativeFunction<
              Void Function(Pointer<Void>, Pointer<Int16>, Int32, Int32, Int32)
            >
          >();
    }
    _clipboardListener =
        NativeCallable<Void Function(Pointer<Void>, Pointer<Char>)>.listener(
          _clipboardChangedCallback,
        );

    _currentArgv = argv;
    _allocatedStrings = allocatedStrings;

    final handle = _bindings.ffi_scrcpy_start(
      argc,
      argv.cast<Pointer<Char>>(),
      _videoFrameListener!.nativeFunction,
      audioCallback,
      nullptr,
      _clipboardListener!.nativeFunction,
    );

    if (handle == nullptr) {
      _freeArgs();
      throw Exception('scrcpy failed to start');
    }

    _handle = handle;
    _isPaused = false;
    // A stopped controller may be started again with the same resolution.
    // The texture widget is recreated on reconnect and needs a first size
    // event even when that size has not changed since the previous session.
    _lastVideoSize = null;
    _frameNotificationPending = false;
    _renderSession++;
    _controllers[handle.address] = this;
    _attachStateEvents(handle);

    await ScrcpyPlatform.instance.setTextureHandle(_textureId!, handle.address);
  }

  /// Stops the current scrcpy session. Safe to call multiple times.
  ///
  /// Phase 1 (synchronous, instant): signals the native layer to stop —
  /// sets running=false, nulls callbacks, interrupts the ADB tunnel.
  /// Phase 2 (async, background): joins the native worker thread and frees
  /// memory on a separate isolate so the UI thread is never blocked.
  @override
  void stop() {
    if (_handle == null) return;

    final handle = _handle!;
    _isPaused = false;
    // Invalidate completions from any platform frame notification belonging
    // to this session. They must not affect the next start().
    _renderSession++;
    _frameNotificationPending = false;
    _lastVideoSize = null;
    _detachStateEvents(handle);
    _handle = null;
    _controllers.remove(handle.address);

    // Do not call _addState() here: its terminal-state behavior is reserved
    // for notifications from the current native session. Calling it from
    // stop() used to schedule a stale callback cleanup that could run after a
    // new start().
    _emitState(ScrcpyState.disconnected);

    // Phase 1 — non-blocking: signal the native side to stop immediately.
    // This returns in microseconds and keeps the UI thread responsive.
    _bindings.ffi_scrcpy_signal_stop(handle);

    // Phase 2 — blocking join runs on a background isolate so the UI is free.
    // ffi_scrcpy_stop() is idempotent when signal_stop already ran: it skips
    // re-signaling and goes straight to sc_thread_join + free.
    _bindings.ffi_scrcpy_stop(handle);


    _releaseCallbacks();
  }

  /// Stops local video/audio decode while retaining the scrcpy connection and
  /// GPU resources. Flutter input/control is ignored until [resume]. Calling
  /// it repeatedly is safe.
  @override
  Future<void> pause() async {
    final handle = _handle;
    if (handle == null || !_bindings.ffi_scrcpy_is_running(handle)) {
      throw StateError('Cannot pause an inactive scrcpy session');
    }
    if (_isPaused) return;

    // Gate queued Dart listener callbacks immediately; native frame sinks
    // independently enforce the same state on decoder threads.
    _isPaused = true;
    if (!_bindings.ffi_scrcpy_pause(handle)) {
      _isPaused = false;
      throw StateError('Native scrcpy session rejected pause');
    }
  }

  /// Restarts local decode at a new keyframe without reconnecting the device.
  @override
  Future<void> resume() async {
    final handle = _handle;
    if (handle == null || !_bindings.ffi_scrcpy_is_running(handle)) {
      throw StateError('Cannot resume an inactive scrcpy session');
    }
    if (!_isPaused) return;

    if (!_bindings.ffi_scrcpy_resume(handle)) {
      throw StateError('Native scrcpy session rejected resume');
    }
    _isPaused = false;
  }

  /// Sends a touch event to the Android device.
  ///
  /// [action] is the touch action (e.g. down, up, move).
  /// [pointerId] is the pointer identifier (e.g. multi-touch finger index).
  /// [x] and [y] are normalized coordinates between 0.0 and 1.0.
  /// [pressure] is the touch pressure.
  @override
  void sendTouch(
    int action,
    int pointerId,
    double x,
    double y,
    double pressure,
  ) {
    if (_handle != null && !_isPaused) {
      _bindings.ffi_scrcpy_send_touch(
        _handle!,
        action,
        pointerId,
        x,
        y,
        pressure,
      );
    }
  }

  /// Sends a key event to the Android device.
  ///
  /// [keycode] is the Android keycode (android.view.KeyEvent.KEYCODE_*).
  /// [action] is the key action (down, up).
  /// [repeat] is the repeat count.
  /// [metastate] is the modifier state mask (e.g. shift, control).
  @override
  void sendKey(int keycode, int action, int repeat, int metastate) {
    if (_handle != null && !_isPaused) {
      _bindings.ffi_scrcpy_send_key(
        _handle!,
        keycode,
        action,
        repeat,
        metastate,
      );
    }
  }

  /// Sends a scroll event to the Android device.
  ///
  /// [x] and [y] are the normalized pointer coordinates.
  /// [hScroll] and [vScroll] represent the scroll steps.
  @override
  void sendScroll(double x, double y, double hScroll, double vScroll) {
    if (_handle != null && !_isPaused) {
      _bindings.ffi_scrcpy_send_scroll(_handle!, x, y, hScroll, vScroll);
    }
  }

  /// Sends a Back button press or wakes the screen if it is off.
  @override
  void sendBackOrScreenOn() {
    if (_handle != null && !_isPaused) {
      _bindings.ffi_scrcpy_send_back_or_screen_on(_handle!);
    }
  }

  /// Sets the Android device's clipboard text.
  ///
  /// If [paste] is true, the clipboard contents will be pasted automatically.
  @override
  void setClipboard(String text, {bool paste = false}) {
    if (_handle == null || _isPaused) return;
    final textPtr = text.toNativeUtf8();
    try {
      _bindings.ffi_scrcpy_set_clipboard(_handle!, textPtr.cast(), paste);
    } finally {
      malloc.free(textPtr);
    }
  }

  /// Injects text directly into the focused field on the Android device.
  @override
  void injectText(String text) {
    if (_handle == null || _isPaused) return;
    final textPtr = text.toNativeUtf8();
    try {
      _bindings.ffi_scrcpy_inject_text(_handle!, textPtr.cast());
    } finally {
      malloc.free(textPtr);
    }
  }

  void _freeArgs() {
    for (final ptr in _allocatedStrings) {
      calloc.free(ptr);
    }
    _allocatedStrings.clear();
    if (_currentArgv != null) {
      calloc.free(_currentArgv!);
      _currentArgv = null;
    }
  }

  void _notifyTextureFrameAvailable() {
    final textureId = _textureId;
    if (_isPaused || textureId == null || _frameNotificationPending) return;

    // The native renderer always consumes the newest retained frame. Coalesce
    // callbacks while a platform notification is in flight instead of doing a
    // MethodChannel round-trip and widget rebuild for every decoded frame.
    _frameNotificationPending = true;
    final renderSession = _renderSession;
    ScrcpyPlatform.instance.notifyFrameAvailable(textureId).whenComplete(() {
      if (_renderSession == renderSession) {
        _frameNotificationPending = false;
      }
    });
  }

  void _attachStateEvents(Pointer<Void> handle) {
    _detachStateEvents();

    final port = ReceivePort();
    _statePort = port;
    _statePortSubscription = port.listen((dynamic message) {
      if (_handle?.address != handle.address || message is! int) return;
      if (message < 0 || message >= ScrcpyState.values.length) return;

      final state = ScrcpyState.values[message];
      if (currentState != state) {
        _addState(state);
      }
    });
    _bindings.ffi_scrcpy_set_state_port(handle, port.sendPort.nativePort);
  }

  void _detachStateEvents([Pointer<Void>? handle]) {
    final nativeHandle = handle ?? _handle;
    if (nativeHandle != null) {
      _bindings.ffi_scrcpy_set_state_port(nativeHandle, 0);
    }
    _statePortSubscription?.cancel();
    _statePortSubscription = null;
    _statePort?.close();
    _statePort = null;
  }

  static void _videoFrameCallback(Pointer<Void> handle, int width, int height) {
    final controller = _controllers[handle.address];
    if (controller != null && !controller._isPaused) {
      controller._notifyTextureFrameAvailable();
      final size = Size(width.toDouble(), height.toDouble());
      if (controller._lastVideoSize != size) {
        controller._lastVideoSize = size;
        controller._videoFrameController.add(size);
      }
    }
  }

  static void _audioFrameCallback(
    Pointer<Void> handle,
    Pointer<Int16> data,
    int samples,
    int sampleRate,
    int channels,
  ) {
    final controller = _controllers[handle.address];
    if (controller != null &&
        !controller._isPaused &&
        !controller._audioFrameController.isClosed) {
      final byteCount = samples * channels * 2;
      final bytes = data.cast<Uint8>().asTypedList(byteCount);
      final copiedData = Uint8List.fromList(bytes);

      controller._audioFrameController.add(
        ScrcpyAudioFrame(
          data: copiedData,
          sampleRate: sampleRate,
          channels: channels,
        ),
      );
    }
  }

  static void _clipboardChangedCallback(
    Pointer<Void> handle,
    Pointer<Char> textPtr,
  ) {
    final controller = _controllers[handle.address];
    if (controller == null || textPtr == nullptr) return;
    try {
      final text = textPtr.cast<Utf8>().toDartString();
      controller._clipboardController.add(text);
    } catch (_) {
      // Ignore parsing errors on malformed payload.
    } finally {
      controller._bindings.ffi_scrcpy_free_string(textPtr);
    }
  }
}
