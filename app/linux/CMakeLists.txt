find_package(Qt5 COMPONENTS Widgets REQUIRED)

add_executable(aauto_sim main.cpp)
target_link_libraries(aauto_sim PRIVATE aauto_linux)
