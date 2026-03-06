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

if(LIBUSB_FOUND)
    # 소스 파일들 수집
    file(GLOB_RECURSE AAUTO_SRC_FILES "src/*.cpp")
    
    # 정적 라이브러리로 빌드 (또는 SHARED로 변경 가능)
    add_library(aauto STATIC ${AAUTO_SRC_FILES})
    
    # aauto 라이브러리를 사용하는 대상에게 include 디렉토리 노출
    target_include_directories(aauto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    
    # libusb 의존성 설정 (aauto_sim에서도 libusb 헤더를 볼 수 있도록 PUBLIC으로 설정)
    target_include_directories(aauto PUBLIC ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(aauto PUBLIC ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(aauto PUBLIC ${LIBUSB_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)
    
    # 메인 시뮬레이터 실행 파일
    add_executable(aauto_sim main.cpp)
    target_link_libraries(aauto_sim PRIVATE aauto)
else()
    message(WARNING "libusb-1.0 not found. aauto library and aauto_sim will not be built.")
endif()
