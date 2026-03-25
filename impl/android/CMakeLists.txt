# Android NDK 빌드
# Java/Kotlin 레이어에서 USB/WiFi/BT 이벤트를 감지하고
# JNI를 통해 transport fd/소켓을 C++로 전달하는 구조

file(GLOB_RECURSE AAUTO_ANDROID_SRC "src/*.cpp")

if(AAUTO_ANDROID_SRC)
    add_library(aauto_android SHARED ${AAUTO_ANDROID_SRC})

    target_include_directories(aauto_android PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

    target_link_libraries(aauto_android PUBLIC
        aauto_core
        android
        log)
endif()
