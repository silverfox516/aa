find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale)
pkg_check_modules(SDL2   REQUIRED sdl2)

file(GLOB_RECURSE AAUTO_LINUX_SRC "src/*.cpp")
add_library(aauto_linux STATIC ${AAUTO_LINUX_SRC})

target_include_directories(aauto_linux PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(aauto_linux SYSTEM PUBLIC
    ${LIBUSB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS})

target_link_directories(aauto_linux PUBLIC
    ${LIBUSB_LIBRARY_DIRS}
    ${FFMPEG_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS})

target_link_libraries(aauto_linux PUBLIC
    aauto_core
    ${LIBUSB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${SDL2_LIBRARIES})
