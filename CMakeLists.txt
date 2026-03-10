cmake_minimum_required(VERSION 3.10)
project(stream_push LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============ 可选功能 ============
option(ENABLE_WHIP "启用 WHIP/WebRTC 推流 (需要 libdatachannel + libcurl)" OFF)

# ============ FFmpeg (始终需要) ============
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(AVDEVICE REQUIRED libavdevice)
pkg_check_modules(SWSCALE REQUIRED libswscale)

add_executable(stream_push main.cpp)

target_include_directories(stream_push PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${AVDEVICE_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_libraries(stream_push PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${AVDEVICE_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)

# ============ WHIP/WebRTC (可选) ============
if(ENABLE_WHIP)
    find_package(LibDataChannel REQUIRED)
    find_package(CURL REQUIRED)
    target_compile_definitions(stream_push PRIVATE ENABLE_WHIP)
    target_link_libraries(stream_push PRIVATE
        LibDataChannel::LibDataChannel
        CURL::libcurl
    )
    message(STATUS "WHIP/WebRTC 支持: 已启用")
else()
    message(STATUS "WHIP/WebRTC 支持: 未启用 (轻量模式，仅 FFmpeg)")
endif()
