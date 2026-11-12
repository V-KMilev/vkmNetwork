# vkmNetwork

A UDP transport for a game: a non-blocking socket and an IPv4 address; a connection that sequences,
acknowledges, times the round trip, counts loss and refuses what is older than what it has seen;
messages that arrive once and in order, riding a packet's spare room; a stateless join token; a bit
stream; and the coders that turn a float into bits the same way on every machine. It knows nothing of
what a game sends - no messages of its own, no snapshots, no scene - and runs on no thread of its own.

## Conventions

- **UDP, and nothing is retransmitted.** A packet's header acknowledges the newest packet seen and the
  32 before it; a sender says what still matters again, newer, in its next packet.
- **A datagram is at most 1200 bytes** (`UdpSocket::MAX_DATAGRAM`) and is never fragmented: one past
  it is refused, not split.
- **The token is not encryption.** It proves an address hears what is sent to it; it hides nothing
  and signs nothing past that.
- **The wire is the same on every machine:** the bit stream packs least significant bit first, the
  connection's header is little-endian, and the library compiles with fused multiply-add off
  (`-ffp-contract=off`, public, so a consumer's own use of the coders rounds alike).
- **No clock inside.** Time is handed in: `Connection::advance(seconds)`, `JoinCookie::issue(to, now)`.

## A connection

```cpp
using namespace Vkm::Network;

UdpSocket socket;             // one for every peer
if (!socket.open(port)) ...   // 0 for a port the system picks
Connection peer;
peer.open(Address::parse("127.0.0.1:27750"));

std::vector<uint8_t> datagram;
peer.frame(payload.data(), payload.size(), datagram);  // the header, then the payload
socket.send(peer.peer(), datagram.data(), datagram.size());

Address from;
while (socket.receive(datagram, from)) {
    std::vector<uint16_t> acknowledged;
    if (from == peer.peer() && peer.accept(datagram.data(), datagram.size(), acknowledged)) {
        BitReader in(datagram.data() + Connection::HEADER_BYTES, datagram.size() - Connection::HEADER_BYTES);
        // ... read the payload
    }
}
peer.advance(frameSeconds);   // and peer.timedOut() once it has gone quiet
```

`docs/how-it-works.md` says what each piece does, and why.

## Requirements

CMake 3.25 or above and a C++17 compiler. On Windows it links Winsock (`ws2_32`).

## Use

Add it as a submodule and `add_subdirectory` it after the two targets it requires and does not
vendor: `glm`, which crosses its API, and `vkm_log`, which it logs through. The build stops with the
reason when either is missing.

```
git submodule add https://github.com/V-KMilev/vkmNetwork.git modules/vkmNetwork
```

The library is `vkm_network`, static and position-independent, for linking into a library of the
consumer's own. Headers are included by the library's folder: `#include "vkm_network/connection.h"`;
everything is in `Vkm::Network`.

| Option                | Default | What                                                                    |
|-----------------------|---------|-------------------------------------------------------------------------|
| `VKM_NETWORK_ASSERTS` | `OFF`   | The library's `VKM_ASSERT`s run whatever the build type (`-UNDEBUG` on its own sources) |

## What it holds

| Header                | What                                                                          |
|-----------------------|-------------------------------------------------------------------------------|
| `udp_socket.h`        | `UdpSocket`: non-blocking, the same on Windows and Linux                       |
| `address.h`           | `Address`: an IPv4 host and a port; `parse`, `parsePort`, `toString`           |
| `connection.h`        | `Connection`: sequence, acknowledgement, round trip, loss; `isNewerSequence`  |
| `reliable_messages.h` | `Outbox` and `Inbox`: messages once each, in order                             |
| `join_cookie.h`       | `JoinCookie`: the stateless token; `sipHash24`                                 |
| `bit_stream.h`        | `BitWriter` and `BitReader`                                                    |
| `quantize.h`          | `toSigned` (zero exact), `toFixed`, `fixedBits`, the smallest-three rotation coder |
| `little_endian.h`     | `readLittleEndian`, `writeLittleEndian`                                        |

Each is under `src/vkm_network/`.

## In vkmEngine

The engine pins it at `modules/vkmNetwork` and links it whole into `libvkm_core`. Its session, its
messages, replication and prediction are the engine's, on top of this. A change is a commit here, then
an engine commit that moves the pin.
