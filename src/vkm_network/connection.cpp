#define VKM_LOG_CATEGORY "NET"

#include "vkm_network/connection.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vkm_log/assert.h"

#include "vkm_network/little_endian.h"

namespace Vkm::Network {

namespace {

constexpr float TRIP_SMOOTHING = 0.1f;  ///< How much of a new measurement is taken. Small, so one packet held
                                        ///< in a router queue barely moves the estimate.

constexpr uint16_t SEQUENCE_HALF = 0x8000;  ///< Half the sequence space: the point past which "newer" stops
                                            ///< being knowable.

} // namespace

bool isNewerSequence(uint16_t a, uint16_t b) {
    return (a > b && a - b < SEQUENCE_HALF) || (b > a && b - a > SEQUENCE_HALF);
}

void Connection::open(const Address& peer) {
    *this  = Connection{};
    m_peer = peer;
}

void Connection::frame(const uint8_t* payload, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    out.resize(HEADER_BYTES + size);

    const uint16_t sequence = m_outgoing++;
    // On the wrap as well as at the start: NO_SEQUENCE means "nothing heard".
    if (m_outgoing == NO_SEQUENCE) ++m_outgoing;
    writeLittleEndian(&out[0], sequence);
    writeLittleEndian(&out[2], m_window.newest);
    writeLittleEndian(&out[4], m_window.seen);
    if (size > 0) std::memcpy(&out[HEADER_BYTES], payload, size);

    m_sent[sequence % SENT_REMEMBERED] = {sequence, m_clock};
}

bool Connection::accept(const uint8_t* bytes, size_t size, std::vector<uint16_t>& acknowledged) {
    // Kept for refuse(), whether or not this one is taken: marking it seen claims a payload not yet read.
    m_wasWindow = m_window;
    if (size < HEADER_BYTES) return false;

    const auto sequence           = readLittleEndian<uint16_t>(&bytes[0]);
    const auto acknowledgedNewest = readLittleEndian<uint16_t>(&bytes[2]);
    const auto acknowledgedBefore = readLittleEndian<uint32_t>(&bytes[4]);

    const bool heard = m_window.newest != NO_SEQUENCE;
    if (sequence == NO_SEQUENCE || (heard && !isNewerSequence(sequence, m_window.newest))) return false;

    if (heard) {
        // Shift and mark the old newest; at exactly 32 it is the last bit,
        // past that nothing is knowable. In 64 bits: a 32-bit shift by 32 is UB.
        const uint16_t advance = static_cast<uint16_t>(sequence - m_window.newest);

        // Across the wrap it steps over NO_SEQUENCE, which nothing sent.
        const uint16_t neverSent = sequence < m_window.newest ? 1 : 0;
        m_window.missed += advance - 1u - neverSent;

        m_window.seen = advance > 32
            ? 0u
            : static_cast<uint32_t>(
                (static_cast<uint64_t>(m_window.seen) << advance) | (uint64_t{1} << (advance - 1))
            );
    }
    m_window.newest = sequence;
    ++m_window.arrived;

    // Nothing acknowledged, window included: timing or reporting it would
    // invent a trip and confirm a packet never received.
    if (acknowledgedNewest != NO_SEQUENCE) {
        recordTrip(acknowledgedNewest, acknowledged);
        for (int bit = 0; bit < 32; ++bit) {
            if (acknowledgedBefore & (1u << bit)) {
                recordTrip(static_cast<uint16_t>(acknowledgedNewest - (bit + 1)), acknowledged);
            }
        }
    }

    m_silent = 0.0f;
    return true;
}

void Connection::refuse() {
    m_window = m_wasWindow;
}

void Connection::recordTrip(uint16_t acknowledged, std::vector<uint16_t>& reported) {
    Sent& sent = m_sent[acknowledged % SENT_REMEMBERED];
    // Not there: never sent, reported already, or its slot taken by a newer one.
    if (acknowledged == NO_SEQUENCE || sent.sequence != acknowledged) return;

    const float trip = static_cast<float>(m_clock - sent.sentAt);
    sent.sequence = NO_SEQUENCE;
    reported.push_back(acknowledged);

    if (m_roundTrip <= 0.0f) {
        m_roundTrip = trip;
        m_variation = 0.0f;
        return;
    }
    const float error = trip - m_roundTrip;
    m_roundTrip += error * TRIP_SMOOTHING;
    m_variation += (std::abs(error) - m_variation) * TRIP_SMOOTHING;
}

void Connection::advance(float seconds) {
    VKM_ASSERT(seconds >= 0.0f, "a connection advances by a time of zero or more, not %f", seconds);
    m_clock  += seconds;
    m_silent += std::min(seconds, MAX_SILENCE_A_FRAME);
}

} // namespace Vkm::Network
