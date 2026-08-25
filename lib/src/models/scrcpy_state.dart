/// Connection state of the scrcpy session.
///
/// Values are aligned with C constants (SCRCPY_STATE_*) in scrcpy_ffi.h.
enum ScrcpyState {
  /// The session is disconnected.
  disconnected,

  /// The session is connecting to the Android device.
  connecting,

  /// The session is connected to the Android device and active.
  connected,

  /// An error occurred during the session.
  error,
}
