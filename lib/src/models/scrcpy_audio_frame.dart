import 'dart:typed_data';

/// Decoded raw PCM audio frame data from Android.
class ScrcpyAudioFrame {
  /// Raw PCM data bytes (signed 16-bit, interleaved stereo).
  final Uint8List data;

  /// Sample rate (e.g. 48000 Hz).
  final int sampleRate;

  /// Number of audio channels (e.g. 2 channels).
  final int channels;

  ScrcpyAudioFrame({
    required this.data,
    required this.sampleRate,
    required this.channels,
  });
}
