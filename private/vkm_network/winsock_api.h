#pragma once

/**
 * @brief The one way the sockets reach Winsock: its headers, and the start-up every entry point
 *        needs first.
 *
 * Private: nothing a consumer includes reaches Windows. Lean, so windows.h, which winsock2.h pulls
 * in, brings no Winsock 1 to clash with it; no min and max macros; no GDI, whose macros (ERROR among
 * them) take names a constant is spelled with.
 * Safe to include unconditionally: the headers are inside a _WIN32 guard.
 */
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef NOGDI
        #define NOGDI
    #endif

    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif

namespace Vkm::Network {

/**
 * @brief Make sure this process can call the Windows socket library.
 *
 * Before **any** entry point, resolving a host included, not just before a
 * socket exists. Started once and never stopped: an address may be parsed
 * after the last socket closes. A no-op returning true off Windows.
 *
 * @return False when the library would not start; nothing can reach a network.
 */
bool ensureWinsock();

} // namespace Vkm::Network
