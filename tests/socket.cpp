// UdpSocket on the loopback, and Address as text.

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include "vkm_log/logger.h"

#include "vkm_network/address.h"
#include "vkm_network/udp_socket.h"

#include "harness/check.h"
#include "harness/scratch.h"

namespace {

using Vkm::Network::Address;
using Vkm::Network::UdpSocket;

/// The next datagram @p socket takes, waiting up to two seconds for it; false when none came.
bool receiveWithin(UdpSocket& socket, std::vector<uint8_t>& out, Address& from) {
    for (int i = 0; i < 2000; ++i) {
        if (socket.receive(out, from)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::vector<uint8_t> bytesOf(const std::string& text) { return {text.begin(), text.end()}; }

/// Send @p bytes to @p to from a socket of the test's own, as any sender can, whatever their size.
void sendRaw(const Address& to, const std::vector<uint8_t>& bytes) {
#if defined(_WIN32)
    (void)to;
    (void)bytes;
    Vkm::Test::skip("a datagram the library would not send is sent here through POSIX sockets");
#else
    const int handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target{};
    target.sin_family      = AF_INET;
    target.sin_addr.s_addr = htonl(to.ipv4);
    target.sin_port        = htons(to.port);
    REQUIRE(handle >= 0);
    const auto*   address = reinterpret_cast<const sockaddr*>(&target);
    const ssize_t sent    = ::sendto(handle, bytes.data(), bytes.size(), 0, address, sizeof(target));
    REQUIRE(sent == static_cast<ssize_t>(bytes.size()));
    ::close(handle);
#endif
}

int countOf(const std::string& text, const std::string& what) {
    int count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
        ++count;
    }
    return count;
}

} // namespace

TEST("two sockets on the loopback talk both ways, each on the port the system gave it") {
    UdpSocket a;
    UdpSocket b;
    REQUIRE(a.open(0));
    REQUIRE(b.open(0));
    CHECK(a.isOpen());
    CHECK(a.localAddress().ipv4 == 0x7F000001u);
    CHECK(a.localAddress().port != 0);
    CHECK(a.localAddress().port != b.localAddress().port);

    const std::vector<uint8_t> ping = bytesOf("ping");
    CHECK(a.send(b.localAddress(), ping.data(), ping.size()));
    std::vector<uint8_t> got;
    Address              from;
    REQUIRE(receiveWithin(b, got, from));
    CHECK(got == ping);
    CHECK(from == a.localAddress());

    const std::vector<uint8_t> pong = bytesOf("pong!");
    CHECK(b.send(from, pong.data(), pong.size()));
    REQUIRE(receiveWithin(a, got, from));
    CHECK(got == pong);
    CHECK(from == b.localAddress());

    // Nothing waiting is not a failure, and says nothing.
    CHECK(!a.receive(got, from));
    CHECK(got.empty());
    CHECK(Vkm::Test::logText().find("WARNING") == std::string::npos);
}

TEST("an empty datagram is one read of its own") {
    UdpSocket socket;
    REQUIRE(socket.open(0));
    sendRaw(socket.localAddress(), {});
    sendRaw(socket.localAddress(), bytesOf("after"));

    std::vector<uint8_t> got = bytesOf("stale");
    Address              from;
    REQUIRE(receiveWithin(socket, got, from));
    CHECK(got.empty());
    CHECK(from.ipv4 == 0x7F000001u);
    REQUIRE(receiveWithin(socket, got, from));
    CHECK(got == bytesOf("after"));
}

TEST("a datagram past MAX_DATAGRAM is refused when sent and dropped when it arrives, each said once") {
    UdpSocket a;
    UdpSocket b;
    REQUIRE(a.open(0));
    REQUIRE(b.open(0));

    const std::vector<uint8_t> big(UdpSocket::MAX_DATAGRAM + 1, 0xAB);
    const std::vector<uint8_t> most(UdpSocket::MAX_DATAGRAM, 0xCD);
    CHECK(!a.send(b.localAddress(), big.data(), big.size()));
    CHECK(!a.send(b.localAddress(), big.data(), big.size()));
    CHECK(countOf(Vkm::Test::logText(), "Refusing a") == 1);
    CHECK(a.send(b.localAddress(), most.data(), most.size()));
    std::vector<uint8_t> got;
    Address              from;
    REQUIRE(receiveWithin(b, got, from));
    CHECK(got == most);

    sendRaw(b.localAddress(), big);
    sendRaw(b.localAddress(), std::vector<uint8_t>(1500, 0xEF));
    sendRaw(b.localAddress(), bytesOf("small"));
    REQUIRE(receiveWithin(b, got, from));
    CHECK(got.empty());
    REQUIRE(receiveWithin(b, got, from));
    CHECK(got.empty());
    REQUIRE(receiveWithin(b, got, from));
    CHECK(got == bytesOf("small"));
    CHECK(countOf(Vkm::Test::logText(), "Dropping a datagram larger") == 1);
}

TEST("a socket closes safely twice and when never opened, and a closed one sends and takes nothing") {
    UdpSocket never;
    never.close();
    never.close();
    CHECK(!never.isOpen());

    UdpSocket socket;
    REQUIRE(socket.open(0));
    socket.close();
    socket.close();
    CHECK(!socket.isOpen());
    CHECK(!socket.localAddress());

    const std::vector<uint8_t> bytes = bytesOf("x");
    CHECK(!socket.send(Address{0x7F000001u, 9}, bytes.data(), bytes.size()));
    std::vector<uint8_t> got;
    Address              from;
    CHECK(!socket.receive(got, from));
    CHECK(!never.send(Address{0x7F000001u, 9}, bytes.data(), bytes.size()));
}

TEST("a port another socket holds is refused, and the library says so and why, as VKM-NETWORK under NET") {
    UdpSocket first;
    REQUIRE(first.open(0));
    UdpSocket second;
    CHECK(!second.open(first.localAddress().port));
    CHECK(!second.isOpen());

    // The port, then the system's reason.
    const std::string said = "Could not bind port " + std::to_string(first.localAddress().port) + ": ";
    bool found = false;
    std::istringstream log(Vkm::Test::logText());
    for (std::string text; std::getline(log, text);) {
        const auto line = Vkm::Log::parseLine(text);
        if (!line || line->message.find(said) == std::string_view::npos) continue;
        found = true;
        CHECK(line->message.size() > said.size());
        CHECK(line->suffix == "VKM-NETWORK");
        CHECK(line->category == "NET");
        CHECK(line->level == Vkm::Log::Level::Error);
    }
    CHECK(found);
}

TEST("an address is read from host:port, or from a host with the fallback port, and written as it is read") {
    const Address full = Address::parse("127.0.0.1:27015");
    CHECK(full.ipv4 == 0x7F000001u);
    CHECK(full.port == 27015);
    CHECK(full.toString() == "127.0.0.1:27015");

    const Address bare = Address::parse("10.1.2.3", 4000);
    CHECK(bare.ipv4 == 0x0A010203u);
    CHECK(bare.port == 4000);

    const Address addresses[] = {{0xC0A80001u, 1}, {0xFFFFFFFEu, 65535}, {0x01020304u, 80}};
    for (const Address& address : addresses) {
        CHECK(Address::parse(address.toString()) == address);
    }
}

TEST("text that names no address is refused: junk ports, a port past 65535, no host, no port") {
    const char* const junk[] = {
        "", ":27015", "127.0.0.1:", "127.0.0.1:abc", "127.0.0.1:27015x", "127.0.0.1:0",
        "127.0.0.1:65536", "127.0.0.1:99999999999", "127.0.0.1:-1",
    };
    for (const char* text : junk) CHECK(!Address::parse(text, 27015));
    CHECK(!Address::parse("127.0.0.1"));

    uint16_t port = 7;
    CHECK(Address::parsePort("65535", port));
    CHECK(port == 65535);
    CHECK(!Address::parsePort("65536", port));
    CHECK(!Address::parsePort("", port));
    CHECK(!Address::parsePort("0", port));
    CHECK(!Address::parsePort(" 80", port));
    CHECK(port == 65535);
}
