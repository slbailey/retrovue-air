cmake_minimum_required(VERSION 3.22)

project(retrovue_air LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Try CONFIG mode first (vcpkg), then fall back to MODULE mode (system packages)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()

find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET
        grpc++
        grpc
        grpc_unsecure
    )
    # Create aliases for compatibility
    if(TARGET PkgConfig::GRPC)
        add_library(gRPC::grpc++ ALIAS PkgConfig::GRPC)
        add_library(gRPC::grpc++_reflection ALIAS PkgConfig::GRPC)
        # Find protoc and grpc_cpp_plugin
        find_program(PROTOC_PROGRAM protoc REQUIRED)
        find_program(GRPC_CPP_PLUGIN_PROGRAM grpc_cpp_plugin REQUIRED)
        # Create a variable for the plugin path (pkg-config doesn't provide a target)
        set(GRPC_CPP_PLUGIN_PATH ${GRPC_CPP_PLUGIN_PROGRAM})
    endif()
endif()

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    find_package(GTest QUIET)
endif()

# FFmpeg libraries for Phase 3 video decoding (optional)
# IMPORTANT: Libraries must be listed in dependency order with avutil LAST
# Correct order: avformat -> avcodec -> swscale -> swresample -> avutil
# This ensures proper symbol resolution and avoids "undefined reference" errors
find_package(PkgConfig)
if(PkgConfig_FOUND)
    # First call to get variables (FFMPEG_LIBRARIES, etc.)
    pkg_check_modules(FFMPEG
        libavformat
        libavcodec
        libswscale
        libswresample
        libavutil)
    # Second call to create imported target (for compatibility)
    pkg_check_modules(FFMPEG_TARGET IMPORTED_TARGET
        libavformat
        libavcodec
        libswscale
        libswresample
        libavutil)
else()
    message(WARNING "PkgConfig not found - FFmpeg detection unavailable. Install pkg-config to build with FFmpeg.")
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
if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
    # Fallback for pkg-config case
    set(_GRPC_CPP_PLUGIN_EXECUTABLE ${GRPC_CPP_PLUGIN_PROGRAM})
endif()

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

add_library(retrovue_air_proto STATIC
    ${GENERATED_PROTO_SRCS}
    ${GENERATED_PROTO_HDRS})

target_include_directories(retrovue_air_proto
    PUBLIC
        ${GENERATED_DIR})

target_link_libraries(retrovue_air_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection)

source_group(TREE ${GENERATED_DIR} FILES ${GENERATED_PROTO_SRCS} ${GENERATED_PROTO_HDRS})

add_executable(retrovue_air_proto_check
    tools/check_api_version.cpp)

target_link_libraries(retrovue_air_proto_check
    PRIVATE retrovue_air_proto)

target_include_directories(retrovue_air_proto_check
    PRIVATE ${GENERATED_DIR})

# Main ffmpeg wrapper executable (Phase 1: CLI wrapper)
add_executable(retrovue_air
    src/main.cpp)

# Set output directory
set_target_properties(retrovue_air PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Enable typical warnings
target_compile_options(retrovue_air PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type)

# Platform-specific socket libraries
if(WIN32)
    # Winsock is linked automatically via pragma comment in source
else()
    # Unix systems need pthread
    find_package(Threads REQUIRED)
    target_link_libraries(retrovue_air PRIVATE Threads::Threads)
endif()

target_include_directories(retrovue_air
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Contract tests (requires GTest via vcpkg)
if(GTest_FOUND)
    message(STATUS "GTest found - building contract tests")
    enable_testing()

    # Contract Tests: MasterClock
    add_executable(contracts_masterclock_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/MasterClock/MasterClockContractTests.cpp
        src/buffer/FrameRingBuffer.cpp
        src/decode/FrameProducer.cpp
        src/decode/FFmpegDecoder.cpp
        src/renderer/FrameRenderer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp)

    target_link_libraries(contracts_masterclock_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_masterclock_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(contracts_masterclock_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    if(FFMPEG_FOUND)
        # Link FFmpeg libraries - use library directories and libraries separately
        target_link_directories(contracts_masterclock_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(contracts_masterclock_tests PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(contracts_masterclock_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(contracts_masterclock_tests PRIVATE ${FFMPEG_CFLAGS_OTHER})
    endif()

    if(SDL2_FOUND)
        target_link_libraries(contracts_masterclock_tests PRIVATE SDL2::SDL2 SDL2::SDL2main)
    endif()

    if(NOT WIN32)
        target_link_libraries(contracts_masterclock_tests PRIVATE Threads::Threads)
    endif()

    add_test(NAME contracts_masterclock COMMAND contracts_masterclock_tests)

    # Contract Tests: Metrics & Timing (combined with Metrics Export)
    add_executable(contracts_metricsandtiming_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/MetricsAndTiming/MetricsAndTimingContractTests.cpp
        tests/contracts/MetricsExport/MetricsExportContractTests.cpp
        src/buffer/FrameRingBuffer.cpp
        src/decode/FrameProducer.cpp
        src/decode/FFmpegDecoder.cpp
        src/renderer/FrameRenderer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp)

    target_link_libraries(contracts_metricsandtiming_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_metricsandtiming_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(FFMPEG_FOUND)
        target_link_directories(contracts_metricsandtiming_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(contracts_metricsandtiming_tests PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(contracts_metricsandtiming_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(contracts_metricsandtiming_tests PRIVATE ${FFMPEG_CFLAGS_OTHER})
    endif()

    if(SDL2_FOUND)
        target_link_libraries(contracts_metricsandtiming_tests PRIVATE SDL2::SDL2 SDL2::SDL2main)
    endif()

    if(NOT WIN32)
        target_link_libraries(contracts_metricsandtiming_tests PRIVATE Threads::Threads)
    endif()

    target_include_directories(contracts_metricsandtiming_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # Combined test executable includes both MetricsAndTiming and MetricsExport tests
    add_test(NAME contracts_metricsandtiming COMMAND contracts_metricsandtiming_tests)

    # Contract Tests: Playout Control
    add_executable(contracts_playoutcontrol_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/PlayoutControl/PlayoutControlContractTests.cpp
        src/runtime/PlayoutControlStateMachine.cpp)

    target_link_libraries(contracts_playoutcontrol_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_playoutcontrol_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(contracts_playoutcontrol_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_playoutcontrol COMMAND contracts_playoutcontrol_tests)

    # Contract Tests: Orchestration Loop
    add_executable(contracts_orchestrationloop_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/OrchestrationLoop/OrchestrationLoopContractTests.cpp
        src/runtime/OrchestrationLoop.cpp
        src/timing/SystemMasterClock.cpp)

    target_link_libraries(contracts_orchestrationloop_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_orchestrationloop_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(contracts_orchestrationloop_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_orchestrationloop COMMAND contracts_orchestrationloop_tests)

    # Contract Tests: Playout Engine
    add_executable(contracts_playoutengine_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/PlayoutEngine/PlayoutEngineContractTests.cpp
        src/buffer/FrameRingBuffer.cpp
        src/decode/FrameProducer.cpp
        src/decode/FFmpegDecoder.cpp
        src/renderer/FrameRenderer.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp
        include/retrovue/telemetry/MetricsExporter.h)

    target_link_libraries(contracts_playoutengine_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_playoutengine_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(FFMPEG_FOUND)
        target_link_directories(contracts_playoutengine_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(contracts_playoutengine_tests PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(contracts_playoutengine_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(contracts_playoutengine_tests PRIVATE ${FFMPEG_CFLAGS_OTHER})
    endif()

    if(SDL2_FOUND)
        target_link_libraries(contracts_playoutengine_tests PRIVATE SDL2::SDL2 SDL2::SDL2main)
    endif()

    if(NOT WIN32)
        target_link_libraries(contracts_playoutengine_tests PRIVATE Threads::Threads)
    endif()

    target_include_directories(contracts_playoutengine_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_playoutengine COMMAND contracts_playoutengine_tests)

    # Contract Tests: Renderer
    add_executable(contracts_renderer_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/Renderer/RendererContractTests.cpp
        src/buffer/FrameRingBuffer.cpp
        src/renderer/FrameRenderer.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp
        include/retrovue/telemetry/MetricsExporter.h)

    target_link_libraries(contracts_renderer_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_renderer_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(SDL2_FOUND)
        target_link_libraries(contracts_renderer_tests PRIVATE SDL2::SDL2 SDL2::SDL2main)
    endif()

    if(NOT WIN32)
        target_link_libraries(contracts_renderer_tests PRIVATE Threads::Threads)
    endif()

    target_include_directories(contracts_renderer_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_renderer COMMAND contracts_renderer_tests)

    # Contract Tests: Video File Producer
    add_executable(contracts_videofileproducer_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/video_file_producer/VideoFileProducerContractTests.cpp
        src/producers/video_file/VideoFileProducer.cpp
        src/buffer/FrameRingBuffer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp)

    target_link_libraries(contracts_videofileproducer_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_videofileproducer_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(FFMPEG_FOUND)
        target_link_directories(contracts_videofileproducer_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(contracts_videofileproducer_tests PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(contracts_videofileproducer_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(contracts_videofileproducer_tests PRIVATE ${FFMPEG_CFLAGS_OTHER})
        target_compile_definitions(contracts_videofileproducer_tests PRIVATE RETROVUE_FFMPEG_AVAILABLE)
    endif()

    target_include_directories(contracts_videofileproducer_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    if(NOT WIN32)
        target_link_libraries(contracts_videofileproducer_tests PRIVATE Threads::Threads)
    endif()

    add_test(NAME contracts_videofileproducer COMMAND contracts_videofileproducer_tests)

    # Contract Tests: MPEG-TS Playout Sink
    add_executable(contracts_mpegtsplayoutsink_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp
        src/playout_sinks/mpegts/EncoderPipeline.cpp
        src/playout_sinks/mpegts/MpegTSPlayoutSink.cpp
        src/playout_sinks/mpegts/PTSController.cpp
        src/playout_sinks/mpegts/ClockUtils.cpp
        src/buffer/FrameRingBuffer.cpp
        src/producers/video_file/VideoFileProducer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp)

    target_link_libraries(contracts_mpegtsplayoutsink_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_mpegtsplayoutsink_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(FFMPEG_FOUND)
        target_link_directories(contracts_mpegtsplayoutsink_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(contracts_mpegtsplayoutsink_tests PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(contracts_mpegtsplayoutsink_tests PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(contracts_mpegtsplayoutsink_tests PRIVATE ${FFMPEG_CFLAGS_OTHER})
        target_compile_definitions(contracts_mpegtsplayoutsink_tests PRIVATE RETROVUE_FFMPEG_AVAILABLE)
    endif()

    target_include_directories(contracts_mpegtsplayoutsink_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    if(NOT WIN32)
        target_link_libraries(contracts_mpegtsplayoutsink_tests PRIVATE Threads::Threads)
    endif()

    # Combined test executable includes all MPEG-TS playout sink contract tests
    add_test(NAME contracts_mpegtsplayoutsink COMMAND contracts_mpegtsplayoutsink_tests)

    # Unit Tests: MPEG-TS Playout Sink
    add_executable(test_sink
        tests/test_sink.cpp
        src/buffer/FrameRingBuffer.cpp
        src/sinks/mpegts/MpegTSPlayoutSink.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp)

    target_link_libraries(test_sink
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(test_sink
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    if(FFMPEG_FOUND)
        target_link_directories(test_sink PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(test_sink PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(test_sink PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(test_sink PRIVATE ${FFMPEG_CFLAGS_OTHER})
        target_compile_definitions(test_sink PRIVATE RETROVUE_FFMPEG_AVAILABLE)
    endif()

    target_include_directories(test_sink
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    if(NOT WIN32)
        target_link_libraries(test_sink PRIVATE Threads::Threads)
    endif()

    add_test(NAME test_sink COMMAND test_sink)

    # Integration Test: Frame Cadence
    add_executable(integration_frame_cadence_tests
        tests/integration/FrameCadenceIntegrationTests.cpp
        src/buffer/FrameRingBuffer.cpp
        src/timing/TestMasterClock.cpp)

    target_link_libraries(integration_frame_cadence_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(integration_frame_cadence_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(integration_frame_cadence_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME integration_frame_cadence COMMAND integration_frame_cadence_tests)

    add_executable(timing_soak
        tools/soak/TimingSoak.cpp
        src/buffer/FrameRingBuffer.cpp
        src/decode/FrameProducer.cpp
        src/decode/FFmpegDecoder.cpp
        src/playout_service.cpp
        src/runtime/OrchestrationLoop.cpp
    src/runtime/PlayoutControlStateMachine.cpp
        src/renderer/FrameRenderer.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp
        src/timing/SystemMasterClock.cpp
        src/timing/TestMasterClock.cpp)

    target_link_libraries(timing_soak
        PRIVATE
            retrovue_air_proto
            protobuf::libprotobuf
            gRPC::grpc++
            gRPC::grpc++_reflection)

    target_compile_definitions(timing_soak
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(timing_soak
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    if(FFMPEG_FOUND)
        target_link_directories(timing_soak PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(timing_soak PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(timing_soak PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_options(timing_soak PRIVATE ${FFMPEG_CFLAGS_OTHER})
    endif()

    if(SDL2_FOUND)
        target_link_libraries(timing_soak PRIVATE SDL2::SDL2 SDL2::SDL2main)
    endif()

    if(NOT WIN32)
        target_link_libraries(timing_soak PRIVATE Threads::Threads)
    endif()
else()
    message(WARNING "GTest not found - skipping unit tests. Install via: vcpkg install gtest")
endif()
