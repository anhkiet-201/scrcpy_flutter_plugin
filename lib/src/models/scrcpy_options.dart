/// Supported video codecs for the scrcpy session.
enum VideoCodec {
  /// H.264 video codec (AVC).
  h264,

  /// H.265 video codec (HEVC).
  h265,

  /// AV1 video codec.
  av1,

  /// VP8 video codec.
  vp8,

  /// VP9 video codec.
  vp9,
}

/// Supported audio codecs for the scrcpy session.
enum AudioCodec {
  /// Opus audio codec.
  opus,

  /// AAC audio codec.
  aac,

  /// FLAC audio codec.
  flac,

  /// Raw PCM audio format.
  raw,
}

/// Configuration options for starting a scrcpy session.
///
/// These options map to the scrcpy command line arguments.
class ScrcpyOptions {
  /// The serial number of the Android device to connect to.
  final String serial;

  /// The log level for the scrcpy server (e.g., 'verbose', 'debug', 'info', 'warn', 'error').
  final String? logLevel;

  /// The video codec to use.
  final VideoCodec videoCodec;

  /// The audio codec to use.
  final AudioCodec audioCodec;

  /// The video source.
  final int videoSource;

  /// The audio source.
  final int audioSource;

  /// The camera facing orientation.
  final int cameraFacing;

  /// The crop region on the Android screen.
  final String? crop;

  /// Additional options for the video codec.
  final String? videoCodecOptions;

  /// Additional options for the audio codec.
  final String? audioCodecOptions;

  /// The video encoder name.
  final String? videoEncoder;

  /// The audio encoder name.
  final String? audioEncoder;

  /// The camera ID to use.
  final String? cameraId;

  /// The camera resolution/size.
  final String? cameraSize;

  /// The camera aspect ratio.
  final String? cameraAr;

  /// The camera zoom level.
  final String? cameraZoom;

  /// The camera frame rate (FPS).
  final int cameraFps;

  /// The first port in the port range for adb tunnel.
  int portRangeFirst;

  /// The last port in the port range for adb tunnel.
  int portRangeLast;

  /// The tunnel host port.
  final int tunnelHost;

  /// The tunnel port.
  final int tunnelPort;

  /// Limit both the width and height of the video to value.
  final int maxSize;

  /// The minimum size alignment.
  final int minSizeAlignment;

  /// The video bit rate in bps.
  final int videoBitRate;

  /// The audio bit rate in bps.
  final int audioBitRate;

  /// Limit the frame rate of the video.
  final double? maxFps;

  /// The rotation angle of the screen.
  final String? angle;

  /// Screen off timeout in milliseconds.
  final int screenOffTimeout;

  /// The capture orientation.
  final int captureOrientation;

  /// The capture orientation lock.
  final int captureOrientationLock;

  /// Whether to control the device (inject inputs).
  final bool control;

  /// The display ID to mirror.
  final int displayId;

  /// Create a new virtual display with the given dimensions.
  final String? newDisplay;

  /// The package name of the app to start on connection.
  final String? startApp;

  /// The display IME policy.
  final int displayImePolicy;

  /// Whether to mirror video.
  final bool video;

  /// Whether to mirror audio.
  final bool audio;

  /// Duplicate audio output on the device.
  final bool audioDup;

  /// Show physical touches on the screen.
  final bool showTouches;

  /// Prevent the device from sleeping.
  final bool stayAwake;

  /// Force adb forward connection instead of reverse.
  final bool forceAdbForward;

  /// Power off the device screen on close.
  final bool powerOffOnClose;

  /// Synchronize the clipboards.
  final bool clipboardAutosync;

  /// Downgrade quality on encoder error.
  final bool downsizeOnError;

  /// Use TCP/IP instead of USB.
  final bool tcpip;

  /// TCP/IP destination address.
  final String? tcpipDst;

  /// Prefer USB device if multiple are available.
  final bool selectUsb;

  /// Prefer TCP/IP device if multiple are available.
  final bool selectTcpip;

  /// Clean up server on close.
  final bool cleanup;

  /// Power on the device screen on start.
  final bool powerOn;

  /// Kill adb daemon on close.
  final bool killAdbOnClose;

  /// Use high speed camera preview if supported.
  final bool cameraHighSpeed;

  /// Turn on the camera torch/flashlight.
  final bool cameraTorch;

  /// Destroy virtual display content on close.
  final bool vdDestroyContent;

  /// Enable system decorations on virtual display.
  final bool vdSystemDecorations;

  /// Keep active connection alive.
  final bool keepActive;

  /// Enable flexible display layout.
  final bool flexDisplay;

  /// Bypass video encoder constraints.
  final bool ignoreVideoEncoderConstraints;

  /// List available options.
  final int list;

  /// Enable software decoding (skip GPU hardware setup on the client).
  final bool softwareDecoding;

  /// Creates a new [ScrcpyOptions] configuration.
  ScrcpyOptions({
    required this.serial,
    this.logLevel = 'info',
    this.videoCodec = VideoCodec.h264,
    this.audioCodec = AudioCodec.opus,
    this.videoSource = 0,
    this.audioSource = 1,
    this.cameraFacing = 0,
    this.crop,
    this.videoCodecOptions,
    this.audioCodecOptions,
    this.videoEncoder,
    this.audioEncoder,
    this.cameraId,
    this.cameraSize,
    this.cameraAr,
    this.cameraZoom,
    this.cameraFps = 0,
    this.portRangeFirst = 27183,
    this.portRangeLast = 27999,
    this.tunnelHost = 0,
    this.tunnelPort = 0,
    this.maxSize = 0,
    this.minSizeAlignment = 8,
    this.videoBitRate = 8000000,
    this.audioBitRate = 128000,
    this.maxFps,
    this.angle,
    this.screenOffTimeout = 0,
    this.captureOrientation = 0,
    this.captureOrientationLock = 0,
    this.control = true,
    this.displayId = 0,
    this.newDisplay,
    this.displayImePolicy = 0,
    this.video = true,
    this.audio = true,
    this.audioDup = false,
    this.showTouches = false,
    this.stayAwake = false,
    this.forceAdbForward = false,
    this.powerOffOnClose = false,
    this.clipboardAutosync = true,
    this.downsizeOnError = true,
    this.tcpip = false,
    this.tcpipDst,
    this.selectUsb = false,
    this.selectTcpip = false,
    this.cleanup = true,
    this.powerOn = true,
    this.killAdbOnClose = false,
    this.cameraHighSpeed = false,
    this.cameraTorch = false,
    this.vdDestroyContent = true,
    this.vdSystemDecorations = true,
    this.keepActive = false,
    this.flexDisplay = false,
    this.ignoreVideoEncoderConstraints = false,
    this.list = 0,
    this.startApp,
    this.softwareDecoding = true,
  });

  /// Converts the options into a list of command line arguments for the scrcpy server.
  List<String> toArgs() {
    final args = <String>[];
    if (serial.isNotEmpty) args.add('--serial=$serial');
    if (logLevel != null) args.add('-V$logLevel');
    if (softwareDecoding) args.add('--software-decoding');

    args.add('--video-codec=${videoCodec.name}');
    args.add('--audio-codec=${audioCodec.name}');
    if (maxSize > 0) args.add('--max-size=$maxSize');
    if (videoBitRate > 0) args.add('--video-bit-rate=$videoBitRate');
    if (audioBitRate > 0) args.add('--audio-bit-rate=$audioBitRate');
    if (maxFps != null) args.add('--max-fps=$maxFps');
    if (angle != null) args.add('--angle=$angle');
    
    args.add('--port=$portRangeFirst:$portRangeLast');

    if (videoCodecOptions != null) args.add('--video-codec-options=$videoCodecOptions');
    if (audioCodecOptions != null) args.add('--audio-codec-options=$audioCodecOptions');
    if (videoEncoder != null) args.add('--video-encoder=$videoEncoder');
    if (audioEncoder != null) args.add('--audio-encoder=$audioEncoder');
    if (minSizeAlignment > 0) args.add('--min-size-alignment=$minSizeAlignment');
    if (ignoreVideoEncoderConstraints) args.add('--ignore-video-encoder-constraints');
    if (!downsizeOnError) args.add('--no-downsize-on-error');

    if (showTouches) args.add('--show-touches');
    if (stayAwake) args.add('--stay-awake');
    if (!control) args.add('--no-control');
    if (!video) args.add('--no-video');
    if (!audio) args.add('--no-audio');
    if (!clipboardAutosync) args.add('--no-clipboard-autosync');
    if (!cleanup) args.add('--no-cleanup');
    if (newDisplay != null) args.add('--new-display=$newDisplay');
    if (startApp != null) args.add('--start-app="$startApp"');
    if (crop != null) args.add('--crop=$crop');
    if (vdSystemDecorations) args.add('--no-vd-system-decorations');
    return args;
  }
}
