# How vkmNetwork works

What each piece of the transport does, and why. The API is in the headers; this page is what they
cannot say.

## The socket

`UdpSocket` is non-blocking, so a frame never waits on the network; one socket serves every peer, so a
caller frames a datagram on a peer's `Connection` and sends it on the socket. `receive` takes one
datagram a call: a caller bounds how many it reads in a frame, so a sender cannot choose what a frame
costs. An empty or oversized datagram answers true with nothing in it, so that bound counts it.

**At most `MAX_DATAGRAM` (1200 bytes).** Past it IP fragments, and one lost fragment loses the whole
datagram; a bigger one is refused when sent and dropped when it arrives, each said once on the
socket's log rather than at the frame rate. On Windows a peer's ICMP "port unreachable" would fail the
next read, so a dead peer would cut a live one's reads short; the socket turns that report off.

`Address` is an IPv4 host and a port in host byte order. `parse` takes "host:port", or a host and the
caller's fallback port, and goes through the system's resolver for a name, which blocks until it
answers, so a name is resolved off the frame; "27015x" is refused whole,
not read as 27015. Winsock is started before any entry point is called, resolving included.

## The connection

Every datagram a `Connection` frames carries an 8-byte header, little-endian on every machine: its
sequence, the newest sequence heard from the peer, and a bitfield of the 32 before that. **Nothing is
retransmitted**: a sender says again, newer, whatever still matters. What an acknowledgement is for is
measuring the round trip and telling a sender what its peer holds.

- **A packet no newer than one already seen is refused**, a copy too, or a receiver would mix two
  moments. Sequences are 16 bits and wrap: past half the space apart, older is the safe reading
  (`isNewerSequence`).
- **Sequence 0 is never sent**, on the first packet or after a wrap, so a header can say "nothing
  heard" and a peer that has heard nothing acknowledges nothing; a packet numbered with it is refused.
- **Each acknowledgement is reported at most once.** The 255 packets sent most recently are
  remembered until acknowledged, each with when it went out, so a reported one is timed; one older
  than that is neither timed nor reported.
- **`refuse()` takes back the window the last `accept` moved.** A sender may read an acknowledgement
  as its payload taken, so a payload the receiver could not decode must be un-seen and said again.
- **The round trip and its variation are smoothed**, a tenth of each new measurement taken, so one
  packet held in a router's queue barely moves them. Loss counts the gaps in the peer's numbering after
  the first packet taken, a refused packet among them.
- **Silence grows by at most `MAX_SILENCE_A_FRAME` an `advance`.** A frame this end spent stalled is
  its own, not the peer's, whose packets wait in the socket; after `TIMEOUT_SECONDS` of silence the
  peer is `timedOut`.

## Messages: once, in order

`Outbox` numbers each message and keeps it until the other end says it has every one up to it. Every
packet carries the oldest messages kept, as many as fit its spare room in order: a message rides each
packet until it is acknowledged - redundancy, not a retransmit timer. `Inbox` hands on only the next
in order; a copy is skipped, and one past a gap waits for the sender to say it again, which it does,
as it starts from what is unacknowledged.

- A message is 1 to `MAX_MESSAGE_BYTES` (255) bytes, its length a byte on the wire.
- At most `MESSAGE_WINDOW` (64) wait unacknowledged; a push past it is refused, and the caller decides
  what to drop. Small against the 16-bit numbering, so a cumulative acknowledgement is never ambiguous.
- An inbox hands on at most `MESSAGE_WINDOW` between two acknowledgements it sends, so a peer sending
  packet after packet of messages costs the receiver no more than one keeping up.

## The join token

A source address is trivially forged, so a server proves an address before it keeps anything for it: it
answers with a token the address must echo. `JoinCookie` makes that token **stateless** - SipHash-2-4 of
the address and a ten-second window under a key drawn from the operating system - so a flood of forged
requests costs a hash and a reply no larger each, and crowds out no real join. A token is good for its
window and the next, and a cookie made again voids every token given before. It proves an address hears;
it is not encryption. `sipHash24` is the reference SipHash-2-4, reading its words little-endian, so
every machine hashes alike.

## The bit stream and the coders

`BitWriter` packs a field into exactly its width, least significant bit first, into whole bytes;
a `u64` goes as two 32-bit words, high first; an `f32` as its exact bits. A write past the capacity
sets a sticky overflow and writes nothing, a read past the end a sticky failure and reads zero, so a
caller asks once at the end.

The coders turn floats into codes the same way on every machine: they use only what IEEE 754 rounds
exactly (+, -, *, /, `sqrt`, `llround`, comparisons), and the library compiles with fused multiply-add
off. Two of their properties are load-bearing:

- **Zero is exact for every signed field** (`toSigned`). Off by half a step, a rotation tilts, a body
  at rest drifts and a stick creeps; so a signed field spends one code fewer than half its codes each
  side, and the middle one is zero. Past the range a value is clamped; NaN is zero.
- **A rotation is its three smallest components** (`writeRotation`), each within +/- 1/sqrt(2), and a
  two-bit index of the largest, which is recovered from the unit length and made positive, since q and
  -q are the same rotation. Its error is an angle, so the width a caller gives it decides how far the
  far end of a long thing may lift.

`toFixed` and `fixedBits` carry a value on a grid from a low end, clamped to the codes the width
holds; what the grid is - how far a world reaches, how fine - is the caller's.
