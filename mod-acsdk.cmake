#
# mod-acsdk CMake integration.
#
# AzerothCore includes modules/<name>/<name>.cmake inline from modules/CMakeLists.txt
# after the `modules` target is created, so this file can add external link deps to it.
#
# This repository is the module: clone it as modules/mod-acsdk (the folder name must be
# mod-acsdk; AzerothCore derives the loader function and this file's name from it).
# Only src/ is compiled and conf/*.conf.dist installed by AzerothCore itself.
#
# Needs gRPC and protobuf with their CMake config packages (find_package below). On Windows
# deps/vcpkg.json lists them for triplet x64-windows-static-md (static libraries with the
# dynamic CRT, matching worldserver); on Linux use vcpkg or a gRPC install that ships CMake
# configs (see docs/server-install.md).
#

find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

if(NOT ACSDK_PROTO_DIR)
  set(ACSDK_PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/proto")
endif()
set(ACSDK_PROTO_DIR "${ACSDK_PROTO_DIR}" CACHE PATH "Directory containing acsdk/v1/*.proto")

if(NOT EXISTS "${ACSDK_PROTO_DIR}/acsdk/v1/server.proto")
  message(FATAL_ERROR "mod-acsdk: protos not found in ACSDK_PROTO_DIR='${ACSDK_PROTO_DIR}'")
endif()

file(GLOB ACSDK_PROTO_FILES "${ACSDK_PROTO_DIR}/acsdk/v1/*.proto")
set(ACSDK_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/acsdk-gen")
file(MAKE_DIRECTORY "${ACSDK_PROTO_GEN_DIR}")

# Generated code is built in its own library so AzerothCore's warning flags don't apply to it.
add_library(acsdk-proto STATIC ${ACSDK_PROTO_FILES})

target_link_libraries(acsdk-proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++)

target_include_directories(acsdk-proto
  PUBLIC
    "${ACSDK_PROTO_GEN_DIR}")

protobuf_generate(
  TARGET acsdk-proto
  LANGUAGE cpp
  IMPORT_DIRS "${ACSDK_PROTO_DIR}"
  PROTOC_OUT_DIR "${ACSDK_PROTO_GEN_DIR}")

protobuf_generate(
  TARGET acsdk-proto
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  IMPORT_DIRS "${ACSDK_PROTO_DIR}"
  PROTOC_OUT_DIR "${ACSDK_PROTO_GEN_DIR}")

set_target_properties(acsdk-proto
  PROPERTIES
    FOLDER "modules")

target_link_libraries(modules
  PUBLIC
    acsdk-proto)

# Golden-byte tests for the packet builders/decoders, and unit tests for account sessions and leases
# (game-free code; links only AC's `shared`). Off by default; -DACSDK_BUILD_TESTS=ON builds
# <build>/bin/<Config>/acsdk-packet-tests and acsdk-session-tests.
option(ACSDK_BUILD_TESTS "Build the mod-acsdk packet tests" OFF)
if(ACSDK_BUILD_TESTS)
add_executable(acsdk-packet-tests
  "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/PacketTests.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/packets/Packets.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/packets/EventPackets.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/packets/InteractionPackets.cpp")

target_include_directories(acsdk-packet-tests
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src/packets")

target_link_libraries(acsdk-packet-tests
  PRIVATE
    acore-core-interface
    shared)

set_target_properties(acsdk-packet-tests
  PROPERTIES
    FOLDER "modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(acsdk-session-tests
  "${CMAKE_CURRENT_LIST_DIR}/tests/cpp/SessionTests.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/auth/SessionManager.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/auth/LoginThrottle.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/leases/LeaseManager.cpp")

target_include_directories(acsdk-session-tests
  PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src/auth"
    "${CMAKE_CURRENT_LIST_DIR}/src/leases")

target_link_libraries(acsdk-session-tests
  PRIVATE
    acore-core-interface
    shared)

set_target_properties(acsdk-session-tests
  PROPERTIES
    FOLDER "modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
