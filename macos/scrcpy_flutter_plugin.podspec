#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint scrcpy_flutter_plugin.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'scrcpy_flutter_plugin'
  s.version          = '0.1.0'
  s.summary          = 'Flutter FFI plugin for Android screen mirroring via scrcpy.'
  s.description      = <<-DESC
    A Flutter FFI plugin that embeds scrcpy, enabling real-time Android screen
    mirroring and device control via ADB. Supports macOS and Linux.
                       DESC
  s.homepage         = 'https://github.com/example/scrcpy_flutter_plugin'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }

  s.source           = { :path => '.' }

  # CocoaPods temporarily consumes the same binary stored inside the SPM
  # XCFramework. No second dylib is kept in the repository.
  s.source_files = 'scrcpy_flutter_plugin/Sources/scrcpy_flutter_plugin/**/*.swift'

  s.vendored_libraries =
    'scrcpy_flutter_plugin/Frameworks/libscrcpy_ffi.xcframework/macos-arm64/libscrcpy_ffi.dylib'
  # Keep CocoaPods on the same resource source as SPM until it is retired.
  s.resources = [
    'scrcpy_flutter_plugin/Sources/scrcpy_flutter_plugin/Resources/*'
  ]

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.14'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'LD_RUNPATH_SEARCH_PATHS' => '$(inherited) @executable_path/../Frameworks @loader_path/../Frameworks'
  }
  s.swift_version = '5.0'
end
