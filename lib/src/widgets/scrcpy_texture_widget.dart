import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';

/// A Flutter widget that renders the scrcpy video stream via a hardware texture.
///
/// The texture is allocated on the platform side (macOS/Linux/Windows) and
/// updated whenever a new frame is decoded by the native library.
class ScrcpyTextureWidget extends StatefulWidget {
  /// The [ScrcpyController] managing the active scrcpy session.
  final ScrcpyController controller;

  /// The [ScrcpyInputHandler] handling input events (keyboard, mouse, scroll).
  ///
  /// Defaults to [ScrcpyInputHandler]. Pass a custom subclass to override behavior.
  final ScrcpyInputHandler inputHandler;

  /// Creates a new [ScrcpyTextureWidget] instance.
  const ScrcpyTextureWidget({
    super.key,
    required this.controller,
    this.inputHandler = const ScrcpyInputHandler(),
  });

  @override
  State<ScrcpyTextureWidget> createState() => _ScrcpyTextureWidgetState();
}

class _ScrcpyTextureWidgetState extends State<ScrcpyTextureWidget> {
  final _videoFocusNode = FocusNode();

  @override
  void dispose() {
    _videoFocusNode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final textureId = widget.controller.textureId;
    if (textureId == null) {
      return const Center(child: Text('Initializing video stream…'));
    }

    return StreamBuilder<Size>(
      initialData: widget.controller.videoSize,
      stream: widget.controller.onVideoFrame,
      builder: (context, snapshot) {
        final size = snapshot.data;
        
        final Widget child = LayoutBuilder(
          builder: (context, constraints) {
            return Focus(
              focusNode: _videoFocusNode,
              autofocus: true,
              onKeyEvent: (node, event) => widget.inputHandler.handleKeyEvent(
                node,
                event,
                widget.controller,
              ),
              child: Listener(
                onPointerDown: (event) => widget.inputHandler.handlePointerDown(
                  event,
                  constraints,
                  widget.controller,
                  _videoFocusNode,
                ),
                onPointerMove: (event) => widget.inputHandler.handlePointerMove(
                  event,
                  constraints,
                  widget.controller,
                ),
                onPointerUp: (event) => widget.inputHandler.handlePointerUp(
                  event,
                  constraints,
                  widget.controller,
                ),
                onPointerCancel: (event) =>
                    widget.inputHandler.handlePointerCancel(
                  event,
                  constraints,
                  widget.controller,
                ),
                onPointerSignal: (event) {
                  if (event is PointerScrollEvent) {
                    widget.inputHandler.handlePointerScroll(
                      event,
                      constraints,
                      widget.controller,
                    );
                  }
                },
                child: Texture(textureId: textureId),
              ),
            );
          },
        );

        if (size == null) {
          return child;
        }

        return AspectRatio(
          aspectRatio: size.width / size.height,
          child: child,
        );
      },
    );
  }
}
