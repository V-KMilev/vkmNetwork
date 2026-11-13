#define VKM_LOG_CATEGORY "NET"

#include "vkm_network/udp_socket.h"

#include <cstring>
#include <string>

#include "vkm_log/logger.h"

#include "vkm_network/winsock_api.h"

#if defined(_WIN32)
    #include <mstcpip.h>   // SIO_UDP_CONNRESET, on the toolchains that have it

    // mingw-w64's mstcpip.h lacks this one.
    #ifndef SIO_UDP_CONNRESET
        #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    using NativeSocket = SOCKET;
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using NativeSocket = int;
#endif

namespace Vkm::Network {

namespace {

/// Close a socket this file opened.
void closeSocket(NativeSocket handle);

/// Whether the call that just failed only found nothing waiting.
bool wouldBlock();

/// Whether the read that just failed met a datagram too big for its buffer.
bool messageTooLong();

/// What the system said of the call that just failed.
std::string lastError();

#if defined(_WIN32)
void closeSocket(NativeSocket handle) { closesocket(handle); }
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool messageTooLong() { return WSAGetLastError() == WSAEMSGSIZE; }
std::string lastError() { return "Winsock error " + std::to_string(WSAGetLastError()); }
#else
void closeSocket(NativeSocket handle) { ::close(handle); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool messageTooLong() { return errno == EMSGSIZE; }
std::string lastError() { return std::strerror(errno); }
#endif

sockaddr_in toSockaddr(const Address& address) {
    sockaddr_in out{};
    out.sin_family      = AF_INET;
    out.sin_addr.s_addr = htonl(address.ipv4);
    out.sin_port        = htons(address.port);
    return out;
}

} // namespace

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(uint16_t port) {
    close();
    if (!ensureWinsock()) return false;

    const NativeSocket handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == static_cast<NativeSocket>(INVALID)) {
        LOG_ERROR("Could not create a UDP socket: %s", lastError().c_str());
        return false;
    }

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(port);

    if (::bind(handle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        LOG_ERROR("Could not bind port %u: %s", port, lastError().c_str());
        closeSocket(handle);
        return false;
    }

#if defined(_WIN32)
    // Windows fails the *next* read on a peer's ICMP "port unreachable", so a
    // dead client would end a live one's read loop.
    BOOL reportUnreachable = FALSE;
    DWORD ignored = 0;
    const bool reportingOff = ::WSAIoctl(
        handle,
        SIO_UDP_CONNRESET,
        &reportUnreachable,
        sizeof(reportUnreachable),
        nullptr,
        0,
        &ignored,
        nullptr,
        nullptr
    ) == 0;
    if (!reportingOff) {
        LOG_WARNING(
            "Could not turn off connection-reset reporting (%s); a peer that goes "
            "away will cut this socket's reads short",
            lastError().c_str()
        );
    }

    u_long nonBlocking = 1;
    const bool set     = ioctlsocket(handle, FIONBIO, &nonBlocking) == 0;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    const bool set  = flags != -1 && ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (!set) {
        LOG_ERROR(
            "A UDP socket would not go non-blocking (%s), and a blocking one would stall the frame",
            lastError().c_str()
        );
        closeSocket(handle);
        return false;
    }

    // Asked back: a caller that passed zero wants the port the system chose.
    sockaddr_in bound{};
    socklen_t   boundSize = sizeof(bound);
    const bool boundKnown = ::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0;
    m_local.port = boundKnown ? ntohs(bound.sin_port) : port;

    // getsockname answers 0.0.0.0, which nothing can connect to.
    m_local.ipv4 = 0x7F000001u;

    m_handle = static_cast<intptr_t>(handle);
    return true;
}

void UdpSocket::close() {
    if (m_handle == INVALID) return;

    closeSocket(static_cast<NativeSocket>(m_handle));
    m_handle      = INVALID;
    m_local       = {};
    m_readFailed  = false;
    m_writeFailed = false;
    m_oversized   = false;
    m_refusedBig  = false;
}

bool UdpSocket::send(const Address& to, const uint8_t* bytes, size_t size) {
    if (m_handle == INVALID || !to || !bytes || size == 0) return false;

    if (size > MAX_DATAGRAM) {
        // Said once a socket, until close(), not at the rate the caller sends.
        if (!m_refusedBig) {
            m_refusedBig = true;
            LOG_ERROR(
                "Refusing a %zu byte datagram: past %zu it is fragmented, and "
                "one lost fragment loses all of it; further oversized sends on this socket are not repeated",
                size,
                MAX_DATAGRAM
            );
        }
        return false;
    }

    const sockaddr_in target = toSockaddr(to);
    const auto written = ::sendto(
        static_cast<NativeSocket>(m_handle),
        reinterpret_cast<const char*>(bytes),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target)
    );
    if (written < 0 || static_cast<size_t>(written) != size) {
        // Once per run of failures: the cause repeats at the frame rate.
        if (!m_writeFailed) {
            // Read before anything else can call the system.
            const std::string reason = written < 0 ? lastError() : "only part of it went";
            m_writeFailed = true;
            LOG_WARNING(
                "A UDP send to %s failed (%s); further failures on this socket "
                "are not repeated until one succeeds",
                to.toString().c_str(),
                reason.c_str()
            );
        }
        return false;
    }
    m_writeFailed = false;
    return true;
}

void UdpSocket::noteOversized() {
    // Once: anyone can send a datagram.
    if (m_oversized) return;
    m_oversized = true;

    LOG_WARNING(
        "Dropping a datagram larger than the %zu bytes this end reads; "
        "further oversized ones on this socket are not repeated",
        MAX_DATAGRAM
    );
}

bool UdpSocket::receive(std::vector<uint8_t>& out, Address& from) {
    out.clear();
    if (m_handle == INVALID) return false;

    // One byte of headroom, so a datagram cut to fit is told from one that fits.
    uint8_t     buffer[MAX_DATAGRAM + 1];
    sockaddr_in sender{};
    socklen_t senderSize = sizeof(sender);

    const auto read = ::recvfrom(
        static_cast<NativeSocket>(m_handle),
        reinterpret_cast<char*>(buffer),
        sizeof(buffer),
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &senderSize
    );

    // Windows reports an oversized datagram as an error, POSIX truncates it.
    const bool oversized = read < 0 ? messageTooLong() : static_cast<size_t>(read) > MAX_DATAGRAM;
    if (read < 0 && !oversized) {
        // Nothing waiting is not a failure; anything else is said once.
        if (!wouldBlock() && !m_readFailed) {
            m_readFailed = true;
            LOG_WARNING(
                "A UDP read failed (%s); further failures on this socket are not repeated until one succeeds",
                lastError().c_str()
            );
        }
        return false;
    }
    m_readFailed = false;

    from.ipv4 = ntohl(sender.sin_addr.s_addr);
    from.port = ntohs(sender.sin_port);

    if (oversized) {
        noteOversized();
        return true;
    }
    out.assign(buffer, buffer + read);
    return true;
}

} // namespace Vkm::Network
