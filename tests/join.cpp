// The join token: proof that an address hears what is sent to it, and the hash under it.

#include <cstdint>
#include <vector>

#include "vkm_network/address.h"
#include "vkm_network/join_cookie.h"

#include "harness/check.h"

using Vkm::Network::Address;
using Vkm::Network::JoinCookie;

TEST("sipHash24 gives the reference outputs its authors publish") {
    // Key 00 01 .. 0f, message 00 01 .. (n - 1), as the reference implementation's vectors; each
    // output's bytes little-endian.
    const uint64_t k0 = 0x0706050403020100ull;
    const uint64_t k1 = 0x0F0E0D0C0B0A0908ull;
    const uint64_t expected[] = {
        0x726FDB47DD0E0E31ull, 0x74F839C593DC67FDull, 0x0D6C8009D9A94F5Aull, 0x85676696D7FB7E2Dull,
        0xCF2794E0277187B7ull, 0x18765564CD99A68Dull, 0xCBC9466E58FEE3CEull, 0xAB0200F58B01D137ull,
        0x93F5F5799A932462ull, 0x9E0082DF0BA9E4B0ull, 0x7A5DBBC594DDB9F3ull, 0xF4B32F46226BADA7ull,
        0x751E8FBC860EE5FBull, 0x14EA5627C0843D90ull, 0xF723CA908E7AF2EEull, 0xA129CA6149BE45E5ull,
    };
    std::vector<uint8_t> message;
    for (const uint64_t output : expected) {
        CHECK(Vkm::Network::sipHash24(k0, k1, message.data(), message.size()) == output);
        message.push_back(static_cast<uint8_t>(message.size()));
    }
    for (size_t i = message.size(); i < 63; ++i) message.push_back(static_cast<uint8_t>(i));
    CHECK(Vkm::Network::sipHash24(k0, k1, message.data(), message.size()) == 0x958A324CEB064572ull);
}

TEST("a join token is taken back from its address for one to two windows, and from nowhere else") {
    JoinCookie cookie;
    const Address  client{0x0A000002u, 50000};
    const double   window = JoinCookie::WINDOW_SECONDS;
    const uint64_t token  = cookie.issue(client, 10.0 * window);

    CHECK(cookie.accepts(client, token, 10.0 * window));
    CHECK(cookie.accepts(client, token, 11.0 * window - 0.001));
    CHECK(cookie.accepts(client, token, 11.0 * window));
    CHECK(cookie.accepts(client, token, 12.0 * window - 0.001));
    CHECK(!cookie.accepts(client, token, 12.0 * window));
    CHECK(!cookie.accepts(client, token, 40.0 * window));

    CHECK(!cookie.accepts(Address{0x0A000003u, 50000}, token, 10.0 * window));
    CHECK(!cookie.accepts(Address{0x0A000002u, 50001}, token, 10.0 * window));
    CHECK(!cookie.accepts(client, token ^ 1u, 10.0 * window));

    // Issued late in its window, it still lasts out the next.
    const uint64_t late = cookie.issue(client, 11.0 * window - 0.001);
    CHECK(cookie.accepts(client, late, 12.0 * window - 0.001));
    CHECK(!cookie.accepts(client, late, 12.0 * window));
}

TEST("rekey voids every token given before") {
    JoinCookie cookie;
    const Address  client{0x7F000001u, 4242};
    const uint64_t before = cookie.issue(client, 5.0);
    cookie.rekey();
    CHECK(!cookie.accepts(client, before, 5.0));
    CHECK(cookie.accepts(client, cookie.issue(client, 5.0), 5.0));

    // Two servers' keys are their own.
    JoinCookie other;
    CHECK(!other.accepts(client, cookie.issue(client, 5.0), 5.0));
}
