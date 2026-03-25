find_package(Qt5 COMPONENTS Widgets OpenGL REQUIRED)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(GST  REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0)

# Q_OBJECT 헤더는 src와 다른 디렉토리에 있으므로 qt5_wrap_cpp로 명시적 MOC 생성
qt5_wrap_cpp(AAUTO_LINUX_MOC
    include/aauto/platform/qt/GlVideoWidget.hpp)

file(GLOB_RECURSE AAUTO_LINUX_SRC "src/*.cpp")
add_library(aauto_linux STATIC ${AAUTO_LINUX_SRC} ${AAUTO_LINUX_MOC})

target_include_directories(aauto_linux PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(aauto_linux SYSTEM PUBLIC
    ${ALSA_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS})

target_link_directories(aauto_linux PUBLIC
    ${ALSA_LIBRARY_DIRS}
    ${GST_LIBRARY_DIRS})

target_link_libraries(aauto_linux PUBLIC
    aauto_common
    Qt5::Widgets
    Qt5::OpenGL
    OpenGL::GL
    ${ALSA_LIBRARIES}
    ${GST_LIBRARIES})
