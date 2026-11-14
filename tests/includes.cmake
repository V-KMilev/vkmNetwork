# Every #include under src/vkm_network and private/vkm_network is the library's own, the standard
# library's, glm's or vkmLog's, or a system header in a file that reaches the sockets; no public header
# includes a private one; and no public header names a consumer's engine, scenes, entities, schemas or
# snapshots (cmake -P, -DSOURCE_DIR=<src> -DPRIVATE_DIR=<private>).

# The system headers each file that reaches the sockets may include: Winsock's behind winsock_api.h,
# POSIX's in the two that call them.
set(_system_winsock_api.h "winsock2.h|ws2tcpip.h")
set(_system_udp_socket.cpp "mstcpip.h|arpa/inet.h|errno.h|fcntl.h|netinet/in.h|sys/socket.h|unistd.h")
set(_system_address.cpp "arpa/inet.h|netdb.h|netinet/in.h|sys/socket.h")

file(GLOB_RECURSE _public "${SOURCE_DIR}/vkm_network/*.h")
file(GLOB_RECURSE _files
    "${SOURCE_DIR}/vkm_network/*.h"
    "${SOURCE_DIR}/vkm_network/*.cpp"
    "${PRIVATE_DIR}/vkm_network/*.h"
)
set(_problems "")
foreach(_file IN LISTS _files)
    get_filename_component(_name "${_file}" NAME)
    list(FIND _public "${_file}" _at)
    set(_is_public FALSE)
    if(NOT _at EQUAL -1)
        set(_is_public TRUE)
    endif()
    file(STRINGS "${_file}" _includes REGEX "^[ \t]*#[ \t]*include")
    foreach(_line IN LISTS _includes)
        string(REGEX MATCH "[<\"]([^>\"]+)[>\"]" _ "${_line}")
        set(_header "${CMAKE_MATCH_1}")
        if(_is_public AND _header MATCHES "^vkm_network/" AND EXISTS "${PRIVATE_DIR}/${_header}")
            string(APPEND _problems "  ${_file} is public and includes a private header: ${_line}\n")
        elseif(_header MATCHES "^vkm_network/"
            AND (EXISTS "${SOURCE_DIR}/${_header}" OR EXISTS "${PRIVATE_DIR}/${_header}"))
        elseif(_line MATCHES "<[a-z_]+>")
        elseif(_header MATCHES "^(glm|vkm_log)/")
        elseif(DEFINED "_system_${_name}" AND _header MATCHES "^(${_system_${_name}})$")
        else()
            string(APPEND _problems "  ${_file}: ${_line}\n")
        endif()
    endforeach()

    # The transport carries bytes: what they mean is the consumer's.
    if(_is_public)
        file(STRINGS "${_file}" _engine REGEX "Vkm::Engine|[Ss]cene|[Ee]ntity|[Ss]chema|[Ss]napshot")
        foreach(_line IN LISTS _engine)
            string(APPEND _problems "  ${_file} names the engine: ${_line}\n")
        endforeach()
    endif()
endforeach()

if(_problems)
    message(FATAL_ERROR "Includes the library may not have:\n${_problems}")
endif()
