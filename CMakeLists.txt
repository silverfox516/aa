cmake_minimum_required(VERSION 3.10)

# 이 디렉토리에서 독자적으로 cmake 커맨드를 실행한 경우를 판별
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AAutoFramework)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
endif()

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# FFmpeg + SDL2 의존성 (비디오 렌더링)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale)
pkg_check_modules(SDL2 REQUIRED sdl2)

# find_package(Protobuf) 는 서브디렉토리(protobuf) 내부에서 처리하도록 위임함

if(LIBUSB_FOUND)
    # 소스 파일들 수집
    file(GLOB_RECURSE AAUTO_SRC_FILES "src/*.cpp")

    # 1. 새 protobuf 폴더를 서브 디렉토리로 추가 (aap_protobuf 빌드)
    add_subdirectory(protobuf)

    # 정적 라이브러리로 빌드
    add_library(aauto STATIC ${AAUTO_SRC_FILES})

    # aauto 라이브러리를 사용하는 대상에게 include 디렉토리 노출
    target_include_directories(aauto PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR})
    # FFmpeg, SDL2는 SYSTEM으로 추가 (project protobuf 헤더보다 낮은 우선순위)
    target_include_directories(aauto SYSTEM PUBLIC
        ${FFMPEG_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS})

    # libusb 의존성 설정 (aauto_sim에서도 libusb 헤더를 볼 수 있도록 PUBLIC으로 설정)
    target_include_directories(aauto PUBLIC ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(aauto PUBLIC ${LIBUSB_LIBRARY_DIRS} ${FFMPEG_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS})

    # aap_protobuf provides necessary Protobuf and Abseil targets
    target_link_libraries(aauto PUBLIC
        aap_protobuf
        ${LIBUSB_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES})

    # 메인 시뮬레이터 실행 파일
    add_executable(aauto_sim main.cpp)
    target_link_libraries(aauto_sim PRIVATE aauto)
else()
    message(WARNING "libusb-1.0 not found. aauto library and aauto_sim will not be built.")
endif()
