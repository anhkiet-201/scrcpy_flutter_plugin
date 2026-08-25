import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import '../controllers/scrcpy_controller.dart';
import '../models/scrcpy_state.dart';

/// Handles input events (keyboard, pointer/mouse, scroll) for a scrcpy session.
///
/// Developers can extend this class and override individual methods to customize
/// key mapping, gesture/touch behavior, or scroll speed/direction.
class ScrcpyInputHandler {
  /// Creates a new [ScrcpyInputHandler] instance.
  const ScrcpyInputHandler();

  /// Calculates relative normalized coordinates (`0.0` to `1.0`) based on widget size constraints.
  Offset calculateNormalizedPosition(
    Offset localPosition,
    BoxConstraints constraints,
  ) {
    if (constraints.maxWidth <= 0 || constraints.maxHeight <= 0) {
      return Offset.zero;
    }
    double nx = localPosition.dx / constraints.maxWidth;
    double ny = localPosition.dy / constraints.maxHeight;
    return Offset(nx.clamp(0.0, 1.0), ny.clamp(0.0, 1.0));
  }

  /// Maps a Flutter [LogicalKeyboardKey] to an Android keycode.
  ///
  /// Returns `null` if the key has no corresponding Android mapping.
  /// Override this method in a subclass to customize or add key mappings.
  int? getAndroidKeycode(LogicalKeyboardKey key) {
    final int keyId = key.keyId;

    // A-Z keys: map to KEYCODE_A (29) through KEYCODE_Z (54)
    if (keyId >= LogicalKeyboardKey.keyA.keyId &&
        keyId <= LogicalKeyboardKey.keyZ.keyId) {
      return 29 + (keyId - LogicalKeyboardKey.keyA.keyId);
    }

    // 0-9 keys: map to KEYCODE_0 (7) through KEYCODE_9 (16)
    if (keyId >= LogicalKeyboardKey.digit0.keyId &&
        keyId <= LogicalKeyboardKey.digit9.keyId) {
      return 7 + (keyId - LogicalKeyboardKey.digit0.keyId);
    }

    // Common action keys
    if (key == LogicalKeyboardKey.backspace) return 67; // KEYCODE_DEL
    if (key == LogicalKeyboardKey.enter) return 66; // KEYCODE_ENTER
    if (key == LogicalKeyboardKey.space) return 62; // KEYCODE_SPACE
    if (key == LogicalKeyboardKey.escape) return 111; // KEYCODE_ESCAPE
    if (key == LogicalKeyboardKey.tab) return 61; // KEYCODE_TAB

    // Navigation keys
    if (key == LogicalKeyboardKey.arrowUp) return 19; // KEYCODE_DPAD_UP
    if (key == LogicalKeyboardKey.arrowDown) return 20; // KEYCODE_DPAD_DOWN
    if (key == LogicalKeyboardKey.arrowLeft) return 21; // KEYCODE_DPAD_LEFT
    if (key == LogicalKeyboardKey.arrowRight) return 22; // KEYCODE_DPAD_RIGHT

    // Modifier keys
    if (key == LogicalKeyboardKey.shiftLeft) return 59;
    if (key == LogicalKeyboardKey.shiftRight) return 60;
    if (key == LogicalKeyboardKey.controlLeft) return 113;
    if (key == LogicalKeyboardKey.controlRight) return 114;
    if (key == LogicalKeyboardKey.metaLeft) return 117;
    if (key == LogicalKeyboardKey.metaRight) return 118;
    if (key == LogicalKeyboardKey.altLeft) return 57;
    if (key == LogicalKeyboardKey.altRight) return 58;

    // Common punctuations
    if (key == LogicalKeyboardKey.comma) return 55; // KEYCODE_COMMA
    if (key == LogicalKeyboardKey.period) return 56; // KEYCODE_PERIOD
    if (key == LogicalKeyboardKey.minus) return 69; // KEYCODE_MINUS
    if (key == LogicalKeyboardKey.equal) return 70; // KEYCODE_EQUALS
    if (key == LogicalKeyboardKey.bracketLeft) {
      return 71; // KEYCODE_LEFT_BRACKET
    }
    if (key == LogicalKeyboardKey.bracketRight) {
      return 72; // KEYCODE_RIGHT_BRACKET
    }
    if (key == LogicalKeyboardKey.backslash) return 73; // KEYCODE_BACKSLASH
    if (key == LogicalKeyboardKey.semicolon) return 74; // KEYCODE_SEMICOLON
    if (key == LogicalKeyboardKey.quoteSingle) return 75; // KEYCODE_APOSTROPHE
    if (key == LogicalKeyboardKey.slash) return 76; // KEYCODE_SLASH
    if (key == LogicalKeyboardKey.backquote) return 68; // KEYCODE_GRAVE

    return null; // Unmapped key
  }

  /// Handles keyboard events captured by the video widget.
  ///
  /// Returns [KeyEventResult.handled] if key was mapped and sent to controller,
  /// or [KeyEventResult.ignored] otherwise.
  KeyEventResult handleKeyEvent(
    FocusNode focusNode,
    KeyEvent event,
    ScrcpyController controller,
  ) {
    if (controller.currentState != ScrcpyState.connected) {
      return KeyEventResult.ignored;
    }

    final int? keycode = getAndroidKeycode(event.logicalKey);
    if (keycode != null) {
      final int action =
          (event is KeyDownEvent || event is KeyRepeatEvent) ? 0 : 1;
      int meta = 0;
      if (HardwareKeyboard.instance.isShiftPressed) {
        meta |= 1; // AMETA_SHIFT_ON
      }
      if (HardwareKeyboard.instance.isAltPressed) {
        meta |= 2; // AMETA_ALT_ON
      }
      if (HardwareKeyboard.instance.isControlPressed) {
        meta |= 4096; // AMETA_CTRL_ON
      }
      if (HardwareKeyboard.instance.isMetaPressed) {
        meta |= 65536; // AMETA_META_ON
      }

      controller.sendKey(keycode, action, 0, meta);
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  /// Handles pointer down events (e.g., touch down or mouse click down).
  void handlePointerDown(
    PointerDownEvent event,
    BoxConstraints constraints,
    ScrcpyController controller,
    FocusNode focusNode,
  ) {
    focusNode.requestFocus();
    _sendTouch(controller, 0, event.localPosition, constraints);
  }

  /// Handles pointer move events (e.g., touch drag or mouse move).
  void handlePointerMove(
    PointerMoveEvent event,
    BoxConstraints constraints,
    ScrcpyController controller,
  ) {
    _sendTouch(controller, 2, event.localPosition, constraints);
  }

  /// Handles pointer up events (e.g., touch release or mouse click up).
  void handlePointerUp(
    PointerUpEvent event,
    BoxConstraints constraints,
    ScrcpyController controller,
  ) {
    _sendTouch(controller, 1, event.localPosition, constraints);
  }

  /// Handles pointer cancel events.
  void handlePointerCancel(
    PointerCancelEvent event,
    BoxConstraints constraints,
    ScrcpyController controller,
  ) {
    _sendTouch(controller, 1, event.localPosition, constraints);
  }

  /// Handles pointer scroll events (e.g., mouse wheel).
  void handlePointerScroll(
    PointerScrollEvent event,
    BoxConstraints constraints,
    ScrcpyController controller,
  ) {
    if (controller.currentState != ScrcpyState.connected) return;

    final pos = calculateNormalizedPosition(event.localPosition, constraints);

    // Flutter scrollDelta has positive value for scrolling down, negative for up.
    // Android scrcpy expects vScroll: negative for down, positive for up. Thus, negate it.
    // Also, scale it down (e.g., divided by 50) to make scroll speed comfortable.
    double hScroll = -event.scrollDelta.dx / 50.0;
    double vScroll = -event.scrollDelta.dy / 50.0;

    controller.sendScroll(pos.dx, pos.dy, hScroll, vScroll);
  }

  /// Helper method to calculate relative coordinates and send touch action.
  void _sendTouch(
    ScrcpyController controller,
    int action,
    Offset localPosition,
    BoxConstraints constraints,
  ) {
    if (controller.currentState != ScrcpyState.connected) return;

    final pos = calculateNormalizedPosition(localPosition, constraints);
    controller.sendTouch(action, 0, pos.dx, pos.dy, 1.0);
  }
}
