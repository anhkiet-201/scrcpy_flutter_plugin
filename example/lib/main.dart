import 'dart:io';
import 'package:flutter/material.dart';
import 'models/device_session.dart';
import 'widgets/device_card.dart';

/// Entry point of the Scrcpy Flutter Plugin Example Application.
void main() {
  runApp(const ScrcpyExampleApp());
}

/// Root widget configuring the Material 3 Dark theme and home screen.
class ScrcpyExampleApp extends StatelessWidget {
  const ScrcpyExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Scrcpy Multi-Device Mirroring Demo',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorSchemeSeed: Colors.blueAccent,
      ),
      home: const MultiDeviceScreen(),
    );
  }
}

/// Main application screen demonstrating multi-device concurrent Android mirroring.
class MultiDeviceScreen extends StatefulWidget {
  const MultiDeviceScreen({super.key});

  @override
  State<MultiDeviceScreen> createState() => _MultiDeviceScreenState();
}

class _MultiDeviceScreenState extends State<MultiDeviceScreen> {
  /// List of active device sessions.
  List<DeviceSession> _sessions = [];
  bool _isLoading = true;

  @override
  void initState() {
    super.initState();
    _fetchDevicesAndStart();
  }

  /// Use adb to fetch connected devices and auto-start mirroring.
  Future<void> _fetchDevicesAndStart() async {
    setState(() {
      _isLoading = true;
    });

    try {
      final result = await Process.run('adb', ['devices']);
      final lines = (result.stdout as String).split('\n');
      final serials = <String>[];

      for (var line in lines) {
        line = line.trim();
        if (line.isNotEmpty && line != 'List of devices attached' && line.endsWith('device')) {
          final parts = line.split(RegExp(r'\s+'));
          if (parts.isNotEmpty) {
            serials.add(parts.first);
          }
        }
      }

      final newSessions = <DeviceSession>[];
      for (int i = 0; i < serials.length; i++) {
        final serial = serials[i];
        final session = DeviceSession(
          id: i + 1,
          name: 'Device ${i + 1} ($serial)',
          initialSerial: serial,
          onClipboardReceived: (text) => _showClipboardSnackBar('Device ${i + 1}', text),
        );
        newSessions.add(session);
      }

      if (mounted) {
        setState(() {
          _sessions = newSessions;
          _isLoading = false;
        });

        final currentSessions = _sessions;
        // Auto-start all fetched sessions with a staggered delay
        // to avoid overwhelming adb when connecting to many devices (e.g. 100+).
        for (final session in newSessions) {
          if (!mounted || _sessions != currentSessions) break;
          session.start().catchError((e) {
            debugPrint('Failed to start session for ${session.serialController.text}: $e');
          });
          await Future.delayed(const Duration(milliseconds: 150));
        }
      }
    } catch (e) {
      debugPrint('Error fetching adb devices: $e');
      if (mounted) {
        setState(() {
          _isLoading = false;
        });
      }
    }
  }

  /// Displays a SnackBar when a device syncs new clipboard text.
  void _showClipboardSnackBar(String deviceName, String text) {
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('$deviceName Clipboard: $text'),
          duration: const Duration(seconds: 3),
        ),
      );
    }
  }

  @override
  void dispose() {
    // Cleanly dispose of all device session controllers and stream listeners
    for (final session in _sessions) {
      session.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Scrcpy Multi-Device Mirroring Demo'),
        centerTitle: false,
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh Devices',
            onPressed: () {
              for (var session in _sessions) {
                session.dispose();
              }
              setState(() {
                _sessions = [];
              });
              _fetchDevicesAndStart();
            },
          ),
          IconButton(
            icon: const Icon(Icons.info_outline),
            tooltip: 'About Scrcpy Demo',
            onPressed: () => _showAboutDialog(context),
          ),
        ],
      ),
      body: _isLoading
          ? const Center(child: CircularProgressIndicator())
          : _sessions.isEmpty
              ? const Center(child: Text('No devices found via adb. Please connect a device.'))
              : GridView.builder(
                  padding: const EdgeInsets.all(8.0),
                  gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
                    maxCrossAxisExtent: 500.0,
                    childAspectRatio: 10 / 16,
                    crossAxisSpacing: 8.0,
                    mainAxisSpacing: 8.0,
                  ),
                  itemCount: _sessions.length,
                  itemBuilder: (context, index) {
                    return DeviceCard(session: _sessions[index]);
                  },
                ),
    );
  }

  /// Shows an informative dialog regarding Scrcpy setup and capabilities.
  void _showAboutDialog(BuildContext context) {
    showAboutDialog(
      context: context,
      applicationName: 'Scrcpy Plugin Demo',
      applicationVersion: '1.0.0',
      applicationIcon: const Icon(Icons.screen_share, size: 48, color: Colors.blueAccent),
      children: const [
        Text(
          'This app demonstrates concurrent multi-device Android screen mirroring and remote control '
          'using native SDL3 andFFmpeg pipelines via scrcpy_flutter_plugin.\n\n'
          'Features:\n'
          '• Real-time video texture rendering with dynamic aspect ratio\n'
          '• Low-latency HID mouse & touch event forwarding\n'
          '• Native SDL3 audio streaming\n'
          '• Automatic device clipboard synchronization\n'
          '• Client-side local stream pause/resume controls',
        ),
      ],
    );
  }
}
