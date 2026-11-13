cmake_minimum_required(VERSION 3.25.0)

set(PROJECT_NAME vkm_network)
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)

project(
  ${PROJECT_NAME}
  VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message("[VKM_NETWORK] Building vkm_network v${PROJECT_VERSION}")

# glm and vkm_log are the consuming project's to provide: glm crosses this library's API (a
# rotation's coder), so a copy in here would be a second one across the boundary. Fail with the
# reason rather than with whatever error a missing target produces three files later.
foreach(_vkm_network_required glm vkm_log)
    if(NOT TARGET ${_vkm_network_required})
        message(FATAL_ERROR
            "vkm_network requires the target '${_vkm_network_required}', which the consuming project "
            "must add before add_subdirectory(vkmNetwork); see README.md.")
    endif()
endforeach()

# Debug features follow flags, each off unless the consumer turns it on.
option(VKM_NETWORK_ASSERTS "Keep the library's VKM_ASSERTs on whatever the build type" OFF)

# Enumerated as one sweep rather than as a list: a forgotten entry in a list is a missing symbol
# somewhere else.
file(GLOB_RECURSE VKM_NETWORK_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Static and position-independent: a consumer links it whole into a shared library of its own, so
# it ships no binary of its own.
add_library(${PROJECT_NAME} STATIC ${VKM_NETWORK_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The root above vkm_network/, so an include names the library: "vkm_network/connection.h".
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(${PROJECT_NAME} PUBLIC
    glm
    vkm_log
)

# Winsock, for the sockets. Nothing outside the library names a Winsock type; a static library's
# private link still reaches whatever links it.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

# Its warning set. Clang reads the doc blocks: a @param naming no parameter is an error.
set(_vkm_network_warnings
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wshadow
    -Wunused-variable
    -Wunused-value
    -Wformat=2
    -Wnon-virtual-dtor
    -Woverloaded-virtual
)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${_vkm_network_warnings}>
    $<$<CXX_COMPILER_ID:Clang>:-Werror=documentation>
)

# No fused multiply-add unless the source writes one: a fused result rounds once where the written
# one rounds twice, and the coders must give the same code on every machine, since both ends of a
# connection compare what they decode. Public, so a consumer's own code built on the coders rounds
# alike.
target_compile_options(${PROJECT_NAME} PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>
)

# VKM_ASSERT keys on NDEBUG, which a RelWithDebInfo or Release build defines: on, the library's own
# sources compile without it.
if(VKM_NETWORK_ASSERTS)
    target_compile_options(${PROJECT_NAME} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
endif()

# Its log lines say whose they are.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VKM_LOG_SUFFIX="VKM-NETWORK"
)

# The suite (tests/), built when the consumer asks for it.
option(VKM_NETWORK_BUILD_TESTS "Build the vkm_network test suite" OFF)
if(VKM_NETWORK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
