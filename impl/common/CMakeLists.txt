find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale)

file(GLOB_RECURSE AAUTO_COMMON_SRC "src/*.cpp")
add_library(aauto_common STATIC ${AAUTO_COMMON_SRC})

target_include_directories(aauto_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(aauto_common SYSTEM PUBLIC
    ${LIBUSB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS})

target_link_directories(aauto_common PUBLIC
    ${LIBUSB_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(aauto_common PUBLIC
    aauto_core
    ${LIBUSB_LIBRARIES}
    ${FFMPEG_LIBRARIES})
