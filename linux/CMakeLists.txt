cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "scrcpy_flutter_plugin")
project(${PROJECT_NAME} LANGUAGES CXX C)

# Build the shared FFI library from the cross-platform src/ directory.
add_subdirectory("../src" "${CMAKE_CURRENT_BINARY_DIR}/shared")

# The Flutter plugin wrapper (MethodChannel handler).
set(PLUGIN_NAME "scrcpy_flutter_plugin_plugin")

add_library(${PLUGIN_NAME} SHARED
  "scrcpy_flutter_plugin.cc"
  "scrcpy_video_texture.cc"
)

# Apply standard settings if defined by the outer Flutter shell
if(COMMAND apply_standard_settings)
  apply_standard_settings(${PLUGIN_NAME})
endif()

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# Tìm kiếm dependencies bắt buộc trên Linux (GTK và Flutter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(FLUTTER REQUIRED IMPORTED_TARGET flutter_linux_gtk)
pkg_check_modules(VAAPI REQUIRED IMPORTED_TARGET libva libva-drm libdrm)

target_include_directories(${PLUGIN_NAME} PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../.build/linux/libs/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  PkgConfig::GTK
  PkgConfig::FLUTTER
  PkgConfig::VAAPI
)

# Bundle both the plugin shared library and the FFI library.
set(scrcpy_flutter_plugin_bundled_libraries
  "$<TARGET_FILE:${PLUGIN_NAME}>"
  "$<TARGET_FILE:scrcpy_ffi>"
  PARENT_SCOPE
)
