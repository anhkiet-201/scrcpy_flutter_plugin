import 'package:flutter/material.dart';
import 'package:scrcpy_flutter_plugin/scrcpy_flutter_plugin.dart';

/// A compact UI badge component displaying the current connection state
/// and pause indicator for a Scrcpy device session.
class StatusBadge extends StatelessWidget {
  /// The current connection state of the device.
  final ScrcpyState state;

  /// Whether client-side video playback is currently paused.
  final bool isPaused;

  /// Creates a new [StatusBadge].
  const StatusBadge({
    super.key,
    required this.state,
    this.isPaused = false,
  });

  @override
  Widget build(BuildContext context) {
    // Map ScrcpyState and pause status to visual color palette and human-readable label
    final (color, label) = switch (state) {
      ScrcpyState.connected when isPaused => (Colors.amber, 'LOCAL PAUSED'),
      ScrcpyState.connected => (Colors.greenAccent, 'CONNECTED'),
      ScrcpyState.connecting => (Colors.orangeAccent, 'CONNECTING'),
      ScrcpyState.error => (Colors.redAccent, 'ERROR'),
      ScrcpyState.disconnected => (Colors.grey, 'DISCONNECTED'),
    };

    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: color.withValues(alpha: 0.5), width: 1),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          // Status indicator dot
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(
              color: color,
              shape: BoxShape.circle,
            ),
          ),
          const SizedBox(width: 6),
          // Status text label
          Text(
            label,
            style: TextStyle(
              color: color,
              fontSize: 11,
              fontWeight: FontWeight.bold,
              letterSpacing: 0.5,
            ),
          ),
        ],
      ),
    );
  }
}
