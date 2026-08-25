import 'package:flutter/material.dart';
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';
import '../models/device_session.dart';
import 'device_controls_bar.dart';
import 'status_badge.dart';

/// A self-contained UI card displaying controls, status, audio info,
/// clipboard sync, and the video texture for a single [DeviceSession].
class DeviceCard extends StatelessWidget {
  /// The device session model instance to render.
  final DeviceSession session;

  /// Creates a new [DeviceCard].
  const DeviceCard({
    super.key,
    required this.session,
  });

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: session,
      builder: (context, _) {
        final isConnected = session.isConnected;

        return Card(
          margin: const EdgeInsets.all(8),
          elevation: 3,
          clipBehavior: Clip.antiAlias,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              // Header & Controls Section
              Padding(
                padding: const EdgeInsets.all(12.0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    // Title and status badge
                    Row(
                      mainAxisAlignment: MainAxisAlignment.spaceBetween,
                      children: [
                        Text(
                          session.name,
                          style: const TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                        StatusBadge(
                          state: session.state,
                          isPaused: session.isPaused,
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),

                    // Audio output notice when connected
                    if (isConnected) ...[
                      const Row(
                        children: [
                          Icon(Icons.volume_up, size: 16, color: Colors.lightBlueAccent),
                          SizedBox(width: 6),
                          Text(
                            'Audio output: Native SDL3 audio stream',
                            style: TextStyle(
                              fontSize: 12,
                              color: Colors.lightBlueAccent,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 10),
                    ],

                    // Device controls bar
                    DeviceControlsBar(
                      session: session,
                      onStart: () => _handleStart(context),
                      onStop: session.stop,
                      onPause: () => _handlePause(context),
                      onResume: () => _handleResume(context),
                    ),
                  ],
                ),
              ),

              const Divider(height: 1),

              // Video Display Area
              Expanded(
                child: Container(
                  color: Colors.black,
                  child: Center(
                    child: _buildVideoDisplayArea(context),
                  ),
                ),
              ),

              // Clipboard Sync Footer
              if (session.lastClipboard.isNotEmpty)
                Container(
                  color: Theme.of(context).colorScheme.surfaceContainerHighest,
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                  child: Row(
                    children: [
                      const Icon(Icons.assignment_outlined, size: 14),
                      const SizedBox(width: 6),
                      Expanded(
                        child: Text(
                          'Clipboard: ${session.lastClipboard}',
                          style: Theme.of(context).textTheme.bodySmall,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                    ],
                  ),
                ),
            ],
          ),
        );
      },
    );
  }

  /// Builds the video display area or a placeholder/status indicator based on connection state.
  Widget _buildVideoDisplayArea(BuildContext context) {
    switch (session.state) {
      case ScrcpyState.connecting:
        return const Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 16),
            Text(
              'Connecting...',
              style: TextStyle(color: Colors.white70),
            ),
          ],
        );
      case ScrcpyState.error:
        return const Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              Icons.error_outline,
              size: 64,
              color: Colors.redAccent,
            ),
            SizedBox(height: 12),
            Text(
              'Connection Failed',
              style: TextStyle(color: Colors.redAccent),
            ),
          ],
        );
      case ScrcpyState.connected:
        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: AspectRatio(
            aspectRatio: session.aspectRatio,
            child: Container(
              decoration: BoxDecoration(
                border: Border.all(color: Colors.blueAccent, width: 1.5),
                borderRadius: BorderRadius.circular(4),
              ),
              child: ScrcpyTextureWidget(
                controller: session.controller,
              ),
            ),
          ),
        );
      case ScrcpyState.disconnected:
      default:
        return const SizedBox.shrink();
    }
  }

  /// Helper to start mirroring with error handling UI feedback.
  Future<void> _handleStart(BuildContext context) async {
    try {
      await session.start();
    } catch (e) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('${session.name} failed to start: $e')),
        );
      }
    }
  }

  /// Helper to pause video stream rendering with error handling UI feedback.
  Future<void> _handlePause(BuildContext context) async {
    try {
      await session.pause();
    } catch (e) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not pause ${session.name}: $e')),
        );
      }
    }
  }

  /// Helper to resume video stream rendering with error handling UI feedback.
  Future<void> _handleResume(BuildContext context) async {
    try {
      await session.resume();
    } catch (e) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Could not resume ${session.name}: $e')),
        );
      }
    }
  }
}
