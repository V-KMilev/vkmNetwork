// Connection: sequencing, acknowledgement and timing between two ends, with no socket between them.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "vkm_network/connection.h"

#include "harness/check.h"

namespace {

using Vkm::Network::Address;
using Vkm::Network::Connection;
using Vkm::Network::isNewerSequence;

/// A packet @p from frames, with one byte of payload.
std::vector<uint8_t> packet(Connection& from, uint8_t payload = 0x42) {
    std::vector<uint8_t> bytes;
    from.frame(&payload, 1, bytes);
    return bytes;
}

/// The sequences of @p to's that @p bytes acknowledges; empty, and @p took false, when @p to refused them.
std::vector<uint16_t> take(Connection& to, const std::vector<uint8_t>& bytes, bool* took = nullptr) {
    std::vector<uint16_t> acknowledged;
    const bool accepted = to.accept(bytes.data(), bytes.size(), acknowledged);
    if (took) *took = accepted;
    return acknowledged;
}

uint16_t sequenceOf(const std::vector<uint8_t>& bytes) {
    return static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
}

void openPair(Connection& a, Connection& b) {
    a.open(Address{0x7F000001u, 1000});
    b.open(Address{0x7F000001u, 2000});
}

} // namespace

TEST("a sequence is newer than the ones before it across the wrap, and older than those after") {
    for (const uint32_t start : {0u, 1u, 100u, 32767u, 32768u, 65000u, 65535u}) {
        int wrong = 0;
        for (uint32_t ahead = 1; ahead < 0x8000; ahead += 97) {
            const uint16_t a = static_cast<uint16_t>(start);
            const uint16_t b = static_cast<uint16_t>(start + ahead);
            if (!isNewerSequence(b, a) || isNewerSequence(a, b)) ++wrong;
        }
        CHECK(wrong == 0);
        CHECK(!isNewerSequence(static_cast<uint16_t>(start), static_cast<uint16_t>(start)));
    }
    CHECK(isNewerSequence(0, 65535));
    CHECK(isNewerSequence(3, 65530));
    CHECK(!isNewerSequence(65530, 3));
}

TEST("no packet is numbered NO_SEQUENCE: the first is 1, and after 65535 comes 1") {
    Connection a;
    a.open(Address{0x7F000001u, 1000});
    CHECK(a.nextSequence() == 1);
    uint16_t previous = 0;
    int zeros = 0;
    int wraps = 0;
    for (int i = 0; i < 70000; ++i) {
        const uint16_t sequence = sequenceOf(packet(a));
        if (sequence == Connection::NO_SEQUENCE) ++zeros;
        if (i == 0) CHECK(sequence == 1);
        if (previous == 65535) {
            ++wraps;
            CHECK(sequence == 1);
        }
        previous = sequence;
    }
    CHECK(zeros == 0);
    CHECK(wraps == 1);
}

TEST("a reply acknowledges the newest packet heard and the 32 before it, the one 32 back included") {
    Connection a;
    Connection b;
    openPair(a, b);
    for (int i = 0; i < 40; ++i) take(b, packet(a));

    std::vector<uint16_t> acknowledged = take(a, packet(b));
    std::sort(acknowledged.begin(), acknowledged.end());
    std::vector<uint16_t> expected;
    for (uint16_t sequence = 8; sequence <= 40; ++sequence) expected.push_back(sequence);
    CHECK(acknowledged == expected);
}

TEST("each acknowledgement is reported once only") {
    Connection a;
    Connection b;
    openPair(a, b);
    for (int i = 0; i < 3; ++i) take(b, packet(a));

    CHECK(take(a, packet(b)).size() == 3u);
    CHECK(take(a, packet(b)).empty());

    take(b, packet(a));
    CHECK(take(a, packet(b)) == std::vector<uint16_t>{4});
}

TEST("a packet older than one already heard is refused, and refuse() takes back what the last accept heard") {
    Connection a;
    Connection b;
    openPair(a, b);
    const std::vector<uint8_t> first  = packet(a);
    const std::vector<uint8_t> second = packet(a);
    const std::vector<uint8_t> third  = packet(a);

    bool took = false;
    take(b, second, &took);
    CHECK(took);
    take(b, first, &took);
    CHECK(!took);
    take(b, second, &took);
    CHECK(!took);

    take(b, third, &took);
    CHECK(took);
    b.refuse();
    // The reply does not claim the third, and the third is taken when it comes again.
    std::vector<uint16_t> acknowledged = take(a, packet(b));
    std::sort(acknowledged.begin(), acknowledged.end());
    CHECK(acknowledged == std::vector<uint16_t>{2});
    take(b, third, &took);
    CHECK(took);
    CHECK(take(a, packet(b)) == std::vector<uint16_t>{3});

    // Too short to hold a header.
    take(b, std::vector<uint8_t>(Connection::HEADER_BYTES - 1, 0xFF), &took);
    CHECK(!took);
}

TEST("refusing after a refused accept takes back nothing, and a refused packet counts once, as missed") {
    Connection a;
    Connection b;
    openPair(a, b);
    const std::vector<uint8_t> first  = packet(a);
    const std::vector<uint8_t> second = packet(a);

    bool took = false;
    take(b, first, &took);
    take(b, second, &took);
    take(b, second, &took);
    CHECK(!took);
    b.refuse();
    take(b, second, &took);
    CHECK(!took);

    // Of 1 to 4 only 3 is refused: it is a gap the fourth steps over, and nothing else.
    take(b, packet(a));
    b.refuse();
    take(b, packet(a));
    CHECK_NEAR(b.lossFraction(), 1.0f / 4.0f, 1e-6f);
}

TEST("loss counts no gap at the wrap, where NO_SEQUENCE is skipped") {
    Connection a;
    Connection b;
    openPair(a, b);
    for (int i = 0; i < 70000; ++i) take(b, packet(a));
    CHECK(b.lossFraction() == 0.0f);
}

TEST("a packet lost a wrap ago times no packet sent since, and reports none twice") {
    Connection a;
    Connection b;
    openPair(a, b);
    packet(a);  // 1, lost
    std::vector<uint16_t> reported;
    for (int i = 0; i < 65545; ++i) {
        take(b, packet(a));
        a.advance(0.05f);
        const std::vector<uint16_t> acknowledged = take(a, packet(b));
        reported.insert(reported.end(), acknowledged.begin(), acknowledged.end());
    }
    CHECK_NEAR(a.roundTrip(), 0.05f, 1e-4f);
    CHECK(a.roundTripVariation() < 0.01f);
    // Each packet taken is reported once, in turn: 2 to 65535, then 1 to 11 after the wrap.
    std::vector<uint16_t> expected;
    for (uint32_t sequence = 2; sequence <= 65535; ++sequence) {
        expected.push_back(static_cast<uint16_t>(sequence));
    }
    for (uint16_t sequence = 1; sequence <= 11; ++sequence) expected.push_back(sequence);
    CHECK(reported.size() == expected.size());
    CHECK(std::equal(reported.begin(), reported.end(), expected.begin(), expected.end()));
}

TEST("a packet numbered NO_SEQUENCE is refused, and one acknowledging it reports nothing") {
    Connection b;
    b.open(Address{0x7F000001u, 2000});
    bool took = true;
    take(b, std::vector<uint8_t>(Connection::HEADER_BYTES, 0), &took);
    CHECK(!took);

    // Sequence 1, acknowledging 5 and, five before it, NO_SEQUENCE, which b never sent.
    const std::vector<uint8_t> header = {0x01, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00};
    CHECK(take(b, header, &took).empty());
    CHECK(took);
    CHECK(b.roundTrip() == 0.0f);
}

TEST("a reply after a packet 32 past the newest heard acknowledges both, and after one 33 past only that one") {
    Connection a;
    Connection b;
    openPair(a, b);
    for (int i = 0; i < 70; ++i) {
        const std::vector<uint8_t> bytes = packet(a);
        const uint16_t sequence = sequenceOf(bytes);
        if (sequence == 1 || sequence == 33 || sequence == 66) take(b, bytes);
    }
    std::vector<uint16_t> acknowledged = take(a, packet(b));
    std::sort(acknowledged.begin(), acknowledged.end());
    CHECK(acknowledged == std::vector<uint16_t>{66});

    Connection c;
    Connection d;
    openPair(c, d);
    for (int i = 0; i < 33; ++i) {
        const std::vector<uint8_t> bytes = packet(c);
        if (sequenceOf(bytes) == 1 || sequenceOf(bytes) == 33) take(d, bytes);
    }
    acknowledged = take(c, packet(d));
    std::sort(acknowledged.begin(), acknowledged.end());
    CHECK(acknowledged == (std::vector<uint16_t>{1, 33}));
}

TEST("a packet is remembered while 254 more go out and not past them, across the wrap too") {
    // Whether a reply to the packet numbered first, after more go out unheard, still reports it.
    const auto reported = [](uint16_t first, int more) {
        Connection a;
        Connection b;
        openPair(a, b);
        while (a.nextSequence() != first) packet(a);
        take(b, packet(a));
        for (int i = 0; i < more; ++i) packet(a);
        return take(a, packet(b)) == std::vector<uint16_t>{first};
    };
    for (const uint16_t first : {uint16_t{1000}, uint16_t{65280}, uint16_t{65281}, uint16_t{65535}}) {
        CHECK(reported(first, 254));
        CHECK(!reported(first, 255));
    }
}

TEST("an end that has heard nothing acknowledges nothing") {
    Connection a;
    Connection b;
    openPair(a, b);
    for (int i = 0; i < 3; ++i) packet(a);

    const std::vector<uint8_t> first = packet(b);
    const auto header = first.begin() + Connection::HEADER_BYTES;
    CHECK(std::all_of(first.begin() + 2, header, [](uint8_t v) { return v == 0; }));
    bool took = false;
    CHECK(take(a, first, &took).empty());
    CHECK(took);
    CHECK(a.roundTrip() == 0.0f);
}

TEST("the round trip settles on a steady trip and moves smoothly to a new one, and loss counts the gaps heard") {
    Connection a;
    Connection b;
    openPair(a, b);

    const auto trip = [&](float seconds) {
        take(b, packet(a));
        a.advance(seconds);
        take(a, packet(b));
    };
    for (int i = 0; i < 50; ++i) trip(0.1f);
    CHECK_NEAR(a.roundTrip(), 0.1f, 1e-5f);
    CHECK_NEAR(a.roundTripVariation(), 0.0f, 1e-5f);

    trip(0.3f);
    CHECK(a.roundTrip() > 0.1f);
    CHECK(a.roundTrip() < 0.15f);
    CHECK(a.roundTripVariation() > 0.0f);
    for (int i = 0; i < 200; ++i) trip(0.3f);
    CHECK_NEAR(a.roundTrip(), 0.3f, 1e-3f);
    CHECK(a.roundTripVariation() < 0.01f);

    // Packets 1, 2, 4 and 7 of eight arrive: three missed of seven.
    Connection c;
    Connection d;
    openPair(c, d);
    CHECK(d.lossFraction() == 0.0f);
    for (int sequence = 1; sequence <= 8; ++sequence) {
        const std::vector<uint8_t> bytes = packet(c);
        if (sequence == 1 || sequence == 2 || sequence == 4 || sequence == 7) take(d, bytes);
    }
    CHECK_NEAR(d.lossFraction(), 3.0f / 7.0f, 1e-6f);
}

TEST("silence grows by at most MAX_SILENCE_A_FRAME a call, times out past TIMEOUT_SECONDS, and a packet ends it") {
    Connection a;
    Connection b;
    openPair(a, b);
    b.advance(60.0f);
    CHECK(!b.timedOut());

    for (int i = 1; i < 20; ++i) b.advance(Connection::MAX_SILENCE_A_FRAME);
    CHECK(!b.timedOut());
    b.advance(0.01f);
    CHECK(b.timedOut());
    CHECK(!b.timedOut(10.0f));

    take(b, packet(a));
    CHECK(!b.timedOut());
}

TEST("a header's bytes are the same on every machine: each number little-endian") {
    Connection a;
    Connection b;
    openPair(a, b);
    // a's next sequence is 0x0102; it has heard b's 0x012B and 0x012C.
    for (int i = 1; i < 0x0102; ++i) packet(a);
    for (int i = 1; i < 0x012B; ++i) packet(b);
    take(a, packet(b));
    take(a, packet(b));

    const uint8_t payload[] = {0xAA, 0xBB};
    std::vector<uint8_t> bytes;
    a.frame(payload, sizeof(payload), bytes);
    CHECK(bytes == std::vector<uint8_t>{0x02, 0x01, 0x2C, 0x01, 0x01, 0x00, 0x00, 0x00, 0xAA, 0xBB});
}
