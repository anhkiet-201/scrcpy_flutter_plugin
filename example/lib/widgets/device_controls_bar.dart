import 'package:flutter/material.dart';
import '../models/device_session.dart';

/// A control panel widget providing user inputs for ADB device serial/IP
/// and action buttons (Start, Stop, Pause, Resume).
class DeviceControlsBar extends StatelessWidget {
  /// The target device session instance.
  final DeviceSession session;

  /// Callback when user triggers start connection.
  final VoidCallback onStart;

  /// Callback when user triggers disconnect/stop.
  final VoidCallback onStop;

  /// Callback when user triggers local video pause.
  final VoidCallback onPause;

  /// Callback when user triggers local video resume.
  final VoidCallback onResume;

  /// Creates a new [DeviceControlsBar].
  const DeviceControlsBar({
    super.key,
    required this.session,
    required this.onStart,
    required this.onStop,
    required this.onPause,
    required this.onResume,
  });

  @override
  Widget build(BuildContext context) {
    final isDisconnected = session.state.name == 'disconnected' || session.state.name == 'error';
    final isConnecting = session.isConnecting;
    final isPaused = session.isPaused;

    return Row(
      children: [
        // TextField for device serial or TCP/IP address
        Expanded(
          child: TextField(
            controller: session.serialController,
            decoration: const InputDecoration(
              labelText: 'Device IP / ADB Serial',
              hintText: 'e.g. 192.168.1.100:5555 or emulator-5554',
              border: OutlineInputBorder(),
              isDense: true,
              prefixIcon: Icon(Icons.phone_android, size: 20),
            ),
            enabled: isDisconnected,
          ),
        ),
        const SizedBox(width: 10),

        // Start button (visible when disconnected)
        if (isDisconnected)
          ElevatedButton.icon(
            onPressed: isConnecting ? null : onStart,
            icon: const Icon(Icons.play_arrow),
            label: Text(isConnecting ? 'Starting...' : 'Start'),
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.green.shade700,
              foregroundColor: Colors.white,
            ),
          )
        else
          // Action button group (Stop, Pause/Resume) when connected
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              // Stop button
              ElevatedButton.icon(
                onPressed: onStop,
                icon: const Icon(Icons.stop),
                label: const Text('Stop'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.red.shade700,
                  foregroundColor: Colors.white,
                ),
              ),
              const SizedBox(width: 8),

              // Pause / Resume button
              ElevatedButton.icon(
                onPressed: isPaused ? onResume : onPause,
                icon: Icon(isPaused ? Icons.play_arrow : Icons.pause),
                label: Text(isPaused ? 'Resume' : 'Pause'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: isPaused ? Colors.green.shade700 : Colors.amber.shade800,
                  foregroundColor: Colors.white,
                ),
              ),
            ],
          ),
      ],
    );
  }
}
