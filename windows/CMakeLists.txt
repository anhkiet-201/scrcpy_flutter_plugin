cmake_minimum_required(VERSION 3.14)
set(PROJECT_NAME "scrcpy_flutter_plugin")
project(${PROJECT_NAME} LANGUAGES CXX)

# The FFI library and its import library are produced by
# build_shared_lib_windows.ps1. Flutter must consume these prebuilt artifacts
# instead of rebuilding scrcpy through its ephemeral plugin symlink.
set(SCRCPY_FFI_DLL "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi.dll")
set(SCRCPY_FFI_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi.dll.a")
if(NOT EXISTS "${SCRCPY_FFI_DLL}" OR NOT EXISTS "${SCRCPY_FFI_IMPLIB}")
  message(FATAL_ERROR
    "Prebuilt scrcpy FFI artifacts were not found. "
    "Run build_shared_lib_windows.ps1 before building the Flutter app.")
endif()

add_library(scrcpy_ffi SHARED IMPORTED GLOBAL)
set_target_properties(scrcpy_ffi PROPERTIES
  IMPORTED_LOCATION "${SCRCPY_FFI_DLL}"
  IMPORTED_IMPLIB "${SCRCPY_FFI_IMPLIB}"
)

# The Flutter plugin wrapper (MethodChannel handler).
set(PLUGIN_NAME "scrcpy_flutter_plugin_plugin")

add_library(${PLUGIN_NAME} SHARED
  "scrcpy_flutter_plugin.cpp"
  "scrcpy_video_texture.cpp"
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  flutter_wrapper_plugin
  d3d11
  dxgi
  dxguid
  scrcpy_ffi
)

# Tự động copy các file DLL phụ thuộc của ADB vào thư mục build đầu ra
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/../assets/AdbWinApi.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/../assets/AdbWinUsbApi.dll"
  $<TARGET_FILE_DIR:${PLUGIN_NAME}>
)

# Bundle both the plugin DLL and the FFI shared library.
set(scrcpy_flutter_plugin_bundled_libraries
  "$<TARGET_FILE:${PLUGIN_NAME}>"
  "$<TARGET_FILE:scrcpy_ffi>"
  PARENT_SCOPE
)
