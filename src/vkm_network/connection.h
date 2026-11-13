#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "vkm_network/address.h"
#include "vkm_network/udp_socket.h"

namespace Vkm::Network {

/**
 * @brief Is @p a a later sequence number than @p b?
 *
 * Sequences wrap: past half the space apart, older is the safe reading.
 *
 * @param a A sequence number.
 * @param b The one to compare it against.
 * @return True when @p a is the later of the two.
 */
bool isNewerSequence(uint16_t a, uint16_t b);

/**
 * @brief One end of one conversation: sequencing, acknowledgement and timing.
 *
 * Every packet carries its sequence, the newest seen the other way, and a
 * bitfield of the thirty-two before that.
 *
 * Nothing is retransmitted: the next packet says it again, newer.
 * Acknowledgement measures the trip and what a peer holds. A packet older than
 * one already seen is refused, or the receiver would mix two moments.
 */
class Connection {
    public:
        static constexpr size_t HEADER_BYTES = 8;  ///< Bytes of header on every packet: sequence,
                                                   ///< acknowledgement, and the bitfield of the thirty-two
                                                   ///< before it.

        static constexpr size_t MAX_PAYLOAD = UdpSocket::MAX_DATAGRAM - HEADER_BYTES;
            ///< The most one packet carries past its header: what a datagram leaves.

        static constexpr float TIMEOUT_SECONDS = 5.0f;  ///< How long silence lasts before a peer is given up
                                                        ///< on.

        static constexpr float MAX_SILENCE_A_FRAME = 0.25f;  ///< The most silence one advance adds: a frame
                                                             ///< this end spent stalled - a load, a long
                                                             ///< frame - is its own, not the peer's, whose
                                                             ///< packets wait in the socket to be read.

        static constexpr uint16_t NO_SEQUENCE = 0;  ///< The sequence number nothing ever sends, so a header
                                                    ///< can say "nothing". A sender skips it, on the first
                                                    ///< packet and on every wrap.

    public:
        Connection() = default;
        ~Connection() = default;

        Connection(const Connection& other) = delete;
        Connection& operator=(const Connection& other) = delete;

        Connection(Connection && other) = default;
        Connection& operator=(Connection && other) = default;

    public:
        /// Point this at @p peer. Resets everything the last one left.
        void open(const Address& peer);

        /**
         * @brief Wrap @p payload in a header and hand it back for sending.
         *
         * The caller owns the socket, which serves every peer of a server.
         *
         * @param payload What to carry; may be empty, which is a keep-alive.
         * @param size    Its length.
         * @param out     Filled with header followed by payload.
         */
        void frame(const uint8_t* payload, size_t size, std::vector<uint8_t>& out);

        /**
         * @brief Take a datagram that arrived from this peer.
         *
         * What rode inside it is the datagram past HEADER_BYTES; the caller reads it there.
         *
         * @param bytes        The whole datagram, header included.
         * @param size         Its length.
         * @param acknowledged Appended with the sequences this datagram confirms among the 255 this
         *                     end sent last, each at most once.
         * @return False when it is malformed, is numbered NO_SEQUENCE, or is older than one already seen.
         */
        bool accept(const uint8_t* bytes, size_t size, std::vector<uint16_t>& acknowledged);

        /**
         * @brief Take back the acknowledgement for the packet just accepted.
         *
         * A sender may read an acknowledgement as the payload taken, so a payload
         * that would not decode is un-seen here, and the sender says it again. After
         * an accept() that answered false, nothing is taken back.
         */
        void refuse();

        /// Move this end's clock, which times each trip, on by @p seconds of wall clock, at least zero,
        /// and the silence timer by as much up to MAX_SILENCE_A_FRAME.
        void advance(float seconds);

        /**
         * @brief Whether the peer has been silent longer than @p after seconds.
         *
         * @param after Silence allowed, in seconds: TIMEOUT_SECONDS unless the caller knows it is busy.
         */
        bool timedOut(float after = TIMEOUT_SECONDS) const { return m_silent > after; }

        /**
         * @brief What frame() will stamp on the next packet.
         *
         * For a sender that must remember what that packet claimed.
         *
         * @return The next outgoing sequence.
         */
        uint16_t nextSequence() const { return m_outgoing; }

        /**
         * @brief Round trip, smoothed.
         *
         * @return Seconds; zero until an acknowledgement has been timed.
         */
        float roundTrip() const { return m_roundTrip; }

        /**
         * @brief What fraction of the peer's packets never arrived, so far.
         *
         * Counted from gaps in its sequence numbering after the first packet taken, a refused one
         * among them; zero until something arrives.
         *
         * @return Lost over sent, in [0, 1).
         */
        float lossFraction() const {
            const uint64_t seen = m_window.arrived + m_window.missed;
            return seen == 0 ? 0.0f : static_cast<float>(m_window.missed) / static_cast<float>(seen);
        }

        /// How much that trip varies, smoothed the same way.
        float roundTripVariation() const { return m_variation; }

        const Address& peer() const { return m_peer; }

    private:
        /**
         * @brief What this end has heard of the peer: what accept() moves and refuse() puts back.
         */
        struct Window {
            uint16_t newest  = NO_SEQUENCE;  ///< Newest sequence seen from the peer; NO_SEQUENCE before any.
            uint32_t seen    = 0;            ///< The thirty-two before it, one bit each.
            uint64_t arrived = 0;            ///< Packets from the peer that landed.
            uint64_t missed  = 0;            ///< Gaps its sequence numbering stepped over.
        };

        /**
         * @brief When one recent packet went out, to time its acknowledgement.
         */
        struct Sent {
            uint16_t sequence = NO_SEQUENCE;  ///< NO_SEQUENCE for a slot that holds none.
            double   sentAt   = 0.0;          ///< This end's clock when it went out.
        };

        static constexpr size_t SENT_REMEMBERED = 255;  ///< Packets remembered until acknowledged, so the
                                                        ///< acknowledgement can be timed; each newer one
                                                        ///< takes the slot of the one this many before it.
                                                        ///< 255 divides the 65535 sequences sent, so it
                                                        ///< holds across the wrap too.

    private:
        void recordTrip(uint16_t acknowledged, std::vector<uint16_t>& reported);

    private:
        Address m_peer;

        uint16_t m_outgoing = NO_SEQUENCE + 1;  ///< Next sequence this end will send.

        Window m_window;
        Window m_wasWindow;  ///< The window before the last accept, for refuse().

        double m_clock  = 0.0;   ///< Seconds advanced since open(): what a trip is timed against.
        float  m_silent = 0.0f;  ///< Since anything arrived.

        float m_roundTrip = 0.0f;
        float m_variation = 0.0f;

        std::array<Sent, SENT_REMEMBERED> m_sent;  ///< Each at its sequence modulo SENT_REMEMBERED.
};

} // namespace Vkm::Network
