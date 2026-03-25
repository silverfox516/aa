find_package(OpenSSL REQUIRED)

file(GLOB_RECURSE AAUTO_CORE_SRC "src/*.cpp")
add_library(aauto_core STATIC ${AAUTO_CORE_SRC})

target_include_directories(aauto_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(aauto_core PUBLIC
    aap_protobuf
    OpenSSL::SSL
    OpenSSL::Crypto)
