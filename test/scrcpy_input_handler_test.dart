import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';

class CustomTestInputHandler extends ScrcpyInputHandler {
  const CustomTestInputHandler();

  @override
  int? getAndroidKeycode(LogicalKeyboardKey key) {
    if (key == LogicalKeyboardKey.keyF) {
      return 3; // Custom map key F to KEYCODE_HOME (3)
    }
    return super.getAndroidKeycode(key);
  }
}

void main() {
  group('ScrcpyInputHandler', () {
    const handler = ScrcpyInputHandler();

    test('calculateNormalizedPosition calculates correct relative offset', () {
      const constraints = BoxConstraints(maxWidth: 200, maxHeight: 100);

      expect(
        handler.calculateNormalizedPosition(const Offset(100, 50), constraints),
        const Offset(0.5, 0.5),
      );
      expect(
        handler.calculateNormalizedPosition(const Offset(0, 0), constraints),
        const Offset(0.0, 0.0),
      );
      expect(
        handler.calculateNormalizedPosition(const Offset(300, 200), constraints),
        const Offset(1.0, 1.0),
      );
    });

    test('getAndroidKeycode maps standard keys correctly', () {
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.keyA), 29); // KEYCODE_A
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.keyZ), 54); // KEYCODE_Z
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.digit0), 7); // KEYCODE_0
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.digit9), 16); // KEYCODE_9
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.enter), 66); // KEYCODE_ENTER
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.backspace), 67); // KEYCODE_DEL
      expect(handler.getAndroidKeycode(LogicalKeyboardKey.f1), null); // Unmapped key
    });

    test('Subclass can override getAndroidKeycode', () {
      const customHandler = CustomTestInputHandler();

      // Overridden key
      expect(customHandler.getAndroidKeycode(LogicalKeyboardKey.keyF), 3);
      // Non-overridden key falls back to super
      expect(customHandler.getAndroidKeycode(LogicalKeyboardKey.keyA), 29);
    });
  });
}
