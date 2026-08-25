cmake_minimum_required(VERSION 3.15)
project(scrcpy_ffi VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(SCRCPY_FFI_PLUGIN)

# State transitions cross from native worker threads to Dart through the
# dynamically linked Dart API. The header and its companion C source ship with
# every Flutter SDK, so no Dart VM library is linked into the plugin.
set(DART_API_DL_INCLUDE_DIR "" CACHE PATH
    "Directory containing dart_api_dl.h and dart_api_dl.c")
if(NOT DART_API_DL_INCLUDE_DIR)
    find_program(DART_EXECUTABLE NAMES dart)
    if(DART_EXECUTABLE)
        get_filename_component(DART_BIN_DIR "${DART_EXECUTABLE}" DIRECTORY)
        set(DART_API_DL_CANDIDATE "${DART_BIN_DIR}/cache/dart-sdk/include")
        if(EXISTS "${DART_API_DL_CANDIDATE}/dart_api_dl.h")
            set(DART_API_DL_INCLUDE_DIR "${DART_API_DL_CANDIDATE}")
        endif()
    endif()
endif()
if(NOT EXISTS "${DART_API_DL_INCLUDE_DIR}/dart_api_dl.h"
        OR NOT EXISTS "${DART_API_DL_INCLUDE_DIR}/dart_api_dl.c")
    message(FATAL_ERROR
        "dart_api_dl headers not found. Set DART_API_DL_INCLUDE_DIR to Flutter's bin/cache/dart-sdk/include")
endif()


# macOS: phải thêm _DARWIN_C_SOURCE toàn cục trước khi include bất kỳ header nào.
# Flag này bật các POSIX extensions: asprintf, v.v.
# Tương đương với cách meson.build của scrcpy dùng '-D_DARWIN_C_SOURCE'.
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
    # Force-include compat_macos.h để khai báo reallocarray cho tất cả file .c
    # (reallocarray không có trong macOS SDK stdlib.h dù macOS 26 + Apple Clang 21)
    add_compile_options(
        -include "${CMAKE_CURRENT_SOURCE_DIR}/compat_macos.h"
    )
elseif(WIN32)
    add_compile_definitions(WIN32 _WIN32_WINNT=0x0600 WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)
endif()

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VAAPI REQUIRED IMPORTED_TARGET libva libva-drm libdrm)
endif()

# ---------------------------------------------------------------------------
# Đường dẫn đến gốc của dự án scrcpy (relative từ thư mục này)
# ---------------------------------------------------------------------------
set(SCRCPY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../scrcpy")
set(SCRCPY_APP  "${SCRCPY_ROOT}/app")
set(SCRCPY_SRC  "${SCRCPY_APP}/src")

# ---------------------------------------------------------------------------
if(WIN32 AND NOT LOCAL_DEPS_DIR)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
        set(WINDOWS_ARCH_DIR "arm64")
    else()
        set(WINDOWS_ARCH_DIR "x64")
    endif()
    set(LOCAL_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.build/windows/libs/${WINDOWS_ARCH_DIR}")
endif()

if(LOCAL_DEPS_DIR)
    message(STATUS "Using local static dependencies from ${LOCAL_DEPS_DIR}")

    # Tự động dò tìm prefix và extension cho các file thư viện tĩnh
    if(EXISTS "${LOCAL_DEPS_DIR}/lib/avformat.lib")
        set(LIB_PREFIX "")
        set(LIB_EXT "lib")
    elseif(EXISTS "${LOCAL_DEPS_DIR}/lib/libavformat.lib")
        set(LIB_PREFIX "lib")
        set(LIB_EXT "lib")
    elseif(EXISTS "${LOCAL_DEPS_DIR}/lib/libavformat.a")
        set(LIB_PREFIX "lib")
        set(LIB_EXT "a")
    else()
        # Mặc định fallback
        set(LIB_PREFIX "lib")
        set(LIB_EXT "a")
    endif()

    # Tự động dò tìm file thư viện SDL3 tĩnh
    if(EXISTS "${LOCAL_DEPS_DIR}/lib/SDL3-static.lib")
        set(SDL3_LIB_NAME "SDL3-static.lib")
    elseif(EXISTS "${LOCAL_DEPS_DIR}/lib/SDL3.lib")
        set(SDL3_LIB_NAME "SDL3.lib")
    elseif(EXISTS "${LOCAL_DEPS_DIR}/lib/libSDL3.a")
        set(SDL3_LIB_NAME "libSDL3.a")
    else()
        set(SDL3_LIB_NAME "${LIB_PREFIX}SDL3.${LIB_EXT}")
    endif()
    
    add_library(avformat STATIC IMPORTED)
    set_target_properties(avformat PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}avformat.${LIB_EXT}")
    
    add_library(avcodec STATIC IMPORTED)
    set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}avcodec.${LIB_EXT}")
    
    add_library(avutil STATIC IMPORTED)
    set_target_properties(avutil PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}avutil.${LIB_EXT}")
    
    add_library(swscale STATIC IMPORTED)
    set_target_properties(swscale PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}swscale.${LIB_EXT}")
    
    add_library(swresample STATIC IMPORTED)
    set_target_properties(swresample PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}swresample.${LIB_EXT}")
    
    add_library(dav1d STATIC IMPORTED)
    set_target_properties(dav1d PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${LIB_PREFIX}dav1d.${LIB_EXT}")
    
    add_library(sdl3 STATIC IMPORTED)
    set_target_properties(sdl3 PROPERTIES IMPORTED_LOCATION "${LOCAL_DEPS_DIR}/lib/${SDL3_LIB_NAME}")
    
    set(FFMPEG_INCLUDE_DIRS "${LOCAL_DEPS_DIR}/include")
    set(SDL3_INCLUDE_DIRS "${LOCAL_DEPS_DIR}/include")
    
    set(FFMPEG_LINK_LIBRARIES avformat avcodec avutil swscale swresample dav1d)
    set(SDL3_LINK_LIBRARIES sdl3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample
    )
    pkg_check_modules(SDL3 REQUIRED sdl3)
endif()

# ---------------------------------------------------------------------------
# Tạo config.h tối giản (scrcpy core cần file này)
# Các giá trị này tương ứng với meson_options.txt của scrcpy
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
)

# ---------------------------------------------------------------------------
# Các source files của scrcpy core CẦN THIẾT để biên dịch
# (Đã loại bỏ: main.c, scrcpy.c, screen.c, texture.c, audio_player.c,
#              audio_regulator.c, input_manager.c, keyboard_sdk.c,
#              mouse_sdk.c, mouse_capture.c, sdl_hints.c, opengl.c,
#              icon.c, file_pusher.c, cli.c, fps_counter.c, frame_buffer.c,
#              disconnect.c, events.c, recorder.c, video_regulator.c
#  — Tất cả file phụ thuộc SDL đồ họa đều bị loại bỏ)
# ---------------------------------------------------------------------------
set(SCRCPY_CORE_SOURCES
    ${SCRCPY_SRC}/cli.c
    # ADB layer
    ${SCRCPY_SRC}/adb/adb.c
    ${SCRCPY_SRC}/adb/adb_device.c
    ${SCRCPY_SRC}/adb/adb_parser.c
    ${SCRCPY_SRC}/adb/adb_tunnel.c

    # Media pipeline (không SDL đồ họa, nhưng giữ audio)
    ${SCRCPY_SRC}/audio_player.c
    ${SCRCPY_SRC}/audio_regulator.c
    ${SCRCPY_SRC}/clock.c
    ${SCRCPY_SRC}/compat.c
    ${SCRCPY_SRC}/control_msg.c
    ${SCRCPY_SRC}/controller.c
    ${SCRCPY_SRC}/decoder.c
    ${SCRCPY_SRC}/demuxer.c
    ${SCRCPY_SRC}/device_msg.c
    ${SCRCPY_SRC}/options.c
    ${SCRCPY_SRC}/packet_merger.c
    ${SCRCPY_SRC}/receiver.c
    ${SCRCPY_SRC}/server.c
    # version.c đã bị loại bỏ — scrcpy_print_version() phụ thuộc SDL_GetVersion()
    # Thay vào đó ta cung cấp stub trong scrcpy_ffi.cpp

    # Traits (pattern kế thừa C)
    ${SCRCPY_SRC}/trait/frame_source.c
    ${SCRCPY_SRC}/trait/packet_source.c

    # UHID input (không OTG/USB)
    ${SCRCPY_SRC}/hid/hid_gamepad.c
    ${SCRCPY_SRC}/hid/hid_keyboard.c
    ${SCRCPY_SRC}/hid/hid_mouse.c
    ${SCRCPY_SRC}/uhid/gamepad_uhid.c
    ${SCRCPY_SRC}/uhid/keyboard_uhid.c
    ${SCRCPY_SRC}/uhid/mouse_uhid.c
    ${SCRCPY_SRC}/uhid/uhid_output.c

    # Utilities
    ${SCRCPY_SRC}/util/acksync.c
    ${SCRCPY_SRC}/util/audiobuf.c
    ${SCRCPY_SRC}/util/average.c
    ${SCRCPY_SRC}/util/env.c
    ${SCRCPY_SRC}/util/file.c
    ${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_log.c
    ${SCRCPY_SRC}/util/intmap.c
    ${SCRCPY_SRC}/util/intr.c
    ${SCRCPY_SRC}/util/memory.c
    ${SCRCPY_SRC}/util/net.c
    ${SCRCPY_SRC}/util/net_intr.c
    ${SCRCPY_SRC}/util/process.c
    ${SCRCPY_SRC}/util/process_intr.c
    ${SCRCPY_SRC}/util/rand.c
    ${SCRCPY_SRC}/util/strbuf.c
    ${SCRCPY_SRC}/util/str.c
    ${SCRCPY_SRC}/util/term.c
    # Override hoàn chỉnh để mọi thread con kế thừa instance TLS của plugin.
    # Không chỉnh util/thread.c của scrcpy.
    ${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_thread.c
    ${SCRCPY_SRC}/util/tick.c
    ${SCRCPY_SRC}/util/timeout.c
)

# Platform-specific sources
if(APPLE)
    list(APPEND SCRCPY_CORE_SOURCES
        ${SCRCPY_SRC}/sys/unix/file.c
        ${SCRCPY_SRC}/sys/unix/process.c
    )
elseif(UNIX)
    list(APPEND SCRCPY_CORE_SOURCES
        ${SCRCPY_SRC}/sys/unix/file.c
        ${SCRCPY_SRC}/sys/unix/process.c
    )
elseif(WIN32)
    list(APPEND SCRCPY_CORE_SOURCES
        ${SCRCPY_SRC}/sys/win/file.c
        ${SCRCPY_SRC}/sys/win/process.c
    )
endif()

# ---------------------------------------------------------------------------
# Build scrcpy core thành static library
# ---------------------------------------------------------------------------
add_library(scrcpy_core STATIC ${SCRCPY_CORE_SOURCES})

target_include_directories(scrcpy_core PUBLIC
    ${SCRCPY_SRC}
    ${CMAKE_CURRENT_BINARY_DIR}   # cho config.h
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL3_INCLUDE_DIRS}
)

if(WIN32)
    target_compile_definitions(scrcpy_core PRIVATE
        WIN32
        _WIN32_WINNT=0x0600
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
else()
    target_compile_definitions(scrcpy_core PRIVATE
        _GNU_SOURCE
        _POSIX_C_SOURCE=200809L
    )
endif()

target_compile_definitions(scrcpy_core PRIVATE
    SDL_IsMainThread=sc_SDL_IsMainThread
    SDL_SetClipboardText=sc_SDL_SetClipboardText
    SDL_GetClipboardText=sc_SDL_GetClipboardText
    avcodec_open2=sc_avcodec_open2
)

if(APPLE)
    target_compile_definitions(scrcpy_core PRIVATE _DARWIN_C_SOURCE)
    # _DARWIN_C_SOURCE bật các POSIX extensions trên macOS,
    # bao gồm reallocarray, asprintf, v.v.
    # Đây là cùng flag mà meson.build của scrcpy dùng cho macOS.
endif()


# Override các hàm adb và process để cô lập app/src
#
# QUAN TRỌNG: sc_process_wait phải được override cho mọi translation unit
# gọi nó, vì Flutter's Dart runtime có zombie reaper riêng khiến waitid()
# gốc của scrcpy trả về ECHILD (errno=10). scrcpy_ffi_process_wait dùng
# waitpid() và xử lý ECHILD đúng cách (coi như success).
set_source_files_properties(${SCRCPY_SRC}/server.c PROPERTIES
    COMPILE_DEFINITIONS "sc_adb_start_server=scrcpy_ffi_adb_start_server;sc_adb_push=scrcpy_ffi_adb_push;sc_adb_execute=scrcpy_ffi_adb_execute;sc_adb_init=scrcpy_ffi_adb_init;sc_adb_destroy=scrcpy_ffi_adb_destroy;sc_process_terminate=scrcpy_ffi_process_terminate;sc_adb_select_device=scrcpy_ffi_adb_select_device;sc_process_wait=scrcpy_ffi_process_wait"
)

set_source_files_properties(${SCRCPY_SRC}/util/intr.c PROPERTIES
    COMPILE_DEFINITIONS "sc_process_terminate=scrcpy_ffi_process_terminate"
)

# sc_process_observer (trong util/process.c) cũng gọi sc_process_wait.
set_source_files_properties(${SCRCPY_SRC}/util/process.c PROPERTIES
    COMPILE_DEFINITIONS "sc_process_wait=scrcpy_ffi_process_wait;sc_process_execute_p=scrcpy_ffi_process_execute_p"
)

set_source_files_properties(${SCRCPY_SRC}/adb/adb.c PROPERTIES
    COMPILE_DEFINITIONS "sc_process_execute_p=scrcpy_ffi_process_execute_p;sc_process_wait=scrcpy_ffi_process_wait"
)

# Decode interception is isolated to the two upstream translation units that
# invoke FFmpeg. Demuxers continue draining their sockets while the plugin is
# paused, but packets never reach the local CPU/GPU decoders.
set_source_files_properties(${SCRCPY_SRC}/decoder.c PROPERTIES
    COMPILE_DEFINITIONS "avcodec_send_packet=sc_avcodec_send_packet;avcodec_receive_frame=sc_avcodec_receive_frame"
)
set_source_files_properties(${SCRCPY_SRC}/demuxer.c PROPERTIES
    COMPILE_DEFINITIONS "avcodec_free_context=sc_avcodec_free_context"
)

if(MSVC)
    target_compile_options(scrcpy_core PRIVATE
        /W3
        /wd4018 # signed/unsigned mismatch
        /wd4244 # loss of data
        /wd4267 # loss of data in conversion
    )
else()
    target_compile_options(scrcpy_core PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -Wno-c23-extensions
    )
endif()

# ---------------------------------------------------------------------------
# Build scrcpy_ffi modular sources into shared library (libscrcpy_ffi.dylib / .so)
# ---------------------------------------------------------------------------
add_library(scrcpy_ffi SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_sinks.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_control.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi_adb.c"
    "${DART_API_DL_INCLUDE_DIR}/dart_api_dl.c"
)

target_include_directories(scrcpy_ffi PRIVATE
    ${SCRCPY_SRC}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
    ${DART_API_DL_INCLUDE_DIR}
)

target_compile_definitions(scrcpy_ffi PRIVATE
    SC_NO_SDL=1
    DART_SHARED_LIB
    SDL_IsMainThread=sc_SDL_IsMainThread
    SDL_SetClipboardText=sc_SDL_SetClipboardText
    SDL_GetClipboardText=sc_SDL_GetClipboardText
)

# Link: FFI Wrapper → scrcpy_core (static) → FFmpeg + SDL3
target_link_libraries(scrcpy_ffi
    PRIVATE
        scrcpy_core
        ${FFMPEG_LINK_LIBRARIES}
        ${SDL3_LINK_LIBRARIES}
)

if(APPLE)
    target_link_libraries(scrcpy_ffi PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
        "-framework AudioToolbox"
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework AVFoundation"
        "-framework Cocoa"
        "-framework IOKit"
        "-framework Carbon"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework GameController"
        "-framework ForceFeedback"
        "-framework CoreAudio"
        "-framework CoreHaptics"
        "-weak_framework UniformTypeIdentifiers"
        "iconv"
        "z"
        "bz2"
    )
elseif(WIN32)
    target_link_libraries(scrcpy_ffi PRIVATE
        ws2_32
        winmm
        mmdevapi
        avrt
        imm32
        version
        setupapi
        shlwapi
        shell32
        gdi32
        ole32
        oleaut32
        uuid
    )
elseif(UNIX)
    target_link_libraries(scrcpy_ffi PRIVATE
        PkgConfig::VAAPI
        ${CMAKE_DL_LIBS}
        pthread
        m
    )
endif()

# Build a plain shared library. The macOS packaging script wraps it in one
# flat framework before creating the XCFramework.
set_target_properties(scrcpy_ffi PROPERTIES
    OUTPUT_NAME "scrcpy_ffi"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
)

# ---------------------------------------------------------------------------
# Cài đặt (Flutter bundling)
# ---------------------------------------------------------------------------
install(TARGETS scrcpy_ffi
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/lib"
    RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/scrcpy_ffi.h"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/include"
)

# NOTE: adb.c overrides are defined above (sc_process_execute_p + sc_process_wait).
