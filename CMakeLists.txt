cmake_minimum_required(VERSION 3.10)
project(streaming_pusher)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options
option(ENABLE_WHIP "Enable WebRTC WHIP support" ON)

# Find Dependencies
find_package(PkgConfig REQUIRED)

set(FFMPEG_LIBS libavformat libavcodec libavutil libswscale libavdevice)
pkg_check_modules(FFMPEG REQUIRED ${FFMPEG_LIBS})

# Source files
set(SOURCES main.cpp)
add_executable(streaming-pusher ${SOURCES})

# Include directories
target_include_directories(streaming-pusher PRIVATE ${FFMPEG_INCLUDE_DIRS})

# Link libraries
target_link_libraries(streaming-pusher PRIVATE ${FFMPEG_LIBRARIES})

if(ENABLE_WHIP)
    find_package(CURL REQUIRED)
    pkg_check_modules(DATACHANNEL libdatachannel)
    if(DATACHANNEL_FOUND)
        target_compile_definitions(streaming-pusher PRIVATE ENABLE_WHIP)
        target_include_directories(streaming-pusher PRIVATE ${CURL_INCLUDE_DIRS} ${DATACHANNEL_INCLUDE_DIRS})
        target_link_libraries(streaming-pusher PRIVATE ${CURL_LIBRARIES} ${DATACHANNEL_LIBRARIES})
    else()
        message(WARNING "libdatachannel not found, WHIP support disabled")
    endif()
endif()

if(WIN32)
    target_link_libraries(streaming-pusher PRIVATE ws2_32)
endif()

# Installation
install(TARGETS streaming-pusher DESTINATION bin)
