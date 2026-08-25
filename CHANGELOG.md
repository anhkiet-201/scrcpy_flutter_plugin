## 0.1.1

* Refactor example application into a clean, modular multi-device architecture (`DeviceSession`, `DeviceCard`, `DeviceControlsBar`, `StatusBadge`).
* Consolidate standalone demo code into `example/` and remove obsolete `scrcpy_demo_app`.
* Add comprehensive English DartDoc (`///`) and inline comments across all example modules.
* Enhance `analysis_options.yaml` with strict analyzer rules and lints.

## 0.1.0

* Expose `ScrcpyController` as the stable abstract public contract while
  preserving its default factory constructor.
* Move the FFI controller, native bindings, dynamic-library loader and platform
  transport behind the package's internal `lib/src` implementation boundary.
* Document the supported public API, session lifecycle and native audio path.

## 0.0.1

* Initial release of scrcpy_flutter_plugin.
* Implement core FFI integration, controller contract, and native GPU-backed texture rendering.
