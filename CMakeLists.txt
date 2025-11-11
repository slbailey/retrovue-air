cmake_minimum_required(VERSION 3.22)

project(retrovue_air LANGUAGES CXX)

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

# Main playout engine executable
add_executable(retrovue_air
    src/main.cpp
    src/playout_service.cpp
    src/buffer/FrameRingBuffer.cpp
    src/decode/FrameProducer.cpp
    src/decode/FFmpegDecoder.cpp
    src/renderer/FrameRenderer.cpp
    src/runtime/OrchestrationLoop.cpp
    src/runtime/PlayoutControlStateMachine.cpp
    src/telemetry/MetricsExporter.cpp
    src/telemetry/MetricsHTTPServer.cpp
    src/timing/SystemMasterClock.cpp
    src/timing/TestMasterClock.cpp
    include/retrovue/buffer/FrameRingBuffer.h
    include/retrovue/decode/FrameProducer.h
    include/retrovue/decode/FFmpegDecoder.h
    include/retrovue/renderer/FrameRenderer.h
    include/retrovue/runtime/OrchestrationLoop.h
    include/retrovue/runtime/PlayoutControlStateMachine.h
    include/retrovue/telemetry/MetricsExporter.h
    include/retrovue/telemetry/MetricsHTTPServer.h)

target_link_libraries(retrovue_air
    PRIVATE 
        retrovue_air_proto)

target_compile_definitions(retrovue_air
    PRIVATE
        RETROVUE_TIMING_STRICT=ON)

if(FFMPEG_FOUND)
    target_link_libraries(retrovue_air PRIVATE PkgConfig::FFMPEG)
endif()

# SDL2 libraries for Phase 3 preview renderer (optional)
find_package(SDL2 CONFIG)
if(SDL2_FOUND)
    message(STATUS "SDL2 found - preview renderer enabled")
    add_compile_definitions(RETROVUE_SDL2_AVAILABLE)
    target_link_libraries(retrovue_air PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    message(WARNING "SDL2 not found - preview renderer disabled. Install SDL2 for preview window support.")
endif()

# Platform-specific socket libraries for HTTP server
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_DIR})

# Unit tests (optional - requires GTest via vcpkg)
if(GTest_FOUND)
    message(STATUS "GTest found - building unit tests")
    enable_testing()

    # Unit Test: Frame Ring Buffer
    add_executable(unit_buffer
        tests/test_buffer.cpp
        src/buffer/FrameRingBuffer.cpp
        include/retrovue/buffer/FrameRingBuffer.h)

    target_link_libraries(unit_buffer
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(unit_buffer
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(unit_buffer
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_test(NAME unit_buffer COMMAND unit_buffer)

    # Unit Test: Frame Producer
    add_executable(unit_decode
        tests/test_decode.cpp
        src/decode/FrameProducer.cpp
        src/decode/FFmpegDecoder.cpp
        include/retrovue/decode/FrameProducer.h
        include/retrovue/decode/FFmpegDecoder.h
        src/buffer/FrameRingBuffer.cpp
        include/retrovue/buffer/FrameRingBuffer.h)

    target_link_libraries(unit_decode
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(unit_decode
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(unit_decode
        PUBLIC
            ${PROJECT_SOURCE_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_test(NAME unit_decode COMMAND unit_decode)
    
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

    add_test(NAME contracts_masterclock COMMAND contracts_masterclock_tests)

    # Contract Tests: Metrics & Timing
    add_executable(contracts_metricsandtiming_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/MetricsAndTiming/MetricsAndTimingContractTests.cpp
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
        target_link_libraries(contracts_metricsandtiming_tests PRIVATE PkgConfig::FFMPEG)
    endif()

    if(NOT WIN32)
        target_link_libraries(contracts_metricsandtiming_tests PRIVATE Threads::Threads)
    endif()

    target_include_directories(contracts_metricsandtiming_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_metricsandtiming COMMAND contracts_metricsandtiming_tests)
    add_test(NAME contracts_metricsandtiming_MT001
        COMMAND contracts_metricsandtiming_tests
                --gtest_filter=MetricsAndTimingContractTest.MT_001_MasterClockMonotonicAndLowJitter)
    add_test(NAME contracts_metricsandtiming_MT002
        COMMAND contracts_metricsandtiming_tests
                --gtest_filter=MetricsAndTimingContractTest.MT_002_PtsToUtcMappingStable)

    # Contract Tests: Metrics Export
    add_executable(contracts_metricsexport_tests
        tests/ContractRegistry.cpp
        tests/contracts/ContractRegistryEnvironment.cpp
        tests/contracts/ContractRegistrySanityTest.cpp
        tests/contracts/MetricsExport/MetricsExportContractTests.cpp
        src/telemetry/MetricsExporter.cpp
        src/telemetry/MetricsHTTPServer.cpp)

    target_link_libraries(contracts_metricsexport_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main)

    target_compile_definitions(contracts_metricsexport_tests
        PRIVATE
            RETROVUE_TIMING_STRICT=ON)

    target_include_directories(contracts_metricsexport_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME contracts_metricsexport COMMAND contracts_metricsexport_tests)

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
        target_link_libraries(contracts_playoutengine_tests PRIVATE PkgConfig::FFMPEG)
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
        target_link_libraries(timing_soak PRIVATE PkgConfig::FFMPEG)
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
