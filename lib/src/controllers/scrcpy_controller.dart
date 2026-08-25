import 'dart:ui' show Size;

import '../models/scrcpy_audio_frame.dart';
import '../models/scrcpy_options.dart';
import '../models/scrcpy_state.dart';
import 'scrcpy_controller_ffi.dart';

/// Public contract for a scrcpy mirroring session.
///
/// The factory returns the package's FFI implementation. Its native handles,
/// callback registration, texture lifecycle and state transport are internal
/// implementation details and are intentionally not part of this API.
abstract class ScrcpyController {
  /// Creates a controller backed by the bundled native scrcpy library.
  ///
  /// Native playback is enabled whenever [ScrcpyOptions.audio] is true. Set
  /// [forwardAudioFramesToDart] only for analysis or custom processing: it
  /// copies every PCM block into Dart and is not suitable for normal realtime
  /// playback.
  factory ScrcpyController({bool forwardAudioFramesToDart = false}) {
    return FfiScrcpyController(
      forwardAudioFramesToDart: forwardAudioFramesToDart,
    );
  }

  /// Stops any orphaned native sessions left by a hot restart.
  static void cleanupAll() => FfiScrcpyController.cleanupAll();

  /// Whether raw PCM frames are additionally copied to [onAudioFrame].
  bool get forwardAudioFramesToDart;

  /// The active Flutter texture ID, or null before a session is initialized.
  int? get textureId;

  /// The most recently decoded video size, or null if no frames have been decoded.
  Size? get videoSize;

  /// Whether local media decoding is paused.
  bool get isPaused;

  /// The most recently observed connection state.
  ScrcpyState? get currentState;

  /// Emits every connection-state change.
  Stream<ScrcpyState> get onStateChanged;

  /// Emits clipboard changes received from the Android device.
  Stream<String> get onSyncClipboard;

  /// Emits the decoded video size when it changes.
  Stream<Size> get onVideoFrame;

  /// Emits raw PCM only when [forwardAudioFramesToDart] is true.
  Stream<ScrcpyAudioFrame> get onAudioFrame;

  /// Starts a session with [options].
  Future<void> start(ScrcpyOptions options);

  /// Stops the session. Calling it more than once is safe.
  void stop();

  /// Stops local video and audio decoding while retaining the server session.
  Future<void> pause();

  /// Resumes local decoding from a fresh video keyframe.
  Future<void> resume();

  /// Releases the session, texture and Dart streams.
  void dispose();

  /// Sends a normalized touch event to Android.
  void sendTouch(
    int action,
    int pointerId,
    double x,
    double y,
    double pressure,
  );

  /// Sends an Android key event.
  void sendKey(int keycode, int action, int repeat, int metastate);

  /// Sends a normalized scroll event to Android.
  void sendScroll(double x, double y, double hScroll, double vScroll);

  /// Sends Back, or wakes the Android screen if necessary.
  void sendBackOrScreenOn();

  /// Sets Android clipboard text and optionally pastes it.
  void setClipboard(String text, {bool paste = false});

  /// Injects text into Android's focused field.
  void injectText(String text);
}
