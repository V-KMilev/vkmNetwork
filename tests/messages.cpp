// Outbox and Inbox: messages that must arrive, once each and in order, over packets that may not.

#include <cstdint>
#include <random>
#include <vector>

#include "vkm_network/bit_stream.h"
#include "vkm_network/reliable_messages.h"
#include "vkm_network/udp_socket.h"

#include "harness/check.h"
#include "harness/scratch.h"

namespace {

using namespace Vkm::Network;

using Message = std::vector<uint8_t>;

constexpr size_t PACKET_BITS = UdpSocket::MAX_DATAGRAM * 8;

/// A packet carrying what @p outbox holds that fits.
std::vector<uint8_t> packetFrom(const Outbox& outbox, size_t budgetBits = PACKET_BITS) {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, UdpSocket::MAX_DATAGRAM);
    outbox.write(out, budgetBits);
    out.finish();
    return bytes;
}

/// What @p inbox hands on from @p bytes.
std::vector<Message> deliver(Inbox& inbox, const std::vector<uint8_t>& bytes, bool* read = nullptr) {
    std::vector<Message> handed;
    BitReader            in(bytes.data(), bytes.size());
    const bool ok = inbox.read(in, [&](const uint8_t* message, size_t size) {
        handed.emplace_back(message, message + size);
    });
    if (read) *read = ok;
    return handed;
}

Message messageOf(uint32_t number, size_t size) {
    Message message(size);
    for (size_t i = 0; i < size; ++i) message[i] = static_cast<uint8_t>(number * 31u + i);
    return message;
}

} // namespace

TEST("through loss, duplication and reordering, every message arrives once and in order") {
    // mt19937's output is fixed by the standard; its draws are taken raw, as a distribution's are not.
    std::mt19937 random(20261112u);
    Outbox       outbox;
    Inbox        inbox;

    std::vector<Message>              sent;
    std::vector<Message>              received;
    std::vector<std::vector<uint8_t>> inFlight;
    const auto take = [&](const std::vector<uint8_t>& bytes) {
        bool read = false;
        for (Message& message : deliver(inbox, bytes, &read)) received.push_back(std::move(message));
        CHECK(read);
    };

    for (int tick = 0; tick < 3000; ++tick) {
        for (uint32_t count = random() % 4; count > 0; --count) {
            const Message message = messageOf(static_cast<uint32_t>(sent.size()), 1 + random() % 60);
            if (outbox.push(message.data(), message.size())) sent.push_back(message);
        }

        // Each packet dropped one time in four and sent again one time in eight, drawn apart, and those
        // in flight taken in any order.
        const std::vector<uint8_t> bytes = packetFrom(outbox);
        if (random() % 4 != 0) inFlight.push_back(bytes);
        if (random() % 8 == 0) inFlight.push_back(bytes);
        while (!inFlight.empty() && random() % 3 != 0) {
            const size_t pick = random() % inFlight.size();
            take(inFlight[pick]);
            inFlight.erase(inFlight.begin() + static_cast<std::ptrdiff_t>(pick));
        }

        // The acknowledgement back, lost a third of the time.
        const uint16_t newest = inbox.acknowledge();
        if (random() % 3 != 0) outbox.acknowledge(newest);
    }
    for (int round = 0; round < 100 && outbox.pending() > 0; ++round) {
        take(packetFrom(outbox));
        outbox.acknowledge(inbox.acknowledge());
    }

    CHECK(outbox.pending() == 0u);
    CHECK(sent.size() > 2000u);
    CHECK(received.size() == sent.size());
    CHECK(received == sent);
}

TEST("a message past one the inbox has not had is not handed on, and is once the sender says both, in order") {
    Outbox outbox;
    Inbox  inbox;
    for (uint32_t i = 1; i <= 3; ++i) {
        const Message message = messageOf(i, 4);
        REQUIRE(outbox.push(message.data(), message.size()));
    }
    const std::vector<uint8_t> all = packetFrom(outbox);

    // A sender that takes the first two as had before they are: its next packet starts past them.
    outbox.acknowledge(2);
    const std::vector<uint8_t> third = packetFrom(outbox);
    bool read = false;
    CHECK(deliver(inbox, third, &read).empty());
    CHECK(read);
    CHECK(inbox.newest() == 0);

    CHECK(deliver(inbox, all) == (std::vector<Message>{messageOf(1, 4), messageOf(2, 4), messageOf(3, 4)}));
    CHECK(deliver(inbox, third).empty());
}

TEST("an empty message, one past MAX_MESSAGE_BYTES, and one past MESSAGE_WINDOW waiting are refused and nothing kept") {
    Outbox        outbox;
    const Message longest(MAX_MESSAGE_BYTES, 0x11);
    const Message tooLong(MAX_MESSAGE_BYTES + 1, 0x22);
    CHECK(!outbox.push(nullptr, 0));
    CHECK(!outbox.push(longest.data(), 0));
    CHECK(!outbox.push(nullptr, 4));
    CHECK(!outbox.push(tooLong.data(), tooLong.size()));
    CHECK(outbox.pending() == 0u);

    CHECK(outbox.push(longest.data(), longest.size()));
    for (uint32_t i = 1; i < MESSAGE_WINDOW; ++i) {
        const Message message = messageOf(i, 3);
        CHECK(outbox.push(message.data(), message.size()));
    }
    const Message past = messageOf(999, 3);
    CHECK(!outbox.push(past.data(), past.size()));
    CHECK(outbox.pending() == MESSAGE_WINDOW);

    // What arrives is what was kept: the longest first, the refused one nowhere.
    Inbox                inbox;
    std::vector<Message> handed;
    for (int round = 0; round < 10 && handed.size() < MESSAGE_WINDOW; ++round) {
        for (Message& message : deliver(inbox, packetFrom(outbox))) handed.push_back(std::move(message));
        outbox.acknowledge(inbox.acknowledge());
    }
    REQUIRE(handed.size() == MESSAGE_WINDOW);
    CHECK(handed.front() == longest);
    CHECK(handed.back() == messageOf(MESSAGE_WINDOW - 1, 3));
    CHECK(outbox.pending() == 0u);
}

TEST("a write with too little budget writes the one bit that says none follow") {
    Outbox        outbox;
    const Message message = messageOf(1, 10);
    REQUIRE(outbox.push(message.data(), message.size()));

    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, UdpSocket::MAX_DATAGRAM);
    CHECK(outbox.write(out, 40) == 0u);
    CHECK(out.bitCount() == Outbox::NONE_BITS);
    out.finish();

    Inbox inbox;
    bool read = false;
    CHECK(deliver(inbox, bytes, &read).empty());
    CHECK(read);
    CHECK(outbox.pending() == 1u);

    // With room, it goes.
    CHECK(deliver(inbox, packetFrom(outbox)) == std::vector<Message>{message});
}

TEST("the inbox hands on at most MESSAGE_WINDOW between two acknowledgements") {
    Outbox outbox;
    Inbox  inbox;
    for (uint32_t i = 0; i < MESSAGE_WINDOW; ++i) {
        const Message message = messageOf(i, 2);
        REQUIRE(outbox.push(message.data(), message.size()));
    }
    CHECK(deliver(inbox, packetFrom(outbox)).size() == MESSAGE_WINDOW);

    // A sender that takes its own word for what arrived, and sends a window more.
    outbox.acknowledge(inbox.newest());
    for (uint32_t i = 0; i < MESSAGE_WINDOW; ++i) {
        const Message message = messageOf(100 + i, 2);
        REQUIRE(outbox.push(message.data(), message.size()));
    }
    CHECK(deliver(inbox, packetFrom(outbox)).empty());

    inbox.acknowledge();
    const std::vector<Message> next = deliver(inbox, packetFrom(outbox));
    CHECK(next.size() == MESSAGE_WINDOW);
    CHECK(next.front() == messageOf(100, 2));
}

TEST("the inbox reads a packet of messages without allocating") {
    Outbox outbox;
    for (uint32_t i = 1; i <= 8; ++i) {
        const Message message = messageOf(i, 200);
        REQUIRE(outbox.push(message.data(), message.size()));
    }
    const std::vector<uint8_t> packet = packetFrom(outbox);

    Inbox inbox;
    size_t handed = 0;
    const Inbox::Deliver count = [&](const uint8_t*, size_t) { ++handed; };
    BitReader in(packet.data(), packet.size());

    const uint64_t before = Vkm::Test::allocations();
    CHECK(inbox.read(in, count));
    CHECK(Vkm::Test::allocations() == before);
    CHECK(handed > 0u);
}

TEST("clear forgets every message and numbers the next from the start") {
    Outbox outbox;
    for (uint32_t i = 0; i < 3; ++i) {
        const Message message = messageOf(i, 5);
        REQUIRE(outbox.push(message.data(), message.size()));
    }
    outbox.clear();
    CHECK(outbox.pending() == 0u);
    CHECK(packetFrom(outbox) == std::vector<uint8_t>{0x00});

    // A fresh inbox takes the next one as the first.
    const Message message = messageOf(7, 5);
    REQUIRE(outbox.push(message.data(), message.size()));
    Inbox inbox;
    CHECK(deliver(inbox, packetFrom(outbox)) == std::vector<Message>{message});
    CHECK(inbox.newest() == 1);
}

TEST("messages that do not decode are refused") {
    Inbox inbox;
    bool read = true;
    // A packet that says messages follow, then ends.
    CHECK(deliver(inbox, std::vector<uint8_t>{0x01}, &read).empty());
    CHECK(!read);

    // A message of no bytes is not one a sender writes.
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 16);
    out.boolean(true);
    out.u16(1);
    out.bits(1, 7);
    out.bits(0, 8);
    out.finish();
    CHECK(deliver(inbox, bytes, &read).empty());
    CHECK(!read);

    // Nor is a packet that says messages follow and counts none.
    bytes.clear();
    BitWriter none(bytes, 16);
    none.boolean(true);
    none.u16(1);
    none.bits(0, 7);
    none.finish();
    read = true;
    deliver(inbox, bytes, &read);
    CHECK(!read);
    CHECK(inbox.newest() == 0);
}
