cmake_minimum_required(VERSION 3.22)
project(retrovue_playout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(GTest CONFIG)

# FFmpeg libraries for Phase 3 video decoding (optional)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale)
endif()

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found - real video decoding enabled")
    add_compile_definitions(RETROVUE_FFMPEG_AVAILABLE)
else()
    message(WARNING "FFmpeg not found - only stub mode available. Install FFmpeg for real decoding.")
endif()

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/retrovue/playout.proto)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

set(GENERATED_PROTO_SRCS
    ${GENERATED_DIR}/retrovue/playout.pb.cc
    ${GENERATED_DIR}/retrovue/playout.grpc.pb.cc)
set(GENERATED_PROTO_HDRS
    ${GENERATED_DIR}/retrovue/playout.pb.h
    ${GENERATED_DIR}/retrovue/playout.grpc.pb.h)

add_custom_command(
    OUTPUT ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS}
    COMMAND ${_PROTOBUF_PROTOC}
            --grpc_out ${GENERATED_DIR}
            --cpp_out ${GENERATED_DIR}
            --grpc_opt generate_mock_code=false
            --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
            ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating gRPC sources from ${PROTO_FILE}"
    VERBATIM)

set_source_files_properties(${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS}
                            PROPERTIES GENERATED TRUE)

add_library(retrovue_playout_proto STATIC
    ${GENERATED_PROTO_SRCS}
    ${GENERATED_PROTO_HDRS})

target_include_directories(retrovue_playout_proto
    PUBLIC
        ${GENERATED_DIR})

target_link_libraries(retrovue_playout_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection)

source_group(TREE ${GENERATED_DIR} FILES ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS})

add_executable(retrovue_playout_proto_check
    tools/check_api_version.cpp)

target_link_libraries(retrovue_playout_proto_check
    PRIVATE retrovue_playout_proto)

target_include_directories(retrovue_playout_proto_check
    PRIVATE ${GENERATED_DIR})

# Main playout engine executable
add_executable(retrovue_playout
    src/main.cpp
    src/playout_service.cpp
    src/playout_service.h
    src/buffer/FrameRingBuffer.cpp
    src/decode/FrameProducer.cpp
    src/decode/FFmpegDecoder.cpp
    src/renderer/FrameRenderer.cpp
    src/telemetry/MetricsExporter.cpp
    src/telemetry/MetricsHTTPServer.cpp
    include/retrovue/buffer/FrameRingBuffer.h
    include/retrovue/decode/FrameProducer.h
    include/retrovue/decode/FFmpegDecoder.h
    include/retrovue/renderer/FrameRenderer.h
    include/retrovue/telemetry/MetricsExporter.h
    include/retrovue/telemetry/MetricsHTTPServer.h)

target_link_libraries(retrovue_playout
    PRIVATE 
        retrovue_playout_proto)

if(FFMPEG_FOUND)
    target_link_libraries(retrovue_playout PRIVATE PkgConfig::FFMPEG)
endif()

# SDL2 libraries for Phase 3 preview renderer (optional)
find_package(SDL2 CONFIG)
if(SDL2_FOUND)
    message(STATUS "SDL2 found - preview renderer enabled")
    add_compile_definitions(RETROVUE_SDL2_AVAILABLE)
    target_link_libraries(retrovue_playout PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    message(WARNING "SDL2 not found - preview renderer disabled. Install SDL2 for preview window support.")
endif()

# Platform-specific socket libraries for HTTP server
if(WIN32)
    # Winsock is linked automatically via pragma comment in source
else()
    # Unix systems need pthread
    find_package(Threads REQUIRED)
    target_link_libraries(retrovue_playout PRIVATE Threads::Threads)
endif()

target_include_directories(retrovue_playout
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})

# Unit tests (optional - requires GTest via vcpkg)
if(GTest_FOUND)
    message(STATUS "GTest found - building unit tests")
    enable_testing()

    # Test: Frame Ring Buffer
    add_executable(test_buffer
        tests/test_buffer.cpp
        src/buffer/FrameRingBuffer.cpp
        include/retrovue/buffer/FrameRingBuffer.h)

    target_link_libraries(test_buffer
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_include_directories(test_buffer
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_test(NAME test_buffer COMMAND test_buffer)

    # Test: Frame Producer
    add_executable(test_decode
        tests/test_decode.cpp
        src/decode/FrameProducer.cpp
        include/retrovue/decode/FrameProducer.h
        src/buffer/FrameRingBuffer.cpp
        include/retrovue/buffer/FrameRingBuffer.h)

    target_link_libraries(test_decode
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_include_directories(test_decode
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_test(NAME test_decode COMMAND test_decode)
else()
    message(WARNING "GTest not found - skipping unit tests. Install via: vcpkg install gtest")
endif()

