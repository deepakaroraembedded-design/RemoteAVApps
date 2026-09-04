cmake_minimum_required(VERSION 3.16)

project(vmc_thin_client
    VERSION 0.1.0
    DESCRIPTION "VMC thin client stack (embedded-oriented C11)"
    LANGUAGES C
)

set(VMC_THINCLIENT_VERSION ${PROJECT_VERSION})

option(VMC_ENABLE_TESTS "Build unit tests" ON)
option(VMC_ENABLE_SANITIZERS "Build with ASan/UBSan" OFF)
option(VMC_DEBUG "DASH jitter instrumentation" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Werror=implicit-function-declaration)
endif()

if(VMC_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(VMC_CORE_SOURCES
    src/core/error.c
    src/core/logger.c
    src/core/ringbuf.c
    src/core/platform.c
    src/session/state.c
    src/session/session.c
    src/session/mapper.c
    src/transport/protocol.c
    src/transport/jitter_buffer.c
    src/transport/transport.c
    src/transport/udp_transport.c
    src/input/evdev_input.c
    src/input/batch.c
    src/audio/mixer.c
    src/audio/pipeline.c
    src/video/fb_display.c
    src/video/fragment.c
)

# Optional DRM scanout backend (libdrm).
find_path(VMC_DRM_INCLUDE_DIR libdrm/drm.h)
find_library(VMC_DRM_LIBRARY drm)
if(VMC_DRM_INCLUDE_DIR AND VMC_DRM_LIBRARY)
    list(APPEND VMC_CORE_SOURCES src/video/drm_scanout.c)
    set(VMC_DRM_FOUND TRUE)
    message(STATUS "DRM scanout backend: ENABLED")
else()
    set(VMC_DRM_FOUND FALSE)
    message(STATUS "DRM scanout backend: DISABLED")
endif()

# Optional FFmpeg H.264 software decoder backend.
find_path(VMC_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
find_library(VMC_AVCODEC_LIBRARY avcodec)
find_library(VMC_AVUTIL_LIBRARY avutil)
find_library(VMC_SWSCALE_LIBRARY swscale)
find_library(VMC_AVFORMAT_LIBRARY avformat)
find_library(VMC_SWRESAMPLE_LIBRARY swresample)
if(VMC_FFMPEG_INCLUDE_DIR AND VMC_AVCODEC_LIBRARY AND VMC_AVUTIL_LIBRARY AND VMC_SWSCALE_LIBRARY)
    list(APPEND VMC_CORE_SOURCES src/video/ffmpeg_decoder.c)
    set(VMC_FFMPEG_FOUND TRUE)
    message(STATUS "FFmpeg H.264 decoder backend: ENABLED")
else()
    set(VMC_FFMPEG_FOUND FALSE)
    message(STATUS "FFmpeg H.264 decoder backend: DISABLED (no dev headers)")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND VMC_CORE_SOURCES src/platform/linux/time.c)
endif()

# Optional ALSA playback sink (audio downlink).
find_path(VMC_ALSA_INCLUDE_DIR alsa/asoundlib.h)
find_library(VMC_ALSA_LIBRARY asound)
if(VMC_ALSA_INCLUDE_DIR AND VMC_ALSA_LIBRARY)
    list(APPEND VMC_CORE_SOURCES src/audio/alsa_sink.c)
    set(VMC_ALSA_FOUND TRUE)
    message(STATUS "ALSA audio sink: ENABLED")
else()
    set(VMC_ALSA_FOUND FALSE)
    message(STATUS "ALSA audio sink: DISABLED (no dev headers)")
endif()

add_library(vmc_thinclient STATIC ${VMC_CORE_SOURCES})
target_include_directories(vmc_thinclient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(vmc_thinclient PUBLIC VMC_VERSION="${VMC_THINCLIENT_VERSION}")
if(VMC_FFMPEG_FOUND)
    target_include_directories(vmc_thinclient PRIVATE ${VMC_FFMPEG_INCLUDE_DIR})
    target_compile_definitions(vmc_thinclient PUBLIC VMC_HAVE_FFMPEG=1)
    target_link_libraries(vmc_thinclient PUBLIC
        ${VMC_AVCODEC_LIBRARY} ${VMC_AVUTIL_LIBRARY} ${VMC_SWSCALE_LIBRARY})
endif()
if(VMC_DRM_FOUND)
    target_include_directories(vmc_thinclient PRIVATE
        ${VMC_DRM_INCLUDE_DIR} ${VMC_DRM_INCLUDE_DIR}/libdrm)
    target_compile_definitions(vmc_thinclient PUBLIC VMC_DRM_FOUND=1)
    target_link_libraries(vmc_thinclient PUBLIC ${VMC_DRM_LIBRARY})
endif()

# Optional CUDA headers (for the CUVID CUDA hw_device_ctx path).
find_path(VMC_CUDA_INCLUDE_DIR cuda.h)
find_library(VMC_CUDA_LIBRARY cuda)
if(VMC_CUDA_INCLUDE_DIR AND VMC_CUDA_LIBRARY)
    target_compile_definitions(vmc_thinclient PUBLIC VMC_HAVE_CUDA=1)
    target_include_directories(vmc_thinclient PRIVATE ${VMC_CUDA_INCLUDE_DIR})
    target_link_libraries(vmc_thinclient PUBLIC ${VMC_CUDA_LIBRARY})
    message(STATUS "CUDA device-ctx support: ENABLED")
endif()

# Optional ALSA playback sink (audio downlink).
if(VMC_ALSA_FOUND)
    target_compile_definitions(vmc_thinclient PUBLIC VMC_HAVE_ALSA=1)
    target_include_directories(vmc_thinclient PRIVATE ${VMC_ALSA_INCLUDE_DIR})
    target_link_libraries(vmc_thinclient PUBLIC ${VMC_ALSA_LIBRARY})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(vmc_thinclient PRIVATE _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L)
endif()

add_executable(vmc-thinclient-app apps/thinclient/main.c)
target_link_libraries(vmc-thinclient-app PRIVATE vmc_thinclient)
if(VMC_AVFORMAT_LIBRARY)
    target_link_libraries(vmc-thinclient-app PRIVATE ${VMC_AVFORMAT_LIBRARY})
endif()
if(VMC_SWRESAMPLE_LIBRARY)
    target_link_libraries(vmc-thinclient-app PRIVATE ${VMC_SWRESAMPLE_LIBRARY})
endif()
find_package(Threads REQUIRED)
target_link_libraries(vmc-thinclient-app PRIVATE Threads::Threads)

if(VMC_DEBUG)
    target_compile_definitions(vmc_thinclient PUBLIC VMC_DEBUG=1)
    target_compile_definitions(vmc-thinclient-app PRIVATE VMC_DEBUG=1)
endif()

if(VMC_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# MEC simulator (C/CUDA sender).
add_subdirectory(tools/vmc_sim)
# LL-DASH live server (ffmpeg DASH packager + HTTP server).
add_subdirectory(tools/vmc-dash-sim)

# Example cross-compile toolchain is documented in cmake/README-toolchains.md
