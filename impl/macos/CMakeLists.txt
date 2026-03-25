find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

file(GLOB_RECURSE AAUTO_MACOS_SRC "src/*.cpp")
add_library(aauto_macos STATIC ${AAUTO_MACOS_SRC})

target_include_directories(aauto_macos PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_include_directories(aauto_macos SYSTEM PUBLIC
    ${SDL2_INCLUDE_DIRS})

target_link_directories(aauto_macos PUBLIC
    ${SDL2_LIBRARY_DIRS})

target_link_libraries(aauto_macos PUBLIC
    aauto_common
    ${SDL2_LIBRARIES})
