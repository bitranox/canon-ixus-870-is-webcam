cmake_minimum_required(VERSION 3.20)
project(chdk-webcam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration (set CMAKE_TOOLCHAIN_FILE to vcpkg.cmake if using vcpkg)
# e.g.: cmake -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ..

# Find dependencies
find_package(PkgConfig QUIET)

# libusb-1.0
find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(STATUS "libusb-1.0 not found via find_path, trying pkg-config")
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    else()
        message(FATAL_ERROR "libusb-1.0 not found. Install via vcpkg: vcpkg install libusb")
    endif()
endif()

# libjpeg-turbo
find_package(JPEG QUIET)
if(NOT JPEG_FOUND)
    find_path(JPEG_INCLUDE_DIRS NAMES jpeglib.h turbojpeg.h)
    find_library(JPEG_LIBRARIES NAMES jpeg turbojpeg jpeg-static turbojpeg-static)
endif()

# Try to find turbojpeg specifically for hardware-accelerated decode
find_path(TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h)
find_library(TURBOJPEG_LIBRARY NAMES turbojpeg turbojpeg-static)

# FFmpeg (for H.264 decoding)
find_path(AVCODEC_INCLUDE_DIR NAMES libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY NAMES avcodec)
find_path(AVUTIL_INCLUDE_DIR NAMES libavutil/frame.h)
find_library(AVUTIL_LIBRARY NAMES avutil)
find_path(SWSCALE_INCLUDE_DIR NAMES libswscale/swscale.h)
find_library(SWSCALE_LIBRARY NAMES swscale)
find_path(AVFORMAT_INCLUDE_DIR NAMES libavformat/avformat.h)
find_library(AVFORMAT_LIBRARY NAMES avformat)

# softcam (virtual webcam DirectShow filter)
find_path(SOFTCAM_INCLUDE_DIR NAMES softcam/softcam.h)
find_library(SOFTCAM_LIBRARY NAMES softcam)

# Sources
set(PTP_SOURCES
    src/ptp/ptp_client.cpp
    src/ptp/ptp_client.h
)

set(WEBCAM_SOURCES
    src/webcam/frame_processor.cpp
    src/webcam/frame_processor.h
    src/webcam/h264_decoder.cpp
    src/webcam/h264_decoder.h
    src/webcam/preview_window.cpp
    src/webcam/preview_window.h
    src/webcam/virtual_webcam.cpp
    src/webcam/virtual_webcam.h
    src/webcam/audio_output.cpp
    src/webcam/audio_output.h
    src/webcam/av_recorder.cpp
    src/webcam/av_recorder.h
)

set(ALL_SOURCES
    src/main.cpp
    ${PTP_SOURCES}
    ${WEBCAM_SOURCES}
)

add_executable(chdk-webcam ${ALL_SOURCES})

target_include_directories(chdk-webcam PRIVATE
    src
    ${LIBUSB_INCLUDE_DIR}
    ${LIBUSB_INCLUDE_DIRS}
)

if(JPEG_FOUND)
    target_include_directories(chdk-webcam PRIVATE ${JPEG_INCLUDE_DIRS})
endif()
if(TURBOJPEG_INCLUDE_DIR)
    target_include_directories(chdk-webcam PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_compile_definitions(chdk-webcam PRIVATE HAS_TURBOJPEG)
endif()
if(AVCODEC_INCLUDE_DIR)
    target_include_directories(chdk-webcam PRIVATE ${AVCODEC_INCLUDE_DIR})
endif()
if(AVUTIL_INCLUDE_DIR)
    target_include_directories(chdk-webcam PRIVATE ${AVUTIL_INCLUDE_DIR})
endif()
if(SWSCALE_INCLUDE_DIR)
    target_include_directories(chdk-webcam PRIVATE ${SWSCALE_INCLUDE_DIR})
endif()

target_link_libraries(chdk-webcam PRIVATE
    ${LIBUSB_LIBRARY}
    ${LIBUSB_LIBRARIES}
)

if(TURBOJPEG_LIBRARY)
    target_link_libraries(chdk-webcam PRIVATE ${TURBOJPEG_LIBRARY})
elseif(JPEG_FOUND)
    target_link_libraries(chdk-webcam PRIVATE ${JPEG_LIBRARIES})
endif()

if(AVCODEC_LIBRARY AND AVUTIL_LIBRARY AND SWSCALE_LIBRARY)
    target_link_libraries(chdk-webcam PRIVATE ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
    target_compile_definitions(chdk-webcam PRIVATE HAS_FFMPEG)
    if(AVFORMAT_LIBRARY)
        target_link_libraries(chdk-webcam PRIVATE ${AVFORMAT_LIBRARY})
        target_compile_definitions(chdk-webcam PRIVATE HAS_AVFORMAT)
        if(AVFORMAT_INCLUDE_DIR)
            target_include_directories(chdk-webcam PRIVATE ${AVFORMAT_INCLUDE_DIR})
        endif()
    endif()
    message(STATUS "FFmpeg found — H.264 decoding enabled")
else()
    message(WARNING "FFmpeg not found — H.264 decoding disabled")
endif()

# softcam virtual webcam
if(SOFTCAM_LIBRARY AND SOFTCAM_INCLUDE_DIR)
    target_include_directories(chdk-webcam PRIVATE ${SOFTCAM_INCLUDE_DIR})
    target_link_libraries(chdk-webcam PRIVATE ${SOFTCAM_LIBRARY})
    message(STATUS "softcam found — virtual webcam enabled")
else()
    message(STATUS "softcam not found — virtual webcam will use shared memory fallback")
endif()

# Windows-specific
if(WIN32)
    target_link_libraries(chdk-webcam PRIVATE ole32 strmiids)
    target_compile_definitions(chdk-webcam PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Install
install(TARGETS chdk-webcam DESTINATION bin)
