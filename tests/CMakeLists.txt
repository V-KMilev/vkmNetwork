# vkm_network's own tests: TEST("what works") in tests/<topic>.cpp (harness/check.h). Each runs in a
# process of its own and is a ctest of its own, named "vkm_network: <what works>" and labeled
# vkm_network, so `ctest -L vkm_network` runs the suite in a consumer's tree too, with -C <config> under
# a multi-config generator. It links the library alone, needs no display, and talks on the loopback
# only. Beside them, checks of the library as a consumer builds it: its includes, its CMake, its debug
# flags.

file(GLOB VKM_NETWORK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(vkm_network_tests ${CMAKE_CURRENT_SOURCE_DIR}/harness/main.cpp ${VKM_NETWORK_TEST_SOURCES})
target_link_libraries(vkm_network_tests PRIVATE ${PROJECT_NAME})
# Beside vkm_log, where Windows looks for the DLL a program needs.
set_target_properties(vkm_network_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:vkm_log>)
target_include_directories(vkm_network_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vkm_network_tests PRIVATE
    VKM_TEST_SUITE="vkm_network"
    VKM_TEST_LOG
    # The library's asserts flag, which check.h's VKM_TEST_LIBRARY_ASSERTS reads beside the file's NDEBUG.
    VKM_TEST_ASSERTS_FLAG=$<BOOL:${VKM_NETWORK_ASSERTS}>
)
target_compile_options(vkm_network_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${_vkm_network_warnings}>
    $<$<CXX_COMPILER_ID:Clang>:-Werror=documentation>
)

# Every test the binary lists becomes a ctest when ctest runs: a test added is a test run, with no
# list to keep.
include(${CMAKE_CURRENT_SOURCE_DIR}/harness/discover.cmake)
vkm_discover_tests(vkm_network_tests vkm_network)

# The library as a consumer builds it.
add_test(NAME "vkm_network: every include is its own, the standard library's, glm's, vkmLog's or the sockets', and no public header names a scene"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src -P ${CMAKE_CURRENT_SOURCE_DIR}/includes.cmake
)
add_test(NAME "vkm_network: a consumer adds it after glm and vkm_log, and without either it stops and says which"
    COMMAND ${CMAKE_COMMAND} -DLIBRARY_DIR=${PROJECT_SOURCE_DIR} -DREQUIRES=glm|vkm_log
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/consumer -DGENERATOR=${CMAKE_GENERATOR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/harness/consumer.cmake
)
set_tests_properties(
    "vkm_network: every include is its own, the standard library's, glm's, vkmLog's or the sockets', and no public header names a scene"
    "vkm_network: a consumer adds it after glm and vkm_log, and without either it stops and says which"
    PROPERTIES LABELS vkm_network
)
if(CMAKE_NM)
    # The library's compile commands, which the check reads, whether or not the consumer exports its own.
    set_property(TARGET ${PROJECT_NAME} PROPERTY EXPORT_COMPILE_COMMANDS ON)
    add_test(NAME "vkm_network: its asserts follow VKM_NETWORK_ASSERTS whatever the build type"
        COMMAND ${CMAKE_COMMAND} -DLIBRARY=${PROJECT_NAME} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src
            -DARCHIVE=$<TARGET_FILE:${PROJECT_NAME}> -DNM=${CMAKE_NM} -DASSERTS=${VKM_NETWORK_ASSERTS}
            -DPROFILER=OFF -DCONFIG=$<CONFIG> -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
            -P ${CMAKE_CURRENT_SOURCE_DIR}/harness/flags.cmake
    )
    set_tests_properties("vkm_network: its asserts follow VKM_NETWORK_ASSERTS whatever the build type"
        PROPERTIES LABELS vkm_network)
endif()
