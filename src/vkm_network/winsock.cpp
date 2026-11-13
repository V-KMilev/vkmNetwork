#define VKM_LOG_CATEGORY "NET"

#include "vkm_network/winsock_api.h"

#include "vkm_log/logger.h"

namespace Vkm::Network {

bool ensureWinsock() {
#if defined(_WIN32)
    // A function-local static: started once, whichever thread arrives first.
    static const bool started = []() {
        WSADATA data{};
        const int failed = WSAStartup(MAKEWORD(2, 2), &data);
        if (failed == 0) return true;
        LOG_ERROR("Winsock would not start (error %d), so nothing here can reach a network", failed);
        return false;
    }();
    return started;
#else
    return true;
#endif
}

} // namespace Vkm::Network
