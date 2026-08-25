/// Flutter plugin for Android screen mirroring via scrcpy.
///
/// Usage:
/// ```dart
/// final controller = ScrcpyController();
/// await controller.start(ScrcpyOptions(serial: 'YOUR_DEVICE_SERIAL'));
/// ```
library;

export 'src/controllers/scrcpy_controller.dart';
export 'src/input/scrcpy_input_handler.dart';
export 'src/models/scrcpy_audio_frame.dart';
export 'src/models/scrcpy_options.dart';
export 'src/models/scrcpy_state.dart';
export 'src/widgets/scrcpy_texture_widget.dart';
