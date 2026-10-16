cmake_minimum_required(VERSION 3.24)
project(KenshiCoopTransports LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Transport providers require modern MSVC Windows x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)
set(COOP_CONFIGURATION "Release" CACHE STRING "Release or Harness output set")
if(NOT COOP_CONFIGURATION MATCHES "^(Release|Harness)$")
    message(FATAL_ERROR "Use Release or Harness; engine Debug CRT is not a shipping ABI")
endif()
set(COOP_PROTOBUF_ROOT "" CACHE PATH "Pinned static protobuf install prefix")
if(NOT EXISTS "${COOP_PROTOBUF_ROOT}/cmake/protobuf-config.cmake")
    message(FATAL_ERROR "Provision static protobuf with scripts/build_transports.ps1 first")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${COOP_PROTOBUF_ROOT}")
set(Protobuf_DIR "${COOP_PROTOBUF_ROOT}/cmake" CACHE PATH "" FORCE)
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(protobuf_MODULE_COMPATIBLE ON CACHE BOOL "" FORCE)

include(FetchContent)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.json" COOP_PINS)
string(JSON GNS_URL GET "${COOP_PINS}" gns url)
string(JSON GNS_SHA GET "${COOP_PINS}" gns sha256)
# Every transport dependency is contained statically in the UDP provider.
# ICE/WebRTC/Steam SDK/Steam relay are deliberately not compiled into it.
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
set(USE_CRYPTO BCrypt CACHE STRING "" FORCE)
set(USE_CRYPTO25519 Reference CACHE STRING "" FORCE)
FetchContent_Declare(gns URL "${GNS_URL}" URL_HASH "SHA256=${GNS_SHA}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(gns)
# Standalone GNS rejects strict IP listeners even with an imported valid
# certificate. This pinned patch fixes admission startup, not peer verification.
string(JSON GNS_PATCH GET "${COOP_PINS}" gns patch)
string(JSON GNS_PATCH_SHA GET "${COOP_PINS}" gns patchSha256)
set(GNS_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/${GNS_PATCH}")
file(SHA256 "${GNS_PATCH}" GNS_PATCH_ACTUAL_SHA)
if(NOT GNS_PATCH_ACTUAL_SHA STREQUAL GNS_PATCH_SHA)
    message(FATAL_ERROR "Pinned GNS authentication patch SHA-256 mismatch")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${gns_SOURCE_DIR}" apply --reverse --check "${GNS_PATCH}"
    RESULT_VARIABLE GNS_PATCH_ALREADY_APPLIED OUTPUT_QUIET ERROR_QUIET)
if(NOT GNS_PATCH_ALREADY_APPLIED EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${gns_SOURCE_DIR}" apply "${GNS_PATCH}"
        RESULT_VARIABLE GNS_PATCH_RESULT ERROR_VARIABLE GNS_PATCH_ERROR)
    if(NOT GNS_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Cannot apply pinned GNS authentication patch: ${GNS_PATCH_ERROR}")
    endif()
endif()
target_compile_definitions(GameNetworkingSockets_s PRIVATE
    STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS)

function(coop_native_target target steam)
    target_compile_definitions(${target} PRIVATE
        _CRT_SECURE_NO_WARNINGS COOP_PROVIDER_STEAM=${steam}
        STEAMNETWORKINGSOCKETS_STATIC_LINK)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${gns_SOURCE_DIR}/include")
    target_compile_options(${target} PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
endfunction()

add_library(KenshiCoopNetSteam SHARED Provider.cpp SteamBackend.cpp)
coop_native_target(KenshiCoopNetSteam 1)
# No steam_api import library: borrow only the game's initialized context.

add_library(KenshiCoopNetUDP SHARED Provider.cpp UdpBackend.cpp Credentials.cpp)
coop_native_target(KenshiCoopNetUDP 0)
target_compile_definitions(KenshiCoopNetUDP PRIVATE
    STEAMNETWORKINGSOCKETS_OPENSOURCE STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS
    VALVE_CRYPTO_ENABLE_25519 VALVE_CRYPTO_BCRYPT VALVE_CRYPTO_25519_DONNA ED25519_HASH_BCRYPT)
target_include_directories(KenshiCoopNetUDP PRIVATE
    "${gns_SOURCE_DIR}/src" "${gns_SOURCE_DIR}/src/common"
    "${gns_SOURCE_DIR}/src/public" "${gns_BINARY_DIR}/src"
    "${gns_SOURCE_DIR}/src/steamnetworkingsockets" "${COOP_PROTOBUF_ROOT}/include")
target_link_libraries(KenshiCoopNetUDP PRIVATE GameNetworkingSockets::static advapi32 crypt32)

# Sanctioned RE_Kenshi-loaded probe; no injection or replacement Steam DLL.
add_library(KenshiCoopSteamProbe SHARED SteamProbe.cpp SteamBackend.cpp Provider.cpp)
coop_native_target(KenshiCoopSteamProbe 1)
target_link_libraries(KenshiCoopSteamProbe PRIVATE user32)

add_executable(transport_integration tests/TransportIntegration.cpp)
coop_native_target(transport_integration 0)
target_link_libraries(transport_integration PRIVATE ws2_32 advapi32 psapi crypt32)

# Private credential provisioning tool: invokes the real ABI, no game core.
add_executable(transport_credentials tools/TransportCredentials.cpp)
coop_native_target(transport_credentials 0)
target_link_libraries(transport_credentials PRIVATE advapi32)
